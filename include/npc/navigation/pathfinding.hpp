#pragma once

#include "../core/vec2.hpp"
#include <vector>
#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <cmath>
#include <algorithm>

namespace npc {

class GameWorld; // forward declaration

struct PathNode {
    int x, y;
    float gCost = 0.0f;  // cost from start
    float hCost = 0.0f;  // heuristic to goal
    float fCost() const { return gCost + hCost; }
    int parentX = -1, parentY = -1;

    bool operator>(const PathNode& o) const { return fCost() > o.fCost(); }
};

class Pathfinder {
public:
    using WalkableCheck = std::function<bool(int, int)>;
    using CostCheck = std::function<float(int, int)>;

    Pathfinder(int gridWidth, int gridHeight,
               WalkableCheck walkable, CostCheck cost)
        : width_(gridWidth), height_(gridHeight)
        , walkable_(std::move(walkable))
        , cost_(std::move(cost)) {}

    std::vector<Vec2> findPath(Vec2 start, Vec2 goal) {
        int sx = start.gridX(), sy = start.gridY();
        int gx = goal.gridX(),  gy = goal.gridY();

        if (!inBounds(gx, gy) || !walkable_(gx, gy)) return {};
        if (sx == gx && sy == gy) return {goal};

        // A* algorithm
        std::priority_queue<PathNode, std::vector<PathNode>, std::greater<PathNode>> open;
        std::unordered_map<int, PathNode> allNodes;

        auto key = [this](int x, int y) { return y * width_ + x; };

        PathNode startNode;
        startNode.x = sx; startNode.y = sy;
        startNode.gCost = 0.0f;
        startNode.hCost = heuristic(sx, sy, gx, gy);
        open.push(startNode);
        allNodes[key(sx, sy)] = startNode;

        std::unordered_set<int> closed;

        // 8-directional movement
        static const int dx[] = {0, 1, 1, 1, 0, -1, -1, -1};
        static const int dy[] = {-1, -1, 0, 1, 1, 1, 0, -1};
        static const float dcost[] = {1.0f, 1.414f, 1.0f, 1.414f, 1.0f, 1.414f, 1.0f, 1.414f};

        while (!open.empty()) {
            auto current = open.top();
            open.pop();

            int ck = key(current.x, current.y);
            if (closed.count(ck)) continue;
            closed.insert(ck);

            // Goal reached
            if (current.x == gx && current.y == gy) {
                return reconstructPath(allNodes, key, sx, sy, gx, gy);
            }

            for (int i = 0; i < 8; ++i) {
                int nx = current.x + dx[i];
                int ny = current.y + dy[i];
                int nk = key(nx, ny);

                if (!inBounds(nx, ny) || !walkable_(nx, ny) || closed.count(nk))
                    continue;

                // Diagonal: check both adjacent cells are walkable
                if (dcost[i] > 1.0f) {
                    if (!walkable_(current.x + dx[i], current.y) ||
                        !walkable_(current.x, current.y + dy[i]))
                        continue;
                }

                float newG = current.gCost + dcost[i] * cost_(nx, ny);

                auto it = allNodes.find(nk);
                if (it != allNodes.end() && newG >= it->second.gCost)
                    continue;

                PathNode neighbor;
                neighbor.x = nx; neighbor.y = ny;
                neighbor.gCost = newG;
                neighbor.hCost = heuristic(nx, ny, gx, gy);
                neighbor.parentX = current.x;
                neighbor.parentY = current.y;
                allNodes[nk] = neighbor;
                open.push(neighbor);
            }
        }

        return {}; // no path found
    }

    std::vector<Vec2> smoothPath(const std::vector<Vec2>& path) {
        if (path.size() <= 2) return path;

        std::vector<Vec2> smoothed;
        smoothed.push_back(path.front());

        size_t current = 0;
        while (current < path.size() - 1) {
            size_t furthest = current + 1;
            for (size_t i = path.size() - 1; i > current + 1; --i) {
                if (hasLineOfSight(path[current], path[i])) {
                    furthest = i;
                    break;
                }
            }
            smoothed.push_back(path[furthest]);
            current = furthest;
        }

        return smoothed;
    }

    bool hasLineOfSight(Vec2 a, Vec2 b) const {
        // Bresenham-like line check
        int x0 = a.gridX(), y0 = a.gridY();
        int x1 = b.gridX(), y1 = b.gridY();

        int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
        int sx = (x0 < x1) ? 1 : -1;
        int sy = (y0 < y1) ? 1 : -1;
        int err = dx - dy;

        while (true) {
            if (!inBounds(x0, y0) || !walkable_(x0, y0)) return false;
            if (x0 == x1 && y0 == y1) return true;

            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 < dx)  { err += dx; y0 += sy; }
        }
    }

    int countWallsOnLine(Vec2 a, Vec2 b) const {
        int x0 = a.gridX(), y0 = a.gridY();
        int x1 = b.gridX(), y1 = b.gridY();
        int walls = 0;

        int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
        int sx = (x0 < x1) ? 1 : -1;
        int sy = (y0 < y1) ? 1 : -1;
        int err = dx - dy;

        while (true) {
            if (!inBounds(x0, y0) || !walkable_(x0, y0)) ++walls;
            if (x0 == x1 && y0 == y1) break;

            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 < dx)  { err += dx; y0 += sy; }
        }
        return walls;
    }

private:
    bool inBounds(int x, int y) const {
        return x >= 0 && x < width_ && y >= 0 && y < height_;
    }

    static float heuristic(int x1, int y1, int x2, int y2) {
        // Octile distance (better for 8-directional movement)
        float dx = std::abs(static_cast<float>(x1 - x2));
        float dy = std::abs(static_cast<float>(y1 - y2));
        return std::max(dx, dy) + 0.414f * std::min(dx, dy);
    }

    std::vector<Vec2> reconstructPath(
            const std::unordered_map<int, PathNode>& nodes,
            std::function<int(int, int)> key,
            int sx, int sy, int gx, int gy) {
        std::vector<Vec2> path;
        int cx = gx, cy = gy;

        while (!(cx == sx && cy == sy)) {
            path.push_back(Vec2(static_cast<float>(cx), static_cast<float>(cy)));
            auto it = nodes.find(key(cx, cy));
            if (it == nodes.end()) break;
            int px = it->second.parentX;
            int py = it->second.parentY;
            cx = px; cy = py;
        }
        path.push_back(Vec2(static_cast<float>(sx), static_cast<float>(sy)));
        std::reverse(path.begin(), path.end());
        return path;
    }

    int width_, height_;
    WalkableCheck walkable_;
    CostCheck cost_;
};

} // namespace npc
