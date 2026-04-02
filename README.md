# MLB Live Scores — Axis C1720 / C1710 ACAP

**Version 1.0.26** — gscarlet22 design

Displays live MLB scores on the Axis C1720 or C1710 speaker display and plays
audio clips on scoring plays. When no game is active, shows the next scheduled
game with date and local start time. The followed team is user-selectable from
a dropdown — all 30 MLB teams are available with their official colors
auto-applied. Uses the free MLB StatsAPI — no API key required.

---

## Features

| Feature | Detail |
|---------|--------|
| **Live scores** | Score, inning, half-inning, and out count on the display during a game |
| **Next game preview** | When no game is live, shows opponent, date, and local start time (looks 14 days ahead) |
| **Final score persist** | Keeps the final score on the display until midnight after a game ends |
| **All 30 teams** | User-selectable from a dropdown in the web UI — change takes effect immediately with no restart |
| **Team colors** | Selecting a team auto-fills the display background and text with that team's official colors |
| **Dual audio clips** | Separate audio clips for any score change vs. your team specifically scoring |
| **Audio clip library** | All clips saved on the device appear in the dropdown automatically |
| **Data source** | `statsapi.mlb.com` — MLB's own free, unofficial public API, no key needed |
| **Compatible devices** | Axis C1720 and C1710 (both aarch64 / ARTPEC-8) |
| **HTTP port** | `2016` (internal, proxied via AXIS OS reverse proxy) |

---

## What the display shows

During a live game:
```
Royals 4 - Cardinals 2 | Bottom 7 | 1 out
```

Final result (persists until midnight if enabled):
```
FINAL: Royals WIN 6 - Cardinals 3
```

No game today — next game found:
```
Next Game  |  vs Cardinals  |  Apr 3, 7:10 PM
```

No game today and nothing scheduled within 14 days:
```
No Games Scheduled
```

---

## Internet requirement

The C1720/C1710 needs outbound internet access to reach `statsapi.mlb.com`
on port 443. All display and audio calls happen locally on the device's
internal network. The app coexists with AXIS Audio Manager Pro without
conflict — PA announcements will take priority over score display, which
is the correct behavior.

---

## Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| Docker Desktop or Engine | 4.11+ / 20.10+ | Required on your build machine |
| AXIS OS on device | 12.x recommended | Must match SDK version below |

> **Apple Silicon (M1/M2/M3) Mac?**
> Add `--platform=linux/amd64` to every `docker` command below.

> **No Docker available (e.g. work laptop)?**
> Use GitHub Actions — see the `.github/workflows/build.yml` included in
> this repo. Push your changes to `main` and the `.eap` is produced as a
> downloadable artifact automatically.

---

## Project structure

```
mlb_scores/
├── .github/workflows/build.yml   ← GitHub Actions build (no Docker needed)
├── Dockerfile                    ← SDK build container
└── app/
    ├── main.c                    ← Main application (C, single file)
    ├── Makefile                  ← Build rules
    ├── manifest.json             ← ACAP package metadata
    ├── LICENSE
    └── html/
        ├── index.html            ← Web UI
        ├── app.js                ← UI logic
        └── style.css             ← Styling
```

---

## Building

### Option A — GitHub Actions (no local Docker needed)

Push to the `main` branch. GitHub Actions pulls the Axis SDK, compiles, and
produces the `.eap` as a downloadable artifact under the **Actions** tab.
Trigger a manual build anytime with **Actions → Build MLB Scores ACAP →
Run workflow**.

### Option B — Local Docker

```bash
# From the mlb_scores/ directory
docker build --build-arg ARCH=aarch64 --tag mlb_scores:1.0.26 .
docker cp $(docker create mlb_scores:1.0.26):/opt/app ./build
```

Your deployable file: `build/mlb_scores_1_0_26_aarch64.eap`

---

## Installing on the device

**Via the web interface (easiest):**

