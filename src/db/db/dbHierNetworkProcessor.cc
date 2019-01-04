
/*

  KLayout Layout Viewer
  Copyright (C) 2006-2018 Matthias Koefferlein

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/


#include "dbHierNetworkProcessor.h"
#include "dbShape.h"
#include "dbShapes.h"
#include "dbInstElement.h"
#include "dbPolygon.h"
#include "dbPolygonTools.h"
#include "dbBoxScanner.h"
#include "dbDeepRegion.h"
#include "tlProgress.h"
#include "tlLog.h"
#include "tlTimer.h"

#include <vector>
#include <map>
#include <list>
#include <set>

namespace db
{

// ------------------------------------------------------------------------------
//  Connectivity implementation

Connectivity::Connectivity ()
{
  //  .. nothing yet ..
}

void
Connectivity::connect (unsigned int la, unsigned int lb)
{
  m_connected [la].insert (lb);
  m_connected [lb].insert (la);
  m_all_layers.insert (la);
  m_all_layers.insert (lb);
}

void
Connectivity::connect (unsigned int l)
{
  m_connected [l].insert (l);
  m_all_layers.insert (l);
}

void
Connectivity::connect (const db::DeepLayer &l)
{
  connect (l.layer ());
}

void
Connectivity::connect (const db::DeepLayer &la, const db::DeepLayer &lb)
{
  connect (la.layer (), lb.layer ());
}

Connectivity::layer_iterator
Connectivity::begin_layers () const
{
  return m_all_layers.begin ();
}

Connectivity::layer_iterator
Connectivity::end_layers () const
{
  return m_all_layers.end ();
}

Connectivity::layers_type s_empty_layers;

Connectivity::layer_iterator
Connectivity::begin_connected (unsigned int layer) const
{
  std::map<unsigned int, layers_type>::const_iterator i = m_connected.find (layer);
  if (i == m_connected.end ()) {
    return s_empty_layers.begin ();
  } else {
    return i->second.begin ();
  }
}

Connectivity::layer_iterator
Connectivity::end_connected (unsigned int layer) const
{
  std::map<unsigned int, layers_type>::const_iterator i = m_connected.find (layer);
  if (i == m_connected.end ()) {
    return s_empty_layers.end ();
  } else {
    return i->second.end ();
  }
}

template <class Trans>
static bool
interaction_test (const db::PolygonRef &a, const db::PolygonRef &b, const Trans &trans)
{
  //  TODO: this could be part of db::interact (including transformation)
  if (a.obj ().is_box () && b.obj ().is_box ()) {
    return db::interact (a.obj ().box ().transformed (a.trans ()), b.obj ().box ().transformed (trans * Trans (b.trans ())));
  } else {
    return db::interact (a.obj ().transformed (a.trans ()), b.obj ().transformed (trans * Trans (b.trans ())));
  }
}

template <class C>
static bool
interaction_test (const db::PolygonRef &a, const db::PolygonRef &b, const db::unit_trans<C> &)
{
  //  TODO: this could be part of db::interact (including transformation)
  if (a.obj ().is_box () && b.obj ().is_box ()) {
    return db::interact (a.obj ().box ().transformed (a.trans ()), b.obj ().box ().transformed (b.trans ()));
  } else {
    return db::interact (a.obj ().transformed (a.trans ()), b.obj ().transformed (b.trans ()));
  }
}

template <class T, class Trans>
bool Connectivity::interacts (const T &a, unsigned int la, const T &b, unsigned int lb, const Trans &trans) const
{
  std::map<unsigned int, layers_type>::const_iterator i = m_connected.find (la);
  if (i == m_connected.end () || i->second.find (lb) == i->second.end ()) {
    return false;
  } else {
    return interaction_test (a, b, trans);
  }
}

//  explicit instantiations
template DB_PUBLIC bool Connectivity::interacts<db::PolygonRef> (const db::PolygonRef &a, unsigned int la, const db::PolygonRef &b, unsigned int lb, const db::UnitTrans &trans) const;
template DB_PUBLIC bool Connectivity::interacts<db::PolygonRef> (const db::PolygonRef &a, unsigned int la, const db::PolygonRef &b, unsigned int lb, const db::ICplxTrans &trans) const;

// ------------------------------------------------------------------------------
//  local_cluster implementation

template <class T>
local_cluster<T>::local_cluster (size_t id)
  : m_id (id), m_needs_update (false), m_size (0)
{
  //  .. nothing yet ..
}

template <class T>
void
local_cluster<T>::clear ()
{
  m_shapes.clear ();
  m_needs_update = false;
  m_size = 0;
  m_bbox = box_type ();
  m_attrs.clear ();
}

template <class T>
void
local_cluster<T>::add_attr (attr_id a)
{
  if (a > 0) {
    m_attrs.insert (a);
  }
}

template <class T>
void
local_cluster<T>::add (const T &s, unsigned int la)
{
  m_shapes[la].insert (s);
  m_needs_update = true;
  ++m_size;
}

template <class T>
void
local_cluster<T>::join_with (const local_cluster<T> &other)
{
  for (typename std::map<unsigned int, tree_type>::const_iterator s = other.m_shapes.begin (); s != other.m_shapes.end (); ++s) {
    tree_type &tree = m_shapes[s->first];
    tree.insert (s->second.begin (), s->second.end ());
  }

  m_attrs.insert (other.m_attrs.begin (), other.m_attrs.end ());
  m_size += other.size ();

  m_needs_update = true;
}

template <class T>
void
local_cluster<T>::ensure_sorted ()
{
  if (! m_needs_update) {
    return;
  }

  //  sort the shape trees
  for (typename std::map<unsigned int, tree_type>::iterator s = m_shapes.begin (); s != m_shapes.end (); ++s) {
    s->second.sort (db::box_convert<T> ());
  }

  //  recompute bounding box
  m_bbox = box_type ();
  db::box_convert<T> bc;
  for (typename std::map<unsigned int, tree_type>::const_iterator s = m_shapes.begin (); s != m_shapes.end (); ++s) {
    for (typename tree_type::const_iterator i = s->second.begin (); i != s->second.end (); ++i) {
      m_bbox += bc (*i);
    }
  }

  m_needs_update = false;
}

namespace
{

template <class T>
struct interaction_receiver
  : public box_scanner_receiver2<T, unsigned int, T, unsigned int>
{
public:
  typedef typename local_cluster<T>::box_type box_type;

  interaction_receiver (const Connectivity &conn, const db::ICplxTrans &trans)
    : mp_conn (&conn), m_any (false), m_trans (trans)
  {
    //  .. nothing yet ..
  }

  void add (const T *s1, unsigned int l1, const T *s2, unsigned int l2)
  {
    if (mp_conn->interacts (*s1, l1, *s2, l2, m_trans)) {
      m_any = true;
    }
  }

  bool stop () const
  {
    return m_any;
  }

private:
  const Connectivity *mp_conn;
  bool m_any;
  db::ICplxTrans m_trans;
};

template <class T, class Trans>
struct transformed_box
{
  typedef db::box_convert<T> base_bc;
  typedef typename T::box_type box_type;

  transformed_box (const Trans &trans)
    : m_trans (trans)
  {
    //  .. nothing yet ..
  }

  box_type operator() (const T &t) const
  {
    return m_bc (t).transformed (m_trans);
  }

private:
  base_bc m_bc;
  Trans m_trans;
};

}

template <class T>
typename local_cluster<T>::shape_iterator local_cluster<T>::begin (unsigned int l) const
{
  static tree_type s_empty_tree;

  typename std::map<unsigned int, tree_type>::const_iterator i = m_shapes.find (l);
  if (i == m_shapes.end ()) {
    return s_empty_tree.begin_flat ();
  } else {
    return i->second.begin_flat ();
  }
}

template <class T>
bool
local_cluster<T>::interacts (const db::Cell &cell, const db::ICplxTrans &trans, const Connectivity &conn) const
{
  db::box_convert<T> bc;

  for (typename std::map<unsigned int, tree_type>::const_iterator s = m_shapes.begin (); s != m_shapes.end (); ++s) {

    db::Box box;

    Connectivity::layer_iterator le = conn.end_connected (s->first);
    for (Connectivity::layer_iterator l = conn.begin_connected (s->first); l != le; ++l) {
      box += cell.bbox (*l);
    }

    if (! box.empty () && ! s->second.begin_touching (box.transformed (trans), bc).at_end ()) {
      return true;
    }

  }

  return false;
}

template <class T>
bool
local_cluster<T>::interacts (const local_cluster<T> &other, const db::ICplxTrans &trans, const Connectivity &conn) const
{
  db::box_convert<T> bc;

  const_cast<local_cluster<T> *> (this)->ensure_sorted ();

  box_type common = other.bbox ().transformed (trans) & bbox ();
  if (common.empty ()) {
    return false;
  }

  box_type common_for_other = common.transformed (trans.inverted ());

  //  shortcut evaluation for disjunct layers

  std::set<unsigned int> ll1;
  for (typename std::map<unsigned int, tree_type>::const_iterator s = m_shapes.begin (); s != m_shapes.end (); ++s) {
    if (! s->second.begin_touching (common, bc).at_end ()) {
      ll1.insert (s->first);
    }
  }

  if (ll1.empty ()) {
    return false;
  }

  std::set<unsigned int> ll2;
  for (typename std::map<unsigned int, tree_type>::const_iterator s = other.m_shapes.begin (); s != other.m_shapes.end (); ++s) {
    if (! s->second.begin_touching (common_for_other, bc).at_end ()) {
      ll2.insert (s->first);
    }
  }

  if (ll2.empty ()) {
    return false;
  }

  bool any = false;

  for (std::set<unsigned int>::const_iterator i = ll1.begin (); i != ll1.end () && !any; ++i) {
    db::Connectivity::layer_iterator je = conn.end_connected (*i);
    for (db::Connectivity::layer_iterator j = conn.begin_connected (*i); j != je && !any; ++j) {
      any = (ll2.find (*j) != ll2.end ());
    }
  }
  if (! any) {
    return false;
  }

  //  detailed analysis

  db::box_scanner2<T, unsigned int, T, unsigned int> scanner;
  transformed_box <T, db::ICplxTrans> bc_t (trans);

  for (typename std::map<unsigned int, tree_type>::const_iterator s = m_shapes.begin (); s != m_shapes.end (); ++s) {
    for (typename tree_type::touching_iterator i = s->second.begin_touching (common, bc); ! i.at_end (); ++i) {
      scanner.insert1 (i.operator-> (), s->first);
    }
  }

  for (typename std::map<unsigned int, tree_type>::const_iterator s = other.m_shapes.begin (); s != other.m_shapes.end (); ++s) {
    for (typename tree_type::touching_iterator i = s->second.begin_touching (common_for_other, bc); ! i.at_end (); ++i) {
      scanner.insert2 (i.operator-> (), s->first);
    }
  }

  interaction_receiver<T> rec (conn, trans);
  return ! scanner.process (rec, 1 /*==touching*/, bc, bc_t);
}

