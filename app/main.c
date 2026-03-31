/*
 * mlb_scores - MLB live score ACAP for Axis C1720
 * Polls MLB StatsAPI (free, no key) and pushes score updates to the
 * C1720 speaker display and triggers an audio clip on scoring plays.
 * The followed team is user-selectable via the web UI.
 *
 * API used:
 *   Schedule: https://statsapi.mlb.com/api/v1/schedule?sportId=1&teamId=<id>&date=YYYY-MM-DD
 *   Live feed: https://statsapi.mlb.com/api/v1.1/game/{gamePk}/feed/live
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <errno.h>

#include <curl/curl.h>
#include "cJSON.h"
#include <axsdk/axparameter.h>

/* ── Constants ──────────────────────────────────────────────────── */
#define APP_NAME            "mlb_scores"
#define MLB_API_BASE        "https://statsapi.mlb.com/api/v1"
#define MLB_API_LIVE        "https://statsapi.mlb.com/api/v1.1"

#define DISPLAY_API         "http://127.0.0.1/config/rest/speaker-display-notification/v1/simple"
#define DISPLAY_STOP_API    "http://127.0.0.1/config/rest/speaker-display-notification/v1/stop"
#define MEDIACLIP_API       "http://127.0.0.1/axis-cgi/mediaclip.cgi?action=play&clip=%d"
#define PARAM_API           "http://127.0.0.1/axis-cgi/param.cgi?action=update&"

#define CONFIG_PATH         "/usr/local/packages/mlb_scores/localdata/config.json"
#define HTTP_PORT           2016

/* ── MLB team lookup table ──────────────────────────────────────── */
typedef struct { int id; const char *name; const char *abbr; } MlbTeam;
static const MlbTeam MLB_TEAMS[] = {
    {108, "Angels",          "LAA"},
    {109, "Diamondbacks",    "ARI"},
    {110, "Orioles",         "BAL"},
    {111, "Red Sox",         "BOS"},
    {112, "Cubs",            "CHC"},
    {113, "Reds",            "CIN"},
    {114, "Guardians",       "CLE"},
    {115, "Rockies",         "COL"},
    {116, "Tigers",          "DET"},
    {117, "Astros",          "HOU"},
    {118, "Royals",          "KC"},
    {119, "Dodgers",         "LAD"},
    {120, "Nationals",       "WSH"},
    {121, "Mets",            "NYM"},
    {133, "Athletics",       "OAK"},
    {134, "Pirates",         "PIT"},
    {135, "Padres",          "SD"},
    {136, "Mariners",        "SEA"},
    {137, "Giants",          "SF"},
    {138, "Cardinals",       "STL"},
    {139, "Rays",            "TB"},
    {140, "Rangers",         "TEX"},
    {141, "Blue Jays",       "TOR"},
    {142, "Twins",           "MIN"},
    {143, "Phillies",        "PHI"},
    {144, "Braves",          "ATL"},
    {145, "White Sox",       "CWS"},
    {146, "Marlins",         "MIA"},
    {147, "Yankees",         "NYY"},
    {158, "Brewers",         "MIL"},
};
#define NUM_TEAMS (int)(sizeof(MLB_TEAMS) / sizeof(MLB_TEAMS[0]))

static const MlbTeam *team_by_id(int id) {
    for (int i = 0; i < NUM_TEAMS; i++)
        if (MLB_TEAMS[i].id == id) return &MLB_TEAMS[i];
    return NULL;
}
#define POLL_INTERVAL_SEC   30   /* live game: poll every 30s */
#define IDLE_INTERVAL_SEC   300  /* no game: poll every 5min */

#define MAX_URL             512
#define MAX_MSG             256
#define MAX_BUF             (512 * 1024)

/* ── Curl response buffer ───────────────────────────────────────── */
typedef struct {
    char  *data;
    size_t len;
} CurlBuf;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    CurlBuf *b = (CurlBuf *)userdata;
    size_t total = size * nmemb;
    char *tmp = realloc(b->data, b->len + total + 1);
    if (!tmp) return 0;
    b->data = tmp;
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = '\0';
    return total;
}

