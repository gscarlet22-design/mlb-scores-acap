#define _GNU_SOURCE   /* timegm() */
/*
 * mlb_scores - MLB live score ACAP v1.1.1
 * Vendor: gscarlet22 design for Axis C1720 / C1710
 *
 * v1.1.1 changes from v1.1.0:
 *   - Per-team display settings: text color, background color, text size,
 *     scroll speed, and duration are now stored and applied per team.
 *     Defaults to the team's official MLB colors when not set.
 *   - Test Audio button restored: POST /api/test_audio accepts {"team_id":N}
 *     to play that team's notify clip.
 *   - Fixed last_play double-escaping in status handler (cJSON handles
 *     JSON escaping internally; manual json_escape was redundant).
 *
 * API used:
 *   Schedule: https://statsapi.mlb.com/api/v1/schedule?sportId=1&teamId=<id>&date=YYYY-MM-DD
 *   Live feed: https://statsapi.mlb.com/api/v1.1/game/{gamePk}/feed/live
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>

#include <curl/curl.h>
#include "cJSON.h"
#include <axsdk/axparameter.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Constants ──────────────────────────────────────────────────── */
#define APP_NAME            "mlb_scores"
#define MLB_API_BASE        "https://statsapi.mlb.com/api/v1"
#define MLB_API_LIVE        "https://statsapi.mlb.com/api/v1.1"

#define DISPLAY_API         "https://127.0.0.1/config/rest/speaker-display-notification/v1/simple"
#define DISPLAY_STOP_API    "https://127.0.0.1/config/rest/speaker-display-notification/v1/stop"
#define MEDIACLIP_API       "http://127.0.0.1/axis-cgi/mediaclip.cgi?action=play&clip=%d"

#define HTTP_PORT           2016
#define MAX_TEAMS           8
#define MAX_URL             512
#define MAX_MSG             256
#define MAX_BUF             (512 * 1024)

/* ── MLB team lookup table ──────────────────────────────────────── */
typedef struct { int id; const char *name; const char *abbr; const char *bg; const char *fg; const char *strobe; } MlbTeam;
static const MlbTeam MLB_TEAMS[] = {
    /* id   name             abbr   bg         fg         strobe (third/accent color) */
    {108, "Angels",          "LAA", "#BA0021", "#C4CED4", "#B9975B"},  /* Vegas Gold  */
    {109, "Diamondbacks",    "ARI", "#A71930", "#E3D4AD", "#000000"},  /* Black       */
    {110, "Orioles",         "BAL", "#DF4601", "#000000", "#FFFFFF"},  /* White       */
    {111, "Red Sox",         "BOS", "#BD3039", "#0D2B56", "#FFFFFF"},  /* White       */
    {112, "Cubs",            "CHC", "#0E3386", "#CC3433", "#FFFFFF"},  /* White       */
    {113, "Reds",            "CIN", "#C6011F", "#FFFFFF", "#000000"},  /* Black       */
    {114, "Guardians",       "CLE", "#00385D", "#E31937", "#FFFFFF"},  /* White       */
    {115, "Rockies",         "COL", "#333366", "#C4CED4", "#000000"},  /* Black       */
    {116, "Tigers",          "DET", "#0C2340", "#FA4616", "#FFFFFF"},  /* White       */
    {117, "Astros",          "HOU", "#002D62", "#EB6E1F", "#C4CED4"},  /* Space Gray  */
    {118, "Royals",          "KC",  "#004687", "#C09A5B", "#FFFFFF"},  /* White       */
    {119, "Dodgers",         "LAD", "#005A9C", "#FFFFFF", "#A5ACAF"},  /* Gray        */
    {120, "Nationals",       "WSH", "#AB0003", "#14225A", "#B7A564"},  /* Gold        */
    {121, "Mets",            "NYM", "#002D72", "#FF5910", "#FFFFFF"},  /* White       */
    {133, "Athletics",       "OAK", "#003831", "#EFB21E", "#FFFFFF"},  /* White       */
    {134, "Pirates",         "PIT", "#27251F", "#FDB827", "#FFFFFF"},  /* White       */
    {135, "Padres",          "SD",  "#2F241D", "#FFC425", "#002D62"},  /* Navy        */
    {136, "Mariners",        "SEA", "#0C2C56", "#005C5C", "#C4CED4"},  /* Silver      */
    {137, "Giants",          "SF",  "#FD5A1E", "#27251F", "#EDE0C4"},  /* Cream       */
    {138, "Cardinals",       "STL", "#C41E3A", "#FEDB00", "#14225A"},  /* Navy        */
    {139, "Rays",            "TB",  "#092C5C", "#8FBCE6", "#F5D130"},  /* Gold        */
    {140, "Rangers",         "TEX", "#003278", "#C0111F", "#FFFFFF"},  /* White       */
    {141, "Blue Jays",       "TOR", "#134A8E", "#E8291C", "#FFFFFF"},  /* White       */
    {142, "Twins",           "MIN", "#002B5C", "#D31145", "#CBA141"},  /* Gold        */
    {143, "Phillies",        "PHI", "#E81828", "#002D72", "#FFFFFF"},  /* White       */
    {144, "Braves",          "ATL", "#CE1141", "#13274F", "#EAAA00"},  /* Gold        */
    {145, "White Sox",       "CWS", "#27251F", "#C4CED4", "#FFFFFF"},  /* White       */
    {146, "Marlins",         "MIA", "#00A3E0", "#EF3340", "#FFD400"},  /* Yellow      */
    {147, "Yankees",         "NYY", "#0C2340", "#C4CED4", "#FFFFFF"},  /* White       */
    {158, "Brewers",         "MIL", "#FFC52F", "#12284B", "#FFFFFF"},  /* White       */
};
#define NUM_MLB_TEAMS (int)(sizeof(MLB_TEAMS) / sizeof(MLB_TEAMS[0]))

static const MlbTeam *team_by_id(int id) {
    for (int i = 0; i < NUM_MLB_TEAMS; i++)
        if (MLB_TEAMS[i].id == id) return &MLB_TEAMS[i];
    return NULL;
}

#define IDLE_INTERVAL_SEC   300  /* no live game: poll every 5min */

/* ── Curl response buffer ───────────────────────────────────────── */
typedef struct { char *data; size_t len; } CurlBuf;

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

/* ── Per-team config (user-configured) ─────────────────────────── */
typedef struct {
    int  team_id;
    int  notify_clip_id;
    int  score_clip_id;
    int  audio_enabled;    /* 1 = play sounds for this team, 0 = silent */
    /* Per-team display settings */
    char text_color[16];   /* e.g. "#FFFFFF" */
    char bg_color[16];     /* e.g. "#004687" */
    char text_size[16];    /* "small" / "medium" / "large" */
    int  scroll_speed;     /* 1–5 */
    int  duration_ms;      /* display duration in milliseconds */
} TeamConfig;

/* ── Per-team live state ────────────────────────────────────────── */
typedef struct {
    int  team_id;
    char team_name[64];

    /* live game */
    int  current_game_pk;
    int  my_score;
    int  opponent_score;
    char opponent_name[64];
    char game_state[32];    /* "Live", "Final", "No Game", etc. */
    char inning_state[32];  /* "Top", "Bottom", etc. */
    int  inning;
    int  outs;
    char last_play[MAX_MSG];
    time_t last_poll_time;
    int  is_live;

    /* change-detection sentinels (display only fires on change) */
    int  last_my_score;
    int  last_opponent_score;
    int  last_inning;
    char last_inning_state[32];

    /* Final score persistence */
    time_t final_detected_at;  /* 0 = no final yet */

    /* next game preview */
    char next_game_opponent[64];
    char next_game_date[32];
    char next_game_time[32];
    int  next_game_home;

    /* set to the game_pk of the last completed game so we don't re-detect
       the same Final game when display_persist_final is off */
    int  last_completed_game_pk;
} TeamState;

/* ── Global app state ───────────────────────────────────────────── */
typedef struct {
    /* team roster */
    int        num_teams;
    TeamConfig teams[MAX_TEAMS];
    TeamState  state[MAX_TEAMS];

    /* shared settings */
    char device_user[64];
    char device_pass[64];
    int  poll_interval_sec;
    int  enabled;
    int  display_enabled;
    int  display_persist_final;
    int  audio_volume;          /* 0–100, applies to AudioOutput.A0.Volume */
    int  strobe_enabled;        /* 1 = flash strobe on run scored */

    /* Siren & Light API state (populated by init_strobe at startup) */
    int  strobe_api_available;
    int  num_strobe_colors;     /* 0 = full RGB; >0 = fixed palette */
    char strobe_colors[16][16]; /* palette from getCapabilities */

    pthread_mutex_t lock;
    AXParameter    *ax_params;
    CURL           *fetch_curl;
    int             running;
} AppState;

static AppState g_app = {
    .num_teams           = 0,
    .device_user         = "root",
    .device_pass         = "",
    .poll_interval_sec   = 30,
    .enabled             = 1,
    .display_enabled     = 1,
    .display_persist_final = 1,
    .audio_volume        = 75,
    .strobe_enabled      = 1,
    .strobe_api_available = 0,
    .num_strobe_colors   = 0,
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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, APP_NAME "/1.1");
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        app_log("http_get failed: %s", curl_easy_strerror(rc));
        free(buf.data);
        return NULL;
    }
    return buf.data;  /* caller must free */
}

