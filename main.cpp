#include "raylib.h"
#include <vector>
#include <queue>
#include <cmath>
#include <chrono>
#include <string>
#include <algorithm>

const int GRID_W = 70;
const int GRID_H = 50;
const int CELL_PX = 12; // 70*12 = 840px map width, 50*12 = 600px map height

struct Point {
    int x, y;
    bool operator==(const Point& o) const { return x == o.x && y == o.y; }
};

struct Airport {
    std::string code;
    Point pt;
};

struct Flight {
    std::string callsign;
    Point pos;
    int originIdx;
    int destIdx;
    int activeGoalIdx;
    bool diverted;
    std::vector<Point> path;
};

// 10 Major US Hubs mapped to the 70x50 Continental US Grid
const std::vector<Airport> AIRPORTS = {
    {"SEA", {10,  7}}, {"SFO", { 7, 23}}, {"LAX", {12, 31}},
    {"DEN", {27, 23}}, {"DFW", {36, 35}}, {"AUS", {35, 40}},
    {"ORD", {46, 18}}, {"ATL", {52, 33}}, {"JFK", {62, 16}},
    {"MIA", {58, 45}}
};

// Continental US Perimeter Polygon (Grid Coordinates)
const std::vector<Point> US_BORDER = {
    {8, 5}, {20, 5}, {36, 5}, {43, 8}, {49, 13}, {56, 12}, {63, 7},
    {66, 11}, {62, 16}, {60, 21}, {61, 26}, {57, 32}, {55, 37},
    {59, 45}, {56, 46}, {52, 39}, {45, 39}, {41, 41}, {36, 46},
    {31, 40}, {26, 41}, {21, 36}, {12, 33}, {7, 24}, {6, 14}, {8, 5}
};

int trueGrid[GRID_H][GRID_W];
int perceivedGrid[GRID_H][GRID_W];

void ClearGrid() {
    for (int y = 0; y < GRID_H; y++)
        for (int x = 0; x < GRID_W; x++)
            trueGrid[y][x] = 1;
}

void LoadScenario(int id, std::vector<Flight>& flights) {
    ClearGrid();
    flights = {
        {"LJ60",  AIRPORTS[5].pt, 5, 9, 9, false, {}}, // AUS -> MIA (Nov 1 Route)
        {"AAL41", AIRPORTS[4].pt, 4, 8, 8, false, {}}, // DFW -> JFK
        {"UAL88", AIRPORTS[6].pt, 6, 2, 2, false, {}}, // ORD -> LAX
        {"DAL19", AIRPORTS[7].pt, 7, 3, 3, false, {}}  // ATL -> DEN
    };

    if (id == 1) {
        // Scenario 1: Gulf & Midwest Supercells
        for (int y = 26; y < 44; y++)
            for (int x = 44; x < 49; x++)
                trueGrid[y][x] = (x >= 45 && x <= 47) ? 999 : 50;

        for (int y = 14; y < 28; y++)
            for (int x = 32; x < 36; x++)
                trueGrid[y][x] = (x == 34) ? 999 : 10;
    } 
    else if (id == 2) {
        // Scenario 2: Cockpit Attenuation Trap + Total MIA Airport Closure (Forces Diversion!)
        for (int y = 35; y <= 46; y++)
            for (int x = 42; x <= 44; x++)
                trueGrid[y][x] = 10; // Yellow rain band ahead of LJ60
        for (int y = 34; y <= 47; y++)
            for (int x = 47; x <= 51; x++)
                trueGrid[y][x] = 999; // Hidden Red Supercell behind yellow rain

        // Completely wall off MIA (Airport 9 at 58,45) so LJ60 MUST divert to ATL or DFW!
        for (int y = 41; y <= 48; y++)
            for (int x = 54; x <= 62; x++)
                if (std::hypot(x - 58, y - 45) >= 2.0) trueGrid[y][x] = 999;
    }
}

void UpdatePerceivedGrid(Point plane, bool cockpitMode) {
    for (int y = 0; y < GRID_H; y++)
        for (int x = 0; x < GRID_W; x++)
            perceivedGrid[y][x] = trueGrid[y][x];

    if (!cockpitMode) return;

    for (int ty = 0; ty < GRID_H; ty++) {
        for (int tx = 0; tx < GRID_W; tx++) {
            int dx = tx - plane.x, dy = ty - plane.y;
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < 1.0f) continue;

            int rainHits = 0, steps = (int)dist;
            for (int s = 1; s < steps; s++) {
                int rx = plane.x + (dx * s) / steps;
                int ry = plane.y + (dy * s) / steps;
                if (trueGrid[ry][rx] >= 10) rainHits++;
            }
            if (rainHits >= 2 || dx < 0) perceivedGrid[ty][tx] = 1;
        }
    }
}

