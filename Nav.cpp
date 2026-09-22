#include <Windows.h>
#include <cmath>
#include <cstring>
#include <queue>
#include <string>
#include "Nav.h"
#include "MiniMap.h"
#include "Binds.h"
#include "Log.h"

namespace Nav
{
    bool showRoute = true;
    float arriveMeters = 35.f;

    namespace
    {
        constexpr int kCell = 4;                    // map pixels per grid cell
        int g_w = 0, g_h = 0;
        std::vector<unsigned char> g_road;
        std::string g_status = "not loaded";
        bool g_tried = false;

        bool g_have = false;
        Point g_wp{};
        std::string g_wpName;
        bool g_dirty = false;
        std::vector<Point> g_route;
        float g_routeM = 0.f;
        bool g_onRoads = false;
        float g_lastX = 0, g_lastY = 0;
        ULONGLONG g_lastRoute = 0;

        std::vector<float> g_cost;
        std::vector<int> g_parent;
        std::vector<unsigned> g_stamp;
        unsigned g_gen = 0;

        // world (cm) -> grid cell (float) and back
        void ToCell(float x, float y, float& cx, float& cy)
        {
            cx = (y - MiniMap::mapOriginY) / MiniMap::mapCmPerPx / kCell;
            cy = (MiniMap::mapOriginX - x) / MiniMap::mapCmPerPx / kCell;
        }
        Point FromCell(float cx, float cy)
        {
            return { MiniMap::mapOriginX - (cy + 0.5f) * kCell * MiniMap::mapCmPerPx, MiniMap::mapOriginY + (cx + 0.5f) * kCell * MiniMap::mapCmPerPx };
        }
        bool IsRoad(int x, int y) { return x >= 0 && y >= 0 && x < g_w && y < g_h && g_road[static_cast<size_t>(y) * g_w + x]; }

        // nearest road cell to a (float) cell position, searched in growing squares
        bool Snap(float fx, float fy, int& ox, int& oy)
        {
            const int cx = static_cast<int>(std::floor(fx)), cy = static_cast<int>(std::floor(fy));
            float best = 1e30f; bool found = false;
            for (int r = 0; r <= 220; ++r)
            {
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx)
                    {
                        if (std::abs(dx) != r && std::abs(dy) != r) continue;
                        const int x = cx + dx, y = cy + dy;
                        if (!IsRoad(x, y)) continue;
                        const float d = (x + 0.5f - fx) * (x + 0.5f - fx) + (y + 0.5f - fy) * (y + 0.5f - fy);
                        if (d < best) { best = d; ox = x; oy = y; found = true; }
                    }
                if (found && r > std::sqrt(best) + 1.5f) break;
            }
            return found;
        }

        bool Search(int sx, int sy, int tx, int ty, std::vector<int>& path)
        {
            const size_t n = static_cast<size_t>(g_w) * g_h;
            if (g_cost.size() != n) { g_cost.assign(n, 0.f); g_parent.assign(n, -1); g_stamp.assign(n, 0); g_gen = 0; }
            ++g_gen;
            struct Node { float f; int idx; bool operator<(const Node& o) const { return f > o.f; } };
            std::priority_queue<Node> open;
            auto H = [&](int x, int y)
            {
                const float dx = static_cast<float>(std::abs(x - tx)), dy = static_cast<float>(std::abs(y - ty));
                return (dx + dy) + (1.41421356f - 2.f) * (dx < dy ? dx : dy);
            };
            const int s = sy * g_w + sx, t = ty * g_w + tx;
            g_cost[s] = 0; g_parent[s] = -1; g_stamp[s] = g_gen;
            open.push({ H(sx, sy), s });
            static const int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
            while (!open.empty())
            {
                const Node cur = open.top(); open.pop();
                if (cur.idx == t) break;
                const int cx = cur.idx % g_w, cy = cur.idx / g_w;
                const float cg = g_cost[cur.idx];
                if (cur.f > cg + H(cx, cy) + 1e-3f) continue;   // stale entry
                for (int k = 0; k < 8; ++k)
                {
                    const int nx = cx + DX[k], ny = cy + DY[k];
                    if (!IsRoad(nx, ny)) continue;
                    if (k >= 4 && (!IsRoad(cx + DX[k], cy) || !IsRoad(cx, cy + DY[k]))) continue;   // no corner cutting through gaps
                    const int ni = ny * g_w + nx;
                    const float ng = cg + (k < 4 ? 1.f : 1.41421356f);
                    if (g_stamp[ni] == g_gen && g_cost[ni] <= ng) continue;
                    g_stamp[ni] = g_gen; g_cost[ni] = ng; g_parent[ni] = cur.idx;
                    open.push({ ng + H(nx, ny), ni });
                }
            }
            if (g_stamp[t] != g_gen) return false;
            path.clear();
            for (int i = t; i != -1; i = g_parent[i]) path.push_back(i);
            return true;
        }