/* ── Self-contained MD5 (RFC 1321) — kept for reference ─────────── */
typedef struct { uint32_t s[4]; uint32_t n[2]; uint8_t buf[64]; } MD5CTX;
static void md5_init(MD5CTX *c) {
    c->s[0]=0x67452301; c->s[1]=0xefcdab89; c->s[2]=0x98badcfe; c->s[3]=0x10325476;
    c->n[0]=c->n[1]=0;
}
static void md5_transform(uint32_t s[4], const uint8_t *b) {
    #define F(x,y,z) ((x&y)|(~x&z))
    #define G(x,y,z) ((x&z)|(y&~z))
    #define H(x,y,z) (x^y^z)
    #define I(x,y,z) (y^(x|~z))
    #define ROL(x,n) ((x<<n)|(x>>(32-n)))
    #define FF(a,b,c,d,x,s,t) a=b+ROL(a+F(b,c,d)+x+t,s)
    #define GG(a,b,c,d,x,s,t) a=b+ROL(a+G(b,c,d)+x+t,s)
    #define HH(a,b,c,d,x,s,t) a=b+ROL(a+H(b,c,d)+x+t,s)
    #define II(a,b,c,d,x,s,t) a=b+ROL(a+I(b,c,d)+x+t,s)
    uint32_t a=s[0],bv=s[1],cv=s[2],dv=s[3],x[16];
    for(int i=0;i<16;i++) x[i]=(uint32_t)b[i*4]|((uint32_t)b[i*4+1]<<8)|((uint32_t)b[i*4+2]<<16)|((uint32_t)b[i*4+3]<<24);
    FF(a,bv,cv,dv,x[0],7,0xd76aa478);FF(dv,a,bv,cv,x[1],12,0xe8c7b756);FF(cv,dv,a,bv,x[2],17,0x242070db);FF(bv,cv,dv,a,x[3],22,0xc1bdceee);
    FF(a,bv,cv,dv,x[4],7,0xf57c0faf);FF(dv,a,bv,cv,x[5],12,0x4787c62a);FF(cv,dv,a,bv,x[6],17,0xa8304613);FF(bv,cv,dv,a,x[7],22,0xfd469501);
    FF(a,bv,cv,dv,x[8],7,0x698098d8);FF(dv,a,bv,cv,x[9],12,0x8b44f7af);FF(cv,dv,a,bv,x[10],17,0xffff5bb1);FF(bv,cv,dv,a,x[11],22,0x895cd7be);
    FF(a,bv,cv,dv,x[12],7,0x6b901122);FF(dv,a,bv,cv,x[13],12,0xfd987193);FF(cv,dv,a,bv,x[14],17,0xa679438e);FF(bv,cv,dv,a,x[15],22,0x49b40821);
    GG(a,bv,cv,dv,x[1],5,0xf61e2562);GG(dv,a,bv,cv,x[6],9,0xc040b340);GG(cv,dv,a,bv,x[11],14,0x265e5a51);GG(bv,cv,dv,a,x[0],20,0xe9b6c7aa);
    GG(a,bv,cv,dv,x[5],5,0xd62f105d);GG(dv,a,bv,cv,x[10],9,0x02441453);GG(cv,dv,a,bv,x[15],14,0xd8a1e681);GG(bv,cv,dv,a,x[4],20,0xe7d3fbc8);
    GG(a,bv,cv,dv,x[9],5,0x21e1cde6);GG(dv,a,bv,cv,x[14],9,0xc33707d6);GG(cv,dv,a,bv,x[3],14,0xf4d50d87);GG(bv,cv,dv,a,x[8],20,0x455a14ed);
    GG(a,bv,cv,dv,x[13],5,0xa9e3e905);GG(dv,a,bv,cv,x[2],9,0xfcefa3f8);GG(cv,dv,a,bv,x[7],14,0x676f02d9);GG(bv,cv,dv,a,x[12],20,0x8d2a4c8a);
    HH(a,bv,cv,dv,x[5],4,0xfffa3942);HH(dv,a,bv,cv,x[8],11,0x8771f681);HH(cv,dv,a,bv,x[11],16,0x6d9d6122);HH(bv,cv,dv,a,x[14],23,0xfde5380c);
    HH(a,bv,cv,dv,x[1],4,0xa4beea44);HH(dv,a,bv,cv,x[4],11,0x4bdecfa9);HH(cv,dv,a,bv,x[7],16,0xf6bb4b60);HH(bv,cv,dv,a,x[10],23,0xbebfbc70);
    HH(a,bv,cv,dv,x[13],4,0x289b7ec6);HH(dv,a,bv,cv,x[0],11,0xeaa127fa);HH(cv,dv,a,bv,x[3],16,0xd4ef3085);HH(bv,cv,dv,a,x[6],23,0x04881d05);
    HH(a,bv,cv,dv,x[9],4,0xd9d4d039);HH(dv,a,bv,cv,x[12],11,0xe6db99e5);HH(cv,dv,a,bv,x[15],16,0x1fa27cf8);HH(bv,cv,dv,a,x[2],23,0xc4ac5665);
    II(a,bv,cv,dv,x[0],6,0xf4292244);II(dv,a,bv,cv,x[7],10,0x432aff97);II(cv,dv,a,bv,x[14],15,0xab9423a7);II(bv,cv,dv,a,x[5],21,0xfc93a039);
    II(a,bv,cv,dv,x[12],6,0x655b59c3);II(dv,a,bv,cv,x[3],10,0x8f0ccc92);II(cv,dv,a,bv,x[10],15,0xffeff47d);II(bv,cv,dv,a,x[1],21,0x85845dd1);
    II(a,bv,cv,dv,x[8],6,0x6fa87e4f);II(dv,a,bv,cv,x[15],10,0xfe2ce6e0);II(cv,dv,a,bv,x[6],15,0xa3014314);II(bv,cv,dv,a,x[13],21,0x4e0811a1);
    II(a,bv,cv,dv,x[4],6,0xf7537e82);II(dv,a,bv,cv,x[11],10,0xbd3af235);II(cv,dv,a,bv,x[2],15,0x2ad7d2bb);II(bv,cv,dv,a,x[9],21,0xeb86d391);
    s[0]+=a; s[1]+=bv; s[2]+=cv; s[3]+=dv;
}
static void md5_update(MD5CTX *c, const uint8_t *d, size_t len) {
    uint32_t idx=(c->n[0]>>3)&0x3f;
    if((c->n[0]+=len<<3)<(uint32_t)(len<<3))c->n[1]++;
    c->n[1]+=len>>29;
    uint32_t part=64-idx;
    size_t i=0;
    if(len>=part){memcpy(&c->buf[idx],d,part);md5_transform(c->s,c->buf);for(i=part;i+63<len;i+=64)md5_transform(c->s,d+i);idx=0;}
    memcpy(&c->buf[idx],d+i,len-i);
}
static void md5_final(uint8_t out[16], MD5CTX *c) {
    uint8_t pad[64]={0x80};
    uint8_t bits[8];
    for(int i=0;i<4;i++){bits[i]=(uint8_t)(c->n[0]>>(i*8));bits[i+4]=(uint8_t)(c->n[1]>>(i*8));}
    uint32_t idx=(c->n[0]>>3)&0x3f;
    md5_update(c,pad,idx<56?56-idx:120-idx);
    md5_update(c,bits,8);
    for(int i=0;i<4;i++){out[i*4]=(uint8_t)(c->s[i]);out[i*4+1]=(uint8_t)(c->s[i]>>8);out[i*4+2]=(uint8_t)(c->s[i]>>16);out[i*4+3]=(uint8_t)(c->s[i]>>24);}
}
static void md5hex(const char *s, char out[33]) {
    MD5CTX c; md5_init(&c);
    md5_update(&c,(const uint8_t*)s,strlen(s));
    uint8_t d[16]; md5_final(d,&c);
    for(int i=0;i<16;i++) snprintf(out+i*2,3,"%02x",d[i]);
    out[32]='\0';
}
/* suppress unused-function warning */
static void _md5hex_unused(void) { char o[33]; md5hex("x",o); (void)o; }

/* ── VAPIX display notification ──────────────────────────────────── */
/* cfg supplies per-team display settings; pass NULL to use safe defaults */
static long display_show_ex(const char *message, const TeamConfig *cfg,
                             char *resp_out, size_t resp_sz) {
    const char *text_color = (cfg && cfg->text_color[0]) ? cfg->text_color : "#FFFFFF";
    const char *bg_color   = (cfg && cfg->bg_color[0])   ? cfg->bg_color   : "#041E42";
    const char *text_size  = (cfg && cfg->text_size[0])  ? cfg->text_size  : "large";
    int scroll_speed = (cfg && cfg->scroll_speed > 0) ? cfg->scroll_speed : 3;
    int duration_ms  = (cfg && cfg->duration_ms  > 0) ? cfg->duration_ms  : 20000;

    char body[1024];
    snprintf(body, sizeof(body),
        "{\"data\":{"
        "\"message\":\"%s\","
        "\"textColor\":\"%s\","
        "\"backgroundColor\":\"%s\","
        "\"textSize\":\"%s\","
        "\"scrollSpeed\":%d,"
        "\"scrollDirection\":\"fromRightToLeft\","
        "\"duration\":{\"type\":\"time\",\"value\":%d}"
        "}}",
        message,
        text_color,
        bg_color,
        text_size,
        scroll_speed,
        duration_ms);

    app_log("display body: %s", body);

    CURL *dc = curl_easy_init();
    if (!dc) {
        if (resp_out) snprintf(resp_out, resp_sz, "curl_easy_init failed");
        return -1;
    }

    char cred[128];
    snprintf(cred, sizeof(cred), "%s:%s", g_app.device_user, g_app.device_pass);

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    CurlBuf resp_buf = {NULL, 0};
    curl_easy_setopt(dc, CURLOPT_URL, DISPLAY_API);
    curl_easy_setopt(dc, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(dc, CURLOPT_COPYPOSTFIELDS, body);
    curl_easy_setopt(dc, CURLOPT_USERPWD, cred);
    curl_easy_setopt(dc, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(dc, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(dc, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(dc, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(dc, CURLOPT_WRITEDATA, &resp_buf);
    curl_easy_setopt(dc, CURLOPT_TIMEOUT, 5L);

    CURLcode rc = curl_easy_perform(dc);
    long http_code = -1;
    char *effective_url = NULL;
    curl_easy_getinfo(dc, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_getinfo(dc, CURLINFO_EFFECTIVE_URL, &effective_url);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(dc);

    if (rc != CURLE_OK) {
        app_log("display curl error: %s", curl_easy_strerror(rc));
        if (resp_out) snprintf(resp_out, resp_sz,
            "curl_error=%s sent_body=%s", curl_easy_strerror(rc), body);
    } else {
        app_log("display http=%ld url=%s body=%s", http_code,
                effective_url ? effective_url : "?",
                resp_buf.data ? resp_buf.data : "(empty)");
        if (resp_out) snprintf(resp_out, resp_sz,
            "http=%ld effective_url=%s sent_body=%s api_resp=%s",
            http_code, effective_url ? effective_url : "?",
            body, resp_buf.data ? resp_buf.data : "(empty)");
    }
    free(resp_buf.data);
    return (rc == CURLE_OK) ? http_code : -1;
}

/* ── Audio Device Control: set output gain (dB) ─────────────────── */
/* Gain range on C1720 internal output: -95 dB (mute) to 0 dB (max).
   This call is synchronous — caller spawns a restore thread as needed. */
static void set_output_gain(int gain_db) {
    if (gain_db > 0)   gain_db = 0;
    if (gain_db < -95) gain_db = -95;

    char cred[128];
    snprintf(cred, sizeof(cred), "%s:%s", g_app.device_user, g_app.device_pass);

    char body[384];
    snprintf(body, sizeof(body),
        "{\"apiVersion\":\"1.0\",\"method\":\"setDevicesSettings\","
        "\"params\":{\"devices\":[{\"id\":\"0\",\"outputs\":[{\"id\":\"0\","
        "\"connectionType\":{\"id\":\"internal\","
        "\"signalingType\":{\"id\":\"unbalanced\",\"gain\":%d}}}]}]}}",
        gain_db);

    CURL *c = curl_easy_init();
    if (!c) return;
    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    CurlBuf buf = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL,           "http://127.0.0.1/axis-cgi/audiodevicecontrol.cgi");
    curl_easy_setopt(c, CURLOPT_USERPWD,       cred);
    curl_easy_setopt(c, CURLOPT_HTTPAUTH,      CURLAUTH_DIGEST);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS,    body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       5L);
    CURLcode rc = curl_easy_perform(c);
    long http_code = -1;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(c);
    curl_slist_free_all(hdrs);
    app_log("set_output_gain db=%d http=%ld rc=%d resp=%s",
            gain_db, http_code, rc, buf.data ? buf.data : "(none)");
    free(buf.data);
}

/* Detached thread: sleep delay_sec then restore gain to 0 dB (max).
   Ensures emergency announcements return to full volume after a clip. */
typedef struct { int delay_sec; } RestoreGainArg;
static void *restore_gain_thread(void *arg) {
    RestoreGainArg *ra = arg;
    sleep((unsigned int)ra->delay_sec);
    free(ra);
    set_output_gain(0);   /* restore to max */
    return NULL;
}

#define CLIP_RESTORE_SEC 20   /* seconds before gain is restored to 0 dB */

/* ── VAPIX audio clip playback ───────────────────────────────────── */
static long play_clip_ex(int clip_id, char *resp_out, size_t resp_sz) {
    /* Map 0-100% slider to -95..0 dB: 0% → -95 dB, 100% → 0 dB (linear in dB). */
    int vol = g_app.audio_volume;
    if (vol < 0) vol = 0; if (vol > 100) vol = 100;
    int gain_db = -95 + (vol * 95 / 100);
    set_output_gain(gain_db);

    /* Spawn a detached thread to restore gain to 0 dB after the clip finishes */
    RestoreGainArg *ra = malloc(sizeof(RestoreGainArg));
    if (ra) {
        ra->delay_sec = CLIP_RESTORE_SEC;
        pthread_t t;
        if (pthread_create(&t, NULL, restore_gain_thread, ra) == 0)
            pthread_detach(t);
        else
            free(ra);
    }

    char url[MAX_URL];
    snprintf(url, sizeof(url), MEDIACLIP_API, clip_id);

    CURL *c = curl_easy_init();
    if (!c) {
        if (resp_out) snprintf(resp_out, resp_sz, "curl_easy_init failed");
        return -1;
    }
    char cred[128];
    snprintf(cred, sizeof(cred), "%s:%s", g_app.device_user, g_app.device_pass);

    CurlBuf buf = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERPWD, cred);
    curl_easy_setopt(c, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);

    CURLcode rc = curl_easy_perform(c);
    long http_code = -1;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        app_log("play_clip curl error: %s", curl_easy_strerror(rc));
        if (resp_out) snprintf(resp_out, resp_sz, "curl error: %s", curl_easy_strerror(rc));
    } else {
        app_log("play_clip id=%d http=%ld body=%s", clip_id, http_code,
                buf.data ? buf.data : "(empty)");
        if (resp_out) snprintf(resp_out, resp_sz,
            "http=%ld body=%s", http_code, buf.data ? buf.data : "(empty)");
    }
    free(buf.data);
    return (rc == CURLE_OK) ? http_code : -1;
}

static void play_clip(int clip_id) { play_clip_ex(clip_id, NULL, 0); }

/* ── Strobe / Siren-and-Light support ───────────────────────────── */

static void hex_to_rgb(const char *hex, int *r, int *g, int *b) {
    const char *h = (hex && hex[0] == '#') ? hex + 1 : (hex ? hex : "000000");
    unsigned rv = 0, gv = 0, bv = 0;
    sscanf(h, "%2x%2x%2x", &rv, &gv, &bv);
    *r = (int)rv; *g = (int)gv; *b = (int)bv;
}

/* Returns pointer to the nearest palette entry, or hex itself if palette is empty */
static const char *closest_strobe_color(const char *hex) {
    if (g_app.num_strobe_colors == 0) return hex;   /* full RGB device */
    int r, g, b;
    hex_to_rgb(hex, &r, &g, &b);
    int best_dist = INT_MAX, best = 0;
    for (int i = 0; i < g_app.num_strobe_colors; i++) {
        int cr, cg, cb;
        hex_to_rgb(g_app.strobe_colors[i], &cr, &cg, &cb);
        int d = (r-cr)*(r-cr) + (g-cg)*(g-cg) + (b-cb)*(b-cb);
        if (d < best_dist) { best_dist = d; best = i; }
    }
    return g_app.strobe_colors[best];
}

/* Called once at startup (before threads launch) to probe siren_and_light.cgi */
static void init_strobe(void) {
    char cred[128];
    snprintf(cred, sizeof(cred), "%s:%s", g_app.device_user, g_app.device_pass);

    const char *body = "{\"apiVersion\":\"1.0\",\"method\":\"getCapabilities\"}";
    CURL *c = curl_easy_init();
    if (!c) return;

    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    CurlBuf buf = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL,           "http://127.0.0.1/axis-cgi/siren_and_light.cgi");
    curl_easy_setopt(c, CURLOPT_USERPWD,       cred);
    curl_easy_setopt(c, CURLOPT_HTTPAUTH,      CURLAUTH_DIGEST);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS,    body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       5L);

    CURLcode rc = curl_easy_perform(c);
    long http_code = -1;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(c);
    curl_slist_free_all(hdrs);

    if (rc != CURLE_OK || http_code < 200 || http_code >= 300) {
        app_log("init_strobe: siren_and_light not available (http=%ld rc=%d) — strobe disabled",
                http_code, (int)rc);
        free(buf.data);
        return;
    }

    g_app.strobe_api_available = 1;
    g_app.num_strobe_colors    = 0;

    /* Parse optional fixed color palette from capabilities response */
    if (buf.data) {
        cJSON *root = cJSON_Parse(buf.data);
        if (root) {
            cJSON *data   = cJSON_GetObjectItem(root, "data");
            cJSON *lights = data ? cJSON_GetObjectItem(data, "lights") : NULL;
            if (cJSON_IsArray(lights)) {
                cJSON *light0  = cJSON_GetArrayItem(lights, 0);
                cJSON *colors  = light0 ? cJSON_GetObjectItem(light0, "colors") : NULL;
                if (cJSON_IsArray(colors)) {
                    int nc = cJSON_GetArraySize(colors);
                    if (nc > 16) nc = 16;
                    for (int i = 0; i < nc; i++) {
                        cJSON *col = cJSON_GetArrayItem(colors, i);
                        if (cJSON_IsString(col)) {
                            strncpy(g_app.strobe_colors[g_app.num_strobe_colors],
                                    col->valuestring, 15);
                            g_app.strobe_colors[g_app.num_strobe_colors][15] = '\0';
                            g_app.num_strobe_colors++;
                        }
                    }
                }
            }
            cJSON_Delete(root);
        }
        free(buf.data);
    }

    app_log("init_strobe: API available, palette colors=%d (0=full RGB)",
            g_app.num_strobe_colors);
}

