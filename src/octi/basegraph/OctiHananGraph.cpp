// Copyright 2017, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Authors: Patrick Brosi <brosi@informatik.uni-freiburg.de>

#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include "octi/basegraph/NodeCost.h"
#include "octi/basegraph/OctiHananGraph.h"
#include "util/Misc.h"
#include "util/geo/output/GeoGraphJsonOutput.h"
#include "util/graph/Node.h"

using namespace octi::basegraph;
using octi::basegraph::NodeCost;
using octi::basegraph::OctiHananGraph;
using util::geo::DBox;
using util::geo::dist;
using util::geo::DPoint;

// _____________________________________________________________________________
GridNode* OctiHananGraph::neigh(size_t cx, size_t cy, size_t i) const {
  auto a = _ndIdx[cx * _grid.getYHeight() + cy];
  if (!a) return 0;

  if (i > 7) return _nds[a - 1];
  return _neighs[a - 1 + i];
}

// _____________________________________________________________________________
void OctiHananGraph::unSettleEdg(CombEdge* ce, GridNode* a, GridNode* b) {
  if (a == b) return;

  auto ge = getNEdg(a, b);
  auto gf = getNEdg(b, a);

  assert(ge);
  assert(gf);

  ge->pl().delResEdg();
  gf->pl().delResEdg();

  _resEdgs[ge].erase(ce);
  _resEdgs[gf].erase(ce);

  if (_resEdgs[ge].size() == 0) {
    if (!a->pl().isSettled() && unused(a)) openTurns(a);
    if (!b->pl().isSettled() && unused(b)) openTurns(b);
  }

  // unblock blocked diagonal edges crossing this edge
  if (getDir(a, b) % 2 != 0 && _resEdgs[ge].size() == 0) {
    auto pairs = _edgePairs.find(ge);
    if (pairs == _edgePairs.end()) return;
    for (auto p : pairs->second) {
      p.first->pl().unblock();
      p.second->pl().unblock();
    }
  }
}

// _____________________________________________________________________________
size_t OctiHananGraph::ang(size_t i, size_t j) const {
  int d = (int)(i) - (int)(j);
  int deg = abs((((d + 4) % 8) + 8) % 8 - 4) % 4;

  int ang = (8 + (i - j)) % 8;
  if (ang > 4) ang = 8 - ang;
  ang = ang % 4;

  assert(deg == ang);

  return ang;
}

// _____________________________________________________________________________
double OctiHananGraph::getBendPen(size_t i, size_t j) const {
  return _bendCosts[ang(i, j)];
}

// _____________________________________________________________________________
void OctiHananGraph::settleEdg(GridNode* a, GridNode* b, CombEdge* e,
                               size_t rndrOrd) {
  if (a == b) return;

  // this closes the grid edge
  auto ge = getNEdg(a, b);
  auto gf = getNEdg(b, a);

  addResEdg(ge, e);
  addResEdg(gf, e);

  ge->pl().setRndrOrder(rndrOrd);

  // this closes both nodes
  // a close means that all major edges reaching this node are closed
  closeTurns(a);
  closeTurns(b);

  // block diagonal edges crossing this edge
  if (getDir(a, b) % 2 != 0) {
    auto pairs = _edgePairs.find(ge);
    if (pairs == _edgePairs.end()) return;
    for (auto p : pairs->second) {
      p.first->pl().block();
      p.second->pl().block();
    }
  }
}

// _____________________________________________________________________________
CrossEdgPairs OctiHananGraph::getCrossEdgPairs() const {
  CrossEdgPairs ret;
  std::unordered_map<const GridEdge*, std::set<const GridEdge*>> have;

  for (const GridNode* n : getNds()) {
    if (!n->pl().isSink()) continue;

    auto eOr = getNEdg(n, neigh(n, 3));
    auto fOr = getNEdg(neigh(n, 3), n);

    if (!eOr) continue;

    auto pairs = _edgePairs.find(eOr);
    if (pairs == _edgePairs.end()) continue;


    for (const auto& p : pairs->second) {
      ret.push_back({{eOr, fOr}, p});
    }
  }

  return ret;
}

