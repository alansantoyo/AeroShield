import os, subprocess, json, urllib.request, base64, random, ssl, math
from http.server import SimpleHTTPRequestHandler, HTTPServer, ThreadingHTTPServer

AIRPORTS = [
    ("SEA", 47.45, -122.30), ("SFO", 37.62, -122.37), ("LAX", 33.94, -118.40),
    ("DEN", 39.85, -104.67), ("DFW", 32.89,  -97.04), ("AUS", 30.19,  -97.66),
    ("ORD", 41.97,  -87.90), ("ATL", 33.64,  -84.42), ("JFK", 40.64,  -73.77),
    ("MIA", 25.79,  -80.29)
]

def calc_initial_fuel(orig_idx, dest_idx):
    lat1, lon1 = AIRPORTS[orig_idx][1], AIRPORTS[orig_idx][2]
    lat2, lon2 = AIRPORTS[dest_idx][1], AIRPORTS[dest_idx][2]
    # Convert lat/lon delta to approximate 120x80 grid steps + 22% reserve (+8 steps minimum)
    gx = abs(lon2 - lon1) / 60.0 * 119.0
    gy = abs(lat2 - lat1) / 26.0 * 79.0
    chebyshev_steps = max(gx, gy)
    return int(chebyshev_steps * 1.22) + 8

def deduplicate_and_reset_flights(reset_coords=True):
    if not os.path.exists("flights.txt"): return
    with open("flights.txt", "r") as f:
        lines = [l.strip().split() for l in f if l.strip()]
    seen_cs, clean = set(), []
    for parts in lines:
        if len(parts) < 5: continue
        cs = parts[0].upper()
        if cs in seen_cs: continue
        seen_cs.add(cs)
        orig_idx, dest_idx = int(parts[3]), int(parts[4])
        if reset_coords:
            parts[1], parts[2] = str(AIRPORTS[orig_idx][1]), str(AIRPORTS[orig_idx][2])
        style = parts[5] if (len(parts) >= 6 and not reset_coords) else str(random.randint(1, 3))
        fuel = str(calc_initial_fuel(orig_idx, dest_idx)) if (reset_coords or len(parts) < 7) else parts[6]
        clean.append(f"{cs} {parts[1]} {parts[2]} {orig_idx} {dest_idx} {style} {fuel}\n")
    with open("flights.txt", "w") as f:
        f.writelines(clean)

def reset_scenario(scen=1):
    # If flights.txt doesn't exist or is empty, create initial defaults once
    if not os.path.exists("flights.txt") or os.path.getsize("flights.txt") == 0:
        defaults = [("LJ60", 5, 9), ("AAL41", 4, 8), ("UAL88", 6, 2), ("DAL19", 7, 3)]
        with open("flights.txt", "w") as f:
            for cs, o, d in defaults:
                f.write(f"{cs} {AIRPORTS[o][1]} {AIRPORTS[o][2]} {o} {d} {random.randint(1, 3)}\n")
    else:
        deduplicate_and_reset_flights(reset_coords=True)

    open("storms.txt", "w").close()

reset_scenario(1)

