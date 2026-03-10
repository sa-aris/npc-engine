#pragma once
// Pathfinding system — complete rewrite of the original basic A*.
//
//  Pathfinder          Enhanced A* with node pool, tie-breaking, diagonal
//                      corner-cut check, partial path fallback, path cache,
//                      dynamic obstacles, Catmull-Rom smoothing.
//  NavRegions          Flood-fill connectivity map — O(1) reachability check.
//  WaypointGraph       Lightweight A* on explicit waypoint graph for large /
//                      open worlds (navmesh substitute).
//  PathRequestQueue    Async batched pathfinding — submit requests, drain a
//                      budget per frame (LOD-friendly).

#include "../core/vec2.hpp"
#include "../core/types.hpp"

#include <vector>
#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <functional>
#include <algorithm>
#include <cmath>
#include <string>
#include <optional>
#include <memory>
#include <cassert>
#include <list>

namespace npc {

class GameWorld;

// ═══════════════════════════════════════════════════════════════════════
// NavRegions — flood-fill connected-component map
// ═══════════════════════════════════════════════════════════════════════
// Call rebuild() once after the world grid is set up, then use
// isReachable(a, b) to cheaply skip A* when the goal is in a different
// region (surrounded by walls, etc.).

class NavRegions {
public:
    NavRegions() = default;
    NavRegions(int w, int h) { resize(w, h); }

    using WalkableCheck = std::function<bool(int, int)>;

    void rebuild(int width, int height, WalkableCheck walkable) {
        width_  = width;
        height_ = height;
        regions_.assign(width * height, -1);
        numRegions_ = 0;

        for (int y = 0; y < height_; ++y)
            for (int x = 0; x < width_; ++x)
                if (regions_[idx(x,y)] < 0 && walkable(x, y))
                    floodFill(x, y, numRegions_++, walkable);
    }

    // O(1) check
    bool isReachable(int x1, int y1, int x2, int y2) const {
        if (!inBounds(x1,y1) || !inBounds(x2,y2)) return false;
        int r1 = regions_[idx(x1,y1)];
        int r2 = regions_[idx(x2,y2)];
        return r1 >= 0 && r1 == r2;
    }
    bool isReachable(Vec2 a, Vec2 b) const {
        return isReachable(a.gridX(), a.gridY(), b.gridX(), b.gridY());
    }

    int regionOf(int x, int y) const {
        if (!inBounds(x,y)) return -1;
        return regions_[idx(x,y)];
    }
    int numRegions() const { return numRegions_; }

    // Invalidate one cell (e.g., dynamic obstacle placed).
    // Full rebuild needed for accuracy, but this marks the cell as isolated.
    void setBlocked(int x, int y) {
        if (inBounds(x,y)) regions_[idx(x,y)] = -1;
    }

private:
    void resize(int w, int h) {
        width_ = w; height_ = h;
        regions_.assign(w * h, -1);
    }
    int  idx(int x, int y) const { return y * width_ + x; }
    bool inBounds(int x, int y) const {
        return x >= 0 && x < width_ && y >= 0 && y < height_;
    }

    void floodFill(int sx, int sy, int rid, WalkableCheck& wk) {
        std::queue<std::pair<int,int>> q;
        q.push({sx, sy});
        regions_[idx(sx,sy)] = rid;
        static const int dx[] = {0,1,0,-1};
        static const int dy[] = {-1,0,1,0};
        while (!q.empty()) {
            auto [x,y] = q.front(); q.pop();
            for (int d = 0; d < 4; ++d) {
                int nx = x+dx[d], ny = y+dy[d];
                if (!inBounds(nx,ny) || regions_[idx(nx,ny)] >= 0 || !wk(nx,ny)) continue;
                regions_[idx(nx,ny)] = rid;
                q.push({nx,ny});
            }
        }
    }

