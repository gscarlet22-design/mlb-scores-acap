# MLB Live Scores — Axis C1720 / C1710 ACAP

**Latest: Version 1.1.3** — gscarlet22 design

Displays live MLB scores on the Axis C1720 or C1710 speaker display and plays
audio clips on scoring plays. Monitor up to 8 teams simultaneously, each with
its own audio clips and display colors. The display only updates when the score
or inning actually changes. When no game is active, shows the next scheduled
game with date and local start time. A week-at-a-glance schedule for all
monitored teams is shown in the Status tab. Uses the free MLB StatsAPI — no
API key required.

---

## Features

| Feature | Detail |
|---------|--------|
| **Multi-team monitoring** | Track up to 8 teams simultaneously — each gets its own scoreboard card in the Status tab |
| **Per-team audio** | Each team has its own Inning Change Sound and Score Sound clip — configure independently |
| **Per-team display colors** | Text color, background, text size, scroll speed, and duration set per team — defaults to that team's official MLB colors |
| **Test Audio per team** | Each team row has its own Test button to play its inning change clip |
| **Display on change only** | Display fires when the score or inning half actually changes, not every poll |
| **Inning change audio** | Inning change clip plays on every half-inning change, Score clip plays when your team scores |
| **Live scores** | Score, inning, half-inning, and out count on the display during a game |
| **Last play text** | Most recent play description shown on each team's status card |
| **Today / Next game preview** | Pre-game cards show "Today:" with time; no-game cards show next matchup up to 14 days ahead |
| **Week schedule** | Status tab shows a 7-day game calendar for all monitored teams, grouped by day |
| **Final score persist** | Keeps the final score on the display until midnight after a game ends |
| **All 30 teams** | All 30 MLB teams available with their official colors shown as swatches |
| **Audio clip library** | All clips saved on the device appear in each team's dropdown automatically |
| **1.0.x / 1.1.x migration** | Upgrading from any prior version auto-migrates config; per-team display defaults to official team colors |
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

With multiple teams, each team's update is shown when its score or inning
changes. Teams not currently live show their next game info in the Status tab
but do not push display updates until game time.

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
> Use GitHub Actions — see `.github/workflows/` in this repo. Push your
> changes and the `.eap` is produced as a downloadable artifact automatically.

---

## Project structure

```
mlb-scores-acap/
├── .github/workflows/
│   ├── build.yml                  ← GitHub Actions build for v1.0.26
│   ├── build-v1.1.0.yml           ← GitHub Actions build for v1.1.0
│   └── build-v1.1.1.yml           ← GitHub Actions build for v1.1.1 / v1.1.2+
│
├── v1.0.26/                       ← Version 1.0.26 (single-team)
│   ├── dockerfile
│   └── app/
│       ├── main.c
│       ├── Makefile
│       ├── manifest.json
│       ├── LICENSE
│       └── html/
│           ├── index.html
│           ├── app.js
│           └── style.css
│
├── v1.1.0/                        ← Version 1.1.0 (multi-team)
│   ├── Dockerfile
│   └── app/
│       ├── main.c
│       ├── Makefile
│       ├── manifest.json
│       ├── LICENSE
│       └── html/
│           ├── index.html
│           ├── app.js
│           └── style.css
│
└── v1.1.1/                        ← Version 1.1.1 / 1.1.2+ (active development)
    ├── Dockerfile
    ├── README.md
    └── app/
        ├── main.c
        ├── Makefile
        ├── manifest.json           ← increment version here for each build
        ├── LICENSE
        └── html/
            ├── index.html
            ├── app.js
            └── style.css
```

---

## Building

### Option A — GitHub Actions (no local Docker needed)

Each version has its own workflow file. Trigger a build manually from
**Actions → select the workflow → Run workflow**, or push changes to the
relevant version folder to trigger automatically.

| Workflow | Triggers on |
|----------|------------|
| `build.yml` | Changes to `v1.0.26/**` |
| `build-v1.1.0.yml` | Changes to `v1.1.0/**` |
| `build-v1.1.1.yml` | Changes to `v1.1.1/**` |

### Option B — Local Docker

**v1.0.26:**
```bash
cd v1.0.26
docker build --build-arg ARCH=aarch64 --tag mlb_scores:1.0.26 .
docker cp $(docker create mlb_scores:1.0.26):/opt/app ./build
```

**v1.1.0:**
```bash
cd v1.1.0
docker build --build-arg ARCH=aarch64 --tag mlb_scores:1.1.0 .
docker cp $(docker create mlb_scores:1.1.0):/opt/app ./build
```

**v1.1.1 / v1.1.2+:**
```bash
cd v1.1.1
docker build --build-arg ARCH=aarch64 --tag mlb_scores:1.1.2 .
docker cp $(docker create mlb_scores:1.1.2):/opt/app ./build
```

Your deployable file will be in `./build/*.eap`.

---

## Installing on the device

**Via the web interface (easiest):**

1. Open `http://<device-ip>` in a browser
2. Go to **Apps → Add app**
3. Upload the `.eap` file
4. The app installs and starts automatically

Installing a newer version over an existing one works in-place — no need
to uninstall first, as long as the version number in `manifest.json` is higher.