        void Recompute(float px, float py)
        {
            g_route.clear(); g_onRoads = false;
            g_lastX = px; g_lastY = py; g_lastRoute = GetTickCount64(); g_dirty = false;
            const float straight = std::sqrt((g_wp.x - px) * (g_wp.x - px) + (g_wp.y - py) * (g_wp.y - py)) * 0.01f;
            g_routeM = straight;
            if (!Available()) { g_route.push_back({ px, py }); g_route.push_back(g_wp); return; }
            float fsx, fsy, ftx, fty;
            ToCell(px, py, fsx, fsy); ToCell(g_wp.x, g_wp.y, ftx, fty);
            int sx, sy, tx, ty;
            std::vector<int> path;
            if (Snap(fsx, fsy, sx, sy) && Snap(ftx, fty, tx, ty) && Search(sx, sy, tx, ty, path))
            {
                g_route.push_back({ px, py });
                const Point a = FromCell(static_cast<float>(sx), static_cast<float>(sy));
                g_route.push_back(a);
                for (int i = static_cast<int>(path.size()) - 1; i >= 0; i -= 2)
                    g_route.push_back(FromCell(static_cast<float>(path[i] % g_w), static_cast<float>(path[i] / g_w)));
                g_route.push_back(FromCell(static_cast<float>(tx), static_cast<float>(ty)));
                g_route.push_back(g_wp);
                g_onRoads = true;
                float m = 0.f;
                for (size_t i = 1; i < g_route.size(); ++i)
                    m += std::sqrt((g_route[i].x - g_route[i - 1].x) * (g_route[i].x - g_route[i - 1].x) + (g_route[i].y - g_route[i - 1].y) * (g_route[i].y - g_route[i - 1].y));
                g_routeM = m * 0.01f;
            }
            else { g_route.push_back({ px, py }); g_route.push_back(g_wp); }
        }
    }

    bool Load()
    {
        if (g_tried) return !g_road.empty();
        g_tried = true;
        const std::string path = std::string(Binds::DataDir()) + "MapData\\roads.bin";
        HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) { g_status = "MapData\\roads.bin not found next to the DLL"; return false; }
        const DWORD size = GetFileSize(f, nullptr);
        std::string d(size, '\0'); DWORD got = 0;
        ReadFile(f, &d[0], size, &got, nullptr);
        CloseHandle(f);
        int w = 0, h = 0;
        if (got != size || size < 12 || memcmp(d.data(), "ROAD", 4) != 0) { g_status = "roads.bin has a wrong header"; return false; }
        memcpy(&w, d.data() + 4, 4); memcpy(&h, d.data() + 8, 4);
        if (w <= 0 || h <= 0 || static_cast<size_t>(w) * h + 12 != size) { g_status = "roads.bin has a wrong size"; return false; }
        g_w = w; g_h = h;
        g_road.assign(reinterpret_cast<const unsigned char*>(d.data()) + 12, reinterpret_cast<const unsigned char*>(d.data()) + size);
        g_status = "road network loaded";
        LogF("Nav: road grid %dx%d loaded", g_w, g_h);
        return true;
    }
    bool Available() { return !g_road.empty() || Load(); }
    const char* Status() { return g_status.c_str(); }

    void SetWaypoint(float x, float y, const char* name)
    {
        g_wp = { x, y }; g_have = true; g_dirty = true; g_wpName = name ? name : "";
    }
    void Clear() { g_have = false; g_route.clear(); g_routeM = 0.f; g_onRoads = false; }
    bool HasWaypoint() { return g_have; }
    Point Waypoint() { return g_wp; }
    const char* WaypointName() { return g_wpName.c_str(); }
    const std::vector<Point>& Route() { return g_route; }
    float RouteMeters() { return g_routeM; }
    bool OnRoads() { return g_onRoads; }

    void Update(float px, float py)
    {
        if (!g_have) return;
        const float straight = std::sqrt((g_wp.x - px) * (g_wp.x - px) + (g_wp.y - py) * (g_wp.y - py)) * 0.01f;
        if (straight < arriveMeters) { Clear(); return; }
        const ULONGLONG now = GetTickCount64();
        const float moved = std::sqrt((px - g_lastX) * (px - g_lastX) + (py - g_lastY) * (py - g_lastY)) * 0.01f;
        if (g_dirty || (now - g_lastRoute > 1200 && moved > 20.f) || now - g_lastRoute > 6000) Recompute(px, py);
        else if (!g_route.empty()) g_route[0] = { px, py };   // the start follows the player between re-routes
    }
}