template <class T>
double local_cluster<T>::area_ratio () const
{
  box_type bx = bbox ();
  if (bx.empty ()) {
    return 0.0;
  }

  db::box_convert<T> bc;

  //  just the sum of the areas of the bounding boxes - this is precise if no overlaps happen and the
  //  polygons are rather rectangular. This criterion is coarse enough to prevent recursion in the split
  //  algorithm and still be fine enough - consider that we a planning to use splitted polygons for
  //  which the bbox is a fairly good approximation.
  typename box_type::area_type a = 0;
  for (typename std::map<unsigned int, tree_type>::const_iterator s = m_shapes.begin (); s != m_shapes.end (); ++s) {
    for (typename tree_type::const_iterator i = s->second.begin (); i != s->second.end (); ++i) {
      a += bc (*i).area ();
    }
  }

  return (a == 0 ? 0.0 : double (bx.area ()) / double (a));
}

template <class T>
std::vector<unsigned int>
local_cluster<T>::layers () const
{
  std::vector<unsigned int> l;
  l.reserve (m_shapes.size ());
  for (typename std::map<unsigned int, tree_type>::const_iterator s = m_shapes.begin (); s != m_shapes.end (); ++s) {
    l.push_back (s->first);
  }
  return l;
}

template <class T, class Iter>
size_t split_cluster (const local_cluster<T> &cl, double max_area_ratio, Iter &output)
{
  if (cl.area_ratio () < max_area_ratio) {
    return 0;  //  no splitting happened
  }

  db::box_convert<T> bc;
  typename local_cluster<T>::box_type bx = cl.bbox ();

  int xthr = bx.width () > bx.height () ? bx.center ().x () : bx.left ();
  int ythr = bx.width () > bx.height () ? bx.bottom () : bx.center ().y ();

  //  split along the longer axis - decide the position according to the bbox center

  local_cluster<T> a (cl.id ()), b (cl.id ());

  std::vector<unsigned int> layers = cl.layers ();

  for (std::vector<unsigned int>::const_iterator l = layers.begin (); l != layers.end (); ++l) {
    for (typename local_cluster<T>::shape_iterator s = cl.begin (*l); ! s.at_end (); ++s) {
      typename local_cluster<T>::box_type::point_type sc = bc (*s).center ();
      if (sc.x () < xthr || sc.y () < ythr) {
        a.add (*s, *l);
      } else {
        b.add (*s, *l);
      }
    }
  }

  if (a.size () == 0 || b.size () == 0) {
    //  give up to prevent infinite recursion
    return 0;
  }

  //  split further if required
  size_t na = split_cluster (a, max_area_ratio, output);
  size_t nb = split_cluster (b, max_area_ratio, output);

  if (na == 0) {
    *output++ = a;
    na = 1;
  }

  if (nb == 0) {
    *output++ = b;
    nb = 1;
  }

  return na + nb;
}

template <class T>
template <class Iter>
size_t local_cluster<T>::split (double max_area_ratio, Iter &output) const
{
  return split_cluster (*this, max_area_ratio, output);
}

//  explicit instantiations
template class DB_PUBLIC local_cluster<db::PolygonRef>;
template DB_PUBLIC size_t local_cluster<db::PolygonRef>::split<std::back_insert_iterator<std::list<local_cluster<db::PolygonRef> > > > (double, std::back_insert_iterator<std::list<local_cluster<db::PolygonRef> > > &) const;

// ------------------------------------------------------------------------------
//  local_clusters implementation

template <class T>
local_clusters<T>::local_clusters ()
  : m_needs_update (false), m_next_dummy_id (0)
{
  //  .. nothing yet ..
}

template <class T>
void local_clusters<T>::clear ()
{
  m_needs_update = false;
  m_clusters.clear ();
  m_bbox = box_type ();
  m_next_dummy_id = 0;
}

template <class T>
const local_cluster<T> &
local_clusters<T>::cluster_by_id (typename local_cluster<T>::id_type id) const
{
  tl_assert (id > 0);

  if (id > m_clusters.size ()) {

    //  dummy connectors are not real ones - they just carry an arbitrary
    //  ID. Still they need to be treated as empty ones.
    static local_cluster<T> empty_cluster;
    return empty_cluster;

  } else {

    //  by convention the ID is the index + 1 so 0 can be used as "nil"
    return m_clusters.objects ().item (id - 1);

  }
}

template <class T>
void
local_clusters<T>::remove_cluster (typename local_cluster<T>::id_type id)
{
  if (id == 0 || id > m_clusters.size ()) {
    return;
  }

  //  TODO: this const_cast is required. But we know what we're doing ...
  //  NOTE: we cannot really delete a cluster as this would shift the indexes. So
  //  we just clear them.
  local_cluster<T> *to_delete = const_cast<local_cluster<T> *> (& m_clusters.objects ().item (id - 1));
  to_delete->clear ();
  m_needs_update = true;
}

