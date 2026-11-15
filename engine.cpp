#include <iostream>
#include <vector>
#include <queue>
#include <cmath>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <string>
#include <iomanip>

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

            int w = perceived[ny][nx];
            if (w >= 999) continue;

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
            double ng = curr.g + step * w + halo;
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
        for (int x = 0; x < GRID_W; x++)
            grid[y][x] = 1;

    // Read dynamic storms painted in the browser (storms.txt)
    std::ifstream sf("storms.txt");
    double slat, slon, srad; int sw;
    while (sf >> slat >> slon >> srad >> sw) {
        Point c = LatLonToGrid(slat, slon);
        int r = std::max(1, (int)srad);
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
                if (dx*dx + dy*dy <= r*r) {
                    int ny = c.y + dy, nx = c.x + dx;
                    if (ny >= 0 && ny < GRID_H && nx >= 0 && nx < GRID_W)
                        grid[ny][nx] = std::max(grid[ny][nx], sw);
                }
    }

    struct FlightReq { std::string id; double lat, lon; int orig, dest; };
    std::vector<FlightReq> flights;
    std::ifstream ff("flights.txt");
    FlightReq fq;
    while (ff >> fq.id >> fq.lat >> fq.lon >> fq.orig >> fq.dest) flights.push_back(fq);

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
        std::vector<Point> path = RunAStar(start, airports[activeGoal].pt, (cockpitMode && i == 0), totalNodes);

        // If destination is walled off, divert to nearest reachable US airport!
        if (path.empty()) {
            std::vector<std::pair<double, int>> cands;
            for (int a = 0; a < (int)airports.size(); a++) {
                if (a == flights[i].dest) continue;
                cands.push_back({std::hypot(airports[a].pt.x - start.x, airports[a].pt.y - start.y), a});
            }
            std::sort(cands.begin(), cands.end());
            for (auto &c : cands) {
                auto alt = RunAStar(start, airports[c.second].pt, (cockpitMode && i == 0), totalNodes);
                if (!alt.empty()) {
                    path = alt;
                    activeGoal = c.second;
                    diverted = true;
                    break;
                }
            }
        }

        std::cout << "    {\"callsign\":\"" << flights[i].id << "\", \"orig\":\"" << airports[flights[i].orig].code
                  << "\", \"dest\":\"" << airports[flights[i].dest].code << "\", \"activeGoal\":\"" << airports[activeGoal].code
                  << "\", \"diverted\":" << (diverted ? "true" : "false") << ", \"path\":[";
        
	for (size_t p = 0; p < path.size(); p++) {
            double plat, plon;
            if (p == 0 && path.size() > 1) {
                plat = flights[i].lat; plon = flights[i].lon; // Exact current position
            } else if (p + 1 == path.size()) {
                plat = airports[activeGoal].lat; plon = airports[activeGoal].lon; // Exact airport pin!
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