typedef struct { char color[16]; } StrobeArg;

static void *trigger_strobe_thread(void *arg) {
    StrobeArg *sa = (StrobeArg *)arg;
    char color[16];
    strncpy(color, sa->color, sizeof(color)-1);
    color[sizeof(color)-1] = '\0';
    free(sa);

    char cred[128];
    pthread_mutex_lock(&g_app.lock);
    snprintf(cred, sizeof(cred), "%s:%s", g_app.device_user, g_app.device_pass);
    pthread_mutex_unlock(&g_app.lock);

    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    CURL *c = curl_easy_init();
    if (!c) { curl_slist_free_all(hdrs); return NULL; }

    CurlBuf buf = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL,           "http://127.0.0.1/axis-cgi/siren_and_light.cgi");
    curl_easy_setopt(c, CURLOPT_USERPWD,       cred);
    curl_easy_setopt(c, CURLOPT_HTTPAUTH,      CURLAUTH_DIGEST);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       5L);

    /* Try updateProfile first; fall back to addProfile if not found */
    char prof_body[640];
    snprintf(prof_body, sizeof(prof_body),
        "{\"apiVersion\":\"1.0\",\"method\":\"updateProfile\","
        "\"params\":{\"profile\":{"
        "\"name\":\"mlb_scores\","
        "\"lights\":[{\"id\":\"0\",\"color\":\"%s\",\"intensity\":100}],"
        "\"pattern\":\"pulsing\",\"speed\":5,"
        "\"duration\":{\"forever\":false,\"time\":5000}}}}",
        color);
    curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, prof_body);
    curl_easy_perform(c);
    long http_code = -1;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    free(buf.data); buf.data = NULL; buf.len = 0;
    app_log("strobe updateProfile http=%ld color=%s", http_code, color);

    if (http_code >= 400 || http_code < 200) {
        char add_body[640];
        snprintf(add_body, sizeof(add_body),
            "{\"apiVersion\":\"1.0\",\"method\":\"addProfile\","
            "\"params\":{\"profile\":{"
            "\"name\":\"mlb_scores\","
            "\"lights\":[{\"id\":\"0\",\"color\":\"%s\",\"intensity\":100}],"
            "\"pattern\":\"pulsing\",\"speed\":5,"
            "\"duration\":{\"forever\":false,\"time\":5000}}}}",
            color);
        curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, add_body);
        curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
        free(buf.data); buf.data = NULL; buf.len = 0;
        app_log("strobe addProfile http=%ld", http_code);
    }

    /* Start the profile */
    const char *start_body =
        "{\"apiVersion\":\"1.0\",\"method\":\"start\","
        "\"params\":{\"profile\":\"mlb_scores\"}}";
    curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, start_body);
    curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    app_log("strobe start http=%ld", http_code);
    free(buf.data);

    curl_easy_cleanup(c);
    curl_slist_free_all(hdrs);
    return NULL;
}

static void trigger_strobe(int team_idx) {
    if (!g_app.strobe_api_available || !g_app.strobe_enabled) return;
    const MlbTeam *mt = team_by_id(g_app.teams[team_idx].team_id);
    if (!mt) return;
    const char *color = closest_strobe_color(mt->strobe);

    StrobeArg *sa = malloc(sizeof(StrobeArg));
    if (!sa) return;
    strncpy(sa->color, color, sizeof(sa->color)-1);
    sa->color[sizeof(sa->color)-1] = '\0';

    pthread_t t;
    if (pthread_create(&t, NULL, trigger_strobe_thread, sa) == 0)
        pthread_detach(t);
    else
        free(sa);
}

/* ── Date helpers ────────────────────────────────────────────────── */
static void today_str(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm tm_tmp;
    localtime_r(&t, &tm_tmp);
    strftime(buf, n, "%Y-%m-%d", &tm_tmp);
}

static void date_offset_str(char *buf, size_t n, int days_ahead) {
    time_t t = time(NULL) + (time_t)days_ahead * 86400;
    struct tm tm_tmp;
    localtime_r(&t, &tm_tmp);
    strftime(buf, n, "%Y-%m-%d", &tm_tmp);
}

/* ── Format ISO datetime → "Apr 3, 7:10 PM" ─────────────────────── */
static void format_game_time(const char *iso, char *date_out, size_t dn,
                              char *time_out, size_t tn) {
    int yr=0, mo=0, dy=0, hr=0, mn=0;
    sscanf(iso, "%d-%d-%dT%d:%d", &yr, &mo, &dy, &hr, &mn);
    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
    const char *mon = (mo >= 1 && mo <= 12) ? months[mo-1] : "???";
    snprintf(date_out, dn, "%s %d", mon, dy);
    struct tm t = {0};
    t.tm_year = yr - 1900; t.tm_mon = mo - 1; t.tm_mday = dy;
    t.tm_hour = hr; t.tm_min = mn; t.tm_isdst = 0;
    time_t utc = timegm(&t);  /* treat parsed fields as UTC, not local */
    struct tm local_tm;
    localtime_r(&utc, &local_tm);
    int lhr = local_tm.tm_hour, lmn = local_tm.tm_min;
    snprintf(time_out, tn, "%d:%02d %s",
             lhr % 12 == 0 ? 12 : lhr % 12, lmn, lhr < 12 ? "AM" : "PM");
}

/* ── Teams JSON serialization ───────────────────────────────────── */
/* Serialize g_app.teams[] to compact JSON array string.
   Short keys (tc/bc/ts/ss/dm) keep the AXParameter value small. */
static void teams_to_json(char *buf, size_t n) {
    size_t pos = 0;
    pos += snprintf(buf + pos, n - pos, "[");
    for (int i = 0; i < g_app.num_teams && pos < n - 2; i++) {
        if (i > 0) pos += snprintf(buf + pos, n - pos, ",");
        pos += snprintf(buf + pos, n - pos,
            "{\"id\":%d,\"notify\":%d,\"score\":%d,\"ae\":%d"
            ",\"tc\":\"%s\",\"bc\":\"%s\",\"ts\":\"%s\",\"ss\":%d,\"dm\":%d}",
            g_app.teams[i].team_id,
            g_app.teams[i].notify_clip_id,
            g_app.teams[i].score_clip_id,
            g_app.teams[i].audio_enabled,
            g_app.teams[i].text_color,
            g_app.teams[i].bg_color,
            g_app.teams[i].text_size,
            g_app.teams[i].scroll_speed,
            g_app.teams[i].duration_ms);
    }
    snprintf(buf + pos, n - pos, "]");
}

/* Populate per-team display defaults from official MLB colors */
static void set_team_display_defaults(TeamConfig *tc, int team_id) {
    const MlbTeam *mt = team_by_id(team_id);
    strncpy(tc->text_color, mt ? mt->fg : "#FFFFFF", sizeof(tc->text_color)-1);
    strncpy(tc->bg_color,   mt ? mt->bg : "#041E42", sizeof(tc->bg_color)-1);
    strncpy(tc->text_size,  "large", sizeof(tc->text_size)-1);
    tc->scroll_speed  = 3;
    tc->duration_ms   = 20000;
    tc->audio_enabled = 1;
}

/* Parse JSON array string into g_app.teams[] and init state names.
   Display fields use short keys (tc/bc/ts/ss/dm); fall back to MLB colors. */