template <class T>
void
local_clusters<T>::join_cluster_with (typename local_cluster<T>::id_type id, typename local_cluster<T>::id_type with_id)
{
  tl_assert (id > 0);
  if (with_id == 0 || with_id > m_clusters.size () || id > m_clusters.size ()) {
    return;
  }

  //  TODO: this const_cast is required. But we know what we're doing ...
  local_cluster<T> *with = const_cast<local_cluster<T> *> (& m_clusters.objects ().item (with_id - 1));
  local_cluster<T> *first = const_cast<local_cluster<T> *> (& m_clusters.objects ().item (id - 1));
  first->join_with (*with);

  //  NOTE: we cannot really delete a cluster as this would shift the indexes. So
  //  we just clear them.
  with->clear ();

  m_needs_update = true;
}

template <class T>
local_cluster<T> *
local_clusters<T>::insert ()
{
  typename tree_type::iterator i = m_clusters.insert (local_cluster<T> ());
  i->set_id (i.index () + 1);
  m_needs_update = true;
  return i.operator-> ();
}

template <class T>
void
local_clusters<T>::ensure_sorted ()
{
  if (! m_needs_update) {
    return;
  }

  //  sort the shape trees
  m_clusters.sort (local_cluster_box_convert<T> ());

  //  recompute bounding box
  m_bbox = box_type ();
  for (typename tree_type::const_iterator i = m_clusters.begin (); i != m_clusters.end (); ++i) {
    m_bbox += i->bbox ();
  }

  m_needs_update = false;
}

namespace
{

template <class T, class BoxTree>
struct cluster_building_receiver
  : public db::box_scanner_receiver<T, std::pair<unsigned int, unsigned int> >
{
  typedef typename local_cluster<T>::id_type id_type;
  typedef std::pair<const T *, std::pair<unsigned int, unsigned int> > shape_value;

  cluster_building_receiver (const db::Connectivity &conn)
    : mp_conn (&conn)
  {
    //  .. nothing yet..
  }

  void generate_clusters (local_clusters<T> &clusters)
  {
    //  build the resulting clusters
    for (typename std::list<std::vector<shape_value> >::const_iterator c = m_clusters.begin (); c != m_clusters.end (); ++c) {

      //  TODO: reserve?
      local_cluster<T> *cluster = clusters.insert ();
      for (typename std::vector<shape_value>::const_iterator s = c->begin (); s != c->end (); ++s) {
        cluster->add (*s->first, s->second.first);
        cluster->add_attr (s->second.second);
      }

    }
  }

  void add (const T *s1, std::pair<unsigned int, unsigned int> p1, const T *s2, std::pair<unsigned int, unsigned int> p2)
  {
    if (! mp_conn->interacts (*s1, p1.first, *s2, p2.first)) {
      return;
    }

    typename std::map<const T *, typename std::list<std::vector<shape_value> >::iterator>::iterator ic1 = m_shape_to_clusters.find (s1);
    typename std::map<const T *, typename std::list<std::vector<shape_value> >::iterator>::iterator ic2 = m_shape_to_clusters.find (s2);

    if (ic1 == m_shape_to_clusters.end ()) {

      if (ic2 == m_shape_to_clusters.end ()) {

        m_clusters.push_back (std::vector<shape_value> ());
        typename std::list<std::vector<shape_value> >::iterator c = --m_clusters.end ();
        c->push_back (std::make_pair (s1, p1));
        c->push_back (std::make_pair (s2, p2));

        m_shape_to_clusters.insert (std::make_pair (s1, c));
        m_shape_to_clusters.insert (std::make_pair (s2, c));

      } else {

        ic2->second->push_back (std::make_pair (s1, p1));
        m_shape_to_clusters.insert (std::make_pair (s1, ic2->second));

      }

    } else if (ic2 == m_shape_to_clusters.end ()) {

      ic1->second->push_back (std::make_pair (s2, p2));
      m_shape_to_clusters.insert (std::make_pair (s2, ic1->second));

    } else if (ic1->second != ic2->second) {

      //  join clusters: use the larger one as the target

      if (ic1->second->size () < ic2->second->size ()) {
        std::swap (ic1, ic2);
      }

      ic1->second->insert (ic1->second->end (), ic2->second->begin (), ic2->second->end ());

      typename std::list<std::vector<shape_value> >::iterator j = ic2->second;
      for (typename std::vector<shape_value>::const_iterator i = j->begin (); i != j->end (); ++i) {
        m_shape_to_clusters [i->first] = ic1->second;
      }

      m_clusters.erase (j);

    }
  }

  void finish (const T *s, std::pair<unsigned int, unsigned> p)
  {
    //  if the shape has not been handled yet, insert a single cluster with only this shape
    typename std::map<const T *, typename std::list<std::vector<shape_value> >::iterator>::const_iterator ic = m_shape_to_clusters.find (s);
    if (ic == m_shape_to_clusters.end ()) {
      m_clusters.push_back (std::vector<shape_value> ());
      typename std::list<std::vector<shape_value> >::iterator c = --m_clusters.end ();
      c->push_back (std::make_pair (s, p));
      m_shape_to_clusters.insert (std::make_pair (s, c));
    }
  }

private:
  const db::Connectivity *mp_conn;
  std::map<const T *, typename std::list<std::vector<shape_value> >::iterator> m_shape_to_clusters;
  std::list<std::vector<shape_value> > m_clusters;
};

}

template <class T>
void
local_clusters<T>::build_clusters (const db::Cell &cell, db::ShapeIterator::flags_type shape_flags, const db::Connectivity &conn)
{
  bool report_progress = tl::verbosity () >= 50;
  static std::string desc = tl::to_string (tr ("Building local clusters"));

  db::box_scanner<T, std::pair<unsigned int, unsigned int> > bs (report_progress, desc);
  typename T::tag object_tag;
  db::box_convert<T> bc;

  for (db::Connectivity::layer_iterator l = conn.begin_layers (); l != conn.end_layers (); ++l) {
    const db::Shapes &shapes = cell.shapes (*l);
    for (db::Shapes::shape_iterator s = shapes.begin (shape_flags); ! s.at_end (); ++s) {
      bs.insert (s->basic_ptr (object_tag), std::make_pair (*l, (attr_id) s->prop_id ()));
    }
  }

  cluster_building_receiver<T, box_type> rec (conn);
  bs.process (rec, 1 /*==touching*/, bc);
  rec.generate_clusters (*this);
}

//  explicit instantiations
template class DB_PUBLIC local_clusters<db::PolygonRef>;

// ------------------------------------------------------------------------------
//  connected_clusters implementation

template <class T>
const typename connected_clusters<T>::connections_type &
connected_clusters<T>::connections_for_cluster (typename local_cluster<T>::id_type id) const
{
  typename std::map<typename local_cluster<T>::id_type, connections_type>::const_iterator c = m_connections.find (id);
  if (c == m_connections.end ()) {
    static connections_type empty_connections;
    return empty_connections;
  } else {
    return c->second;
  }
}

template <class T>
void
connected_clusters<T>::add_connection (typename local_cluster<T>::id_type id, const ClusterInstance &inst)
{
  m_connections [id].push_back (inst);
  m_rev_connections [inst] = id;
}

template <class T>
void
connected_clusters<T>::join_cluster_with (typename local_cluster<T>::id_type id, typename local_cluster<T>::id_type with_id)
{
  if (id == with_id) {
    return;
  }

  //  join the shape clusters
  local_clusters<T>::join_cluster_with (id, with_id);

  //  handle the connections by translating

  const connections_type &to_join = connections_for_cluster (with_id);
  connections_type &target = m_connections [id];
  target.insert (target.end (), to_join.begin (), to_join.end ());

  for (connections_type::const_iterator c = to_join.begin (); c != to_join.end (); ++c) {
    m_rev_connections [*c] = id;
  }

  m_connections.erase (with_id);
}

template <class T>
typename local_cluster<T>::id_type
connected_clusters<T>::find_cluster_with_connection (const ClusterInstance &inst) const
{
  typename std::map<ClusterInstance, typename local_cluster<T>::id_type>::const_iterator rc = m_rev_connections.find (inst);
  if (rc != m_rev_connections.end ()) {
    return rc->second;
  } else {
    return 0;
  }
}

//  explicit instantiations
template class DB_PUBLIC connected_clusters<db::PolygonRef>;

// ------------------------------------------------------------------------------
//  connected_clusters implementation

template <class T>
class DB_PUBLIC cell_clusters_box_converter
{
public:
  typedef db::simple_bbox_tag complexity;
  typedef typename hier_clusters<T>::box_type box_type;