/* ── Global app state ───────────────────────────────────────────── */
typedef struct {
    /* config (from AXParameter) */
    char   device_user[64];
    char   device_pass[64];
    int    poll_interval_sec;
    int    notification_clip_id;
    int    score_clip_id;
    int    display_enabled;
    int    display_duration_ms;
    char   display_text_color[16];
    char   display_bg_color[16];
    char   display_text_size[16];
    int    display_scroll_speed;
    int    display_persist_final;
    int    enabled;

    /* team selection */
    int    team_id;
    char   team_name[64];

    /* live state */
    int    current_game_pk;
    int    my_score;
    int    opponent_score;
    char   opponent_name[64];
    char   game_state[32];   /* "Preview","Live","Final" */
    char   inning_state[32]; /* "Top","Bottom","Middle","End" */
    int    inning;
    int    outs;
    char   last_play[MAX_MSG];
    char   last_display_msg[MAX_MSG];
    time_t last_poll_time;
    int    is_live;

    pthread_mutex_t lock;
    AXParameter    *ax_params;
    CURL           *display_curl;
    CURL           *fetch_curl;
    int             running;
} AppState;

static AppState g_app = {
    .device_user         = "root",
    .device_pass         = "",
    .poll_interval_sec   = POLL_INTERVAL_SEC,
    .notification_clip_id = 38,
    .score_clip_id       = 38,
    .display_enabled     = 1,
    .display_duration_ms = 20000,
    .display_text_color  = "#FFFFFF",
    .display_bg_color    = "#041E42",
    .display_text_size   = "large",
    .display_scroll_speed = 3,
    .display_persist_final = 1,
    .enabled             = 1,
    .team_id             = 118,   /* Kansas City Royals default */
    .team_name           = "Royals",
    .current_game_pk     = 0,
    .my_score            = 0,
    .opponent_score      = 0,
    .opponent_name       = "Unknown",
    .game_state          = "No Game",
    .inning_state        = "",
    .inning              = 0,
    .outs                = 0,
    .last_play           = "",
    .last_display_msg    = "",
    .last_poll_time      = 0,
    .is_live             = 0,
    .running             = 1,
};

/* ── Logging ─────────────────────────────────────────────────────── */
static void app_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, APP_NAME ": ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ── HTTP fetch helper ───────────────────────────────────────────── */
static char *http_get(CURL *curl, const char *url) {
    CurlBuf buf = {NULL, 0};
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, APP_NAME "/1.0");
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        app_log("http_get failed: %s", curl_easy_strerror(rc));
        free(buf.data);
        return NULL;
    }
    return buf.data;  /* caller must free */
}

