// Copyright 2016, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Authors: Patrick Brosi <brosi@informatik.uni-freiburg.de>

#include <glpk.h>
#include <fstream>
#include "octi/basegraph/BaseGraph.h"
#include "octi/ilp/ILPGridOptimizer.h"
#include "shared/optim/ILPSolvProv.h"
#include "util/geo/output/GeoGraphJsonOutput.h"
#include "util/log/Log.h"

using octi::basegraph::BaseGraph;
using octi::basegraph::GeoPensMap;
using octi::basegraph::GridEdge;
using octi::basegraph::GridNode;
using octi::combgraph::Drawing;
using octi::ilp::ILPGridOptimizer;
using octi::ilp::ILPStats;
using shared::optim::ILPSolver;
using shared::optim::StarterSol;

// _____________________________________________________________________________
ILPStats ILPGridOptimizer::optimize(BaseGraph* gg, const CombGraph& cg,
                                    combgraph::Drawing* d, double maxGrDist,
                                    bool noSolve, const GeoPensMap* geoPensMap,
                                    int timeLim, const std::string& cacheDir,
                                    double cacheThreshold,
                                    int numThreads,
                                    const std::string& solverStr,
                                    const std::string& path) const {
  // extract first feasible solution from gridgraph
  ILPStats s{std::numeric_limits<double>::infinity(), 0, 0, 0, 0};
  StarterSol sol = extractFeasibleSol(d, gg, cg, maxGrDist);
  gg->reset();

  for (auto nd : *gg->getNds()) {
    // if we presolve, some edges may be blocked
    for (auto e : nd->getAdjList()) {
      e->pl().open();
      e->pl().unblock();
    }
    if (!nd->pl().isSink()) continue;
    gg->openTurns(nd);
    gg->closeSinkFr(nd);
    gg->closeSinkTo(nd);
  }

  // clear drawing
  d->crumble();

  auto lp = createProblem(gg, cg, geoPensMap, maxGrDist, solverStr);

  s.cols = lp->getNumVars();
  s.rows = lp->getNumConstrs();

  lp->setStarter(sol);

  if (path.size()) {
    std::string basename = path;
    size_t pos = basename.find_last_of(".");
    if (pos != std::string::npos) basename = basename.substr(0, pos);

    std::string outf = basename + ".sol";
    std::string solutionF = basename + ".mst";
    lp->writeMst(solutionF, sol);
    lp->writeMps(path);
  }

  double time;

  if (!noSolve) {
    if (timeLim >= 0) lp->setTimeLim(timeLim);
    if (cacheDir.size()) lp->setCacheDir(cacheDir);
    lp->setCacheThreshold(cacheThreshold);
    if (numThreads != 0) lp->setNumThreads(numThreads);
    T_START(ilp);
    auto status = lp->solve();
    time = T_STOP(ilp);

    if (status == shared::optim::SolveType::INF) {
      delete lp;
      throw std::runtime_error(
          "No solution found for ILP problem (most likely because of a time "
          "limit)!");
    }

    extractSolution(lp, gg, cg, d);
    shared::linegraph::LineGraph tg;
    d->getLineGraph(&tg);

    s.score = lp->getObjVal();
    s.time = time;
    s.optimal = (status == shared::optim::SolveType::OPTIM);
  }

  delete lp;

  return s;
}