  cell_clusters_box_converter (const db::Layout &layout, const hier_clusters<T> &tree)
    : mp_layout (&layout), mp_tree (&tree)
  {
    //  .. nothing yet ..
  }

  const box_type &operator() (const db::CellInst &cell_inst) const
  {
    return (*this) (cell_inst.cell_index ());
  }

  const box_type &operator() (db::cell_index_type cell_index) const
  {
    typename std::map<db::cell_index_type, box_type>::const_iterator b = m_cache.find (cell_index);
    if (b != m_cache.end ()) {

      return b->second;

    } else {

      const connected_clusters<T> &clusters = mp_tree->clusters_per_cell (cell_index);
      box_type box = clusters.bbox ();

      const db::Cell &cell = mp_layout->cell (cell_index);
      for (db::Cell::const_iterator inst = cell.begin (); ! inst.at_end (); ++inst) {
        const db::CellInstArray &inst_array = inst->cell_inst ();
        box += inst_array.bbox (*this);
      }

      return m_cache.insert (std::make_pair (cell_index, box)).first->second;

    }
  }

private:
  mutable std::map<db::cell_index_type, box_type> m_cache;
  const db::Layout *mp_layout;
  const hier_clusters<T> *mp_tree;
};

// ------------------------------------------------------------------------------
//  hier_clusters implementation

template <class T>
hier_clusters<T>::hier_clusters ()
{
  //  .. nothing yet ..
}

template <class T>
void hier_clusters<T>::clear ()
{
  m_per_cell_clusters.clear ();
}

template <class T>
void
hier_clusters<T>::build (const db::Layout &layout, const db::Cell &cell, db::ShapeIterator::flags_type shape_flags, const db::Connectivity &conn)
{
  clear ();
  cell_clusters_box_converter<T> cbc (layout, *this);
  do_build (cbc, layout, cell, shape_flags, conn);
}