// _____________________________________________________________________________
GridEdge* OctiHananGraph::getNEdg(const GridNode* a, const GridNode* b) const {
  if (!a || !b) return 0;

  size_t dir = getDir(a, b);

  if (a->pl().getPort(dir) &&
      b->pl().getPort((dir + maxDeg() / 2) % maxDeg())) {
    return const_cast<GridEdge*>(
        getEdg(a->pl().getPort(dir),
               b->pl().getPort((dir + maxDeg() / 2) % maxDeg())));
  }

  return 0;
}

// _____________________________________________________________________________
void OctiHananGraph::init() {
  std::vector<GridNode*> xSorted;
  std::vector<GridNode*> ySorted;

  _ndIdx.resize(_grid.getXWidth() * _grid.getYHeight());

  std::set<std::pair<size_t, size_t>> coords;

  // get coords
  for (auto cNd : _cg.getNds()) {
    int x = _grid.getCellXFromX(cNd->pl().getGeom()->getX());
    int y = _grid.getCellYFromY(cNd->pl().getGeom()->getY());
    coords.insert({x, y});
  }

  // hanan iterations
  for (size_t i = 1; i < _iters; i++)
    coords = getIterCoords(coords);

  // write nodes
  for (auto coord : coords) {
    int x = coord.first;
    int y = coord.second;

    auto nd = writeNd(x, y);

    xSorted.push_back(nd);
    ySorted.push_back(nd);
  }

  struct {
    bool operator()(const GridNode* a, const GridNode* b) {
      return a->pl().getX() < b->pl().getX();
    }
  } sortByX;

  struct {
    bool operator()(const GridNode* a, const GridNode* b) {
      return a->pl().getY() < b->pl().getY();
    }
  } sortByY;

  if (xSorted.size() == 0) return;

  std::vector<std::vector<GridNode*>> yAct(_grid.getYHeight());
  std::vector<std::vector<GridNode*>> xAct(_grid.getXWidth());

  std::vector<std::vector<GridNode*>> xyAct(_grid.getXWidth() +
                                            _grid.getYHeight());
  std::vector<std::vector<GridNode*>> yxAct(_grid.getXWidth() +
                                            _grid.getYHeight());

  for (auto nd : xSorted) {
    yAct[nd->pl().getY()].push_back(nd);
  }

  for (auto nd : xSorted) {
    xAct[nd->pl().getX()].push_back(nd);
  }

  for (auto nd : xSorted) {
    xyAct[nd->pl().getX() + (_grid.getYHeight() - 1 - nd->pl().getY())]
        .push_back(nd);
  }

  for (auto nd : xSorted) {
    yxAct[nd->pl().getY() + nd->pl().getX()].push_back(nd);
  }

  for (size_t x = 0; x < _grid.getXWidth(); x++) {
    if (!xAct[x].size()) continue;

    for (size_t y = 0; y < _grid.getYHeight(); y++) {
      if (!yAct[y].size()) continue;
      if (getNode(x, y)) continue;
      auto newNd = writeNd(x, y);
      yAct[y].push_back(newNd);
      xAct[x].push_back(newNd);
    }
  }

  for (size_t x = 0; x < _grid.getXWidth(); x++) {
    for (size_t y = 0; y < _grid.getYHeight(); y++) {
      size_t xi = x + (_grid.getYHeight() - 1 - y);
      size_t yi = y + x;
      if ((xyAct[xi].size() &&
           (yxAct[yi].size() || yAct[y].size() || xAct[x].size())) ||
          (yxAct[yi].size() &&
           (xyAct[xi].size() || yAct[y].size() || xAct[x].size()))) {
        bool have = false;

        auto newNd = getNode(x, y);

        if (newNd) {
          have = true;
        }

        if (!have) newNd = writeNd(x, y);

        if (xyAct[xi].size()) {
          xyAct[xi].push_back(newNd);
        }

        if (yxAct[yi].size()) {
          yxAct[yi].push_back(newNd);
        }

        if (have) continue;

        if (yAct[y].size()) {
          yAct[y].push_back(newNd);
        }

        if (xAct[x].size()) {
          xAct[x].push_back(newNd);
        }
      }
    }
  }

  for (size_t x = 0; x < _grid.getXWidth(); x++) {
    std::sort(xAct[x].begin(), xAct[x].end(), sortByY);
  }

  for (size_t y = 0; y < _grid.getYHeight(); y++) {
    std::sort(yAct[y].begin(), yAct[y].end(), sortByX);
  }

  for (size_t i = 0; i < _grid.getYHeight() + _grid.getXWidth(); i++) {
    std::sort(xyAct[i].begin(), xyAct[i].end(), sortByY);
    std::sort(yxAct[i].begin(), yxAct[i].end(), sortByX);
  }


  // init the _neighs size
  _neighs.resize(_nds.size() * 8);

  for (size_t y = 0; y < _grid.getYHeight(); y++) {
    for (size_t i = 1; i < yAct[y].size(); i++) {
      connectNodes(yAct[y][i - 1], yAct[y][i], 2);
    }
  }

  for (size_t x = 0; x < _grid.getXWidth(); x++) {
    for (size_t i = 1; i < xAct[x].size(); i++) {
      connectNodes(xAct[x][i - 1], xAct[x][i], 0);
    }
  }

  for (size_t xi = 0; xi < _grid.getXWidth() + _grid.getYHeight(); xi++) {
    for (size_t i = 1; i < xyAct[xi].size(); i++) {
      connectNodes(xyAct[xi][i - 1], xyAct[xi][i], 1);
    }
  }

  for (size_t yi = 0; yi < _grid.getXWidth() + _grid.getYHeight(); yi++) {
    for (size_t i = 1; i < yxAct[yi].size(); i++) {
      connectNodes(yxAct[yi][i - 1], yxAct[yi][i], 3);
    }
  }

  // diagonal intersections
  for (size_t i = 0; i < _grid.getXWidth() + _grid.getYHeight(); i++) {
    for (size_t j = 1; j < xyAct[i].size(); j++) {
      auto ndA = xyAct[i][j - 1];
      auto ndB = xyAct[i][j];
      if (ndA == ndB) continue;  // there may be duplicates

      auto ea = getNEdg(ndA, ndB);
      auto eb = getNEdg(ndB, ndA);

      size_t yi = ndA->pl().getX() + ndA->pl().getY() + 1;
      if (yi < yxAct.size() && yxAct[yi].size()) {
        auto it =
            std::upper_bound(yxAct[yi].begin(), yxAct[yi].end(), ndA, sortByX);

        if (it != yxAct[yi].end() && it != yxAct[yi].begin()) {
          // it is the first element with an x greater than ndA, which means
          // that the preceeding element at it-1 has a different x, so we are
          // filtering out duplicates here automatically
          auto oNdA = *(it - 1);
          auto oNdB = *(it);
          assert(oNdA != oNdB);

          auto fa = getNEdg(oNdA, oNdB);
          auto fb = getNEdg(oNdB, oNdA);

          _edgePairs[ea].push_back({fa, fb});
          _edgePairs[eb].push_back({fa, fb});

          _edgePairs[fa].push_back({ea, eb});
          _edgePairs[fb].push_back({ea, eb});
        }
      }
    }
  }

  prunePorts();
  writeInitialCosts();
}