// _____________________________________________________________________________
ILPSolver* ILPGridOptimizer::createProblem(BaseGraph* gg, const CombGraph& cg,
                                           const GeoPensMap* geoPensMap,
                                           double maxGrDist,
                                           const std::string& solverStr) const {
  ILPSolver* lp = shared::optim::getSolver(solverStr, shared::optim::MIN);

  // grid nodes that may potentially be a position for an
  // input station
  std::map<const CombNode*, std::set<const GridNode*>> cands;

  for (auto nd : cg.getNds()) {
    if (nd->getDeg() == 0) continue;
    std::stringstream oneAssignment;
    // must sum up to 1
    oneAssignment << "oneass(" << nd << ")";
    int rowStat = lp->addRow(oneAssignment.str(), 1, shared::optim::FIX);

    size_t i = 0;

    for (const GridNode* n : *gg->getNds()) {
      if (!n->pl().isSink()) continue;

      // don't use nodes as candidates which cannot hold the comb node due to
      // their degree
      if (n->getDeg() < nd->getDeg()) {
        continue;
      }

      double gridD = dist(*n->pl().getGeom(), *nd->pl().getGeom());

      // threshold for speedup
      double maxDis = gg->getCellSize() * maxGrDist;
      if (gridD >= maxDis) {
        continue;
      }

      cands[nd].insert(n);

      gg->openSinkFr(const_cast<GridNode*>(n), 0);
      gg->openSinkTo(const_cast<GridNode*>(n), 0);

      auto varName = getStatPosVar(n, nd);

      int col = lp->addCol(varName, shared::optim::BIN, gg->ndMovePen(nd, n));

      lp->addColToRow(rowStat, col, 1);

      i++;
    }
  }

  // for every edge, we define a binary variable telling us whether this edge
  // is used in a path for the original edge
  for (auto nd : cg.getNds()) {
    for (auto edg : nd->getAdjList()) {
      if (edg->getFrom() != nd) continue;
      for (const GridNode* n : *gg->getNds()) {
        for (const GridEdge* e : n->getAdjList()) {
          if (e->getFrom() != n) continue;
          if (e->pl().cost() >= basegraph::SOFT_INF) {
            // skip infinite edges, we cannot use them.
            // this also skips sink edges of nodes not used as
            // candidates
            continue;
          }

          if (e->getFrom()->pl().isSink() &&
              !cands[edg->getFrom()].count(e->getFrom())) {
            continue;
          }

          if (e->getTo()->pl().isSink() &&
              !cands[edg->getTo()].count(e->getTo())) {
            continue;
          }

          auto edgeVarName = getEdgUseVar(e, edg);

          double coef;
          if (geoPensMap && !e->pl().isSecondary()) {
            // add geo pen
            coef = e->pl().cost() +
                   (*geoPensMap).find(edg)->second[e->pl().getId()];
          } else {
            coef = e->pl().cost();
          }
          lp->addCol(edgeVarName, shared::optim::BIN, coef);
        }
      }
    }
  }

  lp->update();

  // an edge can only be used a single time
  std::set<const GridEdge*> proced;
  for (const GridNode* n : *gg->getNds()) {
    for (const GridEdge* e : n->getAdjList()) {
      if (e->pl().isSecondary()) continue;
      if (proced.count(e)) continue;
      auto f = gg->getEdg(e->getTo(), e->getFrom());
      proced.insert(e);
      proced.insert(f);

      std::stringstream constName;
      constName << "ue(" << e->getFrom()->pl().getId() << ","
                << e->getTo()->pl().getId() << ")";
      int row = lp->addRow(constName.str(), 1, shared::optim::UP);

      for (auto nd : cg.getNds()) {
        for (auto edg : nd->getAdjList()) {
          if (edg->getFrom() != nd) continue;
          if (e->pl().cost() >= basegraph::SOFT_INF) continue;

          auto eVarName = getEdgUseVar(e, edg);
          auto fVarName = getEdgUseVar(f, edg);

          int eCol = lp->getVarByName(eVarName);
          if (eCol > -1) lp->addColToRow(row, eCol, 1);
          int fCol = lp->getVarByName(fVarName);
          if (fCol > -1) lp->addColToRow(row, fCol, 1);
        }
      }
    }
  }

  // for every node, the number of outgoing and incoming used edges must be
  // the same, except for the start and end node
  for (const GridNode* n : *gg->getNds()) {
    if (nonInfDeg(n) == 0) continue;

    for (auto nd : cg.getNds()) {
      for (auto edg : nd->getAdjList()) {
        if (edg->getFrom() != nd) continue;
        std::stringstream constName;
        constName << "as(" << n->pl().getId() << "," << edg << ")";

        // an upper bound is enough here
        int row = lp->addRow(constName.str(), 0, shared::optim::UP);

        // normally, we count an incoming edge as 1 and an outgoing edge as -1
        // later on, we make sure that each node has a some of all out and in
        // edges of 0
        int inCost = -1;
        int outCost = 1;

        // for sink nodes, we apply a trick: an outgoing edge counts as 2 here.
        // this means that a sink node cannot make up for an outgoing edge
        // with an incoming edge - it would need 2 incoming edges to achieve
        // that.
        // however, this would mean (as sink nodes are never adjacent) that 2
        // ports
        // have outgoing edges - which would mean the path "split" somewhere
        // before
        // the ports, which is impossible and forbidden by our other
        // constraints.
        // the only way a sink node can make up for in outgoin edge
        // is thus if we add -2 if the sink is marked as the start station of
        // this edge
        if (n->pl().isSink()) {
          // subtract the variable for this start node and edge, if used
          // as a candidate
          int ndColFrom = lp->getVarByName(getStatPosVar(n, edg->getFrom()));
          if (ndColFrom > -1) lp->addColToRow(row, ndColFrom, -2);

          // add the variable for this end node and edge, if used
          // as a candidate
          int ndColTo = lp->getVarByName(getStatPosVar(n, edg->getTo()));
          if (ndColTo > -1) lp->addColToRow(row, ndColTo, 1);

          outCost = 2;
        }

        for (auto e : n->getAdjListIn()) {
          int edgCol = lp->getVarByName(getEdgUseVar(e, edg));
          if (edgCol < 0) continue;
          lp->addColToRow(row, edgCol, inCost);
        }

        for (auto e : n->getAdjListOut()) {
          int edgCol = lp->getVarByName(getEdgUseVar(e, edg));
          if (edgCol < 0) continue;
          lp->addColToRow(row, edgCol, outCost);
        }
      }
    }
  }

  lp->update();

  // only a single sink edge can be activated per input edge and settled grid
  // node
  // THIS RULE IS REDUNDANT AND IMPLICITELY ENFORCED BY OTHER RULES,
  // BUT SEEMS TO LEAD TO FASTER SOLUTION TIMES
  for (GridNode* n : *gg->getNds()) {
    if (!n->pl().isSink()) continue;

    for (auto nd : cg.getNds()) {
      for (auto e : nd->getAdjList()) {
        if (e->getFrom() != nd) continue;

        std::stringstream constName;
        constName << "ss(" << n->pl().getId() << "," << e << ")";

        int row = lp->addRow(constName.str(), 0, shared::optim::FIX);

        if (!cands[e->getFrom()].count(n) && !cands[e->getTo()].count(n)) {
          // node does not appear as start or end cand, so the number of
          // sink edges for this node is 0

        } else {
          if (cands[e->getTo()].count(n)) {
            int ndColTo = lp->getVarByName(getStatPosVar(n, e->getTo()));
            if (ndColTo > -1) lp->addColToRow(row, ndColTo, -1);
          }

          if (cands[e->getFrom()].count(n)) {
            int ndColFr = lp->getVarByName(getStatPosVar(n, e->getFrom()));
            if (ndColFr > -1) lp->addColToRow(row, ndColFr, -1);
          }
        };

        for (size_t p = 0; p < gg->maxDeg(); p++) {
          auto portNd = n->pl().getPort(p);
          if (!portNd) continue;
          auto varSinkTo = getEdgUseVar(gg->getEdg(portNd, n), e);
          auto varSinkFr = getEdgUseVar(gg->getEdg(n, portNd), e);

          int ndColTo = lp->getVarByName(varSinkTo);
          if (ndColTo > -1) lp->addColToRow(row, ndColTo, 1);

          int ndColFr = lp->getVarByName(varSinkFr);
          if (ndColFr > -1) lp->addColToRow(row, ndColFr, 1);
        }
      }
    }
  }

  // a grid node can either be an activated sink, or a single pass through
  // edge is used
  for (GridNode* n : *gg->getNds()) {
    if (!n->pl().isSink()) continue;

    std::stringstream constName;
    constName << "iu(" << n->pl().getId() << ")";

    int row = lp->addRow(constName.str(), 1, shared::optim::UP);

    // a meta grid node can either be a sink for a single input node, or
    // a pass-through

    for (auto nd : cg.getNds()) {
      int ndcolto = lp->getVarByName(getStatPosVar(n, nd).c_str());
      if (ndcolto > -1) lp->addColToRow(row, ndcolto, 1);
    }

    // go over all ports
    for (size_t pf = 0; pf < gg->maxDeg(); pf++) {
      auto from = n->pl().getPort(pf);
      if (!from) continue;
      for (size_t pt = 0; pt < gg->maxDeg(); pt++) {
        auto to = n->pl().getPort(pt);
        if (!to || from == to) continue;

        auto innerE = gg->getEdg(from, to);
        for (auto nd : cg.getNds()) {
          for (auto edg : nd->getAdjList()) {
            if (edg->getFrom() != nd) continue;

            int edgCol = lp->getVarByName(getEdgUseVar(innerE, edg));
            if (edgCol < 0) continue;
            lp->addColToRow(row, edgCol, 1);
          }
        }
      }
    }
  }

  lp->update();

  // dont allow crossing edges
  size_t rowId = 0;
  for (auto edgPair : gg->getCrossEdgPairs()) {
    std::stringstream constName;
    constName << "nc(" << rowId << ")";
    rowId++;

    int row = lp->addRow(constName.str(), 1, shared::optim::UP);

    for (auto nd : cg.getNds()) {
      for (auto edg : nd->getAdjList()) {
        if (edg->getFrom() != nd) continue;

        int col = lp->getVarByName(getEdgUseVar(edgPair.first.first, edg));
        if (col > -1) lp->addColToRow(row, col, 1);

        col = lp->getVarByName(getEdgUseVar(edgPair.first.second, edg));
        if (col > -1) lp->addColToRow(row, col, 1);

        col = lp->getVarByName(getEdgUseVar(edgPair.second.first, edg));
        if (col > -1) lp->addColToRow(row, col, 1);

        col = lp->getVarByName(getEdgUseVar(edgPair.second.second, edg));
        if (col > -1) lp->addColToRow(row, col, 1);
      }
    }
  }

  lp->update();

  // for each input node N, define a var x_dirNE which tells the direction of
  // E at N
  for (auto nd : cg.getNds()) {
    if (nd->getDeg() < 2) continue;  // we don't need this for deg 1 nodes
    for (auto edg : nd->getAdjList()) {
      std::stringstream dirName;
      dirName << "d(" << nd << "," << edg << ")";
      int col =
          lp->addCol(dirName.str(), shared::optim::INT, 0, 0, gg->maxDeg() - 1);

      std::stringstream constName;
      constName << "dc(" << nd << "," << edg << ")";

      int row = lp->addRow(constName.str(), 0, shared::optim::FIX);

      lp->addColToRow(row, col, -1);

      for (GridNode* n : *gg->getNds()) {
        if (!n->pl().isSink()) continue;

        // check if this grid node is used as a candidate for comb node
        // if not, we don't have to add the constraints
        int ndColFrom = lp->getVarByName(getStatPosVar(n, nd));
        if (ndColFrom == -1) continue;

        if (edg->getFrom() == nd) {
          // the 0 can be skipped here
          for (size_t i = 1; i < gg->maxDeg(); i++) {
            auto portNd = n->pl().getPort(i);
            if (!portNd) continue;
            auto e = gg->getEdg(n, portNd);
            int col = lp->getVarByName(getEdgUseVar(e, edg));
            if (col > -1) lp->addColToRow(row, col, i);
          }
        } else {
          // the 0 can be skipped here
          for (size_t i = 1; i < gg->maxDeg(); i++) {
            auto portNd = n->pl().getPort(i);
            if (!portNd) continue;
            auto e = gg->getEdg(portNd, n);
            int col = lp->getVarByName(getEdgUseVar(e, edg));
            if (col > -1) lp->addColToRow(row, col, i);
          }
        }
      }
    }
  }

  lp->update();

  // for each input node N, make sure that the circular ordering of the final
  // drawing matches the input ordering
  int M = gg->maxDeg();
  for (auto nd : cg.getNds()) {
    // for degree < 3, the circular ordering cannot be violated
    if (nd->getDeg() < 3) continue;

    std::stringstream vulnConstName;
    vulnConstName << "vc(" << nd << ")";
    // an upper bound would also work here, at most one
    // of the vuln vars may be 1

    int vulnRow = lp->addRow(vulnConstName.str(), 1, shared::optim::FIX);

    for (size_t i = 0; i < nd->getDeg(); i++) {
      std::stringstream n;
      n << "vuln(" << nd << "," << i << ")";
      int col = lp->addCol(n.str(), shared::optim::BIN, 0);
      lp->addColToRow(vulnRow, col, 1);
    }

    lp->update();

    auto order = nd->pl().getEdgeOrdering().getOrderedSet();
    assert(order.size() > 2);
    for (size_t i = 0; i < order.size(); i++) {
      CombEdge* edgA;
      if (i == 0) {
        edgA = nd->pl().getEdgeOrdering().getOrderedSet().back().first;
      } else {
        edgA = nd->pl().getEdgeOrdering().getOrderedSet()[i - 1].first;
      }
      auto edgB = nd->pl().getEdgeOrdering().getOrderedSet()[i].first;

      assert(edgA != edgB);

      std::stringstream colNameA;
      colNameA << "d(" << nd << "," << edgA << ")";
      int colA = lp->getVarByName(colNameA.str());
      assert(colA > -1);

      std::stringstream colNameB;
      colNameB << "d(" << nd << "," << edgB << ")";
      int colB = lp->getVarByName(colNameB.str());
      assert(colB > -1);

      std::stringstream constName;
      constName << "oc(" << nd << "," << i << ")";
      int row = lp->addRow(constName.str(), 1, shared::optim::LO);

      std::stringstream vulnColName;
      vulnColName << "vuln(" << nd << "," << i << ")";
      int vulnCol = lp->getVarByName(vulnColName.str());
      assert(vulnCol > -1);

      lp->addColToRow(row, colB, 1);
      lp->addColToRow(row, colA, -1);
      lp->addColToRow(row, vulnCol, M);
    }
  }

  lp->update();

  std::vector<double> pens = gg->getCosts();

  // for each adjacent edge pair, add variables telling the accuteness of the
  // angle between them
  for (auto nd : cg.getNds()) {
    for (size_t i = 0; i < nd->getAdjList().size(); i++) {
      auto edgA = nd->getAdjList()[i];
      for (size_t j = i + 1; j < nd->getAdjList().size(); j++) {
        auto edgB = nd->getAdjList()[j];
        assert(edgA != edgB);

        // note: we can identify pairs of edges by the edges only as we dont
        // have a multigraph - we dont need the need for uniqueness

        size_t sharedLines = 0;
        // TODO: not all lines in getChilds are equal, take the "right" end of
        // the childs here!
        for (auto lo : edgA->pl().getChilds().front()->pl().getLines()) {
          if (edgB->pl().getChilds().front()->pl().hasLine(lo.line)) {
            sharedLines++;
          }
        }

        if (!sharedLines) continue;

        std::stringstream negVar;
        negVar << "negdist(" << edgA << "," << edgB << ")";

        int colNeg = lp->addCol(negVar.str(), shared::optim::BIN, 0);

        std::stringstream constName;
        constName << "nc(" << edgA << "," << edgB << ")";

        int row1 = lp->addRow(constName.str() + "lo", 0, shared::optim::LO);
        int row2 = lp->addRow(constName.str() + "up", gg->maxDeg() - 1,
                              shared::optim::UP);

        std::stringstream dirNameA;
        dirNameA << "d(" << nd << "," << edgA << ")";
        std::stringstream dirNameB;
        dirNameB << "d(" << nd << "," << edgB << ")";

        int colA = lp->getVarByName(dirNameA.str());
        assert(colA > -1);
        lp->addColToRow(row1, colA, 1);
        lp->addColToRow(row2, colA, 1);

        int colB = lp->getVarByName(dirNameB.str());
        assert(colB > -1);
        lp->addColToRow(row1, colB, -1);
        lp->addColToRow(row2, colB, -1);

        lp->addColToRow(row1, colNeg, gg->maxDeg());
        lp->addColToRow(row2, colNeg, gg->maxDeg());

        std::stringstream angConst;
        angConst << "ac(" << edgA << "," << edgB << ")";
        int rowAng = lp->addRow(angConst.str(), 0, shared::optim::FIX);

        lp->addColToRow(rowAng, colA, 1);
        lp->addColToRow(rowAng, colB, -1);
        lp->addColToRow(rowAng, colNeg, gg->maxDeg());

        std::stringstream sumConst;
        sumConst << "asc(" << edgA << "," << edgB << ")";

        int rowSum = lp->addRow(sumConst.str(), 1, shared::optim::UP);

        int N = gg->maxDeg() - 1;
        int M = pens.size();

        for (int k = 0; k < N; k++) {
          std::stringstream var;
          size_t pp = pens.size() - 1 - k;
          if (k >= M) {
            pp = k + 1 - pens.size();
            var << "d" << pp << "'(" << edgA << "," << edgB << ")";
          } else {
            var << "d" << pp << "(" << edgA << "," << edgB << ")";
          }

          // TODO: maybe multiply per shared lines - but this actually
          // makes the drawings look worse.
          int col = lp->addCol(var.str(), shared::optim::BIN, pens[pp]);

          lp->addColToRow(rowAng, col, -(k + 1));
          lp->addColToRow(rowSum, col, 1);
        }
      }
    }
  }

  lp->update();

  return lp;
}