namespace
{

/**
 *  @brief The central interaction tester between clusters on a hierarchical level
 *
 *  This receiver is both used for the instance-to-instance and the local-to-instance
 *  interactions. It is employed on cell level for in two box scanners: one
 *  investigating the instance-to-instance interactions and another one invesitating
 *  local cluster to instance interactions.
 */
template <class T>
struct hc_receiver
  : public db::box_scanner_receiver<db::Instance, unsigned int>,
    public db::box_scanner_receiver2<local_cluster<T>, unsigned int, db::Instance, unsigned int>
{
public:
  typedef typename hier_clusters<T>::box_type box_type;
  typedef typename local_cluster<T>::id_type id_type;
  typedef std::map<ClusterInstance, id_type> connector_map;

  /**
   *  @brief Constructor
   */
  hc_receiver (const db::Layout &layout, const db::Cell &cell, db::connected_clusters<T> &cell_clusters, hier_clusters<T> &tree, const cell_clusters_box_converter<T> &cbc, const db::Connectivity &conn)
    : mp_layout (&layout), mp_cell (&cell), mp_tree (&tree), mp_cbc (&cbc), mp_conn (&conn)
  {
    mp_cell_clusters = &cell_clusters;
  }

  /**
   *  @brief Receiver main event for instance-to-instance interactions
   */
  void add (const db::Instance *i1, unsigned int /*p1*/, const db::Instance *i2, unsigned int /*p2*/)
  {
    std::vector<db::InstElement> p;
    db::ICplxTrans t;
    add_pair (box_type::world (), *i1, p, t, *i2, p, t);
  }

  /**
   *  @brief Single-instance treatment - may be required because of interactions between array members
   */
  void finish (const db::Instance *i, unsigned int /*p1*/)
  {
    if (i->size () > 1) {
      add_single_inst (*i);
    }
  }

  /**
   *  @brief Receiver main event for local-to-instance interactions
   */
  void add (const local_cluster<T> *c1, unsigned int /*p1*/, const db::Instance *i2, unsigned int /*p2*/)
  {
    std::vector<db::InstElement> p;
    db::ICplxTrans t;
    add_pair (*c1, *i2, p, t);
  }

  bool stop () const
  {
    return false;
  }

  /**
   *  @brief Finally join the clusters in the join set
   *
   *  This step is postponed because doing this while the iteration happens would
   *  invalidate the box trees.
   */
  void join_superclusters ()
  {
    for (typename std::list<std::set<id_type> >::const_iterator sc = m_cm2join_sets.begin (); sc != m_cm2join_sets.end (); ++sc) {

      if (sc->empty ()) {
        //  dropped ones are empty
        continue;
      }

      typename std::set<id_type>::const_iterator c = sc->begin ();
      typename std::set<id_type>::const_iterator cc = c;
      for (++cc; cc != sc->end (); ++cc) {
        mp_cell_clusters->join_cluster_with (*c, *cc);
      }

    }
  }

private:
  const db::Layout *mp_layout;
  const db::Cell *mp_cell;
  db::connected_clusters<T> *mp_cell_clusters;
  hier_clusters<T> *mp_tree;
  const cell_clusters_box_converter<T> *mp_cbc;
  const db::Connectivity *mp_conn;
  typedef std::list<std::set<id_type> > join_set_list;
  std::map<id_type, typename join_set_list::iterator> m_cm2join_map;
  join_set_list m_cm2join_sets;

  /**
   *  @brief Handles the cluster interactions between two instances or instance arrays
   *  @param common The region under investigation (seen from the top level)
   *  @param i1 The index of the child cell 1
   *  @param p1 The instantiation path to the child cell (not including i1)
   *  @param t1 The accumulated transformation of the path, not including i1
   *  @param i2 The index of the child cell 2
   *  @param p2 The instantiation path to the child cell (not including i2)
   *  @param t2 The accumulated transformation of the path, not including i2
   */
  void add_pair (const box_type &common, const db::Instance &i1, const std::vector<db::InstElement> &p1, const db::ICplxTrans &t1, const db::Instance &i2, const std::vector<db::InstElement> &p2, const db::ICplxTrans &t2)
  {
    box_type bb1 = (*mp_cbc) (i1.cell_index ());
    box_type b1 = i1.cell_inst ().bbox (*mp_cbc).transformed (t1);

    box_type bb2 = (*mp_cbc) (i2.cell_index ());
    box_type b2 = i2.cell_inst ().bbox (*mp_cbc).transformed (t2);

    box_type common_all = common & b1 & b2;

    if (common_all.empty ()) {
      return;
    }

    db::ICplxTrans t1i = t1.inverted ();
    db::ICplxTrans t2i = t2.inverted ();

    for (db::CellInstArray::iterator ii1 = i1.begin_touching (common_all.transformed (t1i), mp_layout); ! ii1.at_end (); ++ii1) {

      db::ICplxTrans tt1 = t1 * i1.complex_trans (*ii1);
      box_type ib1 = bb1.transformed (tt1);

      std::vector<db::InstElement> pp1;
      pp1.reserve (p1.size () + 1);
      pp1.insert (pp1.end (), p1.begin (), p1.end ());
      pp1.push_back (db::InstElement (i1, ii1));

      for (db::CellInstArray::iterator ii2 = i2.begin_touching (ib1.transformed (t2i), mp_layout); ! ii2.at_end (); ++ii2) {

        db::ICplxTrans tt2 = t2 * i2.complex_trans (*ii2);
        box_type ib2 = bb2.transformed (tt2);

        box_type common12 = ib1 & ib2 & common;

        if (! common12.empty ()) {

          std::vector<db::InstElement> pp2;
          pp2.reserve (p2.size () + 1);
          pp2.insert (pp2.end (), p2.begin (), p2.end ());
          pp2.push_back (db::InstElement (i2, ii2));

          add_single_pair (common12, i1.cell_index (), pp1, tt1, i2.cell_index (), pp2, tt2);

          //  dive into cell of ii2
          const db::Cell &cell2 = mp_layout->cell (i2.cell_index ());
          for (db::Cell::touching_iterator jj2 = cell2.begin_touching (common12.transformed (tt2.inverted ())); ! jj2.at_end (); ++jj2) {
            add_pair (common12, i1, p1, t1, *jj2, pp2, tt2);
          }

        }

      }

      box_type common1 = ib1 & b2 & common;

      if (! common1.empty ()) {

        //  dive into cell of ii1
        const db::Cell &cell1 = mp_layout->cell (i1.cell_index ());
        for (db::Cell::touching_iterator jj1 = cell1.begin_touching (common1.transformed (tt1.inverted ())); ! jj1.at_end (); ++jj1) {
          add_pair (common1, *jj1, pp1, tt1, i2, p2, t2);
        }

      }

    }
  }

  /**
   *  @brief Handles the cluster interactions between two specific instances
   *  @param common The region under investigation (seen from the top level)
   *  @param ci1 The cell index of the child cell 1
   *  @param p1 The instantiation path to the child cell (last element is the instance to ci1)
   *  @param t1 The accumulated transformation of the path p1
   *  @param ci2 The cell index of the child cell 2
   *  @param p2 The instantiation path to the child cell (last element is the instance to ci2)
   *  @param t2 The accumulated transformation of the path p2
   */
  void add_single_pair (const box_type &common,
                        db::cell_index_type ci1, const std::vector<db::InstElement> &p1, const db::ICplxTrans &t1,
                        db::cell_index_type ci2, const std::vector<db::InstElement> &p2, const db::ICplxTrans &t2)
  {
    const db::Cell &cell2 = mp_layout->cell (ci2);

    const db::local_clusters<T> &cl1 = mp_tree->clusters_per_cell (ci1);
    const db::local_clusters<T> &cl2 = mp_tree->clusters_per_cell (ci2);

    db::ICplxTrans t1i = t1.inverted ();
    db::ICplxTrans t2i = t2.inverted ();
    db::ICplxTrans t21 = t1i * t2;

    for (typename db::local_clusters<T>::touching_iterator i = cl1.begin_touching (common.transformed (t1i)); ! i.at_end (); ++i) {

      //  skip the test, if this cluster doesn't interact with the whole cell2
      if (! i->interacts (cell2, t21, *mp_conn)) {
        continue;
      }

      box_type bc1 = common & i->bbox ().transformed (t1);
      for (typename db::local_clusters<T>::touching_iterator j = cl2.begin_touching (bc1.transformed (t2i)); ! j.at_end (); ++j) {

        if (i->interacts (*j, t21, *mp_conn)) {

          ClusterInstance k1 = make_path (i->id (), p1);
          ClusterInstance k2 = make_path (j->id (), p2);

          id_type x1 = mp_cell_clusters->find_cluster_with_connection (k1);
          id_type x2 = mp_cell_clusters->find_cluster_with_connection (k2);

          if (x1 == 0) {

            if (x2 == 0) {

              id_type connector = mp_cell_clusters->insert_dummy ();
              mp_cell_clusters->add_connection (connector, k1);
              mp_cell_clusters->add_connection (connector, k2);

            } else {
              mp_cell_clusters->add_connection (x2, k1);
            }

          } else if (x2 == 0) {

            mp_cell_clusters->add_connection (x1, k2);

          } else if (x1 != x2) {

            //  for instance-to-instance interactions the number of connections is more important for the
            //  cost of the join operation: make the one with more connections the target
            if (mp_cell_clusters->connections_for_cluster (x1).size () < mp_cell_clusters->connections_for_cluster (x2).size ()) {
              std::swap (x1, x2);
            }

            mp_cell_clusters->join_cluster_with (x1, x2);
            mp_cell_clusters->remove_cluster (x2);

          }

        }

      }

    }

  }

  /**
   *  @brief Single instance treatment
   */
  void add_single_inst (const db::Instance &i)
  {
    box_type bb = (*mp_cbc) (i.cell_index ());
    const db::Cell &cell = mp_layout->cell (i.cell_index ());

    for (db::CellInstArray::iterator ii = i.begin (); ! ii.at_end (); ++ii) {

      db::ICplxTrans tt = i.complex_trans (*ii);
      box_type ib = bb.transformed (tt);

      std::vector<db::InstElement> pp;
      pp.push_back (db::InstElement (i, ii));

      bool any = false;
      bool first = true;

      for (db::CellInstArray::iterator ii2 = i.begin_touching (ib, mp_layout); ! ii2.at_end (); ++ii2) {

        db::ICplxTrans tt2 = i.complex_trans (*ii2);
        if (tt.equal (tt2)) {
          //  skip the initial instance
          continue;
        }

        box_type ib2 = bb.transformed (tt2);

        if (ib.touches (ib2)) {

          std::vector<db::InstElement> pp2;
          pp2.push_back (db::InstElement (i, ii2));

          box_type common = (ib & ib2);
          add_single_pair (common, i.cell_index (), pp, tt, i.cell_index (), pp2, tt2);

          //  dive into cell of ii2 - this is a self-interaction of a cell with parts of itself
          //  as these self-interactions are expected to be the same always (regular array), we can skip this test the next times.
          if (first) {
            for (db::Cell::touching_iterator jj2 = cell.begin_touching (common.transformed (tt2.inverted ())); ! jj2.at_end (); ++jj2) {
              std::vector<db::InstElement> p;
              db::ICplxTrans t;
              add_pair (common, i, p, t, *jj2, pp2, tt2);
            }
          }

          any = true;

        }

      }

      first = false;

      //  we don't expect more to happen on the next instance
      if (! any) {
        break;
      }

    }
  }

  /**
   *  @brief Handles a local clusters vs. the clusters of a specific child instance or instance array
   *  @param c1 The local cluster
   *  @param i2 The index of the child cell
   *  @param p2 The instantiation path to the child cell (not including i2)
   *  @param t2 The accumulated transformation of the path, not including i2
   */
  void add_pair (const local_cluster<T> &c1, const db::Instance &i2, const std::vector<db::InstElement> &p2, const db::ICplxTrans &t2)
  {
    box_type b1 = c1.bbox ();

    box_type bb2 = (*mp_cbc) (i2.cell_index ());

    db::ICplxTrans t2i = t2 * i2.complex_trans ();
    const db::Cell &cell2 = mp_layout->cell (i2.cell_index ());

    box_type b2 = cell2.bbox ().transformed (t2i);

    if (! b1.touches (b2) || ! c1.interacts (cell2, t2i, *mp_conn)) {
      return;
    }

    std::vector<db::InstElement> pp2;
    pp2.reserve (p2.size () + 1);
    pp2.insert (pp2.end (), p2.begin (), p2.end ());
    pp2.push_back (db::InstElement ());

    for (db::CellInstArray::iterator ii2 = i2.begin_touching ((b1 & b2).transformed (t2.inverted ()), mp_layout); ! ii2.at_end (); ++ii2) {

      db::ICplxTrans tt2 = t2 * i2.complex_trans (*ii2);
      box_type ib2 = bb2.transformed (tt2);

      if (b1.touches (ib2)) {

        pp2.back () = db::InstElement (i2, ii2);
        add_single_pair (c1, i2.cell_index (), pp2, tt2);

        //  dive into cell of ii2
        for (db::Cell::touching_iterator jj2 = cell2.begin_touching ((b1 & ib2).transformed (tt2.inverted ())); ! jj2.at_end (); ++jj2) {
          add_pair (c1, *jj2, pp2, tt2);
        }

      }

    }
  }

  /**
   *  @brief Handles a local clusters vs. the clusters of a specific child instance
   *  @param c1 The local cluster
   *  @param ci2 The cell index of the cell investigated
   *  @param p2 The instantiation path to the child cell (last element is the instance to ci2)
   *  @param t2 The accumulated transformation of the path
   */
  void add_single_pair (const local_cluster<T> &c1,
                        db::cell_index_type ci2, const std::vector<db::InstElement> &p2, const db::ICplxTrans &t2)
  {
    const db::local_clusters<T> &cl2 = mp_tree->clusters_per_cell (ci2);

    for (typename db::local_clusters<T>::touching_iterator j = cl2.begin_touching (c1.bbox ().transformed (t2.inverted ())); ! j.at_end (); ++j) {

      if (c1.interacts (*j, t2, *mp_conn)) {

        ClusterInstance k2 = make_path (j->id (), p2);

        id_type other = mp_cell_clusters->find_cluster_with_connection (k2);
        if (other > 0) {

          //  we found a child cluster that connects two clusters on our own level:
          //  we must join them into one, but not now. We're still iterating and
          //  would invalidate the box trees. So keep this now and combine the clusters later.
          mark_to_join (other, c1.id ());

        } else {
          mp_cell_clusters->add_connection (c1.id (), k2);
        }

      }

    }
  }

  /**
   *  @brief Inserts a pair of clusters to join
   */
  void mark_to_join (id_type a, id_type b)
  {
    typename std::map<id_type, typename join_set_list::iterator>::const_iterator x = m_cm2join_map.find (a);
    typename std::map<id_type, typename join_set_list::iterator>::const_iterator y = m_cm2join_map.find (b);

    if (x == m_cm2join_map.end ()) {

      if (y == m_cm2join_map.end ()) {

        m_cm2join_sets.push_back (std::set<id_type> ());
        m_cm2join_sets.back ().insert (a);
        m_cm2join_sets.back ().insert (b);

        m_cm2join_map [a] = --m_cm2join_sets.end ();
        m_cm2join_map [b] = --m_cm2join_sets.end ();

      } else {

        y->second->insert (a);
        m_cm2join_map [a] = y->second;

      }

    } else if (y == m_cm2join_map.end ()) {

      x->second->insert (b);
      m_cm2join_map [b] = x->second;

    } else if (x->second != y->second) {

      //  join two superclusters
      typename join_set_list::iterator yset = y->second;
      x->second->insert (yset->begin (), yset->end ());
      for (typename std::set<id_type>::const_iterator i = yset->begin (); i != yset->end (); ++i) {
        m_cm2join_map [*i] = x->second;
      }
      m_cm2join_sets.erase (yset);

    }

#if defined(DEBUG_HIER_NETWORK_PROCESSOR)
    //  concistency check for debugging
    for (typename std::map<id_type, typename join_set_list::iterator>::const_iterator j = m_cm2join_map.begin (); j != m_cm2join_map.end (); ++j) {
      tl_assert (j->second->find (j->first) != j->second->end ());
    }

    for (typename std::list<std::set<id_type> >::const_iterator i = m_cm2join_sets.begin (); i != m_cm2join_sets.end (); ++i) {
      for (typename std::set<id_type>::const_iterator j = i->begin(); j != i->end(); ++j) {
        tl_assert(m_cm2join_map.find (*j) != m_cm2join_map.end ());
        tl_assert(m_cm2join_map[*j] == i);
      }
    }

    //  the sets must be disjunct
    std::set<id_type> all;
    for (typename std::list<std::set<id_type> >::const_iterator i = m_cm2join_sets.begin (); i != m_cm2join_sets.end (); ++i) {
      for (typename std::set<id_type>::const_iterator j = i->begin(); j != i->end(); ++j) {
        tl_assert(all.find (*j) == all.end());
        all.insert(*j);
      }
    }
#endif

  }

  /**
   *  @brief Makes a valid path to a child cluster
   *
   *  Cluster connections can only cross one level of hierarchy. This method
   *  creates necessary dummy entries for the given path.
   */
  ClusterInstance make_path (id_type id, const std::vector<db::InstElement> &path) const
  {
    std::vector<db::InstElement>::const_iterator p = path.end ();
    tl_assert (p != path.begin ());

    while (true) {

      --p;

      ClusterInstance ci (id, *p);
      if (p == path.begin ()) {

        //  if we're attaching to a child which is root yet, we need to promote the
        //  cluster to the parent in all places
        connected_clusters<T> &child_cc = mp_tree->clusters_per_cell (p->inst_ptr.cell_index ());
        if (child_cc.is_root (id)) {

          const db::Cell &child_cell = mp_layout->cell (p->inst_ptr.cell_index ());
          for (db::Cell::parent_inst_iterator pi = child_cell.begin_parent_insts (); ! pi.at_end (); ++pi) {

            connected_clusters<T> &parent_cc = mp_tree->clusters_per_cell (pi->parent_cell_index ());
            for (db::CellInstArray::iterator pii = pi->child_inst ().begin (); ! pii.at_end (); ++pii) {

              ClusterInstance ci2 (id, db::InstElement (pi->child_inst (), pii));
              if (mp_cell->cell_index () != pi->parent_cell_index () || ci != ci2) {

                id_type id_dummy = parent_cc.insert_dummy ();
                parent_cc.add_connection (id_dummy, ci2);

              }

            }

          }

          child_cc.reset_root (id);

        }

        return ci;

      }

      db::cell_index_type pci = p [-1].inst_ptr.cell_index ();
      connected_clusters<T> &target_cc = mp_tree->clusters_per_cell (pci);
      id_type parent_cluster = target_cc.find_cluster_with_connection (ci);

      if (parent_cluster > 0) {

        //  taken parent
        id = parent_cluster;

      } else {

        id_type id_new = 0;

        //  if we're attaching to a child which is root yet, we need to promote the
        //  cluster to the parent in all places
        connected_clusters<T> &child_cc = mp_tree->clusters_per_cell (p->inst_ptr.cell_index ());
        if (child_cc.is_root (id)) {

          const db::Cell &child_cell = mp_layout->cell (p->inst_ptr.cell_index ());
          for (db::Cell::parent_inst_iterator pi = child_cell.begin_parent_insts (); ! pi.at_end (); ++pi) {

            connected_clusters<T> &parent_cc = mp_tree->clusters_per_cell (pi->parent_cell_index ());
            for (db::CellInstArray::iterator pii = pi->child_inst ().begin (); ! pii.at_end (); ++pii) {

              id_type id_dummy = parent_cc.insert_dummy ();
              ClusterInstance ci2 (id, db::InstElement (pi->child_inst (), pii));
              parent_cc.add_connection (id_dummy, ci2);

              if (pci == pi->parent_cell_index () && ci == ci2) {
                id_new = id_dummy;
              }

            }

          }

          child_cc.reset_root (id);

        }

        //  no parent -> create vertical connector
        id = id_new;
        tl_assert (id != 0);

      }

    }

  }
};

template <class T>
struct cell_inst_clusters_box_converter
{
  typedef typename local_cluster<T>::box_type box_type;
  typedef db::simple_bbox_tag complexity;

