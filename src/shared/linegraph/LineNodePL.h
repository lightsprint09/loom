// Copyright 2016, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Authors: Patrick Brosi <brosi@informatik.uni-freiburg.de>

#ifndef SHARED_LINEGRAPH_LINENODEPL_H_
#define SHARED_LINEGRAPH_LINENODEPL_H_

#include "shared/linegraph/LineEdgePL.h"
#include "shared/linegraph/Route.h"
#include "util/geo/Geo.h"
#include "util/geo/GeoGraph.h"
#include "util/graph/Edge.h"
#include "util/graph/Node.h"

namespace shared {
namespace linegraph {

typedef util::graph::Edge<LineNodePL, LineEdgePL> LineEdge;
typedef util::graph::Node<LineNodePL, LineEdgePL> LineNode;
typedef std::map<const Route*,
                 std::map<const LineEdge*, std::set<const LineEdge*>>>
    ConnEx;

typedef std::vector<size_t> Ordering;
typedef std::map<const shared::linegraph::LineEdge*, Ordering> OrderingConfig;

class HierarchOrderingConfig
    : public std::map<const shared::linegraph::LineEdge*, std::map<size_t, Ordering>> {
 public:
  void writeFlatCfg(OrderingConfig* c) const {
    for (auto kv : *this) {
      for (auto ordering : kv.second) {
        (*c)[kv.first].insert((*c)[kv.first].begin(), ordering.second.begin(),
                              ordering.second.end());
      }
    }
  }
};

struct NodeFront {
  NodeFront(LineNode* n, LineEdge* e) : n(n), edge(e) {}

  LineNode* n;
  LineEdge* edge;

  double getOutAngle() const;
  // geometry after expansion
  PolyLine<double> geom;

  // geometry before expansion
  PolyLine<double> origGeom;

  void setInitialGeom(const PolyLine<double>& g) {
    geom = g;
    origGeom = g;
  };
  void setGeom(const PolyLine<double>& g) { geom = g; };

  // TODO
  double refEtgLengthBefExp;
};

struct Partner {
  Partner() : front(0), edge(0), route(0){};
  Partner(const NodeFront* f, const LineEdge* e,
          const shared::linegraph::Route* r)
      : front(f), edge(e), route(r){};
  const NodeFront* front;
  const LineEdge* edge;
  const shared::linegraph::Route* route;
};

struct InnerGeometry {
  InnerGeometry(PolyLine<double> g, Partner a, Partner b, size_t slotF,
                size_t slotT)
      : geom(g), from(a), to(b), slotFrom(slotF), slotTo(slotT){};
  PolyLine<double> geom;
  Partner from, to;
  size_t slotFrom, slotTo;
};

struct Station {
  Station(const std::string& id, const std::string& name,
          const util::geo::DPoint& pos)
      : id(id), name(name), pos(pos) {}
  std::string id, name;
  util::geo::DPoint pos;
};

struct ConnException {
  ConnException(const LineEdge* from, const LineEdge* to) : fr(from), to(to) {}
  const LineEdge* fr;
  const LineEdge* to;
};

class LineNodePL : util::geograph::GeoNodePL<double> {
 public:
  LineNodePL(){};
  LineNodePL(util::geo::Point<double> pos);

  const util::geo::Point<double>* getGeom() const;
  void setGeom(const util::geo::Point<double>& p);
  util::json::Dict getAttrs() const;

  void addStop(const Station& i);
  const std::vector<Station>& getStops() const;
  void clearStops();

  size_t getLineDeg() const;

  // TODO refactor
  const std::vector<NodeFront>& getMainDirs() const;
  std::vector<NodeFront>& getMainDirs();
  void delMainDir(const LineEdge* e);
  const NodeFront* getNodeFrontFor(const LineEdge* e) const;
  NodeFront* getNodeFrontFor(const LineEdge* e);

  void addMainDir(const NodeFront& f);

  void addConnExc(const Route* r, const LineEdge* edgeA, const LineEdge* edgeB);

  void delConnExc(const Route* r, const LineEdge* edgeA, const LineEdge* edgeB);

  bool connOccurs(const Route* r, const LineEdge* edgeA,
                  const LineEdge* edgeB) const;

  ConnEx& getConnExc() { return _connEx; }
  const ConnEx& getConnExc() const { return _connEx; }

 private:
  util::geo::Point<double> _pos;
  std::vector<Station> _is;

  std::vector<NodeFront> _mainDirs;

  ConnEx _connEx;
};
}
}

#endif  // SHARED_LINEGRAPH_LINENODEPL_H_