// _____________________________________________________________________________
std::string ILPGridOptimizer::getEdgUseVar(const GridEdge* e,
                                           const CombEdge* cg) const {
  std::stringstream varName;
  varName << "edg(" << e->getFrom()->pl().getId() << ","
          << e->getTo()->pl().getId() << "," << cg << ")";

  return varName.str();
}

// _____________________________________________________________________________
std::string ILPGridOptimizer::getStatPosVar(const GridNode* n,
                                            const CombNode* nd) const {
  std::stringstream varName;
  varName << "sp(" << n->pl().getId() << "," << nd << ")";

  return varName.str();
}

// _____________________________________________________________________________
void ILPGridOptimizer::extractSolution(ILPSolver* lp, BaseGraph* gg,
                                       const CombGraph& cg,
                                       combgraph::Drawing* d) const {
  std::map<const CombNode*, const GridNode*> gridNds;
  std::map<const CombEdge*, std::set<const GridEdge*>> gridEdgs;

  // write solution to grid graph
  for (GridNode* n : *gg->getNds()) {
    for (GridEdge* e : n->getAdjList()) {
      if (e->getFrom() != n) continue;

      for (auto nd : cg.getNds()) {
        for (auto edg : nd->getAdjList()) {
          if (edg->getFrom() != nd) continue;
          auto varName = getEdgUseVar(e, edg);

          int i = lp->getVarByName(varName);
          if (i > -1) {
            double val = lp->getVarVal(i);
            if (val > 0.5) {
              gg->addResEdg(e, edg);
              gridEdgs[edg].insert(e);
            }
          }
        }
      }
    }
  }

  for (GridNode* n : *gg->getNds()) {
    if (!n->pl().isSink()) continue;
    for (auto nd : cg.getNds()) {
      auto varName = getStatPosVar(n, nd);

      int i = lp->getVarByName(varName);
      if (i > -1) {
        double val = lp->getVarVal(i);
        if (val > 0.5) {
          n->pl().setStation();
          gridNds[nd] = n;
        }
      }
    }
  }

  // draw solution
  for (auto nd : cg.getNds()) {
    for (auto edg : nd->getAdjList()) {
      if (edg->getFrom() != nd) continue;

      std::vector<GridEdge*> edges(gridEdgs[edg].size());

      assert(gridNds.count(edg->getFrom()));
      assert(gridNds.count(edg->getTo()));

      // get the start and end grid nodes
      auto grStart = gridNds[edg->getFrom()];
      auto grEnd = gridNds[edg->getTo()];


      assert(grStart);
      assert(grEnd);

      auto curNode = grStart;
      GridEdge* last = 0;

      size_t i = 0;

      while (curNode != grEnd) {
        for (auto adj : curNode->getAdjList()) {
          if (adj != last && gridEdgs[edg].count(adj)) {
            last = adj;
            i++;
            assert(edges.size() >= i);
            edges[edges.size() - i] = adj;
            curNode = adj->getOtherNd(curNode);
            break;
          }
        }
      }

      assert(i == edges.size());

      for (size_t i = 0; i < edges.size(); i++) {
        // TODO: delete
        if (!edges[i]->pl().isSecondary()) {
          assert(gg->getResEdgsDirInd(edges[i]).size());
        }
      }

      d->draw(edg, edges, false);
    }
  }
}