/* ── VAPIX display notification ──────────────────────────────────── */
static void display_show(const char *message) {
    if (!g_app.display_enabled) return;
    if (strncmp(message, g_app.last_display_msg, MAX_MSG - 1) == 0) return;
    strncpy(g_app.last_display_msg, message, MAX_MSG - 1);

    /* Build JSON body — C1710/C1720 speaker-display-notification v1 API
       expects: { "data": { "message", "duration": {"type","value"}, ... } } */
    char body[1024];
    snprintf(body, sizeof(body),
        "{\"data\":{\"message\":\"%s\","
        "\"duration\":{\"type\":\"time\",\"value\":%d},"
        "\"textColor\":\"%s\","
        "\"backgroundColor\":\"%s\","
        "\"scrollSpeed\":%d}}",
        message,
        g_app.display_duration_ms,
        g_app.display_text_color,
        g_app.display_bg_color,
        g_app.display_scroll_speed);

    curl_easy_reset(g_app.display_curl);
    curl_easy_setopt(g_app.display_curl, CURLOPT_URL, DISPLAY_API);
    curl_easy_setopt(g_app.display_curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(g_app.display_curl, CURLOPT_TIMEOUT, 5L);

    char cred[128];
    snprintf(cred, sizeof(cred), "%s:%s", g_app.device_user, g_app.device_pass);
    curl_easy_setopt(g_app.display_curl, CURLOPT_USERPWD, cred);
    curl_easy_setopt(g_app.display_curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(g_app.display_curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(g_app.display_curl);
    curl_slist_free_all(hdrs);

    if (rc != CURLE_OK)
        app_log("display failed: %s", curl_easy_strerror(rc));
    else
        app_log("display: %s", message);
}

/* ── VAPIX audio clip playback ───────────────────────────────────── */
static void play_clip(int clip_id) {
    char url[MAX_URL];
    snprintf(url, sizeof(url), MEDIACLIP_API, clip_id);

    CURL *c = curl_easy_init();
    if (!c) return;
    char cred[128];
    snprintf(cred, sizeof(cred), "%s:%s", g_app.device_user, g_app.device_pass);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERPWD, cred);
    curl_easy_setopt(c, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
    curl_easy_perform(c);
    curl_easy_cleanup(c);
}

/* ── Date helpers ────────────────────────────────────────────────── */
static void today_str(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(buf, n, "%Y-%m-%d", tm);
}

/* ── MLB API: find today's game for the selected team ────────────── */
static int fetch_game_pk(CURL *curl) {
    char date[16], url[MAX_URL];
    today_str(date, sizeof(date));
    snprintf(url, sizeof(url),
        MLB_API_BASE "/schedule?sportId=1&teamId=%d&date=%s&hydrate=team",
        g_app.team_id, date);

    char *body = http_get(curl, url);
    if (!body) return 0;

    int game_pk = 0;
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return 0;

    cJSON *dates = cJSON_GetObjectItem(root, "dates");
    if (cJSON_IsArray(dates) && cJSON_GetArraySize(dates) > 0) {
        cJSON *day   = cJSON_GetArrayItem(dates, 0);
        cJSON *games = cJSON_GetObjectItem(day, "games");
        if (cJSON_IsArray(games) && cJSON_GetArraySize(games) > 0) {
            cJSON *game   = cJSON_GetArrayItem(games, 0);
            cJSON *pk_obj = cJSON_GetObjectItem(game, "gamePk");
            if (cJSON_IsNumber(pk_obj))
                game_pk = (int)pk_obj->valuedouble;
        }
    }

    cJSON_Delete(root);
    return game_pk;
}

/* ── MLB API: parse live feed ────────────────────────────────────── */
typedef struct {
    int  my_score;
    int  opponent;
    char opponent_name[64];
    char state[32];      /* "Preview","Live","Final" */
    char inning_half[32];
    int  inning;
    int  outs;
    char last_play[MAX_MSG];
    int  my_team_scored; /* >0 if followed team scored on last play */
} LiveData;

static int parse_live_feed(const char *body, LiveData *out) {
    cJSON *root = cJSON_Parse(body);
    if (!root) return 0;

    memset(out, 0, sizeof(*out));

    /* game state */
    cJSON *gs = cJSON_GetObjectItemCaseSensitive(root, "gameData");
    if (gs) {
        cJSON *status = cJSON_GetObjectItem(gs, "status");
        if (status) {
            cJSON *ac = cJSON_GetObjectItem(status, "abstractGameState");
            if (cJSON_IsString(ac))
                strncpy(out->state, ac->valuestring, sizeof(out->state)-1);
        }
        /* team names */
        cJSON *teams = cJSON_GetObjectItem(gs, "teams");
        if (teams) {
            cJSON *away = cJSON_GetObjectItem(teams, "away");
            cJSON *home = cJSON_GetObjectItem(teams, "home");
            int away_id = 0;
            if (away) {
                cJSON *tid = cJSON_GetObjectItem(away, "id");
                if (cJSON_IsNumber(tid)) away_id = (int)tid->valuedouble;
            }
            /* opponent name */
            cJSON *opp = (away_id == g_app.team_id) ? home : away;
            if (opp) {
                cJSON *tn = cJSON_GetObjectItem(opp, "teamName");
                if (cJSON_IsString(tn))
                    strncpy(out->opponent_name, tn->valuestring, sizeof(out->opponent_name)-1);
            }
        }
    }

    /* live data (linescore + last play) */
    cJSON *ld = cJSON_GetObjectItemCaseSensitive(root, "liveData");
    if (ld) {
        /* scores */
        cJSON *ls = cJSON_GetObjectItem(ld, "linescore");
        if (ls) {
            cJSON *teams = cJSON_GetObjectItem(ls, "teams");
            if (teams) {
                cJSON *away = cJSON_GetObjectItem(teams, "away");
                cJSON *home = cJSON_GetObjectItem(teams, "home");
                /* figure out which side Royals */
                /* re-derive from gameData - simpler: check gamePk home team */
                /* Determine if followed team is home or away via gameData */
                cJSON *gd   = cJSON_GetObjectItemCaseSensitive(root, "gameData");
                int is_home = 0;
                if (gd) {
                    cJSON *t = cJSON_GetObjectItem(gd, "teams");
                    if (t) {
                        cJSON *h = cJSON_GetObjectItem(t, "home");
                        if (h) {
                            cJSON *id = cJSON_GetObjectItem(h, "id");
                            if (cJSON_IsNumber(id) && (int)id->valuedouble == g_app.team_id)
                                is_home = 1;
                        }
                    }
                }
                cJSON *my_side  = is_home ? home : away;
                cJSON *opp_side = is_home ? away : home;
                if (my_side) {
                    cJSON *r = cJSON_GetObjectItem(my_side, "runs");
                    if (cJSON_IsNumber(r)) out->my_score = (int)r->valuedouble;
                }
                if (opp_side) {
                    cJSON *r = cJSON_GetObjectItem(opp_side, "runs");
                    if (cJSON_IsNumber(r)) out->opponent = (int)r->valuedouble;
                }
            }
            /* inning */
            cJSON *inn = cJSON_GetObjectItem(ls, "currentInning");
            if (cJSON_IsNumber(inn)) out->inning = (int)inn->valuedouble;
            cJSON *half = cJSON_GetObjectItem(ls, "inningHalf");
            if (cJSON_IsString(half)) strncpy(out->inning_half, half->valuestring, sizeof(out->inning_half)-1);
            cJSON *outs = cJSON_GetObjectItem(ls, "outs");
            if (cJSON_IsNumber(outs)) out->outs = (int)outs->valuedouble;
        }

        /* last play description */
        cJSON *plays = cJSON_GetObjectItem(ld, "plays");
        if (plays) {
            cJSON *cp = cJSON_GetObjectItem(plays, "currentPlay");
            if (cp) {
                cJSON *result = cJSON_GetObjectItem(cp, "result");
                if (result) {
                    cJSON *desc = cJSON_GetObjectItem(result, "description");
                    if (cJSON_IsString(desc))
                        strncpy(out->last_play, desc->valuestring, MAX_MSG-1);
                    cJSON *rbi = cJSON_GetObjectItem(result, "rbi");
                    if (cJSON_IsNumber(rbi) && (int)rbi->valuedouble > 0)
                        out->my_team_scored = 1; /* approximate: may fire for opp too */
                }
            }
        }
    }

    cJSON_Delete(root);
    return 1;
}

/* ── Build display message ───────────────────────────────────────── */
static void build_score_message(const LiveData *d, char *buf, size_t n) {
    if (strcmp(d->state, "Live") == 0) {
        snprintf(buf, n,
            "%s %d - %s %d | %s %d | %d out%s",
            g_app.team_name,
            d->my_score,
            d->opponent_name,
            d->opponent,
            d->inning_half,
            d->inning,
            d->outs,
            d->outs == 1 ? "" : "s");
    } else if (strcmp(d->state, "Final") == 0) {
        const char *result = (d->my_score > d->opponent) ? "WIN"
                           : (d->my_score < d->opponent) ? "LOSS" : "TIE";
        snprintf(buf, n,
            "FINAL: %s %s %d - %s %d",
            g_app.team_name, result, d->my_score,
            d->opponent_name, d->opponent);
    } else {
        snprintf(buf, n, "%s vs %s - %s",
            g_app.team_name, d->opponent_name, d->state);
    }
}

/* ── Main poll loop ──────────────────────────────────────────────── */
static void poll_once(void) {
    if (!g_app.enabled) return;

    pthread_mutex_lock(&g_app.lock);

    /* Step 1: get today's game pk if we don't have one */
    if (g_app.current_game_pk == 0) {
        int pk = fetch_game_pk(g_app.fetch_curl);
        if (pk == 0) {
            app_log("no game today");
            pthread_mutex_unlock(&g_app.lock);
            return;
        }
        g_app.current_game_pk = pk;
        app_log("found game pk=%d", pk);
    }

    /* Step 2: fetch live feed */
    char url[MAX_URL];
    snprintf(url, sizeof(url),
        MLB_API_LIVE "/game/%d/feed/live", g_app.current_game_pk);

    char *body = http_get(g_app.fetch_curl, url);
    g_app.last_poll_time = time(NULL);

    if (!body) {
        pthread_mutex_unlock(&g_app.lock);
        return;
    }

    LiveData live;
    if (!parse_live_feed(body, &live)) {
        free(body);
        pthread_mutex_unlock(&g_app.lock);
        return;
    }
    free(body);

    /* Step 3: detect score change */
    int score_changed = (live.my_score  != g_app.my_score      ||
                         live.opponent  != g_app.opponent_score);
    int my_team_scored = score_changed && live.my_score > g_app.my_score;

    /* Update state */
    g_app.my_score       = live.my_score;
    g_app.opponent_score = live.opponent;
    g_app.inning         = live.inning;
    g_app.outs           = live.outs;
    g_app.is_live        = (strcmp(live.state, "Live") == 0);
    strncpy(g_app.game_state,    live.state,         sizeof(g_app.game_state)-1);
    strncpy(g_app.inning_state,  live.inning_half,   sizeof(g_app.inning_state)-1);
    strncpy(g_app.opponent_name, live.opponent_name, sizeof(g_app.opponent_name)-1);
    strncpy(g_app.last_play,     live.last_play,     sizeof(g_app.last_play)-1);

    /* Step 4: build and send display message */
    char msg[MAX_MSG];
    build_score_message(&live, msg, sizeof(msg));
    app_log("state=%s score=%s%d-%s%d inning=%s%d",
            live.state, g_app.team_name, live.my_score,
            live.opponent_name, live.opponent,
            live.inning_half, live.inning);

    pthread_mutex_unlock(&g_app.lock);

    int is_final = (strcmp(live.state, "Final") == 0);

    /* Always update display when live, score changed, or game just ended */
    if (g_app.is_live || score_changed || is_final)
        display_show(msg);

    /* Play clip on scoring plays */
    if (my_team_scored)
        play_clip(g_app.score_clip_id);
    else if (score_changed)
        play_clip(g_app.notification_clip_id);

    /* After final: either persist the score on display or just wait and reset */
    if (is_final) {
        if (g_app.display_persist_final && g_app.display_enabled) {
            /* Keep resending so the final score stays on screen.
               Loop until the local date rolls over (i.e. past midnight). */
            char game_date[16];
            today_str(game_date, sizeof(game_date));

            int resend_sec = (g_app.display_duration_ms / 1000) - 2;
            if (resend_sec < 5) resend_sec = 5;

            while (g_app.running && g_app.display_persist_final) {
                sleep(resend_sec);
                char now_date[16];
                today_str(now_date, sizeof(now_date));
                if (strcmp(now_date, game_date) != 0) break;  /* past midnight */
                g_app.last_display_msg[0] = '\0';
                display_show(msg);
            }
        } else {
            sleep(60);
        }
        g_app.current_game_pk = 0;
    }
}

/* ── Poll thread ─────────────────────────────────────────────────── */
static void *poll_thread(void *arg) {
    (void)arg;
    while (g_app.running) {
        poll_once();
        int interval = g_app.is_live
            ? g_app.poll_interval_sec
            : IDLE_INTERVAL_SEC;
        sleep(interval);
    }
    return NULL;
}

/* ── Tiny HTTP server (same pattern as rss_reader) ───────────────── */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void http_respond(int fd, int code, const char *ctype, const char *body) {
    const char *reason = (code == 200) ? "OK" : (code == 404) ? "Not Found" : "Bad Request";
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        code, reason, ctype, strlen(body));
    (void)write(fd, hdr, hlen);
    (void)write(fd, body, strlen(body));
}

static void handle_request(int fd) {
    char req[2048] = {0};
    int n = read(fd, req, sizeof(req)-1);
    if (n <= 0) return;

    /* Extract method and path */
    char method[8], raw_path[256];
    sscanf(req, "%7s %255s", method, raw_path);

    /* Axis reverse-proxy may forward the full URI including the
       /local/<appname>/api prefix.  Normalise so handler matching
       always sees paths starting with "/api/".                      */
    const char *prefix = "/local/mlb_scores";
    const char *path = raw_path;
    if (strncmp(raw_path, prefix, strlen(prefix)) == 0)
        path = raw_path + strlen(prefix);

    /* Read body for POSTs */
    char body_buf[4096] = {0};
    char *body_start = strstr(req, "\r\n\r\n");
    if (body_start && strcmp(method, "POST") == 0)
        strncpy(body_buf, body_start + 4, sizeof(body_buf)-1);

    /* ── GET /status ── */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
        pthread_mutex_lock(&g_app.lock);
        char ts[32] = "--";
        if (g_app.last_poll_time) {
            struct tm *tm = localtime(&g_app.last_poll_time);
            strftime(ts, sizeof(ts), "%H:%M:%S", tm);
        }
        char resp[1024];
        snprintf(resp, sizeof(resp),
            "{\"enabled\":%s,\"game_pk\":%d,"
            "\"team_id\":%d,\"team_name\":\"%s\","
            "\"my_score\":%d,\"opponent_score\":%d,"
            "\"opponent_name\":\"%s\","
            "\"game_state\":\"%s\","
            "\"inning_state\":\"%s\",\"inning\":%d,\"outs\":%d,"
            "\"last_play\":\"%s\","
            "\"last_poll_time\":\"%s\","
            "\"is_live\":%s}",
            g_app.enabled ? "true" : "false",
            g_app.current_game_pk,
            g_app.team_id, g_app.team_name,
            g_app.my_score, g_app.opponent_score,
            g_app.opponent_name,
            g_app.game_state,
            g_app.inning_state, g_app.inning, g_app.outs,
            g_app.last_play,
            ts,
            g_app.is_live ? "true" : "false");
        pthread_mutex_unlock(&g_app.lock);
        http_respond(fd, 200, "application/json", resp);
        return;
    }

    /* ── GET /config ── */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/config") == 0) {
        pthread_mutex_lock(&g_app.lock);
        char resp[1024];
        snprintf(resp, sizeof(resp),
            "{\"enabled\":\"%s\","
            "\"team_id\":\"%d\","
            "\"poll_interval_sec\":\"%d\","
            "\"notification_clip_id\":\"%d\","
            "\"score_clip_id\":\"%d\","
            "\"display_enabled\":\"%s\","
            "\"display_text_size\":\"%s\","
            "\"display_text_color\":\"%s\","
            "\"display_bg_color\":\"%s\","
            "\"display_scroll_speed\":\"%d\","
            "\"display_persist_final\":\"%s\","
            "\"display_duration_ms\":\"%d\","
            "\"device_user\":\"%s\","
            "\"device_pass_set\":%s}",
            g_app.enabled ? "true" : "false",
            g_app.team_id,
            g_app.poll_interval_sec,
            g_app.notification_clip_id,
            g_app.score_clip_id,
            g_app.display_enabled ? "true" : "false",
            g_app.display_text_size,
            g_app.display_text_color,
            g_app.display_bg_color,
            g_app.display_scroll_speed,
            g_app.display_persist_final ? "true" : "false",
            g_app.display_duration_ms,
            g_app.device_user,
            strlen(g_app.device_pass) > 0 ? "true" : "false");
        pthread_mutex_unlock(&g_app.lock);
        http_respond(fd, 200, "application/json", resp);
        return;
    }

    /* ── POST /config ── */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/config") == 0) {
        cJSON *j = cJSON_Parse(body_buf);
        if (j) {
            pthread_mutex_lock(&g_app.lock);
            cJSON *v;
#define SET_STR(key, field) \
    if ((v = cJSON_GetObjectItem(j, key)) && cJSON_IsString(v)) \
        strncpy(g_app.field, v->valuestring, sizeof(g_app.field)-1);
#define SET_INT_STR(key, field) \
    if ((v = cJSON_GetObjectItem(j, key)) && cJSON_IsString(v)) \
        g_app.field = atoi(v->valuestring);
#define SET_BOOL_STR(key, field) \
    if ((v = cJSON_GetObjectItem(j, key)) && cJSON_IsString(v)) \
        g_app.field = (strcmp(v->valuestring, "true") == 0) ? 1 : 0;

            SET_BOOL_STR("enabled",             enabled)
            SET_INT_STR("poll_interval_sec",    poll_interval_sec)
            SET_INT_STR("notification_clip_id", notification_clip_id)
            SET_INT_STR("score_clip_id",        score_clip_id)
            SET_BOOL_STR("display_enabled",     display_enabled)
            SET_STR("display_text_size",        display_text_size)
            SET_STR("display_text_color",       display_text_color)
            SET_STR("display_bg_color",         display_bg_color)
            SET_INT_STR("display_scroll_speed", display_scroll_speed)
            SET_BOOL_STR("display_persist_final", display_persist_final)
            SET_INT_STR("display_duration_ms",  display_duration_ms)
            SET_STR("device_user",              device_user)
            if ((v = cJSON_GetObjectItem(j, "device_pass")) && cJSON_IsString(v) && strlen(v->valuestring))
                strncpy(g_app.device_pass, v->valuestring, sizeof(g_app.device_pass)-1);
            /* Team selection — reset game_pk when team changes */
            if ((v = cJSON_GetObjectItem(j, "team_id")) && cJSON_IsString(v)) {
                int new_id = atoi(v->valuestring);
                if (new_id != g_app.team_id) {
                    g_app.team_id        = new_id;
                    g_app.current_game_pk = 0;  /* force re-lookup for new team */
                    g_app.my_score        = 0;
                    g_app.opponent_score  = 0;
                    g_app.is_live         = 0;
                    strncpy(g_app.game_state, "No Game", sizeof(g_app.game_state)-1);
                    const MlbTeam *t = team_by_id(new_id);
                    strncpy(g_app.team_name, t ? t->name : "Unknown", sizeof(g_app.team_name)-1);
                }
            }

            /* Persist to AXParameter */
            AXParameter *p = g_app.ax_params;
            char tmp[32];
#define AXSET(name, val) ax_parameter_set(p, name, val, TRUE, NULL)
            AXSET("Enabled",              g_app.enabled ? "true" : "false");
            snprintf(tmp, sizeof(tmp), "%d", g_app.team_id);
            AXSET("TeamId",               tmp);
            snprintf(tmp, sizeof(tmp), "%d", g_app.poll_interval_sec);
            AXSET("PollIntervalSec",      tmp);
            snprintf(tmp, sizeof(tmp), "%d", g_app.notification_clip_id);
            AXSET("NotificationClipId",   tmp);
            snprintf(tmp, sizeof(tmp), "%d", g_app.score_clip_id);
            AXSET("ScoreClipId",          tmp);
            AXSET("DisplayEnabled",       g_app.display_enabled ? "true" : "false");
            AXSET("DisplayTextSize",      g_app.display_text_size);
            AXSET("DisplayTextColor",     g_app.display_text_color);
            AXSET("DisplayBgColor",       g_app.display_bg_color);
            snprintf(tmp, sizeof(tmp), "%d", g_app.display_scroll_speed);
            AXSET("DisplayScrollSpeed",   tmp);
            AXSET("DisplayPersistFinal",  g_app.display_persist_final ? "true" : "false");
            snprintf(tmp, sizeof(tmp), "%d", g_app.display_duration_ms);
            AXSET("DisplayDurationMs",    tmp);
            AXSET("DeviceUser",           g_app.device_user);
            if (strlen(g_app.device_pass))
                AXSET("DevicePass",       g_app.device_pass);

            pthread_mutex_unlock(&g_app.lock);
            cJSON_Delete(j);
        }
        http_respond(fd, 200, "application/json", "{\"message\":\"Saved\"}");
        return;
    }

    /* ── POST /test_display ── */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/test_display") == 0) {
        char tmsg[MAX_MSG];
        snprintf(tmsg, sizeof(tmsg), "MLB SCORES TEST: %s Score Display", g_app.team_name);
        display_show(tmsg);
        http_respond(fd, 200, "application/json", "{\"message\":\"ok\"}");
        return;
    }

    /* ── POST /test_audio ── */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/test_audio") == 0) {
        play_clip(g_app.notification_clip_id);
        http_respond(fd, 200, "application/json", "{\"message\":\"ok\"}");
        return;
    }

    /* ── GET /clips ── */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/clips") == 0) {
        /* Proxy VAPIX mediaclip list — set credentials after http_get's
           curl_easy_reset so they aren't wiped.                          */
        char url[MAX_URL];
        snprintf(url, sizeof(url),
            "http://127.0.0.1/axis-cgi/param.cgi?action=list&group=root.MediaClip");
        CURL *c = curl_easy_init();
        CurlBuf buf = {NULL, 0};
        char cred[128];
        snprintf(cred, sizeof(cred), "%s:%s", g_app.device_user, g_app.device_pass);
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, APP_NAME "/1.0");
        curl_easy_setopt(c, CURLOPT_USERPWD, cred);
        curl_easy_setopt(c, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
        CURLcode rc = curl_easy_perform(c);
        char *raw = (rc == CURLE_OK) ? buf.data : NULL;
        if (rc != CURLE_OK) free(buf.data);
        curl_easy_cleanup(c);

        /* Convert VAPIX param output to JSON clip list */
        cJSON *root = cJSON_CreateObject();
        cJSON *arr  = cJSON_AddArrayToObject(root, "clips");
        if (raw) {
            char *line = strtok(raw, "\n");
            while (line) {
                /* Line format varies by model: root.MediaClip.C1.Name=...
                   or root.MediaClip.M36.Name=... — skip the letter prefix */
                int idx; char name[128]; char prefix;
                if (sscanf(line, "root.MediaClip.%c%d.Name=%127[^\r]", &prefix, &idx, name) == 3) {
                    cJSON *clip = cJSON_CreateObject();
                    cJSON_AddNumberToObject(clip, "id", idx);
                    cJSON_AddStringToObject(clip, "name", name);
                    cJSON_AddItemToArray(arr, clip);
                }
                line = strtok(NULL, "\n");
            }
            free(raw);
        }
        char *out = cJSON_PrintUnformatted(root);
        http_respond(fd, 200, "application/json", out ? out : "{}");
        free(out);
        cJSON_Delete(root);
        return;
    }

    /* ── GET /teams ── */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/teams") == 0) {
        /* Build JSON array of all teams sorted by name */
        cJSON *root = cJSON_CreateObject();
        cJSON *arr  = cJSON_AddArrayToObject(root, "teams");
        for (int i = 0; i < NUM_TEAMS; i++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddNumberToObject(t, "id",   MLB_TEAMS[i].id);
            cJSON_AddStringToObject(t, "name", MLB_TEAMS[i].name);
            cJSON_AddStringToObject(t, "abbr", MLB_TEAMS[i].abbr);
            cJSON_AddItemToArray(arr, t);
        }
        char *out = cJSON_PrintUnformatted(root);
        http_respond(fd, 200, "application/json", out ? out : "{}");
        free(out);
        cJSON_Delete(root);
        return;
    }

    http_respond(fd, 404, "application/json", "{\"error\":\"not found\"}");
}

