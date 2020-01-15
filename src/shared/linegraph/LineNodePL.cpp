// Copyright 2016, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Authors: Patrick Brosi <brosi@informatik.uni-freiburg.de>

#include "shared/linegraph/LineGraph.h"
#include "shared/linegraph/LineNodePL.h"
#include "shared/linegraph/NodeFront.h"

using util::geo::Point;
using util::geo::DPoint;
using namespace shared::linegraph;

// _____________________________________________________________________________
LineNodePL::LineNodePL(Point<double> pos) : _pos(pos) {}

// _____________________________________________________________________________
const Point<double>* LineNodePL::getGeom() const { return &_pos; }

// _____________________________________________________________________________
void LineNodePL::setGeom(const Point<double>& p) { _pos = p; }

// _____________________________________________________________________________
util::json::Dict LineNodePL::getAttrs() const {
  util::json::Dict obj;
  if (_is.size() > 0) {
    obj["station_id"] = _is.begin()->id;
    obj["station_label"] = _is.begin()->name;
  }

  auto arr = util::json::Array();

  for (const auto& ro : _connEx) {
    for (const auto& exFr : ro.second) {
      for (const auto* exTo : exFr.second) {
        util::json::Dict ex;
        ex["route"] = util::toString(ro.first->getId());
        if (exFr.first == exTo) continue;
        auto shrd = LineGraph::sharedNode(exFr.first, exTo);
        auto nd1 = exFr.first->getOtherNd(shrd);
        auto nd2 = exTo->getOtherNd(shrd);
        ex["edge1_node"] = util::toString(nd1);
        ex["edge2_node"] = util::toString(nd2);
        arr.push_back(ex);
      }
    }
  }

  if (arr.size()) obj["excluded_line_conns"] = arr;
  return obj;
}

// _____________________________________________________________________________
void LineNodePL::addStop(const Station& i) { _is.push_back(i); }

// _____________________________________________________________________________
const std::vector<Station>& LineNodePL::getStops() const { return _is; }

// _____________________________________________________________________________
void LineNodePL::clearStops() { _is.clear(); }

// _____________________________________________________________________________
void LineNodePL::addConnExc(const Route* r, const LineEdge* edgeA,
                            const LineEdge* edgeB) {
  _connEx[r][edgeA].insert(edgeB);
  // index the other direction also, will lead to faster lookups later on
  _connEx[r][edgeB].insert(edgeA);
}

// _____________________________________________________________________________
void LineNodePL::delConnExc(const Route* r, const LineEdge* edgeA,
                            const LineEdge* edgeB) {
  _connEx[r][edgeA].erase(edgeB);
  // index the other direction also, will lead to faster lookups later on
  _connEx[r][edgeB].erase(edgeA);
}

// _____________________________________________________________________________
bool LineNodePL::connOccurs(const Route* r, const LineEdge* edgeA,
                            const LineEdge* edgeB) const {
  const auto& i = _connEx.find(r);
  if (_connEx.find(r) == _connEx.end()) return true;

  const auto& ii = i->second.find(edgeA);
  if (ii == i->second.end()) return true;

  return ii->second.find(edgeB) == ii->second.end();
}

// _____________________________________________________________________________
const NodeFront* LineNodePL::getNodeFrontFor(const LineEdge* e) const {
  for (auto& nf : getMainDirs()) {
    if (nf.edge == e) return &nf;
  }

  return 0;
}

// _____________________________________________________________________________
NodeFront* LineNodePL::getNodeFrontFor(const LineEdge* e) {
  for (auto& nf : getMainDirs()) {
    if (nf.edge == e) return &nf;
  }

  return 0;
}

// _____________________________________________________________________________
const std::vector<NodeFront>& LineNodePL::getMainDirs() const {
  return _mainDirs;
}

// _____________________________________________________________________________
void LineNodePL::delMainDir(const LineEdge* e) {
  for (size_t i = 0; i < getMainDirs().size(); i++) {
    if (_mainDirs[i].edge == e) {
      _mainDirs[i] = _mainDirs.back();
      _mainDirs.pop_back();
      return;
    }
  }
}

// _____________________________________________________________________________
std::vector<NodeFront>& LineNodePL::getMainDirs() { return _mainDirs; }

// _____________________________________________________________________________
double NodeFront::getOutAngle() const {
  double checkDist = 10;
  if (edge->getFrom() == n) {
    return angBetween(
        *n->pl().getGeom(),
        PolyLine<double>(*edge->pl().getGeom()).getPointAtDist(checkDist).p);
  } else {
    return angBetween(
        *n->pl().getGeom(),
        PolyLine<double>(*edge->pl().getGeom())
            .getPointAtDist(util::geo::len(*edge->pl().getGeom()) - checkDist)
            .p);
  }
}

// _____________________________________________________________________________
void LineNodePL::addMainDir(const NodeFront& f) { _mainDirs.push_back(f); }

