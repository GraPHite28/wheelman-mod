#pragma once
#include <vector>

// GPS: a route over the roads of the city from the player to a waypoint.
// The road network is taken from the city map picture (MapData\roads.bin: a 1536x1024 grid, one cell = 4x4 map pixels,
// built offline from the road colour of pda_I140 / pda_I142) and searched with A*. World <-> map pixels use the
// calibration of MiniMap (mapOriginX / mapOriginY / mapCmPerPx). World units are centimetres, X = north, Y = east.
namespace Nav
{
    struct Point { float x, y; };          // world coordinates (cm)

    extern bool showRoute;                 // draw the route on the minimap / big map
    extern float arriveMeters;             // the waypoint is cleared when the player is this close

    bool Load();                           // roads.bin; true when available (also called lazily)
    const char* Status();
    bool Available();

    void SetWaypoint(float x, float y, const char* name);   // computes the route on the next Update
    void Clear();
    bool HasWaypoint();
    Point Waypoint();
    const char* WaypointName();

    // per frame with the player's position: re-routes when needed, clears on arrival
    void Update(float playerX, float playerY);
    const std::vector<Point>& Route();     // from the player to the waypoint (empty when there is no road path)
    float RouteMeters();                   // length of the route in metres (straight distance when no path)
    bool OnRoads();                        // the route was found on the road grid
}