static void *http_thread(void *arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port        = htons(HTTP_PORT),
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        app_log("bind failed: %s", strerror(errno));
        return NULL;
    }
    listen(srv, 8);
    app_log("HTTP server on port %d", HTTP_PORT);

    while (g_app.running) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        handle_request(fd);
        close(fd);
    }
    close(srv);
    return NULL;
}

/* ── Load config from AXParameter ───────────────────────────────── */
static void load_params(void) {
    AXParameter *p = g_app.ax_params;
    char *v = NULL;

#define GETSTR(name, field) \
    if (ax_parameter_get(p, name, &v, NULL) && v) { \
        strncpy(g_app.field, v, sizeof(g_app.field)-1); g_free(v); v=NULL; }
#define GETINT(name, field) \
    if (ax_parameter_get(p, name, &v, NULL) && v) { \
        g_app.field = atoi(v); g_free(v); v=NULL; }
#define GETINT_NZ(name, field) \
    if (ax_parameter_get(p, name, &v, NULL) && v) { \
        int _tmp = atoi(v); if (_tmp) g_app.field = _tmp; g_free(v); v=NULL; }
#define GETBOOL(name, field) \
    if (ax_parameter_get(p, name, &v, NULL) && v) { \
        g_app.field = (strcmp(v,"true")==0); g_free(v); v=NULL; }

    GETBOOL("Enabled",            enabled)
    GETINT("TeamId",              team_id)
    GETINT("PollIntervalSec",     poll_interval_sec)
    GETINT_NZ("NotificationClipId",  notification_clip_id)
    GETINT_NZ("ScoreClipId",         score_clip_id)
    GETBOOL("DisplayEnabled",     display_enabled)
    GETSTR("DisplayTextSize",     display_text_size)
    GETSTR("DisplayTextColor",    display_text_color)
    GETSTR("DisplayBgColor",      display_bg_color)
    GETINT("DisplayScrollSpeed",  display_scroll_speed)
    GETBOOL("DisplayPersistFinal", display_persist_final)
    GETINT("DisplayDurationMs",   display_duration_ms)
    GETSTR("DeviceUser",          device_user)
    GETSTR("DevicePass",          device_pass)

    /* Resolve team name from loaded team_id */
    const MlbTeam *t = team_by_id(g_app.team_id);
    if (t) strncpy(g_app.team_name, t->name, sizeof(g_app.team_name)-1);
}

/* ── Entry point ─────────────────────────────────────────────────── */
int main(void) {
    app_log("starting");

    curl_global_init(CURL_GLOBAL_ALL);
    pthread_mutex_init(&g_app.lock, NULL);

    /* AXParameter */
    g_app.ax_params = ax_parameter_new(APP_NAME, NULL);
    if (!g_app.ax_params)
        app_log("warning: ax_parameter_new failed — using defaults");
    else
        load_params();

    /* CURL handles */
    g_app.fetch_curl   = curl_easy_init();
    g_app.display_curl = curl_easy_init();

    app_log("running (port=%d, poll=%ds)", HTTP_PORT, g_app.poll_interval_sec);

    /* Start threads */
    pthread_t poll_tid, http_tid;
    pthread_create(&poll_tid, NULL, poll_thread, NULL);
    pthread_create(&http_tid, NULL, http_thread, NULL);

    pthread_join(poll_tid, NULL);
    pthread_join(http_tid, NULL);

    curl_easy_cleanup(g_app.fetch_curl);
    curl_easy_cleanup(g_app.display_curl);
    curl_global_cleanup();
    if (g_app.ax_params) ax_parameter_free(g_app.ax_params);
    pthread_mutex_destroy(&g_app.lock);
    return 0;
}