struct Node {
    Point pt;
    double g, f;
    bool operator>(const Node& o) const { return f > o.f; }
};


// Returns empty vector if destination is completely blocked by 999 storm cells
std::vector<Point> RunAStar(Point start, Point goal, bool cockpitMode, int &nodesOut) {
    if (perceivedGrid[goal.y][goal.x] >= 999) return {};

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

            int cellW = perceivedGrid[ny][nx];
            if (cellW >= 999) continue; // Impassable storm core

            double halo = 0.0;
            if (!cockpitMode) {
                for (int hy = -1; hy <= 1; hy++) {
                    for (int hx = -1; hx <= 1; hx++) {
                        int cy = ny + hy, cx = nx + hx;
                        if (cy >= 0 && cy < GRID_H && cx >= 0 && cx < GRID_W)
                            if (perceivedGrid[cy][cx] >= 999) halo += 40.0;
                    }
                }
            }

            double step = (d[0] != 0 && d[1] != 0) ? 1.414 : 1.0;
            double ng = curr.g + step * cellW + halo;
            if (ng < gScore[ny][nx]) {
                gScore[ny][nx] = ng;
                parent[ny][nx] = curr.pt;
                openSet.push({{nx, ny}, ng, ng + std::hypot(goal.x - nx, goal.y - ny)});
            }
        }
    }

    if (!reached) return {}; // No safe route exists!

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

// Routes flight to primary destination; if blocked, diverts to nearest safe airport!
void RouteFlightWithDiversion(Flight &fl, bool cockpitMode, int &nodesOut) {
    std::vector<Point> primary = RunAStar(fl.pos, AIRPORTS[fl.destIdx].pt, cockpitMode, nodesOut);
    if (!primary.empty()) {
        fl.path = primary;
        fl.activeGoalIdx = fl.destIdx;
        fl.diverted = false;
        return;
    }

    // Primary destination is blocked! Rank all other airports by distance from aircraft
    std::vector<std::pair<double, int>> candidates;
    for (int i = 0; i < (int)AIRPORTS.size(); i++) {
        if (i == fl.destIdx) continue;
        double dist = std::hypot(AIRPORTS[i].pt.x - fl.pos.x, AIRPORTS[i].pt.y - fl.pos.y);
        candidates.push_back({dist, i});
    }
    std::sort(candidates.begin(), candidates.end());

    for (auto &cand : candidates) {
        std::vector<Point> altPath = RunAStar(fl.pos, AIRPORTS[cand.second].pt, cockpitMode, nodesOut);
        if (!altPath.empty()) {
            fl.path = altPath;
            fl.activeGoalIdx = cand.second;
            fl.diverted = true;
            return;
        }
    }
    fl.path.clear();
}