    int width_ = 0, height_ = 0, numRegions_ = 0;
    std::vector<int> regions_;
};

// ═══════════════════════════════════════════════════════════════════════
// Path Cache — LRU eviction
// ═══════════════════════════════════════════════════════════════════════

struct PathCacheKey {
    int sx, sy, gx, gy;
    bool operator==(const PathCacheKey& o) const {
        return sx==o.sx && sy==o.sy && gx==o.gx && gy==o.gy;
    }
};

struct PathCacheKeyHash {
    size_t operator()(const PathCacheKey& k) const {
        // FNV-like mix
        size_t h = 2166136261u;
        auto mix = [&](int v) { h ^= static_cast<size_t>(v); h *= 16777619u; };
        mix(k.sx); mix(k.sy); mix(k.gx); mix(k.gy);
        return h;
    }
};

class PathCache {
public:
    explicit PathCache(size_t capacity = 128) : capacity_(capacity) {}

    const std::vector<Vec2>* get(const PathCacheKey& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return nullptr;
        // Move to front (LRU)
        lru_.splice(lru_.begin(), lru_, it->second.listIt);
        return &it->second.path;
    }

    void put(PathCacheKey key, std::vector<Vec2> path) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second.path = std::move(path);
            lru_.splice(lru_.begin(), lru_, it->second.listIt);
            return;
        }
        if (map_.size() >= capacity_) {
            auto evict = lru_.back();
            map_.erase(evict);
            lru_.pop_back();
        }
        lru_.push_front(key);
        map_[key] = { std::move(path), lru_.begin() };
    }

    // Invalidate any path that starts or ends at (x,y) — called on obstacle change
    void invalidateCell(int x, int y) {
        std::vector<PathCacheKey> toRemove;
        for (auto& [k, v] : map_)
            if (k.sx==x&&k.sy==y || k.gx==x&&k.gy==y) toRemove.push_back(k);
        for (auto& k : toRemove) {
            lru_.erase(map_[k].listIt);
            map_.erase(k);
        }
    }

    void clear()       { map_.clear(); lru_.clear(); }
    size_t size() const{ return map_.size(); }

private:
    using LRUList = std::list<PathCacheKey>;

    struct Entry {
        std::vector<Vec2>    path;
        LRUList::iterator    listIt;
    };

    size_t capacity_;
    std::unordered_map<PathCacheKey, Entry, PathCacheKeyHash> map_;
    LRUList lru_;
};

// ═══════════════════════════════════════════════════════════════════════
// Pathfinder — Enhanced A* on uniform grid
// ═══════════════════════════════════════════════════════════════════════

struct PathNode {
    int   x = 0, y = 0;
    int   parentX = -1, parentY = -1;
    float g = 0.f, h = 0.f;
    float f() const { return g + h; }
    bool operator>(const PathNode& o) const { return f() > o.f(); }
};

struct PathResult {
    std::vector<Vec2> waypoints;   // final path (may be partial)
    bool              complete    = false; // false → partial path
    float             cost        = 0.f;
    int               nodesVisited= 0;
    bool              fromCache   = false;
};

class Pathfinder {
public:
    using WalkableCheck = std::function<bool(int, int)>;
    using CostFn        = std::function<float(int, int)>;

    Pathfinder(int gridWidth, int gridHeight,
               WalkableCheck walkable,
               CostFn        cost = nullptr)
        : width_(gridWidth), height_(gridHeight)
        , walkable_(std::move(walkable))
        , cost_(cost ? std::move(cost) : [](int,int){ return 1.f; })
    {}

    // ── Configuration ────────────────────────────────────────────────

    // Max nodes expanded per query (prevents frame spikes on huge grids)
    void setNodeBudget(int n)       { nodeBudget_ = n; }
    // Path cache capacity (0 = disabled)
    void setCacheCapacity(size_t n) { cache_ = PathCache(n); }
    // Allow partial paths when goal is unreachable
    void setAllowPartial(bool b)    { allowPartial_ = b; }
    // Tie-break weight: > 1 slightly prefers straight lines, reduces nodes
    void setTieBreak(float w)       { tieBreak_ = w; }

    // ── Dynamic obstacles ────────────────────────────────────────────