  cell_inst_clusters_box_converter (const cell_clusters_box_converter<T> &cbc)
    : mp_cbc (&cbc)
  {
    //  .. nothing yet ..
  }

  box_type operator() (const db::Instance &inst) const
  {
    return inst.cell_inst ().bbox (*mp_cbc);
  }

private:
  const cell_clusters_box_converter<T> *mp_cbc;
};

}

template <class T>
void
hier_clusters<T>::do_build (cell_clusters_box_converter<T> &cbc, const db::Layout &layout, const db::Cell &cell, db::ShapeIterator::flags_type shape_flags, const db::Connectivity &conn)
{
  tl::SelfTimer timer (tl::verbosity () >= 21, tl::to_string (tr ("Computing shape clusters")));

  std::set<db::cell_index_type> called;
  cell.collect_called_cells (called);
  called.insert (cell.cell_index ());

  //  first build all local clusters

  {
    tl::SelfTimer timer (tl::verbosity () >= 31, tl::to_string (tr ("Computing local shape clusters")));
    tl::RelativeProgress progress (tl::to_string (tr ("Computing local clusters")), called.size (), 1);

    for (std::set<db::cell_index_type>::const_iterator c = called.begin (); c != called.end (); ++c) {
      build_local_cluster (layout, layout.cell (*c), shape_flags, conn);
      ++progress;
    }
  }

  //  build the hierarchical connections bottom-up and for all cells whose children are computed already

  {
    tl::SelfTimer timer (tl::verbosity () >= 31, tl::to_string (tr ("Computing hierarchical shape clusters")));
    tl::RelativeProgress progress (tl::to_string (tr ("Computing hierarchical clusters")), called.size (), 1);

    std::set<db::cell_index_type> done;
    std::vector<db::cell_index_type> todo;
    for (db::Layout::bottom_up_const_iterator c = layout.begin_bottom_up (); c != layout.end_bottom_up (); ++c) {

      if (called.find (*c) != called.end ()) {

        bool all_available = true;
        const db::Cell &cell = layout.cell (*c);
        for (db::Cell::child_cell_iterator cc = cell.begin_child_cells (); ! cc.at_end () && all_available; ++cc) {
          all_available = (done.find (*cc) != done.end ());
        }

        if (all_available) {
          todo.push_back (*c);
        } else {
          tl_assert (! todo.empty ());
          build_hier_connections_for_cells (cbc, layout, todo, conn);
          done.insert (todo.begin (), todo.end ());
          todo.clear ();
          todo.push_back (*c);
        }

        ++progress;

      }

    }

    build_hier_connections_for_cells (cbc, layout, todo, conn);
  }
}

