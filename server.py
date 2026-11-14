import os, subprocess, json, urllib.request, base64
from http.server import SimpleHTTPRequestHandler, HTTPServer

AIRPORTS = [
    ("SEA", 47.45, -122.30), ("SFO", 37.62, -122.37), ("LAX", 33.94, -118.40),
    ("DEN", 39.85, -104.67), ("DFW", 32.89,  -97.04), ("AUS", 30.19,  -97.66),
    ("ORD", 41.97,  -87.90), ("ATL", 33.64,  -84.42), ("JFK", 40.64,  -73.77),
    ("MIA", 25.79,  -80.29)
]

def reset_scenario(scen=1):
    with open("flights.txt", "w") as f:
        f.write("LJ60 30.19 -97.66 5 9\n")   # AUS -> MIA (Nov 1 Route)
        f.write("AAL41 32.89 -97.04 4 8\n")  # DFW -> JFK
        f.write("UAL88 41.97 -87.90 6 2\n")  # ORD -> LAX
        f.write("DAL19 33.64 -84.42 7 3\n")  # ATL -> DEN
    with open("storms.txt", "w") as f:
        if scen == 1:
            f.write("28.5 -89.5 4 999\n30.5 -88.0 3 50\n37.0 -95.0 3 999\n")
        elif scen == 2:
            # Scenario 2: Cockpit Attenuation Trap + MIA Blockade (Diverts to ATL)
            f.write("29.0 -91.5 3 10\n28.2 -87.0 5 999\n25.79 -80.29 4 999\n")

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
            # Advance each aircraft 1 step along its computed A* path
            cockpit = "1" if body.get("cockpitMode") else "0"
            data = json.loads(subprocess.check_output(["./clearvector_cli", cockpit]).decode())
            with open("flights.txt", "r") as f:
                lines = [l.strip().split() for l in f if l.strip()]
            with open("flights.txt", "w") as f:
                for i, parts in enumerate(lines):
                    if i < len(data["flights"]) and len(data["flights"][i]["path"]) > 1:
                        nxt = data["flights"][i]["path"][1]
                        parts[1], parts[2] = str(nxt[0]), str(nxt[1])
                    f.write(" ".join(parts) + "\n")
            self.send_response(200); self.end_headers()

        elif self.path == '/add_flight':
            o, d = int(body['orig']), int(body['dest'])
            cs = body.get('callsign', 'SWA99')
            lat, lon = AIRPORTS[o][1], AIRPORTS[o][2]
            with open("flights.txt", "a") as f:
                f.write(f"{cs} {lat} {lon} {o} {d}\n")
            self.send_response(200); self.end_headers()

        elif self.path == '/add_storm':
            with open("storms.txt", "a") as f:
                f.write(f"{body['lat']} {body['lon']} {body['rad']} {body['weight']}\n")
            self.send_response(200); self.end_headers()

        elif self.path == '/scenario':
            reset_scenario(body.get('id', 1))
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