    void addObstacle(int x, int y) {
        if (!inBounds(x,y)) return;
        dynObstacles_.insert(cellKey(x,y));
        cache_.invalidateCell(x,y);
        regions_.setBlocked(x,y);
    }
    void removeObstacle(int x, int y) {
        dynObstacles_.erase(cellKey(x,y));
        cache_.invalidateCell(x,y);
    }
    bool isDynamicObstacle(int x, int y) const {
        return dynObstacles_.count(cellKey(x,y));
    }
    void clearObstacles() { dynObstacles_.clear(); cache_.clear(); }

    // ── Region connectivity ──────────────────────────────────────────

    void buildRegions() {
        regions_.rebuild(width_, height_, [this](int x, int y){
            return isWalkable(x,y);
        });
    }

    bool isReachable(Vec2 a, Vec2 b) const {
        return regions_.isReachable(a, b);
    }

    // ── Main path query ──────────────────────────────────────────────

    // Full result with metadata
    PathResult query(Vec2 start, Vec2 goal) {
        int sx = start.gridX(), sy = start.gridY();
        int gx = goal.gridX(),  gy = goal.gridY();

        PathResult res;

        // Cache hit
        PathCacheKey ck{sx, sy, gx, gy};
        if (auto* cached = cache_.get(ck)) {
            res.waypoints = *cached;
            res.complete  = !res.waypoints.empty();
            res.fromCache = true;
            return res;
        }

        // Quick reachability — skip A* entirely
        if (regions_.numRegions() > 0 && !regions_.isReachable(sx,sy,gx,gy)) {
            if (allowPartial_) {
                res.waypoints = partialPath(start, goal);
                res.complete  = false;
            }
            return res;
        }

        if (!isWalkable(gx, gy)) {
            // Snap goal to nearest walkable
            auto snapped = snapToWalkable(gx, gy, 4);
            if (!snapped) return res;
            gx = snapped->first; gy = snapped->second;
        }

        if (sx == gx && sy == gy) {
            res.waypoints = { goal };
            res.complete  = true;
            cache_.put(ck, res.waypoints);
            return res;
        }

        auto raw = astar(sx, sy, gx, gy, res.nodesVisited, res.cost);
        if (!raw.empty()) {
            res.waypoints = smoothPath(raw);
            res.complete  = true;
            cache_.put(ck, res.waypoints);
        } else if (allowPartial_) {
            res.waypoints = partialPath(start, goal);
            res.complete  = false;
        }
        return res;
    }

    // Convenience — returns only waypoints (backward-compatible)
    std::vector<Vec2> findPath(Vec2 start, Vec2 goal) {
        return query(start, goal).waypoints;
    }

    // ── Smoothing ────────────────────────────────────────────────────

    // Greedy line-of-sight path reduction
    std::vector<Vec2> smoothPath(const std::vector<Vec2>& path) const {
        if (path.size() <= 2) return path;
        std::vector<Vec2> out;
        out.push_back(path.front());
        size_t cur = 0;
        while (cur < path.size() - 1) {
            size_t farthest = cur + 1;
            for (size_t i = path.size() - 1; i > cur + 1; --i) {
                if (hasLineOfSight(path[cur], path[i])) { farthest = i; break; }
            }
            out.push_back(path[farthest]);
            cur = farthest;
        }
        return out;
    }

    // Catmull-Rom spline interpolation for smooth movement (call after smoothPath)
    // `segments` = sub-points between each waypoint pair
    std::vector<Vec2> splinePath(const std::vector<Vec2>& waypoints,
                                  int segments = 4) const {
        if (waypoints.size() < 2) return waypoints;
        std::vector<Vec2> out;
        out.push_back(waypoints.front());

        auto catmull = [](Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float t) -> Vec2 {
            float t2 = t*t, t3 = t2*t;
            float x = 0.5f * ((2*p1.x) + (-p0.x+p2.x)*t
                + (2*p0.x-5*p1.x+4*p2.x-p3.x)*t2
                + (-p0.x+3*p1.x-3*p2.x+p3.x)*t3);
            float y = 0.5f * ((2*p1.y) + (-p0.y+p2.y)*t
                + (2*p0.y-5*p1.y+4*p2.y-p3.y)*t2
                + (-p0.y+3*p1.y-3*p2.y+p3.y)*t3);
            return {x, y};
        };

        size_t n = waypoints.size();
        for (size_t i = 0; i + 1 < n; ++i) {
            Vec2 p0 = waypoints[i > 0   ? i-1 : 0];
            Vec2 p1 = waypoints[i];
            Vec2 p2 = waypoints[i+1];
            Vec2 p3 = waypoints[i+2 < n ? i+2 : n-1];
            for (int s = 1; s <= segments; ++s) {
                float t = static_cast<float>(s) / static_cast<float>(segments);
                out.push_back(catmull(p0, p1, p2, p3, t));
            }
        }
        return out;
    }