template <class T>
void
hier_clusters<T>::build_local_cluster (const db::Layout &layout, const db::Cell &cell, db::ShapeIterator::flags_type shape_flags, const db::Connectivity &conn)
{
  std::string msg = tl::to_string (tr ("Computing local clusters for cell: ")) + std::string (layout.cell_name (cell.cell_index ()));
  if (tl::verbosity () >= 40) {
    tl::log << msg;
  }
  tl::SelfTimer timer (tl::verbosity () >= 41, msg);

  connected_clusters<T> &local = m_per_cell_clusters [cell.cell_index ()];
  local.build_clusters (cell, shape_flags, conn);
}

template <class T>
void
hier_clusters<T>::build_hier_connections_for_cells (cell_clusters_box_converter<T> &cbc, const db::Layout &layout, const std::vector<db::cell_index_type> &cells, const db::Connectivity &conn)
{
  for (std::vector<db::cell_index_type>::const_iterator c = cells.begin (); c != cells.end (); ++c) {
    build_hier_connections (cbc, layout, layout.cell (*c), conn);
  }
}

template <class T>
void
hier_clusters<T>::build_hier_connections (cell_clusters_box_converter<T> &cbc, const db::Layout &layout, const db::Cell &cell, const db::Connectivity &conn)
{
  std::string msg = tl::to_string (tr ("Computing hierarchical clusters for cell: ")) + std::string (layout.cell_name (cell.cell_index ()));
  if (tl::verbosity () >= 40) {
    tl::log << msg;
  }
  tl::SelfTimer timer (tl::verbosity () >= 41, msg);

  connected_clusters<T> &local = m_per_cell_clusters [cell.cell_index ()];

  //  NOTE: this is a receiver for both the child-to-child and
  //  local to child interactions.
  hc_receiver<T> rec (layout, cell, local, *this, cbc, conn);
  cell_inst_clusters_box_converter<T> cibc (cbc);

  //  The box scanner needs pointers so we have to first store the instances
  //  delivered by the cell's iterator (which does not deliver real pointer).

  std::vector<db::Instance> inst_storage;

  //  TODO: there should be a cell.size () for this ...
  size_t n = 0;
  for (db::Cell::const_iterator inst = cell.begin (); ! inst.at_end (); ++inst) {
    n += 1;
  }

  inst_storage.reserve (n);
  for (db::Cell::const_iterator inst = cell.begin (); ! inst.at_end (); ++inst) {
    inst_storage.push_back (*inst);
  }

  //  handle instance to instance connections

  {
    static std::string desc = tl::to_string (tr ("Instance to instance treatment"));
    tl::SelfTimer timer (tl::verbosity () >= 51, desc);

    bool report_progress = tl::verbosity () >= 50;
    db::box_scanner<db::Instance, unsigned int> bs (report_progress, desc);

    for (std::vector<db::Instance>::const_iterator inst = inst_storage.begin (); inst != inst_storage.end (); ++inst) {
      bs.insert (inst.operator-> (), 0);
    }

    bs.process (rec, 1 /*touching*/, cibc);
  }

  //  handle local to instance connections

  {
    std::list<local_cluster<db::PolygonRef> > heap;
    double area_ratio = 10.0;

    static std::string desc = tl::to_string (tr ("Local to instance treatment"));
    tl::SelfTimer timer (tl::verbosity () >= 51, desc);

    bool report_progress = tl::verbosity () >= 50;
    db::box_scanner2<db::local_cluster<T>, unsigned int, db::Instance, unsigned int> bs2 (report_progress, desc);

    for (typename connected_clusters<T>::const_iterator c = local.begin (); c != local.end (); ++c) {

      //  we do not actually need the original clusters. For a better performance we optimize the
      //  area ratio and split, but we keep the ID the same.
      std::back_insert_iterator<std::list<local_cluster<db::PolygonRef> > > iout = std::back_inserter (heap);
      size_t n = c->split (area_ratio, iout);
      if (n == 0) {
        bs2.insert1 (c.operator-> (), 0);
      } else {
        std::list<local_cluster<db::PolygonRef> >::iterator h = heap.end ();
        while (n-- > 0) {
          bs2.insert1 ((--h).operator-> (), 0);
        }
      }
    }

    for (std::vector<db::Instance>::const_iterator inst = inst_storage.begin (); inst != inst_storage.end (); ++inst) {
      bs2.insert2 (inst.operator-> (), 0);
    }

    bs2.process (rec, 1 /*touching*/, local_cluster_box_convert<T> (), cibc);
  }

  //  finally join local clusters which got connected by child clusters
  rec.join_superclusters ();
}

template <class T>
const connected_clusters<T> &
hier_clusters<T>::clusters_per_cell (db::cell_index_type cell_index) const
{
  typename std::map<db::cell_index_type, connected_clusters<T> >::const_iterator c = m_per_cell_clusters.find (cell_index);
  if (c == m_per_cell_clusters.end ()) {
    static connected_clusters<T> empty;
    return empty;
  } else {
    return c->second;
  }
}

template <class T>
connected_clusters<T> &
hier_clusters<T>::clusters_per_cell (db::cell_index_type cell_index)
{
  typename std::map<db::cell_index_type, connected_clusters<T> >::iterator c = m_per_cell_clusters.find (cell_index);
  if (c == m_per_cell_clusters.end ()) {
    c = m_per_cell_clusters.insert (std::make_pair (cell_index, connected_clusters<T> ())).first;
  }
  return c->second;
}

template <class Shape, class Trans> void insert_transformed (db::Layout &layout, db::Shapes &shapes, const Shape &s, const Trans &t);

template <class Trans> void insert_transformed (db::Layout &layout, db::Shapes &shapes, const db::PolygonRef &s, const Trans &t)
{
  db::Polygon poly = s.obj ();
  poly.transform (s.trans ());
  if (! t.is_unity ()) {
    poly.transform (t);
  }
  shapes.insert (db::PolygonRef (poly, layout.shape_repository ()));
}

template <class T>
void
hier_clusters<T>::return_to_hierarchy (db::Layout &layout, const std::map<unsigned int, unsigned int> &lm) const
{
  for (db::Layout::bottom_up_iterator c = layout.begin_bottom_up (); c != layout.end_bottom_up (); ++c) {

    const db::connected_clusters<T> &cc = clusters_per_cell (*c);
    db::Cell &target_cell = layout.cell (*c);

    for (typename db::connected_clusters<T>::all_iterator lc = cc.begin_all (); ! lc.at_end (); ++lc) {

      if (cc.is_root (*lc)) {

        for (typename std::map<unsigned int, unsigned int>::const_iterator m = lm.begin (); m != lm.end (); ++m) {

          db::Shapes &shapes = target_cell.shapes (m->second);

          for (recursive_cluster_shape_iterator<T> si (*this, m->first, *c, *lc); ! si.at_end (); ++si) {
            insert_transformed (layout, shapes, *si, si.trans ());
          }

        }

      }

    }

  }
}

//  explicit instantiations
template class DB_PUBLIC hier_clusters<db::PolygonRef>;

// ------------------------------------------------------------------------------
//  recursive_cluster_shape_iterator implementation

template <class T>
recursive_cluster_shape_iterator<T>::recursive_cluster_shape_iterator (const hier_clusters<T> &hc, unsigned int layer, db::cell_index_type ci, typename local_cluster<T>::id_type id)
  : mp_hc (&hc), m_layer (layer), m_id (id)
{
  down (ci, id, db::ICplxTrans ());

  while (m_shape_iter.at_end () && ! m_conn_iter_stack.empty ()) {
    next_conn ();
  }
}

