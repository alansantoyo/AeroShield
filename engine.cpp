#include <iostream>
#include <vector>
#include <queue>
#include <cmath>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <string>
#include <iomanip>
#include <sstream>

const int GRID_W = 120; // High-res 120x80 grid across Continental US
const int GRID_H = 80;
const double MIN_LAT = 24.0, MAX_LAT = 50.0;
const double MIN_LON = -126.0, MAX_LON = -66.0;

struct Point {
    int x, y;
    bool operator==(const Point& o) const { return x == o.x && y == o.y; }
};

struct Airport {
    std::string code;
    double lat, lon;
    Point pt;
};

Point LatLonToGrid(double lat, double lon) {
    int x = (int)std::round((lon - MIN_LON) / (MAX_LON - MIN_LON) * (GRID_W - 1));
    int y = (int)std::round((MAX_LAT - lat) / (MAX_LAT - MIN_LAT) * (GRID_H - 1));
    return {std::clamp(x, 0, GRID_W - 1), std::clamp(y, 0, GRID_H - 1)};
}

void GridToLatLon(Point p, double &lat, double &lon) {
    lon = MIN_LON + ((double)p.x / (GRID_W - 1)) * (MAX_LON - MIN_LON);
    lat = MAX_LAT - ((double)p.y / (GRID_H - 1)) * (MAX_LAT - MIN_LAT);
}

int grid[GRID_H][GRID_W];
int perceived[GRID_H][GRID_W];
int airwayTraffic[GRID_H][GRID_W];

struct Node {
    Point pt;
    double g, f;
    bool operator>(const Node& o) const { return f > o.f; }
};

std::vector<Point> RunAStar(Point start, Point goal, bool cockpitMode, int &nodesOut) {
    if (perceived[goal.y][goal.x] >= 999) return {};

    std::vector<std::vector<double>> gScore(GRID_H, std::vector<double>(GRID_W, 1e9));
    std::vector<std::vector<Point>> parent(GRID_H, std::vector<Point>(GRID_W, {-1, -1}));
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> openSet;

    gScore[start.y][start.x] = 0.0;
    openSet.push({start, 0.0, std::hypot(goal.x - start.x, goal.y - start.y)});
    const int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};

    bool reached = false;
    while (!openSet.empty()) {
        Node curr = openSet.top();
        openSet.pop();
        nodesOut++;

        if (curr.pt == goal) { reached = true; break; }
        if (curr.g > gScore[curr.pt.y][curr.pt.x]) continue;

        for (auto& d : dirs) {
            int nx = curr.pt.x + d[0], ny = curr.pt.y + d[1];
            if (nx < 0 || nx >= GRID_W || ny < 0 || ny >= GRID_H) continue;
            double w = perceived[ny][nx];
            if (w >= 999.0 && !(nx == goal.x && ny == goal.y && std::abs(curr.pt.x - goal.x) <= 1 && std::abs(curr.pt.y - goal.y) <= 1)) continue;
            if (w >= 999.0) w = 50.0;

            double halo = 0.0;
            if (!cockpitMode) {
                for (int hy = -2; hy <= 2; hy++)
                    for (int hx = -2; hx <= 2; hx++) {
                        int cy = ny + hy, cx = nx + hx;
                        if (cy >= 0 && cy < GRID_H && cx >= 0 && cx < GRID_W)
                            if (perceived[cy][cx] >= 999) halo += 45.0;
                    }
            }

            double step = (d[0] != 0 && d[1] != 0) ? 1.414 : 1.0;
            double ng = curr.g + step * w + halo + airwayTraffic[ny][nx] * 6.0;
            if (ng < gScore[ny][nx]) {
                gScore[ny][nx] = ng;
                parent[ny][nx] = curr.pt;
                openSet.push({{nx, ny}, ng, ng + std::hypot(goal.x - nx, goal.y - ny)});
            }
        }
    }
    if (!reached) return {};

    std::vector<Point> path;
    Point c = goal;
    while (!(c == Point{-1, -1})) {
        path.push_back(c);
        if (c == start) break;
        c = parent[c.y][c.x];
    }
    std::reverse(path.begin(), path.end());
    return path;
}