static void teams_from_json(const char *json) {
    if (!json || json[0] == '\0') return;
    cJSON *arr = cJSON_Parse(json);
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return; }
    int n = cJSON_GetArraySize(arr);
    if (n > MAX_TEAMS) n = MAX_TEAMS;
    g_app.num_teams = 0;
    for (int i = 0; i < n; i++) {
        cJSON *item   = cJSON_GetArrayItem(arr, i);
        cJSON *id     = cJSON_GetObjectItem(item, "id");
        cJSON *notify = cJSON_GetObjectItem(item, "notify");
        cJSON *score  = cJSON_GetObjectItem(item, "score");
        if (!cJSON_IsNumber(id)) continue;
        int idx = g_app.num_teams++;
        g_app.teams[idx].team_id        = (int)id->valuedouble;
        g_app.teams[idx].notify_clip_id = cJSON_IsNumber(notify) ? (int)notify->valuedouble : 38;
        g_app.teams[idx].score_clip_id  = cJSON_IsNumber(score)  ? (int)score->valuedouble  : 38;

        /* Display: short keys in stored JSON; fall back to MLB team colors */
        cJSON *ae = cJSON_GetObjectItem(item, "ae");
        g_app.teams[idx].audio_enabled = cJSON_IsNumber(ae) ? (int)ae->valuedouble : 1;

        cJSON *tc = cJSON_GetObjectItem(item, "tc");
        cJSON *bc = cJSON_GetObjectItem(item, "bc");
        cJSON *ts = cJSON_GetObjectItem(item, "ts");
        cJSON *ss = cJSON_GetObjectItem(item, "ss");
        cJSON *dm = cJSON_GetObjectItem(item, "dm");
        if (cJSON_IsString(tc) && tc->valuestring[0])
            strncpy(g_app.teams[idx].text_color, tc->valuestring, sizeof(g_app.teams[idx].text_color)-1);
        if (cJSON_IsString(bc) && bc->valuestring[0])
            strncpy(g_app.teams[idx].bg_color,   bc->valuestring, sizeof(g_app.teams[idx].bg_color)-1);
        if (cJSON_IsString(ts) && ts->valuestring[0])
            strncpy(g_app.teams[idx].text_size,  ts->valuestring, sizeof(g_app.teams[idx].text_size)-1);
        if (cJSON_IsNumber(ss)) g_app.teams[idx].scroll_speed = (int)ss->valuedouble;
        if (cJSON_IsNumber(dm)) g_app.teams[idx].duration_ms  = (int)dm->valuedouble;

        /* Fill any missing display fields from official MLB colors */
        if (!g_app.teams[idx].text_color[0] || !g_app.teams[idx].bg_color[0] ||
            !g_app.teams[idx].text_size[0]  || !g_app.teams[idx].scroll_speed ||
            !g_app.teams[idx].duration_ms) {
            TeamConfig defaults = {0};
            set_team_display_defaults(&defaults, g_app.teams[idx].team_id);
            if (!g_app.teams[idx].text_color[0])
                strncpy(g_app.teams[idx].text_color, defaults.text_color, sizeof(g_app.teams[idx].text_color)-1);
            if (!g_app.teams[idx].bg_color[0])
                strncpy(g_app.teams[idx].bg_color,   defaults.bg_color,   sizeof(g_app.teams[idx].bg_color)-1);
            if (!g_app.teams[idx].text_size[0])
                strncpy(g_app.teams[idx].text_size,  defaults.text_size,  sizeof(g_app.teams[idx].text_size)-1);
            if (!g_app.teams[idx].scroll_speed) g_app.teams[idx].scroll_speed = defaults.scroll_speed;
            if (!g_app.teams[idx].duration_ms)  g_app.teams[idx].duration_ms  = defaults.duration_ms;
        }

        g_app.state[idx].team_id = g_app.teams[idx].team_id;
        const MlbTeam *t = team_by_id(g_app.teams[idx].team_id);
        strncpy(g_app.state[idx].team_name, t ? t->name : "Unknown",
                sizeof(g_app.state[idx].team_name)-1);
    }
    cJSON_Delete(arr);
    app_log("loaded %d team(s) from JSON", g_app.num_teams);
}

/* ── Reset a team's live state (called when game resets or team removed) ── */
static void reset_team_state(int idx) {
    TeamState *s = &g_app.state[idx];
    /* preserve identity */
    int   saved_id = s->team_id;
    char  saved_name[64];
    strncpy(saved_name, s->team_name, sizeof(saved_name)-1);
    saved_name[sizeof(saved_name)-1] = '\0';

    memset(s, 0, sizeof(*s));
    s->team_id = saved_id;
    strncpy(s->team_name, saved_name, sizeof(s->team_name)-1);
    strcpy(s->game_state, "No Game");
}

/* ── Update teams from a POST /config teams array ───────────────── */
static void update_teams_from_config(cJSON *teams_arr) {
    int n = cJSON_GetArraySize(teams_arr);
    if (n > MAX_TEAMS) n = MAX_TEAMS;

    /* Zero out all states */
    for (int i = 0; i < MAX_TEAMS; i++) {
        memset(&g_app.state[i], 0, sizeof(TeamState));
        strcpy(g_app.state[i].game_state, "No Game");
    }

    g_app.num_teams = 0;
    for (int i = 0; i < n; i++) {
        cJSON *item   = cJSON_GetArrayItem(teams_arr, i);
        cJSON *id     = cJSON_GetObjectItem(item, "team_id");
        cJSON *notify = cJSON_GetObjectItem(item, "notify_clip_id");
        cJSON *score  = cJSON_GetObjectItem(item, "score_clip_id");

        int tid = 0;
        if (cJSON_IsString(id))      tid = atoi(id->valuestring);
        else if (cJSON_IsNumber(id)) tid = (int)id->valuedouble;
        if (tid == 0) continue;

        int idx = g_app.num_teams++;
        g_app.teams[idx].team_id = tid;
        g_app.teams[idx].notify_clip_id = cJSON_IsString(notify) ? atoi(notify->valuestring) :
                                          cJSON_IsNumber(notify) ? (int)notify->valuedouble : 38;
        g_app.teams[idx].score_clip_id  = cJSON_IsString(score) ? atoi(score->valuestring) :
                                          cJSON_IsNumber(score)  ? (int)score->valuedouble  : 38;

        cJSON *ae = cJSON_GetObjectItem(item, "audio_enabled");
        g_app.teams[idx].audio_enabled = ae ? (cJSON_IsTrue(ae) || (cJSON_IsString(ae) && strcmp(ae->valuestring,"true")==0) || (cJSON_IsNumber(ae) && ae->valuedouble != 0.0)) ? 1 : 0 : 1;

        /* Per-team display settings from POST body (long key names) */
        cJSON *tc = cJSON_GetObjectItem(item, "text_color");
        cJSON *bc = cJSON_GetObjectItem(item, "bg_color");
        cJSON *ts = cJSON_GetObjectItem(item, "text_size");
        cJSON *ss = cJSON_GetObjectItem(item, "scroll_speed");
        cJSON *dm = cJSON_GetObjectItem(item, "duration_ms");

        /* Start with official team color defaults, then override with supplied values */
        set_team_display_defaults(&g_app.teams[idx], tid);
        if (cJSON_IsString(tc) && tc->valuestring[0])
            strncpy(g_app.teams[idx].text_color, tc->valuestring, sizeof(g_app.teams[idx].text_color)-1);
        if (cJSON_IsString(bc) && bc->valuestring[0])
            strncpy(g_app.teams[idx].bg_color,   bc->valuestring, sizeof(g_app.teams[idx].bg_color)-1);
        if (cJSON_IsString(ts) && ts->valuestring[0])
            strncpy(g_app.teams[idx].text_size,  ts->valuestring, sizeof(g_app.teams[idx].text_size)-1);
        if (cJSON_IsString(ss) && ss->valuestring[0])
            g_app.teams[idx].scroll_speed = atoi(ss->valuestring);
        else if (cJSON_IsNumber(ss))
            g_app.teams[idx].scroll_speed = (int)ss->valuedouble;
        if (cJSON_IsString(dm) && dm->valuestring[0])
            g_app.teams[idx].duration_ms = atoi(dm->valuestring);
        else if (cJSON_IsNumber(dm))
            g_app.teams[idx].duration_ms = (int)dm->valuedouble;

        g_app.state[idx].team_id = tid;
        const MlbTeam *t = team_by_id(tid);
        strncpy(g_app.state[idx].team_name, t ? t->name : "Unknown",
                sizeof(g_app.state[idx].team_name)-1);
    }
    app_log("teams updated: %d team(s)", g_app.num_teams);
}

/* ── MLB API: find today's game pk for a team ───────────────────── */
/* Also populates state->next_game_opponent/home/date/time so that
   Preview-state cards can display the same info as No-Game cards. */
static int fetch_game_pk_for_team(CURL *curl, int idx) {
    TeamConfig *cfg   = &g_app.teams[idx];
    TeamState  *state = &g_app.state[idx];

    char date[16], url[MAX_URL];
    today_str(date, sizeof(date));
    snprintf(url, sizeof(url),
        MLB_API_BASE "/schedule?sportId=1&teamId=%d&date=%s&hydrate=team",
        cfg->team_id, date);

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

            /* Populate today's opponent/home/time so Preview cards render correctly */
            if (game_pk) {
                cJSON *gt    = cJSON_GetObjectItem(game, "gameDate");
                cJSON *teams = cJSON_GetObjectItem(game, "teams");
                if (teams && cJSON_IsString(gt)) {
                    cJSON *home = cJSON_GetObjectItem(teams, "home");
                    cJSON *away = cJSON_GetObjectItem(teams, "away");
                    int is_home = 0;
                    if (home) {
                        cJSON *ht  = cJSON_GetObjectItem(home, "team");
                        cJSON *hid = ht ? cJSON_GetObjectItem(ht, "id") : NULL;
                        if (cJSON_IsNumber(hid) && (int)hid->valuedouble == cfg->team_id)
                            is_home = 1;
                    }
                    cJSON *opp_side = cJSON_GetObjectItem(is_home ? away : home, "team");
                    cJSON *opp_name = opp_side ? cJSON_GetObjectItem(opp_side, "teamName") : NULL;
                    if (cJSON_IsString(opp_name))
                        strncpy(state->next_game_opponent, opp_name->valuestring,
                                sizeof(state->next_game_opponent)-1);
                    state->next_game_home = is_home;
                    format_game_time(gt->valuestring,
                                     state->next_game_date, sizeof(state->next_game_date),
                                     state->next_game_time, sizeof(state->next_game_time));
                }
            }
        }
    }
    cJSON_Delete(root);
    return game_pk;
}

/* ── MLB API: find next scheduled game within 14 days ───────────── */
static int fetch_next_game_for_team(CURL *curl, int idx) {
    TeamConfig *cfg   = &g_app.teams[idx];
    TeamState  *state = &g_app.state[idx];

    char start[16], end[16], url[MAX_URL];
    date_offset_str(start, sizeof(start), 1);
    date_offset_str(end,   sizeof(end),   14);
    snprintf(url, sizeof(url),
        MLB_API_BASE "/schedule?sportId=1&teamId=%d&startDate=%s&endDate=%s&hydrate=team",
        cfg->team_id, start, end);

    char *body = http_get(curl, url);
    if (!body) return 0;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return 0;

    int found = 0;
    cJSON *dates = cJSON_GetObjectItem(root, "dates");
    if (cJSON_IsArray(dates)) {
        int nd = cJSON_GetArraySize(dates);
        for (int di = 0; di < nd && !found; di++) {
            cJSON *day   = cJSON_GetArrayItem(dates, di);
            cJSON *games = cJSON_GetObjectItem(day, "games");
            if (!cJSON_IsArray(games)) continue;
            int ng = cJSON_GetArraySize(games);
            for (int gi = 0; gi < ng && !found; gi++) {
                cJSON *game = cJSON_GetArrayItem(games, gi);
                cJSON *gt   = cJSON_GetObjectItem(game, "gameDate");
                if (!cJSON_IsString(gt)) continue;

                cJSON *teams = cJSON_GetObjectItem(game, "teams");
                if (!teams) continue;
                cJSON *home = cJSON_GetObjectItem(teams, "home");
                cJSON *away = cJSON_GetObjectItem(teams, "away");
                if (!home || !away) continue;

                int is_home = 0;
                cJSON *ht   = cJSON_GetObjectItem(home, "team");
                if (ht) {
                    cJSON *hid = cJSON_GetObjectItem(ht, "id");
                    if (cJSON_IsNumber(hid) && (int)hid->valuedouble == cfg->team_id)
                        is_home = 1;
                }
                cJSON *opp_team = cJSON_GetObjectItem(is_home ? away : home, "team");
                if (!opp_team) continue;
                cJSON *opp_name = cJSON_GetObjectItem(opp_team, "teamName");
                if (!cJSON_IsString(opp_name)) continue;

                strncpy(state->next_game_opponent, opp_name->valuestring,
                        sizeof(state->next_game_opponent)-1);
                state->next_game_home = is_home;
                format_game_time(gt->valuestring,
                                 state->next_game_date, sizeof(state->next_game_date),
                                 state->next_game_time, sizeof(state->next_game_time));
                found = 1;
                app_log("[%s] next: %s vs %s @ %s %s",
                        state->team_name,
                        is_home ? "home" : "away",
                        state->next_game_opponent,
                        state->next_game_date,
                        state->next_game_time);
            }
        }
    }
    if (!found) {
        state->next_game_opponent[0] = '\0';
        state->next_game_date[0]     = '\0';
        state->next_game_time[0]     = '\0';
    }
    cJSON_Delete(root);
    return found;
}