class Handler(SimpleHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0))
        body = json.loads(self.rfile.read(length)) if length else {}

        if self.path == '/compute':
            cockpit = "1" if body.get("cockpitMode") else "0"
            out = subprocess.check_output(["./clearvector_cli", cockpit]).decode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(out.encode())


        elif self.path == '/step_flights':
            cockpit = "1" if body.get("cockpitMode") else "0"
            data = json.loads(subprocess.check_output(["./clearvector_cli", cockpit]).decode())
            with open("flights.txt", "r") as f:
                lines = [l.strip().split() for l in f if l.strip()]
            
            occupied = set() # Tracks (lat, lon) of aircraft ahead for ATC in-trail spacing
            with open("flights.txt", "w") as f:
                for i, parts in enumerate(lines):
                    if len(parts) < 7:
                        parts.append(str(calc_initial_fuel(int(parts[3]), int(parts[4]))))
                    if i < len(data["flights"]) and len(data["flights"][i]["path"]) > 1:
                        nxt = data["flights"][i]["path"][1]
                        dest_pt = data["flights"][i]["path"][-1]
                        nxt_key = (round(nxt[0], 2), round(nxt[1], 2))
                        dest_key = (round(dest_pt[0], 2), round(dest_pt[1], 2))
                        fuel_left = int(parts[6])
                        if fuel_left > 0 and (nxt_key not in occupied or nxt_key == dest_key):
                            parts[1], parts[2] = str(nxt[0]), str(nxt[1])
                            parts[6] = str(fuel_left - 1)
                        # Only mark airspace occupied if aircraft has not landed yet
                        cur_key = (round(float(parts[1]), 2), round(float(parts[2]), 2))
                        if cur_key != dest_key:
                            occupied.add(cur_key)
                    f.write(" ".join(parts[:7]) + "\n")



            self.send_response(200); self.end_headers()

        elif self.path == '/add_flight':
            o, d = str(body['orig']), str(body['dest'])
            cs = body.get('callsign', 'SWA99').strip().upper()
            with open("flights.txt", "r") as f:
                existing = [l.strip().split() for l in f if l.strip()]
            # Reject if callsign or (orig, dest) route already exists
            if not any(p[0].upper() == cs for p in existing):
                lat, lon = AIRPORTS[int(o)][1], AIRPORTS[int(o)][2]
                fuel = calc_initial_fuel(int(o), int(d))
                with open("flights.txt", "a") as f:
                    f.write(f"{cs} {lat} {lon} {o} {d} {random.randint(1, 3)} {fuel}\n")
            self.send_response(200); self.end_headers()


        elif self.path == '/add_storm':
            with open("storms.txt", "a") as f:
                f.write(f"{body['lat']} {body['lon']} {body['type']}\n")
            self.send_response(200); self.end_headers()

        elif self.path == '/get_storms':
            storms = []
            if os.path.exists("storms.txt"):
                with open("storms.txt", "r") as f:
                    for line in f:
                        p = line.strip().split()
                        if len(p) >= 3:
                            storms.append({"lat": float(p[0]), "lon": float(p[1]), "type": int(p[2])})
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps(storms).encode())

        elif self.path == '/reset_flights':
            deduplicate_and_reset_flights(reset_coords=True)
            self.send_response(200); self.end_headers()

        elif self.path == '/clear_weather':
            with open("storms.txt", "w") as f:
                pass
            self.send_response(200); self.end_headers()

        elif self.path == '/ai_briefing':
            summary = body.get("summary", "Flight LJ60 encountering severe squall line.")
            g_key = os.environ.get("GEMINI_API_KEY", "").strip()
            e_key = os.environ.get("ELEVENLABS_API_KEY", "").strip()

            # Default structured Tactical Action Card fallback
            # 1. Check active storm count on map & parse flight state from summary
            storm_count = 0
            if os.path.exists("storms.txt"):
                with open("storms.txt", "r") as sf:
                    storm_count = sum(1 for line in sf if line.strip())

            callsign = body.get("callsign", "Flight")
            is_arrived = "Waypoints Needed: 0u" in summary and "FUEL EXHAUSTED" not in summary
            is_fuel_dry = "FUEL EXHAUSTED" in summary or "Fuel Remaining: 0u" in summary
            is_weather_trapped = ("BLOCKED_BY_STORM" in summary or "WEATHER BLOCKADE" in summary) and not is_fuel_dry
            is_diverted = "Diverted: true" in summary

            if is_arrived:
                tactical_card = {
                    "voice_squawk": f"AEOLUS-ATC to {callsign}: Welcome to safe harbor in Ithaca. Touchdown confirmed at destination gate with nominal fuel reserves.",
                    "risk_score": 1,
                    "fuel_impact_est": "0 lbs (ARRIVED ON TIME)"
                }
            elif is_fuel_dry:
                tactical_card = {
                    "voice_squawk": f"MAYDAY {callsign}: Fuel tanks exhausted in Poseidon's squall. Activating NTSB Black-Box locator beacon at last known grid coordinates.",
                    "risk_score": 10,
                    "fuel_impact_est": "CRITICAL (TANKS DRY)"
                }
            elif is_weather_trapped:
                tactical_card = {
                    "voice_squawk": f"EMERGENCY {callsign}: All approach corridors walled off by Scylla and Charybdis supercells. Enter immediate holding pattern on remaining fuel until weather clears.",
                    "risk_score": 9,
                    "fuel_impact_est": "HOLDING PATTERN (BURNING RESERVE)"
                }
            elif storm_count == 0:
                tactical_card = {
                    "voice_squawk": f"AEOLUS-ATC to {callsign}: Favorable tailwinds from King Aeolus across clear national airspace. Maintain optimal great-circle vector to destination.",
                    "risk_score": 1,
                    "fuel_impact_est": "0 lbs (CLEAR SKIES)"
                }
            else:
                tactical_card = {
                    "voice_squawk": f"AEOLUS-ATC to {callsign}: Navigating around active weather cells with sufficient fuel reserve. Hold current A-Star avoidance vector.",
                    "risk_score": 4,
                    "fuel_impact_est": "+420 lbs (+6 mins)"
                }

            # 3. Query Gemini Flash with thinking disabled (0.7s latency) & state-specific rules
            if g_key and not is_arrived:
                prompt = (
                    f"You are AEOLUS-ATC (inspired by Homer's Odyssey). Active storms on US map: {storm_count}. "
                    f"Aircraft Telemetry: {summary}\n"
                    f"Follow these strict state rules:\n"
                    f"- If Diverted is false and Active storms is 0: give a calm, clear-skies Odyssey blessing (Aeolus favorable winds), risk_score 1 or 2, fuel_impact_est '0 lbs (ON TIME)'. Do NOT tell them to divert.\n"
                    f"- If Diverted is false and Active storms > 0: advise holding the A-Star avoidance corridor around hazards, risk_score 3 to 5, minor fuel impact.\n"
                    f"- If FUEL EXHAUSTED or Fuel Remaining is 0u: issue a Mayday ditching squawk, risk_score 10, fuel_impact_est 'CRITICAL (TANKS DRY)'.\n"
                    f"- If BLOCKED_BY_STORM with Fuel Remaining > 0u: instruct aircraft to enter an emergency holding pattern using remaining fuel reserves until corridors open, risk_score 9, fuel_impact_est 'HOLDING (BURNING RESERVE)'.\n"
                    f"- If Diverted is true: issue an urgent diversion squawk to Active Goal referencing Zeus/Poseidon/Scylla, risk_score 8, high fuel impact.\n"
                    f"Reply in ONE single line separated by pipes (|) with no markdown:\n"
                    f"2-sentence ATC radio call | integer risk 1-10 | fuel & time impact string"
                )
                ctx = ssl._create_unverified_context()
                try:
                    url = f"https://generativelanguage.googleapis.com/v1beta/models/gemini-flash-latest:generateContent?key={g_key}"
                    payload = {
                        "contents": [{"parts": [{"text": prompt}]}],
                        "generationConfig": {
                            "temperature": 0.4,
                            "maxOutputTokens": 120,
                            "thinkingConfig": {"thinkingBudget": 0}
                        }
                    }
                    req = urllib.request.Request(
                        url, data=json.dumps(payload).encode(),
                        headers={"Content-Type": "application/json"}
                    )
                    res = json.loads(urllib.request.urlopen(req, timeout=10, context=ctx).read().decode())
                    raw_txt = res["candidates"][0]["content"]["parts"][0]["text"].strip()
                    parts = [p.strip() for p in raw_txt.split("|")]
                    if len(parts) >= 3:
                        tactical_card["voice_squawk"] = parts[0]
                        digits = "".join(c for c in parts[1] if c.isdigit())
                        if digits:
                            tactical_card["risk_score"] = max(1, min(10, int(digits)))
                        tactical_card["fuel_impact_est"] = parts[2]
                except Exception as ex:
                    print(f"[Gemini fallback]: {ex}")

            # Send ONLY the voice_squawk string to ElevenLabs TTS
            audio_b64 = ""
            if e_key:
                try:
                    voice_id = "JBFqnCBsd6RMkjVDRZzb"
                    e_url = f"https://api.elevenlabs.io/v1/text-to-speech/{voice_id}"
                    e_req = urllib.request.Request(e_url, data=json.dumps({
                        "text": tactical_card["voice_squawk"], "model_id": "eleven_turbo_v2_5"
                    }).encode(), headers={"xi-api-key": e_key, "Content-Type": "application/json", "Accept": "audio/mpeg"})
                    audio_bytes = urllib.request.urlopen(e_req, timeout=12, context=ssl._create_unverified_context()).read()
                    audio_b64 = base64.b64encode(audio_bytes).decode()
                except Exception:
                    audio_b64 = ""

            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps({
                "briefing": tactical_card["voice_squawk"],
                "risk_score": tactical_card["risk_score"],
                "fuel_impact_est": tactical_card["fuel_impact_est"],
                "audio_b64": audio_b64
            }).encode())


print("ClearVector Server live at http://localhost:8000")
ThreadingHTTPServer(('localhost', 8000), Handler).serve_forever()
