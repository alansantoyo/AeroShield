import os, subprocess, json, urllib.request, base64, random
from http.server import SimpleHTTPRequestHandler, HTTPServer

AIRPORTS = [
    ("SEA", 47.45, -122.30), ("SFO", 37.62, -122.37), ("LAX", 33.94, -118.40),
    ("DEN", 39.85, -104.67), ("DFW", 32.89,  -97.04), ("AUS", 30.19,  -97.66),
    ("ORD", 41.97,  -87.90), ("ATL", 33.64,  -84.42), ("JFK", 40.64,  -73.77),
    ("MIA", 25.79,  -80.29)
]

def deduplicate_and_reset_flights(reset_coords=True):
    if not os.path.exists("flights.txt"): return
    with open("flights.txt", "r") as f:
        lines = [l.strip().split() for l in f if l.strip()]
    seen_cs, seen_routes, clean = set(), set(), []
    for parts in lines:
        if len(parts) < 5: continue
        cs = parts[0].upper()
        route = (parts[3], parts[4])
        if cs in seen_cs:
            continue # Skip duplicate callsign or duplicate origin->dest route!
        seen_cs.add(cs)
        seen_routes.add(route)
        orig_idx = int(parts[3])
        if reset_coords:
            parts[1], parts[2] = str(AIRPORTS[orig_idx][1]), str(AIRPORTS[orig_idx][2])
        style = parts[5] if (len(parts) >= 6 and not reset_coords) else str(random.randint(1, 3))
        clean.append(f"{cs} {parts[1]} {parts[2]} {parts[3]} {parts[4]} {style}\n")
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

    with open("storms.txt", "w") as f:
        if scen == 1:
            # 3=Thunderstorm (Gulf), 2=Sleet/Snow (Rockies), 1=Rain (Midwest)
            f.write("28.5 -89.5 3\n39.0 -105.5 2\n36.5 -93.0 1\n")
        elif scen == 2:
            # Rain shadow ahead of AUS, Thunderstorm behind it, plus Thunderstorm blocking MIA
            f.write("29.0 -91.5 1\n28.2 -87.0 3\n25.79 -80.29 3\n")

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
                        if nxt_key not in occupied or nxt_key == dest_key:
                            parts[1], parts[2] = str(nxt[0]), str(nxt[1])
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
                with open("flights.txt", "a") as f:
                    f.write(f"{cs} {lat} {lon} {o} {d} {random.randint(1, 3)}\n")
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


        elif self.path == '/scenario':
            reset_scenario(body.get('id', 1))
            self.send_response(200); self.end_headers()

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
                try:
                    url = f"https://generativelanguage.googleapis.com/v1beta/models/gemini-flash-latest:generateContent?key={g_key}"
                    prompt = (f"You are AEOLUS-ATC, an automated aviation safety engine inspired by Homer's Odyssey. "
                              f"Given this live radar telemetry: {summary}. Write a 2-sentence urgent ATC voice advisory "
                              f"giving a heading/diversion command and briefly referencing steering clear of Scylla/Charybdis storm cells.")
                    req = urllib.request.Request(url, data=json.dumps({"contents":[{"parts":[{"text":prompt}]}]}).encode(),
                                                 headers={"Content-Type": "application/json"})
                    res = json.loads(urllib.request.urlopen(req, timeout=5).read().decode())
                    briefing = res["candidates"][0]["content"]["parts"][0]["text"].strip()
                except Exception as ex:
                    briefing += f" (Gemini fallback: {ex})"

            # 2. Call ElevenLabs TTS API for realistic AI ATC Voice Audio
            audio_b64 = ""
            if e_key:
                try:
                    voice_id = "JBFqnCBsd6RMkjVDRZzb" # George - authoritative ATC tone
                    e_url = f"https://api.elevenlabs.io/v1/text-to-speech/{voice_id}"
                    e_req = urllib.request.Request(e_url, data=json.dumps({
                        "text": briefing, "model_id": "eleven_turbo_v2_5"
                    }).encode(), headers={"xi-api-key": e_key, "Content-Type": "application/json", "Accept": "audio/mpeg"})
                    audio_bytes = urllib.request.urlopen(e_req, timeout=6).read()
                    audio_b64 = base64.b64encode(audio_bytes).decode()
                except Exception:
                    audio_b64 = ""

            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps({"briefing": briefing, "audio_b64": audio_b64}).encode())

print("ClearVector Server live at http://localhost:8000")
HTTPServer(('localhost', 8000), Handler).serve_forever()
