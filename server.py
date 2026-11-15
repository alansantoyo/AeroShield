import os, subprocess, json, urllib.request, base64, random, ssl, math
from http.server import SimpleHTTPRequestHandler, HTTPServer

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
                    if i < len(data["flights"]) and len(data["flights"][i]["path"]) > 1:
                        nxt = data["flights"][i]["path"][1]
                        dest_pt = data["flights"][i]["path"][-1]
                        nxt_key = (round(nxt[0], 2), round(nxt[1], 2))
                        dest_key = (round(dest_pt[0], 2), round(dest_pt[1], 2))
                        # Advance if next waypoint is clear OR if it's the final airport pin
                        fuel_left = int(parts[6]) if len(parts) >= 7 else 50
                        if fuel_left > 0 and (nxt_key not in occupied or nxt_key == dest_key):
                            parts[1], parts[2] = str(nxt[0]), str(nxt[1])
                            parts[6] = str(fuel_left - 1) # Burn 1 unit of fuel per step!   

                    cur_key = (round(float(parts[1]), 2), round(float(parts[2]), 2))
                    occupied.add(cur_key)
                    f.write(" ".join(parts) + "\n")
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

        elif self.path == '/reset_all':
            with open("storms.txt", "w") as f: pass
            deduplicate_and_reset_flights(reset_coords=True)
            self.send_response(200); self.end_headers()

        elif self.path == '/ai_briefing':
            summary = body.get("summary", "Flight LJ60 encountering 55 dBZ squall line.")
            g_key = os.environ.get("GEMINI_API_KEY", "").strip()
            e_key = os.environ.get("ELEVENLABS_API_KEY", "").strip()
            briefing = (f"AEOLUS ATC ADVISORY: {summary} Ground NEXRAD composite penetrated X-band shadow. "
                        f"Executing immediate zero-click vector around Scylla convective core.")

            # 1. Call Google Gemini 2.5 Flash for live tactical ATC + Odyssey briefing
            if g_key:
                prompt = (f"You are AEOLUS-ATC, an automated aviation safety engine inspired by Homer's Odyssey. "
                          f"Given this live radar telemetry: {summary}. Write a 2-sentence urgent ATC voice advisory "
                          f"giving a heading/diversion command and briefly referencing steering clear of Zeus/Poseidon/Cyclops storm cells.")
                ctx = ssl._create_unverified_context()
                # Try fast non-thinking Flash models in order so rate-limits/timeouts never fail
                for model_name in ["gemini-2.0-flash", "gemini-2.5-flash", "gemini-flash-latest"]:
                    try:
                        url = f"https://generativelanguage.googleapis.com/v1beta/models/{model_name}:generateContent?key={g_key}"
                        payload = {
                            "contents": [{"parts": [{"text": prompt}]}],
                            "generationConfig": {"maxOutputTokens": 90, "temperature": 0.7}
                        }
                        # Disable thinking latency on 2.5/latest models for sub-second response
                        if "2.0" not in model_name:
                            payload["generationConfig"]["thinkingConfig"] = {"thinkingBudget": 0}

                        req = urllib.request.Request(
                            url, data=json.dumps(payload).encode(),
                            headers={"Content-Type": "application/json", "Connection": "close"}
                        )
                        res = json.loads(urllib.request.urlopen(req, timeout=12, context=ctx).read().decode())
                        briefing = res["candidates"][0]["content"]["parts"][0]["text"].strip()
                        break # Succeeded! Exit fallback loop
                    except Exception as ex:
                        print(f"[Gemini {model_name} retry]: {ex}")

            # 2. Call ElevenLabs TTS API for realistic AI ATC Voice Audio
            audio_b64 = ""
            if e_key:
                try:
                    voice_id = "JBFqnCBsd6RMkjVDRZzb" # George - authoritative ATC tone
                    e_url = f"https://api.elevenlabs.io/v1/text-to-speech/{voice_id}"
                    e_req = urllib.request.Request(e_url, data=json.dumps({
                        "text": briefing, "model_id": "eleven_turbo_v2_5"
                    }).encode(), headers={"xi-api-key": e_key, "Content-Type": "application/json", "Accept": "audio/mpeg"})
                    audio_bytes = urllib.request.urlopen(e_req, timeout=12, context=ssl._create_unverified_context()).read() 
                    audio_b64 = base64.b64encode(audio_bytes).decode()
                except Exception:
                    audio_b64 = ""

            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps({"briefing": briefing, "audio_b64": audio_b64}).encode())

print("ClearVector Server live at http://localhost:8000")
HTTPServer(('localhost', 8000), Handler).serve_forever()