// _____________________________________________________________________________
void OctiHananGraph::connectNodes(GridNode* grNdFr, GridNode* grNdTo,
                                  size_t p) {
  if (grNdFr == 0 || grNdTo == 0) return;
  if (grNdFr == grNdTo) return;
  GridNode* fr = grNdFr->pl().getPort(p);
  GridNode* to = grNdTo->pl().getPort((p + maxDeg() / 2) % maxDeg());

  auto e = addEdg(fr, to, GridEdgePL(9, false, false));
  e->pl().setId(_edgeCount);
  _edgeCount++;

  _neighs[grNdFr->pl().getId() + p] = grNdTo;
  _neighs[grNdTo->pl().getId() + (p + maxDeg() / 2) % maxDeg()] = grNdFr;

  auto f = addEdg(to, fr, GridEdgePL(9, false, false));
  f->pl().setId(_edgeCount);
  _edgeCount++;
}

// _____________________________________________________________________________
void OctiHananGraph::writeInitialCosts() {
  for (auto n : *getNds()) {
      if (!n->pl().isSink()) continue;
      for (size_t p = 0; p < maxDeg(); p++) {
        auto port = n->pl().getPort(p);
        auto neighbor = neigh(n->pl().getX(), n->pl().getY(), p);

        if (!neighbor || !port) continue;

        auto oPort = neighbor->pl().getPort((p + maxDeg() / 2) % maxDeg());

        int xDist = labs((int)n->pl().getX() - (int)neighbor->pl().getX());
        int yDist = labs((int)n->pl().getY() - (int)neighbor->pl().getY());

        auto e = getEdg(port, oPort);

        double cost = 0;
        if (p % 4 == 0)
          cost = (_c.verticalPen + _heurHopCost) * yDist - _heurHopCost;
        else if ((p + 2) % 4 == 0)
          cost = (_c.horizontalPen + _heurHopCost) * xDist - _heurHopCost;
        else if (p % 2)
          cost = (_c.diagonalPen + _heurHopCost) * yDist - _heurHopCost;

        e->pl().setCost(cost);
      }
    }
}