    // ── Line of sight ────────────────────────────────────────────────

    bool hasLineOfSight(Vec2 a, Vec2 b) const {
        int x0=a.gridX(), y0=a.gridY(), x1=b.gridX(), y1=b.gridY();
        return bresenham(x0, y0, x1, y1, [&](int x, int y) {
            return inBounds(x,y) && isWalkable(x,y);
        });
    }

    // ── Diagnostics ──────────────────────────────────────────────────

    PathCache&       cache()          { return cache_; }
    const NavRegions& regions() const { return regions_; }
    size_t cacheSize()           const{ return cache_.size(); }
    int    width()               const{ return width_; }
    int    height()              const{ return height_; }

private:
    // ── A* core ──────────────────────────────────────────────────────

    std::vector<Vec2> astar(int sx, int sy, int gx, int gy,
                             int& nodesVisited, float& outCost) const {
        using PQ = std::priority_queue<PathNode,
                                       std::vector<PathNode>,
                                       std::greater<PathNode>>;
        PQ open;
        std::unordered_map<int, PathNode> nodes;
        std::unordered_set<int> closed;

        PathNode start;
        start.x=sx; start.y=sy; start.g=0;
        start.h = heuristic(sx,sy,gx,gy) * tieBreak_;
        open.push(start);
        nodes[cellKey(sx,sy)] = start;
        nodesVisited = 0;

        static const int DX[] = { 0, 1, 1, 1, 0,-1,-1,-1 };
        static const int DY[] = {-1,-1, 0, 1, 1, 1, 0,-1 };
        static const float DC[] = {1.f, 1.414f, 1.f, 1.414f, 1.f, 1.414f, 1.f, 1.414f};

        while (!open.empty()) {
            auto cur = open.top(); open.pop();
            int ck = cellKey(cur.x, cur.y);
            if (closed.count(ck)) continue;
            closed.insert(ck);
            ++nodesVisited;

            if (nodesVisited > nodeBudget_) break; // budget exceeded

            if (cur.x == gx && cur.y == gy) {
                outCost = cur.g;
                return reconstructPath(nodes, sx, sy, gx, gy);
            }

            for (int d = 0; d < 8; ++d) {
                int nx = cur.x+DX[d], ny = cur.y+DY[d];
                if (!inBounds(nx,ny) || !isWalkable(nx,ny)) continue;
                if (closed.count(cellKey(nx,ny))) continue;

                // Diagonal: block if either adjacent cardinal is solid
                if (DC[d] > 1.f) {
                    if (!isWalkable(cur.x+DX[d], cur.y) ||
                        !isWalkable(cur.x, cur.y+DY[d])) continue;
                }

                float ng = cur.g + DC[d] * cost_(nx, ny);
                int nk = cellKey(nx, ny);
                auto it = nodes.find(nk);
                if (it != nodes.end() && ng >= it->second.g) continue;

                PathNode nb;
                nb.x=nx; nb.y=ny;
                nb.g=ng; nb.h=heuristic(nx,ny,gx,gy) * tieBreak_;
                nb.parentX=cur.x; nb.parentY=cur.y;
                nodes[nk] = nb;
                open.push(nb);
            }
        }
        return {};
    }