/* ── MLB API: parse live feed ────────────────────────────────────── */
typedef struct {
    int  my_score;
    int  opponent;
    char opponent_name[64];
    char state[32];
    char inning_half[32];
    int  inning;
    int  outs;
    char last_play[MAX_MSG];
} LiveData;

static int parse_live_feed(const char *body, int team_id, LiveData *out) {
    cJSON *root = cJSON_Parse(body);
    if (!root) return 0;
    memset(out, 0, sizeof(*out));

    cJSON *gs = cJSON_GetObjectItemCaseSensitive(root, "gameData");
    if (gs) {
        cJSON *status = cJSON_GetObjectItem(gs, "status");
        if (status) {
            cJSON *ac = cJSON_GetObjectItem(status, "abstractGameState");
            if (cJSON_IsString(ac))
                strncpy(out->state, ac->valuestring, sizeof(out->state)-1);
        }
        cJSON *teams = cJSON_GetObjectItem(gs, "teams");
        if (teams) {
            cJSON *away = cJSON_GetObjectItem(teams, "away");
            cJSON *home = cJSON_GetObjectItem(teams, "home");
            int away_id = 0;
            if (away) {
                cJSON *tid = cJSON_GetObjectItem(away, "id");
                if (cJSON_IsNumber(tid)) away_id = (int)tid->valuedouble;
            }
            cJSON *opp = (away_id == team_id) ? home : away;
            if (opp) {
                cJSON *tn = cJSON_GetObjectItem(opp, "teamName");
                if (cJSON_IsString(tn))
                    strncpy(out->opponent_name, tn->valuestring, sizeof(out->opponent_name)-1);
            }
        }
    }

    cJSON *ld = cJSON_GetObjectItemCaseSensitive(root, "liveData");
    if (ld) {
        cJSON *ls = cJSON_GetObjectItem(ld, "linescore");
        if (ls) {
            cJSON *teams = cJSON_GetObjectItem(ls, "teams");
            if (teams) {
                /* determine home/away for this team */
                int is_home = 0;
                cJSON *gd = cJSON_GetObjectItemCaseSensitive(root, "gameData");
                if (gd) {
                    cJSON *t = cJSON_GetObjectItem(gd, "teams");
                    if (t) {
                        cJSON *h = cJSON_GetObjectItem(t, "home");
                        if (h) {
                            cJSON *id = cJSON_GetObjectItem(h, "id");
                            if (cJSON_IsNumber(id) && (int)id->valuedouble == team_id)
                                is_home = 1;
                        }
                    }
                }
                cJSON *away = cJSON_GetObjectItem(teams, "away");
                cJSON *home = cJSON_GetObjectItem(teams, "home");
                cJSON *my_side  = is_home ? home : away;
                cJSON *opp_side = is_home ? away : home;
                if (my_side)  { cJSON *r = cJSON_GetObjectItem(my_side,  "runs"); if (cJSON_IsNumber(r)) out->my_score = (int)r->valuedouble; }
                if (opp_side) { cJSON *r = cJSON_GetObjectItem(opp_side, "runs"); if (cJSON_IsNumber(r)) out->opponent = (int)r->valuedouble; }
            }
            cJSON *inn  = cJSON_GetObjectItem(ls, "currentInning");
            if (cJSON_IsNumber(inn)) out->inning = (int)inn->valuedouble;
            cJSON *half = cJSON_GetObjectItem(ls, "inningHalf");
            if (cJSON_IsString(half)) strncpy(out->inning_half, half->valuestring, sizeof(out->inning_half)-1);
            cJSON *outs = cJSON_GetObjectItem(ls, "outs");
            if (cJSON_IsNumber(outs)) out->outs = (int)outs->valuedouble;
        }
        cJSON *plays = cJSON_GetObjectItem(ld, "plays");
        if (plays) {
            cJSON *cp = cJSON_GetObjectItem(plays, "currentPlay");
            if (cp) {
                cJSON *result = cJSON_GetObjectItem(cp, "result");
                if (result) {
                    cJSON *desc = cJSON_GetObjectItem(result, "description");
                    if (cJSON_IsString(desc))
                        strncpy(out->last_play, desc->valuestring, MAX_MSG-1);
                }
            }
        }
    }
    cJSON_Delete(root);
    return 1;
}

/* ── Build display message from live data ────────────────────────── */
static void build_score_message(const char *team_name, const LiveData *d, char *buf, size_t n) {
    if (strcmp(d->state, "Live") == 0) {
        snprintf(buf, n,
            "%s %d - %s %d | %s %d | %d out%s",
            team_name, d->my_score,
            d->opponent_name, d->opponent,
            d->inning_half, d->inning,
            d->outs, d->outs == 1 ? "" : "s");
    } else if (strcmp(d->state, "Final") == 0) {
        const char *result = (d->my_score > d->opponent) ? "WIN"
                           : (d->my_score < d->opponent) ? "LOSS" : "TIE";
        snprintf(buf, n,
            "FINAL: %s %s %d - %s %d",
            team_name, result, d->my_score,
            d->opponent_name, d->opponent);
    } else {
        snprintf(buf, n, "%s vs %s - %s", team_name, d->opponent_name, d->state);
    }
}

/* ── Build final score message from stored state ─────────────────── */
static void build_final_message(int idx, char *buf, size_t n) {
    TeamState *s = &g_app.state[idx];
    const char *result = (s->my_score > s->opponent_score) ? "WIN"
                       : (s->my_score < s->opponent_score) ? "LOSS" : "TIE";
    snprintf(buf, n, "FINAL: %s %s %d - %s %d",
             s->team_name, result, s->my_score,
             s->opponent_name, s->opponent_score);
}

/* ── Poll one team ───────────────────────────────────────────────── */
static void poll_one_team(int idx) {
    TeamConfig *cfg   = &g_app.teams[idx];
    TeamState  *state = &g_app.state[idx];

    /* ── Final persist: if this team finished, handle until midnight ── */
    if (state->final_detected_at > 0) {
        time_t now = time(NULL);
        struct tm now_tm, final_tm_s;
        localtime_r(&now, &now_tm);
        localtime_r(&state->final_detected_at, &final_tm_s);

        int past_midnight = (now_tm.tm_yday != final_tm_s.tm_yday ||
                             now_tm.tm_year != final_tm_s.tm_year);
        if (past_midnight || !g_app.display_persist_final) {
            app_log("[%s] final: resetting after midnight / persist disabled", state->team_name);
            int completed_pk = state->current_game_pk;
            reset_team_state(idx);
            if (!past_midnight) {
                /* Same calendar day — remember this game so we don't re-detect it as
                   newly_final on the next poll when fetch_game_pk_for_team finds it again */
                state->last_completed_game_pk = completed_pk;
            }
        } else {
            /* Show final score again (display expired after duration_ms) */
            if (g_app.display_enabled) {
                char msg[MAX_MSG];
                build_final_message(idx, msg, sizeof(msg));
                display_show_ex(msg, cfg, NULL, 0);
            }
            state->last_poll_time = now;
        }
        return;
    }

    /* ── Find today's game if we don't have one ── */
    if (state->current_game_pk == 0) {
        int pk = fetch_game_pk_for_team(g_app.fetch_curl, idx);
        if (pk != 0 && pk == state->last_completed_game_pk) {
            /* This game already completed today and persist is off — skip it */
            app_log("[%s] skipping already-completed game pk=%d", state->team_name, pk);
            pk = 0;
        }
        if (pk == 0) {
            state->next_game_opponent[0] = '\0';
            fetch_next_game_for_team(g_app.fetch_curl, idx);
            state->last_poll_time = time(NULL);
            strncpy(state->game_state, "No Game", sizeof(state->game_state)-1);
            state->is_live = 0;
            return;
        }
        /* next_game_opponent/home/date/time were populated by fetch_game_pk_for_team
           so Preview-state cards show the correct opponent and time */
        state->current_game_pk = pk;
        app_log("[%s] found game pk=%d", state->team_name, pk);
    }

    /* ── Fetch live feed ── */
    char url[MAX_URL];
    snprintf(url, sizeof(url), MLB_API_LIVE "/game/%d/feed/live", state->current_game_pk);

    char *body = http_get(g_app.fetch_curl, url);
    state->last_poll_time = time(NULL);
    if (!body) return;

    LiveData live;
    if (!parse_live_feed(body, cfg->team_id, &live)) { free(body); return; }
    free(body);

    /* ── Update common state ── */
    state->is_live = (strcmp(live.state, "Live") == 0);
    strncpy(state->game_state,    live.state,         sizeof(state->game_state)-1);
    strncpy(state->opponent_name, live.opponent_name, sizeof(state->opponent_name)-1);
    strncpy(state->last_play,     live.last_play,     sizeof(state->last_play)-1);
    state->outs = live.outs;

    /* ── Change detection ── */
    int score_changed  = (live.my_score != state->last_my_score ||
                          live.opponent  != state->last_opponent_score);
    int inning_changed = (live.inning    != state->last_inning  ||
                          strcmp(live.inning_half, state->last_inning_state) != 0);
    int newly_final    = (strcmp(live.state, "Final") == 0 && state->final_detected_at == 0);
    int my_scored      = score_changed && (live.my_score > state->my_score);

    /* Update current scores/inning */
    state->my_score       = live.my_score;
    state->opponent_score = live.opponent;
    state->inning         = live.inning;
    strncpy(state->inning_state, live.inning_half, sizeof(state->inning_state)-1);

    app_log("[%s] state=%s score=%d-%d inning=%s%d outs=%d changed=%d%d",
            state->team_name, live.state, live.my_score, live.opponent,
            live.inning_half, live.inning, live.outs, score_changed, inning_changed);

    /* ── Display: only on score or inning change while live ── */
    if (state->is_live && (score_changed || inning_changed)) {
        /* Update sentinels */
        state->last_my_score       = live.my_score;
        state->last_opponent_score = live.opponent;
        state->last_inning         = live.inning;
        strncpy(state->last_inning_state, live.inning_half, sizeof(state->last_inning_state)-1);

        if (g_app.display_enabled) {
            char msg[MAX_MSG];
            build_score_message(state->team_name, &live, msg, sizeof(msg));
            display_show_ex(msg, cfg, NULL, 0);
        }

        /* Audio: per-team clips */
        if (cfg->audio_enabled) {
            if (score_changed) {
                if (my_scored) play_clip(cfg->score_clip_id);
                else           play_clip(cfg->notify_clip_id);
            } else if (inning_changed) {
                play_clip(cfg->notify_clip_id);
            }
        }

        /* Strobe: flash team accent color on run scored only */
        if (my_scored)
            trigger_strobe(idx);
    }

    /* ── Final: show once, then enter persist loop ── */
    if (newly_final) {
        state->final_detected_at = time(NULL);
        /* Update sentinels so persist re-show loop has correct data */
        state->last_my_score       = live.my_score;
        state->last_opponent_score = live.opponent;

        if (g_app.display_enabled) {
            char msg[MAX_MSG];
            build_score_message(state->team_name, &live, msg, sizeof(msg));
            display_show_ex(msg, cfg, NULL, 0);
        }
        app_log("[%s] final detected: persist=%d", state->team_name, g_app.display_persist_final);
    }

    /* Reset game_pk if Final and not persisting */
    if (strcmp(live.state, "Final") == 0 && !g_app.display_persist_final &&
        state->final_detected_at == 0) {
        state->final_detected_at = time(NULL);
    }
}

/* ── Poll thread ─────────────────────────────────────────────────── */
static void *poll_thread(void *arg) {
    (void)arg;
    while (g_app.running) {
        pthread_mutex_lock(&g_app.lock);
        if (g_app.enabled && g_app.num_teams > 0) {
            for (int i = 0; i < g_app.num_teams; i++)
                poll_one_team(i);
        }

        int any_live = 0;
        for (int i = 0; i < g_app.num_teams; i++)
            if (g_app.state[i].is_live) { any_live = 1; break; }
        int interval = any_live ? g_app.poll_interval_sec : IDLE_INTERVAL_SEC;
        pthread_mutex_unlock(&g_app.lock);

        sleep(interval);
    }
    return NULL;
}

