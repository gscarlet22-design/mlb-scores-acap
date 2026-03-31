[README.md](https://github.com/user-attachments/files/26364349/README.md)
# MLB Live Scores — Axis C1720 ACAP

Displays live MLB scores on the C1720 speaker display and plays audio clips on
scoring plays. The followed team is **user-selectable** from a dropdown in the
web UI — all 30 MLB teams are available. Uses the free MLB StatsAPI — no API
key required.

---

## How it works

| Component | Detail |
|-----------|--------|
| **Data source** | `statsapi.mlb.com` — MLB's own free, unofficial public API |
| **Team selection** | Any of the 30 MLB teams, configurable live from the web UI |
| **Poll rate** | Every 30 s during a live game, every 5 min otherwise |
| **Display** | Axis `speaker-display-notification` REST API (built into AXIS OS) |
| **Audio** | VAPIX `mediaclip.cgi` — separate clips for score change vs. your team scoring |
| **HTTP port** | `2016` (internal, proxied via AXIS OS reverse proxy) |

Score line shown on display during a live game:

```
Royals 4 - Cardinals 2 | Bottom 7 | 1 out
```

Final result:

```
FINAL: Royals WIN 6 - Cardinals 3
```

Switching teams in the UI resets the game lookup immediately — no restart needed.

---

## Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| Docker Desktop or Engine | 4.11+ / 20.10+ | Required on your build machine |
| AXIS OS on C1720 | 12.x recommended | Must match SDK version below |

> **Apple Silicon (M1/M2/M3) Mac?**
> Add `--platform=linux/amd64` to every `docker` command below.

---

## Project structure

```
mlb_scores/
├── Dockerfile
└── app/
    ├── main.c
    ├── Makefile
    ├── manifest.json
    └── html/
        ├── index.html
        ├── app.js
        └── style.css
```

---

## Step 1 — Build the .eap file

Open a terminal in the `mlb_scores/` directory.

```bash
docker build --build-arg ARCH=aarch64 --tag mlb_scores:1.0 .
```

Extract the `.eap` file:

```bash
docker cp $(docker create mlb_scores:1.0):/opt/app ./build
```

Your deployable file: `build/mlb_scores_1_0_0_aarch64.eap`

---

## Step 2 — Install on the C1720

Via the web interface: **Apps → Add app** → upload the `.eap`.

Via VAPIX:

```bash
curl -u root:<password> \
     -F packfil=@build/mlb_scores_1_0_0_aarch64.eap \
     "http://<camera-ip>/axis-cgi/applications/upload.cgi"
```

---

## Step 3 — Configure

1. **Apps → MLB Live Scores → Open**
2. Config tab: pick your **team** from the dropdown, enter device credentials, choose audio clips, set display options
3. **Save Settings** → **Test Display** to verify

Changing the team takes effect on the next poll with no restart.

---

## All 30 supported teams

| ID  | Team | Abbr | | ID  | Team | Abbr |
|-----|------|------|-|-----|------|------|
| 108 | Angels | LAA | | 133 | Athletics | OAK |
| 109 | Diamondbacks | ARI | | 134 | Pirates | PIT |
| 110 | Orioles | BAL | | 135 | Padres | SD |
| 111 | Red Sox | BOS | | 136 | Mariners | SEA |
| 112 | Cubs | CHC | | 137 | Giants | SF |
| 113 | Reds | CIN | | 138 | Cardinals | STL |
| 114 | Guardians | CLE | | 139 | Rays | TB |
| 115 | Rockies | COL | | 140 | Rangers | TEX |
| 116 | Tigers | DET | | 141 | Blue Jays | TOR |
| 117 | Astros | HOU | | 142 | Twins | MIN |
| 118 | Royals | KC | | 143 | Phillies | PHI |
| 119 | Dodgers | LAD | | 144 | Braves | ATL |
| 120 | Nationals | WSH | | 145 | White Sox | CWS |
| 121 | Mets | NYM | | 146 | Marlins | MIA |
| 147 | Yankees | NYY | | 158 | Brewers | MIL |

---

## Adjusting the SDK version

```bash
docker build --build-arg ARCH=aarch64 --build-arg VERSION=12.6.0 --tag mlb_scores:1.0 .
```

| AXIS OS | SDK version |
|---------|-------------|
| 12.9.x  | 12.9.0 *(default)* |
| 12.6.x  | 12.6.0 |
| 12.5.x  | 12.5.0 |

Check firmware at `http://<camera-ip>` → **System → About**.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Display does nothing | Re-enter device password in Config tab |
| No score / team dropdown empty | Check Status tab for last poll time; wait for app to start |
| App shows "Disabled" | Enable in Config tab |
| Build fails: package not found | Adjust `VERSION` build arg to match your firmware |
| Apple Silicon warning | Add `--platform=linux/amd64` |

View live logs:

```bash
ssh root@<camera-ip>
journalctl -u mlb_scores -f
```