// _____________________________________________________________________________
GridNode* OctiHananGraph::writeNd(size_t x, size_t y) {
  double xPos = _bbox.getLowerLeft().getX() + x * _cellSize;
  double yPos = _bbox.getLowerLeft().getY() + y * _cellSize;

  GridNode* n = addNd(DPoint(xPos, yPos));
  n->pl().setId(_nds.size());
  _nds.push_back(n);
  _ndIdx[x * _grid.getYHeight() + y] = _nds.size();
  n->pl().setSink();
  _grid.add(x, y, n);
  n->pl().setXY(x, y);
  n->pl().setParent(n);

  for (int i = 0; i < 8; i++) {
    int xi = (4 - (i % 8)) % 4;
    xi /= abs(abs(xi) - 1) + 1;
    int yi = (4 - ((i + 2) % 8)) % 4;
    yi /= abs(abs(yi) - 1) + 1;

    GridNode* nn = addNd(DPoint(xPos + xi * _spacer, yPos + yi * _spacer));
    nn->pl().setId(_nds.size());
    _nds.push_back(nn);
    nn->pl().setParent(n);
    n->pl().setPort(i, nn);

    auto e = addEdg(n, nn, GridEdgePL(INF, true, true));
    e->pl().setId(_edgeCount);
    _edgeCount++;

    e = addEdg(nn, n, GridEdgePL(INF, true, true));
    e->pl().setId(_edgeCount);
    _edgeCount++;
  }

  // in-node connections
  for (size_t i = 0; i < maxDeg(); i++) {
    for (size_t j = i + 1; j < maxDeg(); j++) {
      double pen = getBendPen(i, j);

      if (x == 0 && (i == 5 || i == 6 || i == 7)) pen = INF;
      if (y == 0 && (i == 0 || i == 7 || i == 1)) pen = INF;
      if (x == _grid.getXWidth() - 1 && (i == 1 || i == 2 || i == 3)) pen = INF;
      if (y == _grid.getYHeight() - 1 && (i == 3 || i == 4 || i == 5))
        pen = INF;

      auto e = addEdg(n->pl().getPort(i), n->pl().getPort(j),
                            GridEdgePL(pen, true, false));
      e->pl().setId(_edgeCount);
      _edgeCount++;

      e = addEdg(n->pl().getPort(j), n->pl().getPort(i),
                       GridEdgePL(pen, true, false));
      e->pl().setId(_edgeCount);
      _edgeCount++;
    }
  }

  return n;
}

// _____________________________________________________________________________
size_t OctiHananGraph::maxDeg() const { return 8; }