    // Best-effort partial path: A* towards goal; return closest reached
    std::vector<Vec2> partialPath(Vec2 start, Vec2 goal) const {
        int sx=start.gridX(), sy=start.gridY();
        int gx=goal.gridX(),  gy=goal.gridY();

        using PQ = std::priority_queue<PathNode,
                                       std::vector<PathNode>,
                                       std::greater<PathNode>>;
        PQ open;
        std::unordered_map<int, PathNode> nodes;
        std::unordered_set<int> closed;

        PathNode s; s.x=sx; s.y=sy; s.g=0;
        s.h = heuristic(sx,sy,gx,gy);
        open.push(s); nodes[cellKey(sx,sy)] = s;

        int   bestKey = cellKey(sx,sy);
        float bestH   = s.h;
        int   visited = 0;

        static const int DX[]={0,1,1,1,0,-1,-1,-1};
        static const int DY[]={-1,-1,0,1,1,1,0,-1};
        static const float DC[]={1.f,1.414f,1.f,1.414f,1.f,1.414f,1.f,1.414f};

        while (!open.empty() && visited < nodeBudget_/2) {
            auto cur = open.top(); open.pop();
            int ck = cellKey(cur.x, cur.y);
            if (closed.count(ck)) continue;
            closed.insert(ck); ++visited;

            if (cur.h < bestH) { bestH = cur.h; bestKey = ck; }

            for (int d=0; d<8; ++d) {
                int nx=cur.x+DX[d], ny=cur.y+DY[d];
                if (!inBounds(nx,ny)||!isWalkable(nx,ny)||closed.count(cellKey(nx,ny))) continue;
                if (DC[d]>1.f&&(!isWalkable(cur.x+DX[d],cur.y)||!isWalkable(cur.x,cur.y+DY[d]))) continue;
                float ng = cur.g + DC[d]*cost_(nx,ny);
                int nk = cellKey(nx,ny);
                auto it = nodes.find(nk);
                if (it!=nodes.end()&&ng>=it->second.g) continue;
                PathNode nb; nb.x=nx; nb.y=ny; nb.g=ng; nb.h=heuristic(nx,ny,gx,gy);
                nb.parentX=cur.x; nb.parentY=cur.y;
                nodes[nk]=nb; open.push(nb);
            }
        }

        // Reconstruct from bestKey
        auto it = nodes.find(bestKey);
        if (it==nodes.end()) return {};
        auto& best = it->second;
        return smoothPath(reconstructPath(nodes, sx, sy, best.x, best.y));
    }

    // ── Helpers ──────────────────────────────────────────────────────

    bool isWalkable(int x, int y) const {
        return walkable_(x,y) && !dynObstacles_.count(cellKey(x,y));
    }
    bool inBounds(int x, int y) const {
        return x>=0&&x<width_&&y>=0&&y<height_;
    }
    int cellKey(int x, int y) const { return y*width_+x; }

    static float heuristic(int x1, int y1, int x2, int y2) {
        float dx=std::abs(float(x1-x2)), dy=std::abs(float(y1-y2));
        return std::max(dx,dy) + 0.414f*std::min(dx,dy); // octile
    }

    std::optional<std::pair<int,int>> snapToWalkable(int x, int y, int radius) const {
        for (int r=1; r<=radius; ++r)
            for (int dx=-r; dx<=r; ++dx)
                for (int dy=-r; dy<=r; ++dy)
                    if (std::abs(dx)==r||std::abs(dy)==r)
                        if (inBounds(x+dx,y+dy)&&isWalkable(x+dx,y+dy))
                            return std::make_pair(x+dx,y+dy);
        return std::nullopt;
    }

    std::vector<Vec2> reconstructPath(const std::unordered_map<int,PathNode>& nodes,
                                       int sx, int sy, int gx, int gy) const {
        std::vector<Vec2> path;
        int cx=gx, cy=gy;
        while (!(cx==sx&&cy==sy)) {
            path.push_back({float(cx), float(cy)});
            auto it = nodes.find(cellKey(cx,cy));
            if (it==nodes.end()) break;
            int px=it->second.parentX, py=it->second.parentY;
            cx=px; cy=py;
        }
        path.push_back({float(sx),float(sy)});
        std::reverse(path.begin(), path.end());
        return path;
    }