int main() {
    InitWindow(1220, 600, "ClearVector - US National Airspace Rerouting & Diversion Engine");
    SetTargetFPS(60);

    std::vector<Flight> flights;
    LoadScenario(1, flights);
    int selectedFlight = 0;
    bool cockpitMode = false;
    bool autoFly = false;
    int frameCount = 0;

    while (!WindowShouldClose()) {
        // Mouse Storm Painting on 70x50 Grid
        int mx = GetMouseX() / CELL_PX, my = GetMouseY() / CELL_PX;
        if (mx >= 0 && mx < GRID_W && my >= 0 && my < GRID_H) {
            if (IsMouseButtonDown(MOUSE_LEFT_BUTTON))  trueGrid[my][mx] = 999;
            if (IsMouseButtonDown(MOUSE_RIGHT_BUTTON)) trueGrid[my][mx] = 10;
        }

        // Keyboard Controls
        if (IsKeyPressed(KEY_ONE))   LoadScenario(1, flights);
        if (IsKeyPressed(KEY_TWO))   LoadScenario(2, flights);
        if (IsKeyPressed(KEY_SPACE)) cockpitMode = !cockpitMode;
        if (IsKeyPressed(KEY_P))     autoFly = !autoFly;
        if (IsKeyPressed(KEY_C))     ClearGrid();
        if (IsKeyPressed(KEY_TAB))   selectedFlight = (selectedFlight + 1) % flights.size();

        // [B] Instantly ring the selected flight's destination in Red Storm to demo Diversion!
        if (IsKeyPressed(KEY_B)) {
            Point d = AIRPORTS[flights[selectedFlight].destIdx].pt;
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++)
                    if (std::abs(dx) == 2 || std::abs(dy) == 2)
                        if (d.y+dy >= 0 && d.y+dy < GRID_H && d.x+dx >= 0 && d.x+dx < GRID_W)
                            trueGrid[d.y+dy][d.x+dx] = 999;
        }

        // [N] Spawn a new random flight between major US hubs
        if (IsKeyPressed(KEY_N) && flights.size() < 10) {
            int o = GetRandomValue(0, AIRPORTS.size() - 1);
            int d = (o + GetRandomValue(1, AIRPORTS.size() - 1)) % AIRPORTS.size();
            std::string cs = "SWA" + std::to_string(GetRandomValue(10, 99));
            flights.push_back({cs, AIRPORTS[o].pt, o, d, d, false, {}});
            selectedFlight = flights.size() - 1;
        }

        // Compute A* & Alternate Diversions for all active flights
        auto t0 = std::chrono::high_resolution_clock::now();
        int totalNodes = 0;
        for (size_t i = 0; i < flights.size(); i++) {
            UpdatePerceivedGrid(flights[i].pos, (i == (size_t)selectedFlight) ? cockpitMode : false);
            RouteFlightWithDiversion(flights[i], (i == (size_t)selectedFlight) ? cockpitMode : false, totalNodes);
            if (autoFly && flights[i].path.size() > 1 && frameCount % 15 == 0) {
                flights[i].pos = flights[i].path[1];
            }
        }
        frameCount++;
        auto t1 = std::chrono::high_resolution_clock::now();
        double calcMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // Re-sync perceived grid for the currently viewed aircraft
        UpdatePerceivedGrid(flights[selectedFlight].pos, cockpitMode);

        // --- RENDER ---
        BeginDrawing();
        ClearBackground({8, 14, 26, 255});

        // 1. Draw Grid & Weather Cells
        for (int y = 0; y < GRID_H; y++) {
            for (int x = 0; x < GRID_W; x++) {
                int w = perceivedGrid[y][x];
                Color c = {15, 25, 44, 255};
                if (w == 10)  c = YELLOW;
                if (w == 50)  c = ORANGE;
                if (w >= 999) c = RED;

                if (cockpitMode && trueGrid[y][x] >= 999 && w == 1)
                    DrawRectangleLines(x * CELL_PX, y * CELL_PX, CELL_PX - 1, CELL_PX - 1, Fade(RED, 0.35f));
                else
                    DrawRectangle(x * CELL_PX, y * CELL_PX, CELL_PX - 1, CELL_PX - 1, c);
            }
        }

        // 2. Draw Continental US Vector Map Outline
        for (size_t i = 0; i + 1 < US_BORDER.size(); i++) {
            Vector2 p1 = {US_BORDER[i].x * CELL_PX + 6.0f, US_BORDER[i].y * CELL_PX + 6.0f};
            Vector2 p2 = {US_BORDER[i+1].x * CELL_PX + 6.0f, US_BORDER[i+1].y * CELL_PX + 6.0f};
            DrawLineEx(p1, p2, 2.5f, {60, 110, 150, 200});
        }

        // 3. Draw All US Airports
        for (const auto &ap : AIRPORTS) {
            Vector2 p = {ap.pt.x * CELL_PX + 6.0f, ap.pt.y * CELL_PX + 6.0f};
            DrawCircleV(p, 5.0f, WHITE);
            DrawText(ap.code.c_str(), (int)p.x - 12, (int)p.y - 16, 12, LIGHTGRAY);
        }

        // 4. Draw All Active Flights & Trajectories
        for (size_t i = 0; i < flights.size(); i++) {
            const auto &fl = flights[i];
            bool isSel = (i == (size_t)selectedFlight);
            Vector2 posPx = {fl.pos.x * CELL_PX + 6.0f, fl.pos.y * CELL_PX + 6.0f};
            Vector2 origDestPx = {AIRPORTS[fl.destIdx].pt.x * CELL_PX + 6.0f, AIRPORTS[fl.destIdx].pt.y * CELL_PX + 6.0f};

            // Faint red line to original intended destination
            DrawLineEx(posPx, origDestPx, 1.5f, Fade(RED, isSel ? 0.5f : 0.2f));

            // Active A* Route (Green = Normal, Magenta = Emergency Diversion, Orange = Blindspot)
            Color pathCol = fl.diverted ? MAGENTA : (cockpitMode && isSel ? ORANGE : GREEN);
            if (!isSel && !fl.diverted) pathCol = Fade(GREEN, 0.55f);

            for (size_t j = 0; j + 1 < fl.path.size(); j++) {
                Vector2 p1 = {fl.path[j].x * CELL_PX + 6.0f, fl.path[j].y * CELL_PX + 6.0f};
                Vector2 p2 = {fl.path[j+1].x * CELL_PX + 6.0f, fl.path[j+1].y * CELL_PX + 6.0f};
                DrawLineEx(p1, p2, isSel ? 3.5f : 2.0f, pathCol);
            }

            DrawCircleV(posPx, isSel ? 7.5f : 5.5f, isSel ? SKYBLUE : LIGHTGRAY);
            DrawText(fl.callsign.c_str(), (int)posPx.x - 14, (int)posPx.y + 9, 12, isSel ? SKYBLUE : GRAY);
        }

        // 5. Right Telemetry Sidebar (x = 840 to 1220)
        DrawRectangle(840, 0, 380, 600, {22, 26, 34, 255});
        DrawText("CLEARVECTOR // US NAS", 860, 20, 20, WHITE);
        DrawText(TextFormat("TOTAL A* LATENCY (%d FLIGHTS): %.2f ms", (int)flights.size(), calcMs), 860, 50, 14, SKYBLUE);

        const Flight &sel = flights[selectedFlight];
        DrawRectangle(860, 80, 340, 95, {32, 38, 50, 255});
        DrawText(TextFormat("SELECTED: %s (%s -> %s)", sel.callsign.c_str(),
                 AIRPORTS[sel.originIdx].code.c_str(), AIRPORTS[sel.destIdx].code.c_str()), 872, 90, 16, WHITE);

        if (sel.diverted) {
            DrawText(TextFormat("STATUS: DIVERTED TO %s!", AIRPORTS[sel.activeGoalIdx].code.c_str()), 872, 118, 16, MAGENTA);
            DrawText("REASON: DESTINATION BLOCKED BY 55dBZ", 872, 142, 13, YELLOW);
        } else if (sel.path.empty()) {
            DrawText("STATUS: TRAPPED - NO SAFE ALTERNATE", 872, 118, 15, RED);
        } else {
            DrawText(cockpitMode ? "SENSOR: COCKPIT X-BAND (BLINDSPOT)" : "STATUS: EN ROUTE (NEXRAD SAFE HALO)",
                     872, 118, 14, cockpitMode ? ORANGE : GREEN);
            DrawText(TextFormat("REMAINING WAYPOINTS: %d", (int)sel.path.size()), 872, 142, 14, LIGHTGRAY);
        }

        // Active Flight Roster Table
        DrawText("ACTIVE NATIONAL AIRSPACE ROSTER:", 860, 195, 14, LIGHTGRAY);
        for (size_t i = 0; i < flights.size(); i++) {
            const auto &f = flights[i];
            Color rc = (i == (size_t)selectedFlight) ? SKYBLUE : GRAY;
            std::string status = f.diverted ? ("DIVERT -> " + AIRPORTS[f.activeGoalIdx].code)
                                            : ("TO " + AIRPORTS[f.destIdx].code);
            DrawText(TextFormat("%s %s : %s", (i == (size_t)selectedFlight ? ">" : " "),
                     f.callsign.c_str(), status.c_str()), 865, 220 + i * 20, 14, f.diverted ? MAGENTA : rc);
        }

        // Controls Legend
        DrawText("HOTKEYS:", 860, 410, 15, WHITE);
        DrawText("[B]     Block Dest & Force Alternate Diversion!", 860, 432, 13, MAGENTA);
        DrawText("[N]     Spawn New Multi-Hub Flight", 860, 452, 13, GREEN);
        DrawText("[TAB]   Cycle Selected Aircraft", 860, 472, 13, SKYBLUE);
        DrawText("[SPACE] Toggle Cockpit Blindspot Mode", 860, 492, 13, YELLOW);
        DrawText("[P]     Play / Pause All Flights", 860, 512, 13, LIGHTGRAY);
        DrawText("[1]/[2] Load Preset Weather Scenarios", 860, 532, 13, LIGHTGRAY);
        DrawText("[C]     Clear Weather | [MOUSE] Paint Storms", 860, 552, 13, LIGHTGRAY);

        EndDrawing();
    }
    CloseWindow();
    return 0;
}