**Via VAPIX (scripted):**

```bash
curl -u root:<password> \
     -F packfil=@build/mlb_scores_1_1_2_aarch64.eap \
     "http://<device-ip>/axis-cgi/applications/upload.cgi"
```

---

## Configuring (v1.1.3)

1. Go to **Apps → MLB Live Scores → Open**
2. On the **Config** tab:

**Teams**
- Click **+ Add Team** and select any of the 30 MLB teams from the dropdown
- Each team row has its own **Inning Change Sound** and **Score Sound** clip selectors
- Each team row also has a **Test** button to audition the inning change clip
- Each team has its own **Text Color**, **Background**, **Text Size**, **Speed**, and **Duration** — pre-filled with official MLB team colors
- Add up to 8 teams — each is monitored independently
- Click **Remove** on a team row to stop monitoring that team

**Device Credentials**
- Enter the device username and password — required for VAPIX display and audio calls

**Audio**
- Output Volume slider (0–100%) — sets `AudioOutput.A0.Volume` on the device via VAPIX; applied immediately on save and on every app start

**Polling**
- Live game poll interval: 15 / 30 / 60 / 120 seconds (default 30 s)
- When any monitored team has a live game, the short interval is used
- Between games the app polls every 5 minutes automatically
- Enabled toggle — disable the app without uninstalling

**Display**
- Show on Display toggle
- Persist Final Score — keeps the final score on the display until midnight
- **Test Display** button — sends a test message using the first team's colors

3. Click **Save Settings**
4. Switch to the **Status** tab — each monitored team shows its own scoreboard card, followed by the week schedule

---

## Status tab

The Status tab shows one scoreboard card per monitored team, then a 7-day
schedule for all monitored teams grouped by day:

**Scoreboard cards**
- **Live games** — highlighted with a gold border; shows score, inning, outs, last play
- **Final games** — shows final score with WIN / LOSS / TIE
- **Today / pre-game** — shows "Today: vs Opponent | Time"
- **No game / scheduled** — shows next game opponent, date, and local time
- Cards refresh automatically every 10 seconds

**Week schedule**
- Lists every game for monitored teams over today + 6 days
- Grouped by day with a day/date header
- Shows away @ home and local start time
- Live games show LIVE badge; Final games show Final
- Refreshes every 60 seconds

---

## Audio behavior

| Event | Clip played |
|-------|------------|
| Half-inning changes (Top/Bottom) | **Inning Change Sound** for that team |
| Opponent scores (score changes, your team did not) | **Inning Change Sound** for that team |
| Your team scores | **Score Sound** for that team |

---

## Migrating from any prior version

No action required. On first boot after installing, the app reads existing
parameters and auto-migrates. Per-team display settings default to each
team's official MLB colors if not previously configured.

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
docker build --build-arg ARCH=aarch64 --build-arg VERSION=12.6.0 --tag mlb_scores:1.1.2 .
```

Check your firmware at `http://<device-ip>` → **System → About**.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Display does nothing | Re-enter device password in Config tab, Save Settings, then Test Display |
| Score not updating | Check Status tab — each team card shows its last poll time |
| No scoreboard cards | No teams configured — go to Config and add at least one team |
| Schedule shows nothing | No teams configured, or no games in the next 7 days for monitored teams |
| Next game not showing | Device may not have internet access, or no games in next 14 days |
| Audio dropdown empty | Re-enter device password in Config tab and Save Settings |
| App shows "Disabled" | Enable toggle in Config tab |
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
| 1.1.3 | Output volume slider in Config tab (0–100%); applied via VAPIX param.cgi on save and on startup |
| 1.1.2 | Week schedule in Status tab (7-day game calendar for monitored teams); inning change audio now fires on every half-inning change; "Notify Sound" renamed to "Inning Change Sound"; Today: label on pre-game status cards |
| 1.1.1 | Per-team display settings (text color, background, text size, scroll speed, duration) — each defaults to team's official MLB colors; Test Audio button restored per-team; fixed last_play showing in status cards |
| 1.1.0 | Multi-team support (up to 8 teams); per-team audio clips; display fires only on score or inning change; scoreboard cards in Status tab; auto-migration from 1.0.x single-team config; repo reorganized into versioned folders |
| 1.0.26 | Fixed display API JSON schema (correct Axis field names and `data` wrapper); HTTPS + basic auth for display calls; audio clip dropdown now shows all device clips dynamically |
| 1.0.22 | Fixed audio clip dropdown parser (`M%d` format); display request buffer increased |
| 1.0.20 | Manual digest auth implementation for display; self-contained MD5; socket headers reorganized |
| 1.0.19 | Team color pickers now update when team is changed in dropdown |
| 1.0.18 | Display call diagnostics; clips raw response visible for debugging |
| 1.0.10 | Fixed all API route paths (full path forwarded by Axis reverse proxy) |
| 1.0.5  | Fixed missing `display_persist_final` field in `/api/config` response causing server crash and empty team dropdown |
| 1.0.2  | Team colors auto-fill on selection; next game preview (14-day lookahead); persist final score until midnight |
| 1.0.0  | Initial release — live scores, all 30 teams selectable, dual audio clips |