/* ── Tiny HTTP server ────────────────────────────────────────────── */

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
    char req[8192] = {0};
    int n = read(fd, req, sizeof(req)-1);
    if (n <= 0) return;

    char method[8], path[256];
    sscanf(req, "%7s %255s", method, path);

    char body_buf[8192] = {0};
    char *body_start = strstr(req, "\r\n\r\n");
    if (body_start && strcmp(method, "POST") == 0)
        strncpy(body_buf, body_start + 4, sizeof(body_buf)-1);

    #define ROUTE(r) (strstr(path, (r)) != NULL)

    /* ── GET /status ── */
    if (strcmp(method, "GET") == 0 && ROUTE("/status")) {
        pthread_mutex_lock(&g_app.lock);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "enabled", g_app.enabled);
        cJSON *tarr = cJSON_AddArrayToObject(root, "teams");

        for (int i = 0; i < g_app.num_teams; i++) {
            TeamState *s = &g_app.state[i];
            char ts[32] = "--";
            if (s->last_poll_time) {
                struct tm tm_tmp;
                localtime_r(&s->last_poll_time, &tm_tmp);
                strftime(ts, sizeof(ts), "%H:%M:%S", &tm_tmp);
            }

            cJSON *team = cJSON_CreateObject();
            cJSON_AddNumberToObject(team, "team_id",           s->team_id);
            cJSON_AddStringToObject(team, "team_name",         s->team_name);
            cJSON_AddStringToObject(team, "game_state",        s->game_state);
            cJSON_AddNumberToObject(team, "my_score",          s->my_score);
            cJSON_AddNumberToObject(team, "opponent_score",    s->opponent_score);
            cJSON_AddStringToObject(team, "opponent_name",     s->opponent_name);
            cJSON_AddNumberToObject(team, "inning",            s->inning);
            cJSON_AddStringToObject(team, "inning_state",      s->inning_state);
            cJSON_AddNumberToObject(team, "outs",              s->outs);
            cJSON_AddBoolToObject(team,   "is_live",           s->is_live);
            /* Pass last_play directly — cJSON handles JSON escaping internally */
            cJSON_AddStringToObject(team, "last_play",         s->last_play);
            cJSON_AddStringToObject(team, "last_poll_time",    ts);
            cJSON_AddStringToObject(team, "next_game_opponent",s->next_game_opponent);
            cJSON_AddStringToObject(team, "next_game_date",    s->next_game_date);
            cJSON_AddStringToObject(team, "next_game_time",    s->next_game_time);
            cJSON_AddBoolToObject(team,   "next_game_home",    s->next_game_home);
            cJSON_AddItemToArray(tarr, team);
        }

        pthread_mutex_unlock(&g_app.lock);

        char *out = cJSON_PrintUnformatted(root);
        http_respond(fd, 200, "application/json", out ? out : "{}");
        free(out);
        cJSON_Delete(root);
        return;
    }

    /* ── GET /config ── */
    if (strcmp(method, "GET") == 0 && ROUTE("/config")) {
        pthread_mutex_lock(&g_app.lock);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "enabled",              g_app.enabled ? "true" : "false");
        {
            char tmp[16]; snprintf(tmp, sizeof(tmp), "%d", g_app.poll_interval_sec);
            cJSON_AddStringToObject(root, "poll_interval_sec", tmp);
        }
        cJSON_AddStringToObject(root, "display_enabled",      g_app.display_enabled ? "true" : "false");
        cJSON_AddStringToObject(root, "display_persist_final",g_app.display_persist_final ? "true" : "false");
        cJSON_AddStringToObject(root, "device_user",          g_app.device_user);
        cJSON_AddBoolToObject(root,   "device_pass_set",      strlen(g_app.device_pass) > 0);
        {
            char tmp[16]; snprintf(tmp, sizeof(tmp), "%d", g_app.audio_volume);
            cJSON_AddStringToObject(root, "audio_volume", tmp);
        }
        cJSON_AddBoolToObject(root, "strobe_enabled", g_app.strobe_enabled);

        /* teams array — includes per-team display settings */
        cJSON *tarr = cJSON_AddArrayToObject(root, "teams");
        for (int i = 0; i < g_app.num_teams; i++) {
            cJSON *t = cJSON_CreateObject();
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%d", g_app.teams[i].team_id);
            cJSON_AddStringToObject(t, "team_id", tmp);
            snprintf(tmp, sizeof(tmp), "%d", g_app.teams[i].notify_clip_id);
            cJSON_AddStringToObject(t, "notify_clip_id", tmp);
            snprintf(tmp, sizeof(tmp), "%d", g_app.teams[i].score_clip_id);
            cJSON_AddStringToObject(t, "score_clip_id", tmp);
            cJSON_AddBoolToObject(t, "audio_enabled", g_app.teams[i].audio_enabled);
            cJSON_AddStringToObject(t, "text_color", g_app.teams[i].text_color);
            cJSON_AddStringToObject(t, "bg_color",   g_app.teams[i].bg_color);
            cJSON_AddStringToObject(t, "text_size",  g_app.teams[i].text_size);
            snprintf(tmp, sizeof(tmp), "%d", g_app.teams[i].scroll_speed);
            cJSON_AddStringToObject(t, "scroll_speed", tmp);
            snprintf(tmp, sizeof(tmp), "%d", g_app.teams[i].duration_ms);
            cJSON_AddStringToObject(t, "duration_ms", tmp);
            cJSON_AddItemToArray(tarr, t);
        }

        pthread_mutex_unlock(&g_app.lock);

        char *out = cJSON_PrintUnformatted(root);
        http_respond(fd, 200, "application/json", out ? out : "{}");
        free(out);
        cJSON_Delete(root);
        return;
    }

    /* ── POST /config ── */
    if (strcmp(method, "POST") == 0 && ROUTE("/config")) {
        cJSON *j = cJSON_Parse(body_buf);
        if (j) {
            pthread_mutex_lock(&g_app.lock);
            cJSON *v;
#define SET_BOOL_STR(key, field) \
    if ((v = cJSON_GetObjectItem(j, key)) && cJSON_IsString(v)) \
        g_app.field = (strcmp(v->valuestring, "true") == 0) ? 1 : 0;
#define SET_INT_STR(key, field) \
    if ((v = cJSON_GetObjectItem(j, key)) && cJSON_IsString(v)) \
        g_app.field = atoi(v->valuestring);
#define SET_STR(key, field) \
    if ((v = cJSON_GetObjectItem(j, key)) && cJSON_IsString(v)) \
        strncpy(g_app.field, v->valuestring, sizeof(g_app.field)-1);

            SET_BOOL_STR("enabled",              enabled)
            SET_INT_STR("poll_interval_sec",     poll_interval_sec)
            SET_BOOL_STR("display_enabled",      display_enabled)
            SET_BOOL_STR("display_persist_final",display_persist_final)
            SET_INT_STR("audio_volume",          audio_volume)
            if (g_app.audio_volume < 0)   g_app.audio_volume = 0;
            if (g_app.audio_volume > 100) g_app.audio_volume = 100;
            SET_BOOL_STR("strobe_enabled",       strobe_enabled)
            /* also accept raw bool from JS */
            if ((v = cJSON_GetObjectItem(j, "strobe_enabled")) && cJSON_IsBool(v))
                g_app.strobe_enabled = cJSON_IsTrue(v) ? 1 : 0;
            SET_STR("device_user",               device_user)
            if ((v = cJSON_GetObjectItem(j, "device_pass")) && cJSON_IsString(v) && strlen(v->valuestring))
                strncpy(g_app.device_pass, v->valuestring, sizeof(g_app.device_pass)-1);

            /* teams array (includes per-team display settings) */
            cJSON *tarr = cJSON_GetObjectItem(j, "teams");
            if (tarr && cJSON_IsArray(tarr))
                update_teams_from_config(tarr);

            /* Persist to AXParameter */
            AXParameter *p = g_app.ax_params;
            if (p) {
                char tmp[1024];
#define AXSET(name, val) ax_parameter_set(p, name, val, TRUE, NULL)
                AXSET("Enabled",             g_app.enabled ? "true" : "false");
                snprintf(tmp, sizeof(tmp), "%d", g_app.poll_interval_sec);
                AXSET("PollIntervalSec",     tmp);
                AXSET("DisplayEnabled",      g_app.display_enabled ? "true" : "false");
                AXSET("DisplayPersistFinal", g_app.display_persist_final ? "true" : "false");
                snprintf(tmp, sizeof(tmp), "%d", g_app.audio_volume);
                AXSET("AudioVolume",         tmp);
                AXSET("StrobeEnabled",       g_app.strobe_enabled ? "true" : "false");
                AXSET("DeviceUser",          g_app.device_user);
                if (strlen(g_app.device_pass))
                    AXSET("DevicePass",      g_app.device_pass);
                /* Store all teams (with per-team display settings) as JSON string */
                teams_to_json(tmp, sizeof(tmp));
                AXSET("Teams", tmp);
            }
            pthread_mutex_unlock(&g_app.lock);
            cJSON_Delete(j);
        }
        http_respond(fd, 200, "application/json", "{\"message\":\"Saved\"}");
        return;
    }

    /* ── POST /test_display ── */
    if (strcmp(method, "POST") == 0 && ROUTE("/test_display")) {
        char tmsg[MAX_MSG];
        TeamConfig test_cfg;
        pthread_mutex_lock(&g_app.lock);
        if (g_app.num_teams > 0) {
            test_cfg = g_app.teams[0];
            snprintf(tmsg, sizeof(tmsg), "MLB SCORES TEST: %s Score Display",
                     g_app.state[0].team_name);
        } else {
            memset(&test_cfg, 0, sizeof(test_cfg));
            strncpy(test_cfg.text_color, "#FFFFFF", sizeof(test_cfg.text_color)-1);
            strncpy(test_cfg.bg_color,   "#041E42", sizeof(test_cfg.bg_color)-1);
            strncpy(test_cfg.text_size,  "large",   sizeof(test_cfg.text_size)-1);
            test_cfg.scroll_speed = 3;
            test_cfg.duration_ms  = 20000;
            snprintf(tmsg, sizeof(tmsg), "MLB SCORES TEST Display");
        }
        pthread_mutex_unlock(&g_app.lock);

        char disp_resp[512] = "";
        long http_code = display_show_ex(tmsg, &test_cfg, disp_resp, sizeof(disp_resp));
        char out[768];
        snprintf(out, sizeof(out),
            "{\"message\":\"ok\",\"display_http\":%ld,\"display_resp\":\"%s\",\"sent_msg\":\"%s\"}",
            http_code, disp_resp, tmsg);
        http_respond(fd, 200, "application/json", out);
        return;
    }

    /* ── POST /test_audio ── */
    /* Accepts optional {"team_id": N} body to play that team's notify clip */
    if (strcmp(method, "POST") == 0 && ROUTE("/test_audio")) {
        cJSON *j = cJSON_Parse(body_buf);
        int team_id = 0;
        if (j) {
            cJSON *v = cJSON_GetObjectItem(j, "team_id");
            if (cJSON_IsNumber(v))      team_id = (int)v->valuedouble;
            else if (cJSON_IsString(v)) team_id = atoi(v->valuestring);
            cJSON_Delete(j);
        }

        pthread_mutex_lock(&g_app.lock);
        int clip_id = 38;
        for (int i = 0; i < g_app.num_teams; i++) {
            if (g_app.teams[i].team_id == team_id) {
                clip_id = g_app.teams[i].notify_clip_id;
                break;
            }
        }
        /* If team_id not found or 0, fall back to first team */
        if (clip_id == 38 && g_app.num_teams > 0 && team_id == 0)
            clip_id = g_app.teams[0].notify_clip_id;
        pthread_mutex_unlock(&g_app.lock);

        /* Volume is baked into the play URL inside play_clip_ex — no device-level change */
        char clip_resp[512] = "";
        long clip_http = play_clip_ex(clip_id, clip_resp, sizeof(clip_resp));
        char out[700];
        snprintf(out, sizeof(out),
            "{\"message\":\"ok\",\"clip_id\":%d,\"clip_http\":%ld,\"volume\":%d,\"clip_resp\":\"%s\"}",
            clip_id, clip_http, g_app.audio_volume, clip_resp);
        http_respond(fd, 200, "application/json", out);
        return;
    }

    /* ── POST /upload_clips ── */
    /* Uploads the bundled default audio clips to the device via mediaclip.cgi.
       Files live at /usr/local/packages/<appname>/audio/ after ACAP install. */
    if (strcmp(method, "POST") == 0 && ROUTE("/upload_clips")) {
        #define INSTALL_DIR "/usr/local/packages/" APP_NAME "/"
        static const struct { const char *file; const char *name; } BUNDLED[] = {
            { INSTALL_DIR "audio/hit_a_run.mp3",           "Hit a Run!"          },
            { INSTALL_DIR "audio/inning_change_organ.mp3", "Inning Change Organ" },
        };
        int nb = (int)(sizeof(BUNDLED) / sizeof(BUNDLED[0]));

        pthread_mutex_lock(&g_app.lock);
        char user[64], pass[64];
        strncpy(user, g_app.device_user, sizeof(user)-1);
        strncpy(pass, g_app.device_pass, sizeof(pass)-1);
        pthread_mutex_unlock(&g_app.lock);

        char cred[128];
        snprintf(cred, sizeof(cred), "%s:%s", user, pass);

        cJSON *clips_out = cJSON_CreateArray();
        for (int ci = 0; ci < nb; ci++) {
            CURL *uc = curl_easy_init();
            struct curl_httppost *form = NULL, *last = NULL;
            curl_formadd(&form, &last,
                CURLFORM_COPYNAME,    "clip",
                CURLFORM_FILE,        BUNDLED[ci].file,
                CURLFORM_CONTENTTYPE, "audio/mpeg",
                CURLFORM_END);
            curl_formadd(&form, &last,
                CURLFORM_COPYNAME,     "name",
                CURLFORM_COPYCONTENTS, BUNDLED[ci].name,
                CURLFORM_END);

            CurlBuf resp = {NULL, 0};
            curl_easy_setopt(uc, CURLOPT_URL,           "http://127.0.0.1/axis-cgi/mediaclip.cgi?action=upload");
            curl_easy_setopt(uc, CURLOPT_USERPWD,       cred);
            curl_easy_setopt(uc, CURLOPT_HTTPAUTH,      CURLAUTH_DIGEST);
            curl_easy_setopt(uc, CURLOPT_HTTPPOST,      form);
            curl_easy_setopt(uc, CURLOPT_WRITEFUNCTION, curl_write_cb);
            curl_easy_setopt(uc, CURLOPT_WRITEDATA,     &resp);
            curl_easy_setopt(uc, CURLOPT_TIMEOUT,       30L);

            CURLcode rc = curl_easy_perform(uc);
            long http_code = -1;
            curl_easy_getinfo(uc, CURLINFO_RESPONSE_CODE, &http_code);
            curl_easy_cleanup(uc);
            curl_formfree(form);

            app_log("upload_clips: %s http=%ld rc=%d resp=%s",
                    BUNDLED[ci].name, http_code, rc,
                    resp.data ? resp.data : "(none)");

            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "name",      BUNDLED[ci].name);
            cJSON_AddNumberToObject(r, "http_code", (double)http_code);
            cJSON_AddNumberToObject(r, "curl_code", (double)rc);
            cJSON_AddStringToObject(r, "response",  resp.data ? resp.data : "(none)");
            cJSON_AddItemToArray(clips_out, r);
            free(resp.data);
        }
        #undef INSTALL_DIR

        cJSON *root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "clips", clips_out);
        char *out_str = cJSON_PrintUnformatted(root);
        http_respond(fd, 200, "application/json", out_str ? out_str : "{}");
        free(out_str);
        cJSON_Delete(root);
        return;
    }

    /* ── GET /volume_diag ── */
    /* Shows the clip play URL format (volume baked in) and device audio params. */
    if (strcmp(method, "GET") == 0 && ROUTE("/volume_diag")) {
        pthread_mutex_lock(&g_app.lock);
        int cur_vol    = g_app.audio_volume;
        int ex_clip_id = (g_app.num_teams > 0) ? g_app.teams[0].notify_clip_id : 38;
        char user[64], pass[64];
        strncpy(user, g_app.device_user, sizeof(user)-1);
        strncpy(pass, g_app.device_pass, sizeof(pass)-1);
        pthread_mutex_unlock(&g_app.lock);

        /* Map slider to dB (same formula as play_clip_ex) */
        int gain_db = -95 + (cur_vol * 95 / 100);

        char example_url[MAX_URL];
        snprintf(example_url, sizeof(example_url),
            MEDIACLIP_API " — gain set to %d dB via audiodevicecontrol.cgi before play",
            ex_clip_id, gain_db);

        char cred[128];
        snprintf(cred, sizeof(cred), "%s:%s", user, pass);

        /* Test: actually call setDevicesSettings with the current gain */
        char set_body[384];
        snprintf(set_body, sizeof(set_body),
            "{\"apiVersion\":\"1.0\",\"method\":\"setDevicesSettings\","
            "\"params\":{\"devices\":[{\"id\":\"0\",\"outputs\":[{\"id\":\"0\","
            "\"connectionType\":{\"id\":\"internal\","
            "\"signalingType\":{\"id\":\"unbalanced\",\"gain\":%d}}}]}]}}",
            gain_db);
        CURL *sc = curl_easy_init();
        CurlBuf set_buf = {NULL, 0};
        struct curl_slist *s_hdrs = curl_slist_append(NULL, "Content-Type: application/json");
        curl_easy_setopt(sc, CURLOPT_URL,           "http://127.0.0.1/axis-cgi/audiodevicecontrol.cgi");
        curl_easy_setopt(sc, CURLOPT_USERPWD,       cred);
        curl_easy_setopt(sc, CURLOPT_HTTPAUTH,      CURLAUTH_DIGEST);
        curl_easy_setopt(sc, CURLOPT_HTTPHEADER,    s_hdrs);
        curl_easy_setopt(sc, CURLOPT_POSTFIELDS,    set_body);
        curl_easy_setopt(sc, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(sc, CURLOPT_WRITEDATA,     &set_buf);
        curl_easy_setopt(sc, CURLOPT_TIMEOUT,       5L);
        CURLcode set_rc = curl_easy_perform(sc);
        long set_http = -1;
        curl_easy_getinfo(sc, CURLINFO_RESPONSE_CODE, &set_http);
        curl_easy_cleanup(sc);
        curl_slist_free_all(s_hdrs);

        /* Query audiodevicecontrol.cgi for device gain capabilities */
        const char *adc_body = "{\"apiVersion\":\"1.0\",\"method\":\"getDevicesCapabilities\"}";
        CURL *ac = curl_easy_init();
        CurlBuf adc_buf = {NULL, 0};
        struct curl_slist *adc_hdrs = curl_slist_append(NULL, "Content-Type: application/json");
        curl_easy_setopt(ac, CURLOPT_URL,           "http://127.0.0.1/axis-cgi/audiodevicecontrol.cgi");
        curl_easy_setopt(ac, CURLOPT_USERPWD,       cred);
        curl_easy_setopt(ac, CURLOPT_HTTPAUTH,      CURLAUTH_DIGEST);
        curl_easy_setopt(ac, CURLOPT_HTTPHEADER,    adc_hdrs);
        curl_easy_setopt(ac, CURLOPT_POSTFIELDS,    adc_body);
        curl_easy_setopt(ac, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(ac, CURLOPT_WRITEDATA,     &adc_buf);
        curl_easy_setopt(ac, CURLOPT_TIMEOUT,       5L);
        CURLcode adc_rc = curl_easy_perform(ac);
        long adc_http = -1;
        curl_easy_getinfo(ac, CURLINFO_RESPONSE_CODE, &adc_http);
        curl_easy_cleanup(ac);
        curl_slist_free_all(adc_hdrs);

        /* List root.Audio params for reference */
        char list_url[MAX_URL];
        snprintf(list_url, sizeof(list_url),
            "http://127.0.0.1/axis-cgi/param.cgi?action=list&group=root.Audio");
        CURL *lc = curl_easy_init();
        CurlBuf list_buf = {NULL, 0};
        curl_easy_setopt(lc, CURLOPT_URL, list_url);
        curl_easy_setopt(lc, CURLOPT_USERPWD, cred);
        curl_easy_setopt(lc, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
        curl_easy_setopt(lc, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(lc, CURLOPT_WRITEDATA, &list_buf);
        curl_easy_setopt(lc, CURLOPT_TIMEOUT, 5L);
        CURLcode list_rc = curl_easy_perform(lc);
        long list_http = -1;
        curl_easy_getinfo(lc, CURLINFO_RESPONSE_CODE, &list_http);
        curl_easy_cleanup(lc);

        cJSON *diag = cJSON_CreateObject();
        cJSON_AddNumberToObject(diag, "audio_volume_setting",  cur_vol);
        cJSON_AddNumberToObject(diag, "gain_db_applied",       gain_db);
        cJSON_AddStringToObject(diag, "note",
            "gain set before each clip via audiodevicecontrol.cgi; restored to 0 dB after ~20s");
        cJSON_AddNumberToObject(diag, "set_gain_http_code",    (double)set_http);
        cJSON_AddNumberToObject(diag, "set_gain_curl_code",    (double)set_rc);
        cJSON_AddStringToObject(diag, "set_gain_response",
            set_buf.data ? set_buf.data : "(no response)");
        cJSON_AddNumberToObject(diag, "capabilities_http",  (double)adc_http);
        cJSON_AddStringToObject(diag, "capabilities",
            adc_buf.data ? adc_buf.data : "(no response)");
        cJSON_AddNumberToObject(diag, "list_http_code",  (double)list_http);
        cJSON_AddStringToObject(diag, "audio_params",
            list_buf.data ? list_buf.data : "(no response)");

        free(set_buf.data);
        free(adc_buf.data);
        free(list_buf.data);

        char *diag_out = cJSON_PrintUnformatted(diag);
        http_respond(fd, 200, "application/json", diag_out ? diag_out : "{}");
        free(diag_out);
        cJSON_Delete(diag);
        return;
    }

    /* ── GET /clips ── */
    if (strcmp(method, "GET") == 0 && ROUTE("/clips")) {
        char url[MAX_URL];
        snprintf(url, sizeof(url),
            "http://127.0.0.1/axis-cgi/param.cgi?action=list&group=root.MediaClip");
        CURL *c = curl_easy_init();
        char cred[128];
        snprintf(cred, sizeof(cred), "%s:%s", g_app.device_user, g_app.device_pass);
        CurlBuf clips_buf = {NULL, 0};
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_USERPWD, cred);
        curl_easy_setopt(c, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &clips_buf);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
        curl_easy_perform(c);
        curl_easy_cleanup(c);
        char *raw = clips_buf.data;

        cJSON *root = cJSON_CreateObject();
        cJSON *arr  = cJSON_AddArrayToObject(root, "clips");
        cJSON_AddStringToObject(root, "raw", raw ? raw : "(empty)");

        if (raw) {
            char *line = strtok(raw, "\n");
            while (line) {
                int idx; char name[128];
                if (sscanf(line, "root.MediaClip.M%d.Name=%127[^\r\n]", &idx, name) == 2) {
                    cJSON *clip = cJSON_CreateObject();
                    cJSON_AddNumberToObject(clip, "id",   idx);
                    cJSON_AddStringToObject(clip, "name", name);
                    cJSON_AddItemToArray(arr, clip);
                }
                line = strtok(NULL, "\n");
            }
            free(raw);
        }

        if (cJSON_GetArraySize(arr) == 0) {
            cJSON *clip = cJSON_CreateObject();
            cJSON_AddNumberToObject(clip, "id",   38);
            cJSON_AddStringToObject(clip, "name", "Default Notification");
            cJSON_AddItemToArray(arr, clip);
        }

        char *out = cJSON_PrintUnformatted(root);
        http_respond(fd, 200, "application/json", out ? out : "{}");
        free(out);
        cJSON_Delete(root);
        return;
    }

    /* ── GET /teams ── */
    if (strcmp(method, "GET") == 0 && ROUTE("/teams")) {
        cJSON *root = cJSON_CreateObject();
        cJSON *arr  = cJSON_AddArrayToObject(root, "teams");
        for (int i = 0; i < NUM_MLB_TEAMS; i++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddNumberToObject(t, "id",   MLB_TEAMS[i].id);
            cJSON_AddStringToObject(t, "name", MLB_TEAMS[i].name);
            cJSON_AddStringToObject(t, "abbr", MLB_TEAMS[i].abbr);
            cJSON_AddStringToObject(t, "bg",     MLB_TEAMS[i].bg);
            cJSON_AddStringToObject(t, "fg",     MLB_TEAMS[i].fg);
            cJSON_AddStringToObject(t, "strobe", MLB_TEAMS[i].strobe);
            cJSON_AddItemToArray(arr, t);
        }
        char *out = cJSON_PrintUnformatted(root);
        http_respond(fd, 200, "application/json", out ? out : "{}");
        free(out);
        cJSON_Delete(root);
        return;
    }

    /* ── GET /schedule ── */
    /* Returns ALL MLB games over today + 6 days, grouped by day. */
    if (strcmp(method, "GET") == 0 && ROUTE("/schedule")) {

        /* Pre-build 7 date buckets in order (today … today+6) */
        #define SCHED_DAYS 7
        char     day_raw[SCHED_DAYS][16];
        cJSON   *day_obj[SCHED_DAYS];
        cJSON   *day_games[SCHED_DAYS];
        for (int d = 0; d < SCHED_DAYS; d++) {
            date_offset_str(day_raw[d], sizeof(day_raw[d]), d);
            day_obj[d]   = NULL;
            day_games[d] = NULL;
        }

        {
            char start[16], end_dt[16];
            today_str(start, sizeof(start));
            date_offset_str(end_dt, sizeof(end_dt), SCHED_DAYS - 1);

            /* Single request — no teamId filter → all 30 teams */
            char url[MAX_URL];
            snprintf(url, sizeof(url),
                MLB_API_BASE "/schedule?sportId=1&startDate=%s&endDate=%s&hydrate=team",
                start, end_dt);

            CURL *sc = curl_easy_init();
            CurlBuf sbuf = {NULL, 0};
            curl_easy_setopt(sc, CURLOPT_URL, url);
            curl_easy_setopt(sc, CURLOPT_WRITEFUNCTION, curl_write_cb);
            curl_easy_setopt(sc, CURLOPT_WRITEDATA, &sbuf);
            curl_easy_setopt(sc, CURLOPT_TIMEOUT, 15L);
            curl_easy_setopt(sc, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(sc, CURLOPT_USERAGENT, APP_NAME "/1.1");
            curl_easy_perform(sc);
            curl_easy_cleanup(sc);

            if (sbuf.data) {
                cJSON *sroot = cJSON_Parse(sbuf.data);
                free(sbuf.data);
                if (sroot) {
                    static const char *mos[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                                "Jul","Aug","Sep","Oct","Nov","Dec"};
                    static const char *wds[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

                    cJSON *sdates = cJSON_GetObjectItem(sroot, "dates");
                    if (cJSON_IsArray(sdates)) {
                        int nd = cJSON_GetArraySize(sdates);
                        for (int di = 0; di < nd; di++) {
                            cJSON *sday  = cJSON_GetArrayItem(sdates, di);
                            cJSON *dval  = cJSON_GetObjectItem(sday, "date");
                            cJSON *sgms  = cJSON_GetObjectItem(sday, "games");
                            if (!cJSON_IsString(dval) || !cJSON_IsArray(sgms)) continue;

                            /* Find matching bucket */
                            int slot = -1;
                            for (int d = 0; d < SCHED_DAYS; d++)
                                if (strcmp(day_raw[d], dval->valuestring) == 0) { slot = d; break; }
                            if (slot < 0) continue;

                            /* Lazily create bucket object */
                            if (!day_obj[slot]) {
                                int yr=0, mo=0, dy=0;
                                sscanf(day_raw[slot], "%d-%d-%d", &yr, &mo, &dy);
                                struct tm dt = {0};
                                dt.tm_year = yr-1900; dt.tm_mon = mo-1; dt.tm_mday = dy;
                                mktime(&dt);
                                char lbl[32];
                                snprintf(lbl, sizeof(lbl), "%s, %s %d",
                                         wds[dt.tm_wday],
                                         (mo >= 1 && mo <= 12) ? mos[mo-1] : "???", dy);
                                day_obj[slot]   = cJSON_CreateObject();
                                cJSON_AddStringToObject(day_obj[slot], "date", lbl);
                                day_games[slot] = cJSON_AddArrayToObject(day_obj[slot], "games");
                            }

                            int ng = cJSON_GetArraySize(sgms);
                            for (int gi = 0; gi < ng; gi++) {
                                cJSON *game = cJSON_GetArrayItem(sgms, gi);
                                cJSON *gdate  = cJSON_GetObjectItem(game, "gameDate");
                                cJSON *status = cJSON_GetObjectItem(game, "status");
                                cJSON *abst   = status ? cJSON_GetObjectItem(status, "abstractGameState") : NULL;
                                cJSON *det    = status ? cJSON_GetObjectItem(status, "detailedState")     : NULL;
                                cJSON *gteams = cJSON_GetObjectItem(game, "teams");
                                cJSON *home_t = gteams ? cJSON_GetObjectItem(gteams, "home") : NULL;
                                cJSON *away_t = gteams ? cJSON_GetObjectItem(gteams, "away") : NULL;
                                cJSON *ht     = home_t ? cJSON_GetObjectItem(home_t, "team") : NULL;
                                cJSON *at     = away_t ? cJSON_GetObjectItem(away_t, "team") : NULL;
                                cJSON *hn     = ht     ? cJSON_GetObjectItem(ht, "teamName") : NULL;
                                cJSON *an     = at     ? cJSON_GetObjectItem(at, "teamName") : NULL;

                                char tstr[32] = "--", dummy[32];
                                if (cJSON_IsString(gdate))
                                    format_game_time(gdate->valuestring, dummy, sizeof(dummy),
                                                     tstr, sizeof(tstr));

                                cJSON *g_obj = cJSON_CreateObject();
                                cJSON_AddStringToObject(g_obj, "time",     tstr);
                                cJSON_AddStringToObject(g_obj, "away",     cJSON_IsString(an)   ? an->valuestring   : "");
                                cJSON_AddStringToObject(g_obj, "home",     cJSON_IsString(hn)   ? hn->valuestring   : "");
                                cJSON_AddStringToObject(g_obj, "state",    cJSON_IsString(abst) ? abst->valuestring : "");
                                cJSON_AddStringToObject(g_obj, "detailed", cJSON_IsString(det)  ? det->valuestring  : "");
                                cJSON_AddItemToArray(day_games[slot], g_obj);
                            }
                        }
                    }
                    cJSON_Delete(sroot);
                }
            }
        }

        /* Assemble output in chronological order, skip empty days */
        cJSON *out_root = cJSON_CreateObject();
        cJSON *days_out = cJSON_AddArrayToObject(out_root, "days");
        for (int d = 0; d < SCHED_DAYS; d++) {
            if (day_obj[d] && cJSON_GetArraySize(day_games[d]) > 0)
                cJSON_AddItemToArray(days_out, day_obj[d]);
            else if (day_obj[d])
                cJSON_Delete(day_obj[d]);
        }
        #undef SCHED_DAYS

        char *out = cJSON_PrintUnformatted(out_root);
        http_respond(fd, 200, "application/json", out ? out : "{\"days\":[]}");
        free(out);
        cJSON_Delete(out_root);
        return;
    }

    char not_found[512];
    snprintf(not_found, sizeof(not_found),
        "{\"error\":\"not found\",\"method\":\"%s\",\"path\":\"%s\"}", method, path);
    http_respond(fd, 404, "application/json", not_found);
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
#define GETBOOL(name, field) \
    if (ax_parameter_get(p, name, &v, NULL) && v) { \
        g_app.field = (strcmp(v,"true")==0); g_free(v); v=NULL; }

    GETBOOL("Enabled",            enabled)
    GETINT("PollIntervalSec",     poll_interval_sec)
    GETBOOL("DisplayEnabled",     display_enabled)
    GETBOOL("DisplayPersistFinal",display_persist_final)
    GETINT("AudioVolume",         audio_volume)
    GETBOOL("StrobeEnabled",      strobe_enabled)
    GETSTR("DeviceUser",          device_user)
    GETSTR("DevicePass",          device_pass)

    /* Load teams from "Teams" param (includes per-team display settings) */
    char *teams_json = NULL;
    if (ax_parameter_get(p, "Teams", &teams_json, NULL) && teams_json && strlen(teams_json) > 2) {
        teams_from_json(teams_json);
        g_free(teams_json);
    } else {
        /* Migration from 1.0.x or 1.1.0: single-team legacy params */
        app_log("Teams param empty — migrating from legacy single-team params");
        int legacy_id = 118, legacy_notify = 38, legacy_score = 38;
        if (ax_parameter_get(p, "TeamId",             &v, NULL) && v) { legacy_id     = atoi(v); g_free(v); v=NULL; }
        if (ax_parameter_get(p, "NotificationClipId", &v, NULL) && v) { legacy_notify = atoi(v); g_free(v); v=NULL; }
        if (ax_parameter_get(p, "ScoreClipId",        &v, NULL) && v) { legacy_score  = atoi(v); g_free(v); v=NULL; }

        g_app.num_teams = 1;
        g_app.teams[0].team_id        = legacy_id;
        g_app.teams[0].notify_clip_id = legacy_notify;
        g_app.teams[0].score_clip_id  = legacy_score;
        /* Per-team display defaults from official MLB team colors */
        set_team_display_defaults(&g_app.teams[0], legacy_id);

        g_app.state[0].team_id = legacy_id;
        const MlbTeam *t = team_by_id(legacy_id);
        strncpy(g_app.state[0].team_name, t ? t->name : "Unknown",
                sizeof(g_app.state[0].team_name)-1);
        strcpy(g_app.state[0].game_state, "No Game");

        /* Write migrated data back to Teams param */
        char tmp[1024];
        teams_to_json(tmp, sizeof(tmp));
        ax_parameter_set(p, "Teams", tmp, TRUE, NULL);
        app_log("migrated team_id=%d to Teams param: %s", legacy_id, tmp);
        if (teams_json) g_free(teams_json);
    }

    /* Initialize game_state for any un-initialized slots */
    for (int i = 0; i < g_app.num_teams; i++) {
        if (g_app.state[i].game_state[0] == '\0')
            strcpy(g_app.state[i].game_state, "No Game");
    }

    app_log("loaded: %d team(s), poll=%ds, display=%d",
            g_app.num_teams, g_app.poll_interval_sec, g_app.display_enabled);
}

/* ── Entry point ─────────────────────────────────────────────────── */
int main(void) {
    app_log("starting v1.1.10");

    curl_global_init(CURL_GLOBAL_ALL);
    pthread_mutex_init(&g_app.lock, NULL);

    /* suppress unused function warning */
    _md5hex_unused();

    /* Initialize all team states */
    for (int i = 0; i < MAX_TEAMS; i++)
        strcpy(g_app.state[i].game_state, "No Game");

    /* AXParameter */
    g_app.ax_params = ax_parameter_new(APP_NAME, NULL);
    if (!g_app.ax_params)
        app_log("warning: ax_parameter_new failed — using defaults");
    else
        load_params();

    g_app.fetch_curl = curl_easy_init();

    /* Probe siren_and_light.cgi — sets strobe_api_available and color palette */
    init_strobe();

    app_log("running: port=%d teams=%d poll=%ds",
            HTTP_PORT, g_app.num_teams, g_app.poll_interval_sec);

    pthread_t poll_tid, http_tid;
    pthread_create(&poll_tid, NULL, poll_thread, NULL);
    pthread_create(&http_tid, NULL, http_thread, NULL);

    pthread_join(poll_tid, NULL);
    pthread_join(http_tid, NULL);

    curl_easy_cleanup(g_app.fetch_curl);
    curl_global_cleanup();
    if (g_app.ax_params) ax_parameter_free(g_app.ax_params);
    pthread_mutex_destroy(&g_app.lock);
    return 0;
}
