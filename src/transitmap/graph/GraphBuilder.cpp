// Copyright 2016, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Authors: Patrick Brosi <brosi@informatik.uni-freiburg.de>

#include <istream>
#include <set>
#include <stack>
#include <vector>
#include "GraphBuilder.h"
#include "shared/rendergraph/RenderGraph.h"
#include "shared/linegraph/Line.h"
#include "transitmap/config/TransitMapConfig.h"
#include "util/geo/PolyLine.h"
#include "util/log/Log.h"

using shared::rendergraph::RenderGraph;
using shared::rendergraph::OrderCfg;
using shared::rendergraph::Ordering;
using shared::linegraph::Line;
using shared::linegraph::LineEdge;
using shared::linegraph::LineNode;
using shared::linegraph::NodeFront;
using transitmapper::graph::GraphBuilder;
using util::geo::DPoint;
using util::geo::LinePoint;
using util::geo::LinePointCmp;
using util::geo::PolyLine;

// _____________________________________________________________________________
GraphBuilder::GraphBuilder(const config::Config* cfg) : _cfg(cfg) {}

// _____________________________________________________________________________
void GraphBuilder::writeNodeFronts(RenderGraph* graph) {
  for (auto n : *graph->getNds()) {
    std::set<LineEdge*> eSet;
    eSet.insert(n->getAdjList().begin(), n->getAdjList().end());

    for (LineEdge* e : eSet) {
      NodeFront f(n, e);
      PolyLine<double> pl;

      f.refEtgLengthBefExp = util::geo::len(*e->pl().getGeom());

      if (e->getTo() == n) {
        pl = PolyLine<double>(*e->pl().getGeom())
                 .getOrthoLineAtDist(util::geo::len(*e->pl().getGeom()),
                                     graph->getTotalWidth(e));
      } else {
        pl = PolyLine<double>(*e->pl().getGeom())
                 .getOrthoLineAtDist(0, graph->getTotalWidth(e));
        pl.reverse();
      }

      f.setInitialGeom(pl);

      n->pl().addFront(f);
    }
  }
}

// _____________________________________________________________________________
void GraphBuilder::expandOverlappinFronts(RenderGraph* g) {
  // now, look at the nodes entire front geometries and expand them
  // until nothing overlaps
  double step = 4;

  while (true) {
    bool stillFree = false;
    for (auto n : *g->getNds()) {
      std::set<NodeFront*> overlaps = nodeGetOverlappingFronts(g, n);
      for (auto f : overlaps) {
        stillFree = true;
        if (f->edge->getTo() == n) {
          f->geom = PolyLine<double>(*f->edge->pl().getGeom())
                        .getOrthoLineAtDist(
                            util::geo::len(*f->edge->pl().getGeom()) - step,
                            g->getTotalWidth(f->edge));
        } else {
          f->geom = PolyLine<double>(*f->edge->pl().getGeom())
                        .getOrthoLineAtDist(step, g->getTotalWidth(f->edge));
          f->geom.reverse();
        }

        // cut the edges to fit the new front
        freeNodeFront(n, f);
      }
    }
    if (!stillFree) break;
  }
}

// _____________________________________________________________________________
std::set<NodeFront*> GraphBuilder::nodeGetOverlappingFronts(
    const RenderGraph* g, const LineNode* n) const {
  std::set<NodeFront*> ret;
  double minLength = 10;

  // TODO: why are nodefronts accessed via index?
  for (size_t i = 0; i < n->pl().fronts().size(); ++i) {
    const NodeFront& fa = n->pl().fronts()[i];

    for (size_t j = i + 1; j < n->pl().fronts().size(); ++j) {
      const NodeFront& fb = n->pl().fronts()[j];

      if (fa.geom.equals(fb.geom, 5) || j == i) continue;

      bool overlap = false;

      double maxNfDist = 2 * g->getMaxNdFrontWidth(n);

      if (n->pl().stops().size() && !g->notCompletelyServed(n)) {
        maxNfDist = .5 * g->getMaxNdFrontWidth(n);
        double fac = 0;
        if (_cfg->tightStations) maxNfDist = _cfg->lineWidth + _cfg->lineSpacing;
        overlap = nodeFrontsOverlap(g, fa, fb, (g->getWidth(fa.edge) + g->getSpacing(fa.edge)) * fac);
      } else {
        size_t numShr = g->getSharedLines(fa.edge, fb.edge).size();
        double fac = 5;
        if (!numShr) fac = 1;

        overlap = nodeFrontsOverlap(g, fa, fb, (g->getWidth(fa.edge) + g->getSpacing(fa.edge)) * fac);
      }

      if (overlap) {
        if (util::geo::len(*fa.edge->pl().getGeom()) > minLength &&
            fa.geom.distTo(*n->pl().getGeom()) < maxNfDist) {
          ret.insert(const_cast<NodeFront*>(&fa));
        }
        if (util::geo::len(*fb.edge->pl().getGeom()) > minLength &&
            fb.geom.distTo(*n->pl().getGeom()) < maxNfDist) {
          ret.insert(const_cast<NodeFront*>(&fb));
        }
      }
    }
  }

  return ret;
}

// _____________________________________________________________________________
bool GraphBuilder::nodeFrontsOverlap(const RenderGraph* g, const NodeFront& a,
                                     const NodeFront& b, double d) const {
  UNUSED(g);
  return b.geom.distTo(a.geom) <= d;
}

// _____________________________________________________________________________
void GraphBuilder::freeNodeFront(const LineNode* n, NodeFront* f) {
  PolyLine<double> cutLine = f->geom;

  std::set<LinePoint<double>, LinePointCmp<double>> iSects =
      cutLine.getIntersections(*f->edge->pl().getGeom());
  if (iSects.size() > 0) {
    if (f->edge->getTo() != n) {
      // cut at beginning
      f->edge->pl().setGeom(PolyLine<double>(*f->edge->pl().getGeom())
                                .getSegment(iSects.begin()->totalPos, 1)
                                .getLine());
    } else {
      // cut at end
      f->edge->pl().setGeom(PolyLine<double>(*f->edge->pl().getGeom())
                                .getSegment(0, (--iSects.end())->totalPos)
                                .getLine());
    }
  }
}