    template<typename Fn>
    static bool bresenham(int x0, int y0, int x1, int y1, Fn&& passable) {
        int dx=std::abs(x1-x0), dy=std::abs(y1-y0);
        int sx=(x0<x1)?1:-1, sy=(y0<y1)?1:-1, err=dx-dy;
        while (true) {
            if (!passable(x0,y0)) return false;
            if (x0==x1&&y0==y1) return true;
            int e2=2*err;
            if (e2>-dy){err-=dy; x0+=sx;}
            if (e2< dx){err+=dx; y0+=sy;}
        }
    }

    int   width_, height_;
    int   nodeBudget_  = 4096;
    float tieBreak_    = 1.001f; // slight tie-breaking
    bool  allowPartial_= true;

    WalkableCheck walkable_;
    CostFn        cost_;

    std::unordered_set<int> dynObstacles_;
    PathCache   cache_{128};
    NavRegions  regions_;
};

// ═══════════════════════════════════════════════════════════════════════
// WaypointGraph — A* on explicit node graph
// ═══════════════════════════════════════════════════════════════════════
// Use for large open worlds where grid A* is too slow.
// Place waypoints at key locations (road junctions, building entrances,
// dungeon rooms) and connect them.  Use the grid Pathfinder only for
// the local "last mile" to the nearest waypoint.

class WaypointGraph {
public:
    struct Node {
        EntityId            id;
        Vec2                pos;
        std::string         name;
        std::vector<std::pair<EntityId, float>> edges; // {neighbor, cost}
    };

    // ── Build ─────────────────────────────────────────────────────────

    EntityId addNode(Vec2 pos, std::string name = "") {
        EntityId id = nextId_++;
        nodes_[id] = {id, pos, std::move(name), {}};
        return id;
    }

    void connect(EntityId a, EntityId b, float cost = -1.f) {
        if (!nodes_.count(a) || !nodes_.count(b)) return;
        if (cost < 0.f)
            cost = nodes_[a].pos.distanceTo(nodes_[b].pos);
        addEdge(a, b, cost);
        addEdge(b, a, cost);
    }

    void connectOneWay(EntityId a, EntityId b, float cost = -1.f) {
        if (!nodes_.count(a) || !nodes_.count(b)) return;
        if (cost < 0.f) cost = nodes_[a].pos.distanceTo(nodes_[b].pos);
        addEdge(a, b, cost);
    }

    void removeNode(EntityId id) {
        nodes_.erase(id);
        for (auto& [nid, n] : nodes_) {
            auto& e = n.edges;
            e.erase(std::remove_if(e.begin(), e.end(),
                [id](auto& p){ return p.first == id; }), e.end());
        }
    }

    // ── Query ─────────────────────────────────────────────────────────

    // A* on graph nodes — returns list of waypoint IDs
    std::vector<EntityId> findPathIds(EntityId start, EntityId goal) const {
        if (!nodes_.count(start) || !nodes_.count(goal)) return {};
        if (start == goal) return {start};

        using PQ = std::priority_queue<
            std::pair<float, EntityId>,
            std::vector<std::pair<float, EntityId>>,
            std::greater<>>;

        std::unordered_map<EntityId, float>    gCost;
        std::unordered_map<EntityId, EntityId> parent;
        PQ open;

        gCost[start] = 0.f;
        open.push({heuristic(start, goal), start});

        while (!open.empty()) {
            auto [f, cur] = open.top(); open.pop();
            if (cur == goal) return reconstruct(parent, start, goal);

            for (auto& [nb, cost] : nodes_.at(cur).edges) {
                float ng = gCost[cur] + cost;
                if (!gCost.count(nb) || ng < gCost[nb]) {
                    gCost[nb]  = ng;
                    parent[nb] = cur;
                    open.push({ng + heuristic(nb, goal), nb});
                }
            }
        }
        return {};
    }