template <class T>
std::vector<ClusterInstance> recursive_cluster_shape_iterator<T>::inst_path () const
{
  std::vector<db::ClusterInstance> p;
  if (!m_conn_iter_stack.empty ()) {
    p.reserve (m_conn_iter_stack.size ());
    for (size_t i = 0; i < m_conn_iter_stack.size () - 1; ++i) {
      p.push_back (*m_conn_iter_stack [i].first);
    }
  }
  return p;
}

template <class T>
recursive_cluster_shape_iterator<T> &recursive_cluster_shape_iterator<T>::operator++ ()
{
  ++m_shape_iter;

  while (m_shape_iter.at_end () && ! m_conn_iter_stack.empty ()) {
    next_conn ();
  }

  return *this;
}

template <class T>
void recursive_cluster_shape_iterator<T>::next_conn ()
{
  if (m_conn_iter_stack.back ().first != m_conn_iter_stack.back ().second) {

    const ClusterInstance &cli = *m_conn_iter_stack.back ().first;
    down (cli.inst ().inst_ptr.cell_index (), cli.id (), cli.inst ().complex_trans ());

  } else {

    while (m_conn_iter_stack.back ().first == m_conn_iter_stack.back ().second) {

      up ();
      if (m_conn_iter_stack.empty ()) {
        return;
      }

      ++m_conn_iter_stack.back ().first;

    }

  }
}

template <class T>
void recursive_cluster_shape_iterator<T>::up ()
{
  m_conn_iter_stack.pop_back ();
  m_trans_stack.pop_back ();
  m_cell_index_stack.pop_back ();
}

template <class T>
void recursive_cluster_shape_iterator<T>::down (db::cell_index_type ci, typename db::local_cluster<T>::id_type id, const db::ICplxTrans &t)
{
  const connected_clusters<T> &clusters = mp_hc->clusters_per_cell (ci);
  const typename connected_clusters<T>::connections_type &conn = clusters.connections_for_cluster (id);

  if (! m_trans_stack.empty ()) {
    m_trans_stack.push_back (m_trans_stack.back () * t);
  } else {
    m_trans_stack.push_back (t);
  }

  m_cell_index_stack.push_back (ci);
  m_conn_iter_stack.push_back (std::make_pair (conn.begin (), conn.end ()));

  const local_cluster<T> &cluster = mp_hc->clusters_per_cell (cell_index ()).cluster_by_id (cluster_id ());
  m_shape_iter = cluster.begin (m_layer);
}

//  explicit instantiations
template class DB_PUBLIC recursive_cluster_shape_iterator<db::PolygonRef>;

// ------------------------------------------------------------------------------
//  recursive_cluster_iterator implementation

template <class T>
recursive_cluster_iterator<T>::recursive_cluster_iterator (const hier_clusters<T> &hc, db::cell_index_type ci, typename local_cluster<T>::id_type id)
  : mp_hc (&hc), m_id (id)
{
  down (ci, id);
}

template <class T>
std::vector<ClusterInstance> recursive_cluster_iterator<T>::inst_path () const
{
  std::vector<db::ClusterInstance> p;
  if (!m_conn_iter_stack.empty ()) {
    p.reserve (m_conn_iter_stack.size ());
    for (size_t i = 0; i < m_conn_iter_stack.size () - 1; ++i) {
      p.push_back (*m_conn_iter_stack [i].first);
    }
  }
  return p;
}

template <class T>
recursive_cluster_iterator<T> &recursive_cluster_iterator<T>::operator++ ()
{
  next_conn ();
  return *this;
}

template <class T>
void recursive_cluster_iterator<T>::next_conn ()
{
  while (m_conn_iter_stack.back ().first == m_conn_iter_stack.back ().second) {

    up ();
    if (m_conn_iter_stack.empty ()) {
      return;
    }

    ++m_conn_iter_stack.back ().first;

  }

  if (m_conn_iter_stack.back ().first != m_conn_iter_stack.back ().second) {

    const ClusterInstance &cli = *m_conn_iter_stack.back ().first;
    down (cli.inst ().inst_ptr.cell_index (), cli.id ());

  }
}

template <class T>
void recursive_cluster_iterator<T>::up ()
{
  m_conn_iter_stack.pop_back ();
  m_cell_index_stack.pop_back ();
}

template <class T>
void recursive_cluster_iterator<T>::down (db::cell_index_type ci, typename db::local_cluster<T>::id_type id)
{
  const connected_clusters<T> &clusters = mp_hc->clusters_per_cell (ci);
  const typename connected_clusters<T>::connections_type &conn = clusters.connections_for_cluster (id);

  m_cell_index_stack.push_back (ci);
  m_conn_iter_stack.push_back (std::make_pair (conn.begin (), conn.end ()));
}

//  explicit instantiations
template class DB_PUBLIC recursive_cluster_iterator<db::PolygonRef>;

// ------------------------------------------------------------------------------
//  incoming_cluster_connections implementation

template <class T>
incoming_cluster_connections<T>::incoming_cluster_connections (const db::Layout &layout, const db::Cell &cell, const hier_clusters<T> &hc)
  : mp_layout (const_cast<db::Layout *> (&layout)), mp_hc (const_cast<hier_clusters<T> *> (&hc))
{
  cell.collect_called_cells (m_called_cells);
  m_called_cells.insert (cell.cell_index ());
}

template <class T>
bool
incoming_cluster_connections<T>::has_incoming (db::cell_index_type ci, size_t cluster_id) const
{
  std::map<db::cell_index_type, std::map<size_t, incoming_connections> >::const_iterator i = m_incoming.find (ci);
  if (i == m_incoming.end ()) {
    ensure_computed (ci);
    i = m_incoming.find (ci);
    tl_assert (i != m_incoming.end ());
  }

  tl_assert (i != m_incoming.end ());
  return (i->second.find (cluster_id) != i->second.end ());
}

template <class T>
const typename incoming_cluster_connections<T>::incoming_connections &
incoming_cluster_connections<T>::incoming (db::cell_index_type ci, size_t cluster_id) const
{
  std::map<db::cell_index_type, std::map<size_t, incoming_connections> >::const_iterator i = m_incoming.find (ci);
  if (i == m_incoming.end ()) {
    ensure_computed (ci);
    i = m_incoming.find (ci);
    tl_assert (i != m_incoming.end ());
  }

  std::map<size_t, incoming_connections>::const_iterator ii = i->second.find (cluster_id);
  if (ii != i->second.end ()) {
    return ii->second;
  } else {
    static incoming_connections empty;
    return empty;
  }
}

template <class T>
void
incoming_cluster_connections<T>::ensure_computed (db::cell_index_type ci) const
{
  tl_assert (mp_layout.get () != 0);
  m_incoming.insert (std::make_pair (ci, std::map<size_t, incoming_connections> ()));

  const db::Cell &cell = mp_layout->cell (ci);
  for (db::Cell::parent_cell_iterator pc = cell.begin_parent_cells (); pc != cell.end_parent_cells (); ++pc) {
    if (m_called_cells.find (*pc) != m_called_cells.end ()) {
      ensure_computed_parent (*pc);
    }
  }

  m_called_cells.erase (ci);
}

template <class T>
void
incoming_cluster_connections<T>::ensure_computed_parent (db::cell_index_type ci) const
{
  ensure_computed (ci);

  const connected_clusters<T> &cc = ((const hier_clusters<T> *) mp_hc.get ())->clusters_per_cell (ci);
  for (typename connected_clusters<T>::connections_iterator x = cc.begin_connections (); x != cc.end_connections (); ++x) {
    for (typename connected_clusters<T>::connections_type::const_iterator xx = x->second.begin (); xx != x->second.end (); ++xx) {
      m_incoming [xx->inst ().inst_ptr.cell_index ()][xx->id ()].push_back (IncomingClusterInstance (ci, x->first, xx->inst ()));
    }
  }
}

//  explicit instantiations
template class DB_PUBLIC incoming_cluster_connections<db::PolygonRef>;

}