1. Open `http://<device-ip>` in a browser
2. Go to **Apps → Add app**
3. Upload `mlb_scores_1_0_26_aarch64.eap`
4. The app installs and starts automatically

Installing a newer version over an existing one works in-place — no need
to uninstall first, as long as the version number is higher.

**Via VAPIX (scripted):**

```bash
curl -u root:<password> \
     -F packfil=@build/mlb_scores_1_0_26_aarch64.eap \
     "http://<device-ip>/axis-cgi/applications/upload.cgi"
```

---

## Configuring

1. Go to **Apps → MLB Live Scores → Open**
2. On the **Config** tab:

**Team**
- Pick any of the 30 MLB teams from the dropdown
- The display background and text colors auto-fill with that team's official colors when you change teams
- Colors can be manually adjusted after auto-fill

**Device Credentials**
- Enter the device username and password — required for VAPIX display and audio calls

**Polling**
- Live game poll interval: 15 / 30 / 60 / 120 seconds (default 30 s)
- Between games the app polls every 5 minutes automatically
- Enabled toggle — disable the app without uninstalling

**Display**
- Show on Display toggle
- Text size: Small / Medium / Large
- Text color and background color (auto-filled from team colors, adjustable)
- Scroll speed (1–5)
- Duration in seconds
- Persist Final Score — keeps the final score on the display until midnight

**Audio**
- Notification Sound — plays on any score change (all device audio clips available)
- Scoring Sound — plays specifically when your team scores
- New clips uploaded to the device appear in the dropdown automatically
- Test Sound button

3. Click **Save Settings**
4. Click **Test Display** to confirm the display is working
5. Switch to the **Status** tab to monitor live

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

Match the SDK version to your device's AXIS OS firmware:

| AXIS OS | SDK version |
|---------|-------------|
| 12.9.x  | 12.9.0 *(default)* |
| 12.6.x  | 12.6.0 |
| 12.5.x  | 12.5.0 |

```bash
docker build --build-arg ARCH=aarch64 --build-arg VERSION=12.6.0 --tag mlb_scores:1.0.26 .
```

Check your firmware at `http://<device-ip>` → **System → About**.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Display does nothing | Re-enter device password in Config tab, Save Settings, then Test Display |
| Score not updating | Check Status tab — Last Poll time shows if API is reachable |
| Next game not showing | Device may not have internet access, or no games in next 14 days |
| Team dropdown empty | Wait a few seconds and refresh — app may still be starting |
| Audio dropdown empty | Re-enter device password in Config tab and Save Settings |
| App shows "Disabled" | Enable toggle in Config tab |
| Colors not auto-filling | Change team selection in the dropdown — colors apply on change |
| Build fails: package not found | Adjust `VERSION` build arg to match your firmware |
| Apple Silicon Docker warning | Add `--platform=linux/amd64` |

**View live logs on the device:**

```bash
ssh root@<device-ip>
journalctl -u mlb_scores -f
```

---

## Version history

| Version | Changes |
|---------|---------|
| 1.0.26 | Fixed display API JSON schema (correct Axis field names and `data` wrapper); HTTPS + basic auth for display calls; audio clip dropdown now shows all device clips dynamically |
| 1.0.22 | Fixed audio clip dropdown parser (`M%d` format); display request buffer increased |
| 1.0.20 | Manual digest auth implementation for display; self-contained MD5; socket headers reorganized |
| 1.0.19 | Team color pickers now update when team is changed in dropdown |
| 1.0.18 | Display call diagnostics; clips raw response visible for debugging |
| 1.0.10 | Fixed all API route paths (full path forwarded by Axis reverse proxy) |
| 1.0.5  | Fixed missing `display_persist_final` field in `/api/config` response causing server crash and empty team dropdown |
| 1.0.2  | Team colors auto-fill on selection; next game preview (14-day lookahead); persist final score until midnight |
| 1.0.0  | Initial release — live scores, all 30 teams selectable, dual audio clips |