// _____________________________________________________________________________
size_t ILPGridOptimizer::nonInfDeg(const GridNode* g) const {
  size_t ret = 0;
  for (auto e : g->getAdjList()) {
    if (e->pl().cost() < basegraph::SOFT_INF) ret++;
  }

  return ret;
}

// _____________________________________________________________________________
StarterSol ILPGridOptimizer::extractFeasibleSol(Drawing* d, BaseGraph* gg,
                                                const CombGraph& cg,
                                                double maxGrDist) const {
  StarterSol sol;

  for (auto nd : cg.getNds()) {
    if (nd->getDeg() == 0) continue;
    auto settled = gg->getSettled(nd);

    for (auto gnd : *gg->getNds()) {
      if (!gnd->pl().isSink()) continue;
      double gridD = dist(*nd->pl().getGeom(), *gnd->pl().getGeom());

      // threshold for speedup
      double maxDis = gg->getCellSize() * maxGrDist;
      if (gridD >= maxDis) continue;

      auto varName = getStatPosVar(gnd, nd);
      if (gnd == settled) {
        sol[varName] = 1;

        // if settled, all bend edges are unused
        for (size_t p = 0; p < gg->maxDeg(); p++) {
          auto portNd = gnd->pl().getPort(p);
          if (!portNd) continue;  // may be pruned
          for (auto bendEdg : portNd->getAdjList()) {
            if (!bendEdg->pl().isSecondary()) continue;
            for (auto cEdg : nd->getAdjList()) {
              if (cEdg->getFrom() != nd) continue;
              auto varName = getEdgUseVar(bendEdg, cEdg);
              sol[varName] = 0;
            }
          }
        }
      } else {
        sol[varName] = 0;

        // if not settled, all sink edges are unused
        // for all input edges
        for (auto sinkEdg : gnd->getAdjList()) {
          assert(sinkEdg->pl().isSecondary());
          for (auto cEdg : nd->getAdjList()) {
            if (cEdg->getFrom() != nd) continue;
            auto varName = getEdgUseVar(sinkEdg, cEdg);
            sol[varName] = 0;
          }
        }
      }
    }
  }

  // init edge use vars to 0
  for (auto grNd : *gg->getNds()) {
    for (auto grEdg : grNd->getAdjListOut()) {
      if (grEdg->pl().isSecondary()) continue;

      for (auto cNd : cg.getNds()) {
        for (auto cEdg : cNd->getAdjList()) {
          if (cEdg->getFrom() != cNd) continue;
          auto varName = getEdgUseVar(grEdg, cEdg);
          sol[varName] = 0;
        }
      }
    }
  }

  // write edge use vars from heuristic solution
  for (const auto& a : d->getEdgPaths()) {
    auto cEdg = a.first;
    const auto& grEdgList = a.second;
    for (auto xy : grEdgList) {
      auto grEdg = gg->getGrEdgById(xy);
      auto varName = getEdgUseVar(grEdg, cEdg);
      sol[varName] = 1;
    }
  }

  // TODO: we don't write the bend edge variables here, these can
  // typically be filled by the solver using the information given above
  return sol;
}