// _____________________________________________________________________________
GridNode* OctiHananGraph::getNode(size_t x, size_t y) const {
  auto a = _ndIdx[x * _grid.getYHeight() + y];
  if (a == 0) return 0;
  return _nds[a - 1];
}

// _____________________________________________________________________________
double OctiHananGraph::ndMovePen(const CombNode* cbNd,
                                 const GridNode* grNd) const {
  // the move penalty has to be at least the max cost of saving a single
  // grid hop - otherwise we could move the node closer and closer to the
  // other node without ever increasing the total cost.

  // additional penalty per grid move
  // TODO: make configurable
  double PEN = 0.5;

  // we may substitute a diagonal edge be a horizontal + 90 deg bend + vertical
  // edge
  double diagCost =
      _bendCosts[0] +
      fmin(_c.diagonalPen, _c.horizontalPen + _c.verticalPen + _bendCosts[2]);

  double vertCost =
      _bendCosts[0] +
      fmin(_c.verticalPen, _c.horizontalPen + _c.diagonalPen + _bendCosts[3]);

  double horiCost =
      _bendCosts[0] +
      fmin(_c.horizontalPen, _c.verticalPen + _c.diagonalPen + _bendCosts[3]);

  double penPerGrid = PEN + fmax(diagCost, fmax(vertCost, horiCost));

  // real distance between grid node and input node
  double d = dist(*cbNd->pl().getGeom(), *grNd->pl().getGeom());

  // distance normalized to grid length
  double gridD = d / getCellSize();

  // and multiplied per grid hop penalty
  return gridD * penPerGrid;
}

// _____________________________________________________________________________
std::set<std::pair<size_t, size_t>> OctiHananGraph::getIterCoords(const std::set<std::pair<size_t, size_t>>& inCoords) const {

  std::set<std::pair<size_t, size_t>> ret;

  std::vector<std::pair<size_t, size_t>> xSorted;
  std::vector<std::pair<size_t, size_t>> ySorted;

  // write nodes
  for (auto coord : inCoords) {
    xSorted.push_back(coord);
    ySorted.push_back(coord);
  }

  std::vector<std::vector<std::pair<size_t, size_t>>> yAct(_grid.getYHeight());
  std::vector<std::vector<std::pair<size_t, size_t>>> xAct(_grid.getXWidth());

  std::vector<std::vector<std::pair<size_t, size_t>>> xyAct(_grid.getXWidth() +
                                            _grid.getYHeight());
  std::vector<std::vector<std::pair<size_t, size_t>>> yxAct(_grid.getXWidth() +
                                            _grid.getYHeight());

  for (auto c : xSorted) {
    yAct[c.second].push_back(c);
  }

  for (auto c : xSorted) {
    xAct[c.first].push_back(c);
  }

  for (auto c : xSorted) {
    xyAct[c.first + (_grid.getYHeight() - 1 - c.second)]
        .push_back(c);
  }

  for (auto c : xSorted) {
    yxAct[c.second + c.first].push_back(c);
  }

  for (size_t x = 0; x < _grid.getXWidth(); x++) {
    if (!xAct[x].size()) continue;

    for (size_t y = 0; y < _grid.getYHeight(); y++) {
      if (!yAct[y].size()) continue;
      if (ret.count({x, y})) continue;
      ret.insert({x, y});
      yAct[y].push_back({x, y});
      xAct[x].push_back({x, y});
    }
  }

  for (size_t x = 0; x < _grid.getXWidth(); x++) {
    for (size_t y = 0; y < _grid.getYHeight(); y++) {
      size_t xi = x + (_grid.getYHeight() - 1 - y);
      size_t yi = y + x;
      if ((xyAct[xi].size() &&
           (yxAct[yi].size() || yAct[y].size() || xAct[x].size())) ||
          (yxAct[yi].size() &&
           (xyAct[xi].size() || yAct[y].size() || xAct[x].size()))) {
        if (!ret.count({x, y})) ret.insert({x, y});
      }
    }
  }

  return ret;
}