int main(int argc, char* argv[]) {
    bool cockpitMode = (argc > 1 && std::string(argv[1]) == "1");

    std::vector<Airport> airports = {
        {"SEA", 47.45, -122.30, {}}, {"SFO", 37.62, -122.37, {}},
        {"LAX", 33.94, -118.40, {}}, {"DEN", 39.85, -104.67, {}},
        {"DFW", 32.89,  -97.04, {}}, {"AUS", 30.19,  -97.66, {}},
        {"ORD", 41.97,  -87.90, {}}, {"ATL", 33.64,  -84.42, {}},
        {"JFK", 40.64,  -73.77, {}}, {"MIA", 25.79,  -80.29, {}}
    };
    for (auto &a : airports) a.pt = LatLonToGrid(a.lat, a.lon);

    for (int y = 0; y < GRID_H; y++)
        for (int x = 0; x < GRID_W; x++) {
            grid[y][x] = 1;
	    airwayTraffic[y][x] = 0;
        }

    // Read dynamic storms painted in the browser: lat lon type (1=Rain, 2=Sleet/Snow, 3=Thunderstorm)
    std::ifstream sf("storms.txt");
    double slat, slon; int stype;
    while (sf >> slat >> slon >> stype) {
        Point c = LatLonToGrid(slat, slon);
        int maxR = (stype == 3) ? 6 : (stype == 2 ? 5 : 3);
        for (int dy = -maxR; dy <= maxR; dy++) {
            for (int dx = -maxR; dx <= maxR; dx++) {
                double dist = std::hypot(dx, dy);
                if (dist > maxR) continue;
                int ny = c.y + dy, nx = c.x + dx;
                if (ny < 0 || ny >= GRID_H || nx < 0 || nx >= GRID_W) continue;

		int w = 1;
                if (stype == 3) {        // Thunderstorm: Red core (999) -> Yellow (120) -> Green (40)
                    w = (dist <= 2.5) ? 999 : (dist <= 4.5 ? 120 : 40);
                } else if (stype == 2) { // Sleet/Snow: Dark Purple core (999) -> Light Pink (90)
                    w = (dist <= 2.5) ? 999 : 90;
                } else {                 // Rain: Inner core (999) -> Outer Green ring (50)
                    w = (dist <= 1.8) ? 999 : 50;
                }
                grid[ny][nx] = std::max(grid[ny][nx], w);
            
	    }
        }
    }

    struct FlightReq { std::string id; double lat, lon; int orig, dest, style, fuel; };
    std::vector<FlightReq> flights;
    std::ifstream ff("flights.txt");
    std::string line;
    while (std::getline(ff, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        FlightReq fq{"", 0, 0, 0, 0, 1, 100}; // Defaults to style=1, fuel=100 if missing!
        if (iss >> fq.id >> fq.lat >> fq.lon >> fq.orig >> fq.dest) {
            iss >> fq.style >> fq.fuel;
            flights.push_back(fq);
        }
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    int totalNodes = 0;

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "{\n  \"flights\": [\n";
    for (size_t i = 0; i < flights.size(); i++) {
        Point start = LatLonToGrid(flights[i].lat, flights[i].lon);

        // Apply Cockpit X-Band Attenuation if enabled for Flight 0 (LJ60)
        for (int y = 0; y < GRID_H; y++)
            for (int x = 0; x < GRID_W; x++)
                perceived[y][x] = grid[y][x];

        if (cockpitMode && i == 0) {
            for (int ty = 0; ty < GRID_H; ty++) {
                for (int tx = 0; tx < GRID_W; tx++) {
                    int dx = tx - start.x, dy = ty - start.y;
                    float dist = std::hypot(dx, dy);
                    if (dist < 1.0f) continue;
                    int rainHits = 0, steps = (int)dist;
                    for (int s = 1; s < steps; s++) {
                        int rx = start.x + (dx * s) / steps;
                        int ry = start.y + (dy * s) / steps;
                        if (grid[ry][rx] >= 10) rainHits++;
                    }
                    if (rainHits >= 2) perceived[ty][tx] = 1; // Blinded behind light rain!
                }
            }
        }

        int activeGoal = flights[i].dest;
        bool diverted = false;
        std::string divertReason = "NONE";
        std::vector<Point> path = RunAStar(start, airports[activeGoal].pt, (cockpitMode && i == 0), totalNodes);

        int reqSteps = path.empty() ? 999999 : ((int)path.size() - 1);

        // Divert if destination is blocked by weather OR detour exceeds remaining fuel!
        if (path.empty() || reqSteps > flights[i].fuel) {
            divertReason = path.empty() ? "WEATHER BLOCKADE" : "LOW FUEL (DETOUR EXCEEDS RANGE)";
            std::vector<std::pair<double, int>> cands;
            for (int a = 0; a < (int)airports.size(); a++) {
                if (a == flights[i].dest) continue;
                if (a == flights[i].orig && start == airports[flights[i].orig].pt) continue;
                // Do not divert to an airport that is itself buried under a 999 storm core!
                if (grid[airports[a].pt.y][airports[a].pt.x] >= 999.0) continue;
                cands.push_back({std::hypot(airports[a].pt.x - start.x, airports[a].pt.y - start.y), a});
            }
            std::sort(cands.begin(), cands.end());

            bool foundWithinFuel = false;
            // 1st pass: Find nearest reachable alternate airport within remaining fuel
            for (auto &c : cands) {
                auto alt = RunAStar(start, airports[c.second].pt, (cockpitMode && i == 0), totalNodes);
                if (!alt.empty() && ((int)alt.size() - 1) <= flights[i].fuel) {
                    path = alt;
                    activeGoal = c.second;
                    diverted = true;
                    foundWithinFuel = true;
                    break;
                }
            }
            // 2nd pass: Critical emergency — pick absolute shortest path to any open airport
            if (!foundWithinFuel) {
                for (auto &c : cands) {
                    auto alt = RunAStar(start, airports[c.second].pt, (cockpitMode && i == 0), totalNodes);
                    if (!alt.empty()) {
                        path = alt;
                        activeGoal = c.second;
                        diverted = true;
                        divertReason = "MAYDAY FUEL CRITICAL";
                        break;
                    }
                }
            }
        }

        bool atGoal = (start == airports[activeGoal].pt);
        bool fuelExhausted = (!atGoal && flights[i].fuel <= 0);
        if (fuelExhausted) {
            divertReason = "FUEL EXHAUSTED - DITCHED / BLACKBOX ACTIVE";
        }

        std::cout << "    {\"callsign\":\"" << flights[i].id << "\", \"orig\":\"" << airports[flights[i].orig].code
                  << "\", \"dest\":\"" << airports[flights[i].dest].code << "\", \"activeGoal\":\"" << airports[activeGoal].code
                  << "\", \"shipStyle\":" << flights[i].style << ", \"fuel\":" << flights[i].fuel
                  << ", \"lat\":" << flights[i].lat << ", \"lon\":" << flights[i].lon
                  << ", \"gridX\":" << start.x << ", \"gridY\":" << start.y
                  << ", \"fuelExhausted\":" << (fuelExhausted ? "true" : "false")
                  << ", \"divertReason\":\"" << divertReason << "\""
                  << ", \"diverted\":" << (diverted ? "true" : "false") << ", \"path\":[";

 
        // Register corridor usage so subsequent flights prefer parallel cells
        for (size_t p = 1; p + 1 < path.size(); p++) {
            airwayTraffic[path[p].y][path[p].x]++;
        }

        // Count how many prior flights share this exact origin->dest pair for visual lateral spacing
        for (size_t p = 0; p < path.size(); p++) {
            double plat, plon;
            if (p == 0 && path.size() > 1) {
                plat = flights[i].lat; plon = flights[i].lon;
            } else if (p + 1 == path.size()) {
                plat = airports[activeGoal].lat; plon = airports[activeGoal].lon;
            } else {
                GridToLatLon(path[p], plat, plon);
            }
            std::cout << "[" << plat << "," << plon << "]" << (p + 1 < path.size() ? "," : "");
        }

        std::cout << "]}" << (i + 1 < flights.size() ? ",\n" : "\n");
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "  ],\n  \"calcMs\": " << ms << ",\n  \"nodes\": " << totalNodes << "\n}\n";
    return 0;
}