    // Returns world-space Vec2 path (via waypoint snapping)
    std::vector<Vec2> findPath(Vec2 start, Vec2 goal,
                                float snapRadius = 20.f) const {
        EntityId sId = nearestNode(start, snapRadius);
        EntityId gId = nearestNode(goal,  snapRadius);
        if (sId == INVALID_ENTITY || gId == INVALID_ENTITY) return {};

        auto ids = findPathIds(sId, gId);
        std::vector<Vec2> path;
        path.push_back(start);
        for (auto id : ids) path.push_back(nodes_.at(id).pos);
        path.push_back(goal);
        return path;
    }

    // Nearest waypoint within maxDist
    EntityId nearestNode(Vec2 pos, float maxDist = 1e9f) const {
        EntityId best = INVALID_ENTITY;
        float    bestD = maxDist * maxDist;
        for (auto& [id, n] : nodes_) {
            float d = pos.distanceSquaredTo(n.pos);
            if (d < bestD) { bestD = d; best = id; }
        }
        return best;
    }

    // Accessors
    const Node* node(EntityId id) const {
        auto it = nodes_.find(id);
        return it != nodes_.end() ? &it->second : nullptr;
    }
    size_t nodeCount() const { return nodes_.size(); }
    const std::unordered_map<EntityId, Node>& nodes() const { return nodes_; }

private:
    void addEdge(EntityId a, EntityId b, float cost) {
        auto& edges = nodes_[a].edges;
        for (auto& e : edges) if (e.first == b) { e.second = cost; return; }
        edges.push_back({b, cost});
    }

    float heuristic(EntityId a, EntityId b) const {
        return nodes_.at(a).pos.distanceTo(nodes_.at(b).pos);
    }

    std::vector<EntityId> reconstruct(
        const std::unordered_map<EntityId,EntityId>& parent,
        EntityId start, EntityId goal) const
    {
        std::vector<EntityId> path;
        EntityId cur = goal;
        while (cur != start) {
            path.push_back(cur);
            auto it = parent.find(cur);
            if (it == parent.end()) break;
            cur = it->second;
        }
        path.push_back(start);
        std::reverse(path.begin(), path.end());
        return path;
    }

    std::unordered_map<EntityId, Node> nodes_;
    EntityId nextId_ = 1;
};

// ═══════════════════════════════════════════════════════════════════════
// PathRequestQueue — async / budgeted pathfinding
// ═══════════════════════════════════════════════════════════════════════
// Submit requests (with optional priority); drain up to N per frame.
// Designed for background-tier NPCs that shouldn't block the main thread.

enum class PathPriority { High = 0, Normal = 1, Low = 2 };

struct PathRequest {
    EntityId                            requester   = INVALID_ENTITY;
    Vec2                                start, goal;
    PathPriority                        priority    = PathPriority::Normal;
    bool                                allowPartial= true;
    std::function<void(PathResult)>     callback;   // called with result
};

class PathRequestQueue {
public:
    void submit(PathRequest req) {
        queue_.push(std::move(req));
    }

    // Process up to `budget` requests using the given Pathfinder.
    // Returns how many were processed.
    int process(Pathfinder& pf, int budget = 8) {
        int done = 0;
        while (!queue_.empty() && done < budget) {
            PathRequest req = std::move(const_cast<PathRequest&>(queue_.top()));
            queue_.pop();
            pf.setAllowPartial(req.allowPartial);
            PathResult res = pf.query(req.start, req.goal);
            if (req.callback) req.callback(std::move(res));
            ++done;
        }
        return done;
    }

    bool   empty()   const { return queue_.empty(); }
    size_t pending() const { return queue_.size(); }
    void   clear()         { while (!queue_.empty()) queue_.pop(); }

private:
    struct Cmp {
        bool operator()(const PathRequest& a, const PathRequest& b) const {
            return static_cast<int>(a.priority) > static_cast<int>(b.priority);
        }
    };
    std::priority_queue<PathRequest,
                        std::vector<PathRequest>, Cmp> queue_;
};

} // namespace npc
