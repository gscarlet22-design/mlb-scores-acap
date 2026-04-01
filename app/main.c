/*
 * mlb_scores - MLB live score ACAP
 * Vendor: gscarlet22 design for Axis C1720
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
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <errno.h>

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

#define DISPLAY_API         "http://127.0.0.1/config/rest/speaker-display-notification/v1/simple"
#define DISPLAY_STOP_API    "http://127.0.0.1/config/rest/speaker-display-notification/v1/stop"
#define MEDIACLIP_API       "http://127.0.0.1/axis-cgi/mediaclip.cgi?action=play&clip=%d"
#define PARAM_API           "http://127.0.0.1/axis-cgi/param.cgi?action=update&"

#define CONFIG_PATH         "/usr/local/packages/mlb_scores/localdata/config.json"
#define HTTP_PORT           2016

/* ── MLB team lookup table ──────────────────────────────────────── */
typedef struct { int id; const char *name; const char *abbr; const char *bg; const char *fg; } MlbTeam;
static const MlbTeam MLB_TEAMS[] = {
    /* id   name              abbr    bg(dark)    fg(light) */
    {108, "Angels",          "LAA",  "#BA0021",  "#C4CED4"},
    {109, "Diamondbacks",    "ARI",  "#A71930",  "#E3D4AD"},
    {110, "Orioles",         "BAL",  "#DF4601",  "#000000"},
    {111, "Red Sox",         "BOS",  "#BD3039",  "#0D2B56"},
    {112, "Cubs",            "CHC",  "#0E3386",  "#CC3433"},
    {113, "Reds",            "CIN",  "#C6011F",  "#FFFFFF"},
    {114, "Guardians",       "CLE",  "#00385D",  "#E31937"},
    {115, "Rockies",         "COL",  "#333366",  "#C4CED4"},
    {116, "Tigers",          "DET",  "#0C2340",  "#FA4616"},
    {117, "Astros",          "HOU",  "#002D62",  "#EB6E1F"},
    {118, "Royals",          "KC",   "#004687",  "#C09A5B"},
    {119, "Dodgers",         "LAD",  "#005A9C",  "#FFFFFF"},
    {120, "Nationals",       "WSH",  "#AB0003",  "#14225A"},
    {121, "Mets",            "NYM",  "#002D72",  "#FF5910"},
    {133, "Athletics",       "OAK",  "#003831",  "#EFB21E"},
    {134, "Pirates",         "PIT",  "#27251F",  "#FDB827"},
    {135, "Padres",          "SD",   "#2F241D",  "#FFC425"},
    {136, "Mariners",        "SEA",  "#0C2C56",  "#005C5C"},
    {137, "Giants",          "SF",   "#FD5A1E",  "#27251F"},
    {138, "Cardinals",       "STL",  "#C41E3A",  "#FEDB00"},
    {139, "Rays",            "TB",   "#092C5C",  "#8FBCE6"},
    {140, "Rangers",         "TEX",  "#003278",  "#C0111F"},
    {141, "Blue Jays",       "TOR",  "#134A8E",  "#E8291C"},
    {142, "Twins",           "MIN",  "#002B5C",  "#D31145"},
    {143, "Phillies",        "PHI",  "#E81828",  "#002D72"},
    {144, "Braves",          "ATL",  "#CE1141",  "#13274F"},
    {145, "White Sox",       "CWS",  "#27251F",  "#C4CED4"},
    {146, "Marlins",         "MIA",  "#00A3E0",  "#EF3340"},
    {147, "Yankees",         "NYY",  "#0C2340",  "#C4CED4"},
    {158, "Brewers",         "MIL",  "#FFC52F",  "#12284B"},
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

    /* next game (when no game today) */
    char   next_game_opponent[64];
    char   next_game_date[32];   /* "Apr 3" */
    char   next_game_time[32];   /* "7:10 PM" */
    int    next_game_home;       /* 1 = home, 0 = away */

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
    .next_game_opponent  = "",
    .next_game_date      = "",
    .next_game_time      = "",
    .next_game_home      = 0,
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

/* ── JSON string escape helper ──────────────────────────────────── */
/* Escapes quotes, backslashes, and control chars for safe JSON embedding */
static void json_escape(const char *src, char *dst, size_t dst_size) {
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 2 < dst_size; si++) {
        unsigned char c = (unsigned char)src[si];
        if (c == '"' || c == '\\') {
            if (di + 3 >= dst_size) break;
            dst[di++] = '\\';
            dst[di++] = (char)c;
        } else if (c < 0x20) {
            /* Skip control characters (newlines, tabs, etc.) */
        } else {
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
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

/* ── Self-contained MD5 (RFC 1321) for digest auth ───────────────── */
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
    uint32_t idx = (c->n[0]>>3)&0x3f;
    if ((c->n[0]+=len<<3)<(uint32_t)(len<<3)) c->n[1]++;
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

/* ── Raw HTTP POST with manual digest auth ───────────────────────── */
/* Bypasses curl's broken digest+POST retry by doing the two steps manually
   using plain TCP sockets — same socket code already used by our HTTP server. */
static long raw_http_post_digest(const char *host, int port, const char *path,
                                  const char *body, const char *user, const char *pass,
                                  char *resp_out, size_t resp_sz) {
    /* ── Step 1: unauthenticated POST to get WWW-Authenticate ── */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        if (resp_out) snprintf(resp_out, resp_sz, "socket failed");
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        if (resp_out) snprintf(resp_out, resp_sz, "connect failed");
        return -1;
    }
    size_t blen = strlen(body);
    char req1[2048];
    int r1len = snprintf(req1, sizeof(req1),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n"
        "%s",
        path, host, blen, body);
    (void)write(sock, req1, r1len);

    /* Read 401 response, extract WWW-Authenticate header */
    char rbuf[4096] = {0};
    int rn = read(sock, rbuf, sizeof(rbuf)-1);
    close(sock);
    (void)rn;

    /* Parse realm and nonce from WWW-Authenticate: Digest realm="...",nonce="..." */
    char realm[128] = "", nonce[128] = "";
    char *wa = strstr(rbuf, "WWW-Authenticate:");
    if (!wa) {
        /* No challenge — might be 200 already (no auth needed) or error */
        char *status_end = strchr(rbuf, '\r');
        if (resp_out) snprintf(resp_out, resp_sz, "no WWW-Authenticate: %.80s", rbuf);
        (void)status_end;
        return -1;
    }
    char *rp = strstr(wa, "realm=\"");
    if (rp) { rp += 7; char *re = strchr(rp, '"'); if (re) { int l = re-rp < 127 ? re-rp : 127; strncpy(realm, rp, l); realm[l]='\0'; } }
    char *np = strstr(wa, "nonce=\"");
    if (np) { np += 7; char *ne = strchr(np, '"'); if (ne) { int l = ne-np < 127 ? ne-np : 127; strncpy(nonce, np, l); nonce[l]='\0'; } }

    app_log("display digest realm=%s nonce=%s", realm, nonce);

    /* ── Step 2: compute digest response and POST with auth ── */
    char ha1src[256], ha2src[256], rsrc[512];
    char ha1[33], ha2[33], resp_hash[33];
    snprintf(ha1src, sizeof(ha1src), "%s:%s:%s", user, realm, pass);
    snprintf(ha2src, sizeof(ha2src), "POST:%s", path);
    md5hex(ha1src, ha1);
    md5hex(ha2src, ha2);
    snprintf(rsrc, sizeof(rsrc), "%s:%s:%s", ha1, nonce, ha2);
    md5hex(rsrc, resp_hash);

    char auth_hdr[512];
    snprintf(auth_hdr, sizeof(auth_hdr),
        "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\","
        " uri=\"%s\", response=\"%s\"",
        user, realm, nonce, path, resp_hash);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    addr.sin_addr.s_addr = inet_addr(host);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        if (resp_out) snprintf(resp_out, resp_sz, "connect2 failed");
        return -1;
    }
    char req2[2048];
    int r2len = snprintf(req2, sizeof(req2),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "%s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n"
        "%s",
        path, host, auth_hdr, blen, body);
    (void)write(sock, req2, r2len);

    char rbuf2[4096] = {0};
    int rn2 = read(sock, rbuf2, sizeof(rbuf2)-1);
    close(sock);

    /* Parse HTTP status code */
    long http_code = 0;
    sscanf(rbuf2, "HTTP/%*s %ld", &http_code);

    /* Find body after \r\n\r\n */
    char *body_start = strstr(rbuf2, "\r\n\r\n");
    const char *resp_body = body_start ? body_start + 4 : rbuf2;
    app_log("display raw http=%ld body=%.200s", http_code, resp_body);
    if (resp_out) snprintf(resp_out, resp_sz, "http=%ld body=%s", http_code, resp_body);
    (void)rn2;
    return http_code;
}

/* ── VAPIX display notification ──────────────────────────────────── */
/* Returns HTTP status code, or -1 on error. Stores response in resp_out if non-NULL. */
static long display_show_ex(const char *message, char *resp_out, size_t resp_sz) {

    int font_size = 24;
    if (strcmp(g_app.display_text_size, "small") == 0)  font_size = 18;
    if (strcmp(g_app.display_text_size, "large") == 0)  font_size = 32;

    char body[1024];
    snprintf(body, sizeof(body),
        "{\"text\":\"%s\","
        "\"duration\":%d,"
        "\"textColor\":\"%s\","
        "\"backgroundColor\":\"%s\","
        "\"fontSize\":%d,"
        "\"scrollSpeed\":%d}",
        message,
        g_app.display_duration_ms,
        g_app.display_text_color,
        g_app.display_bg_color,
        font_size,
        g_app.display_scroll_speed);

    app_log("display body: %s", body);

    return raw_http_post_digest(
        "127.0.0.1", 80,
        "/config/rest/speaker-display-notification/v1/simple",
        body, g_app.device_user, g_app.device_pass,
        resp_out, resp_sz);
}

static void display_show(const char *message) {
    if (!g_app.display_enabled) return;
    if (strncmp(message, g_app.last_display_msg, MAX_MSG - 1) == 0) return;
    strncpy(g_app.last_display_msg, message, MAX_MSG - 1);
    display_show_ex(message, NULL, 0);
}

/* ── VAPIX audio clip playback ───────────────────────────────────── */
static long play_clip_ex(int clip_id, char *resp_out, size_t resp_sz) {
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
        app_log("play_clip id=%d http=%ld body=%s", clip_id, http_code, buf.data ? buf.data : "(empty)");
        if (resp_out) snprintf(resp_out, resp_sz, "http=%ld body=%s", http_code, buf.data ? buf.data : "(empty)");
    }
    free(buf.data);
    return (rc == CURLE_OK) ? http_code : -1;
}

static void play_clip(int clip_id) {
    play_clip_ex(clip_id, NULL, 0);
}

/* ── Date helpers ────────────────────────────────────────────────── */
static void today_str(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(buf, n, "%Y-%m-%d", tm);
}

/* ── Date offset helper ─────────────────────────────────────────── */
static void date_offset_str(char *buf, size_t n, int days_ahead) {
    time_t t = time(NULL) + (time_t)days_ahead * 86400;
    struct tm *tm = localtime(&t);
    strftime(buf, n, "%Y-%m-%d", tm);
}

/* ── Format ISO datetime "2026-04-03T19:10:00Z" → "Apr 3, 7:10 PM" ─ */
static void format_game_time(const char *iso, char *date_out, size_t dn,
                              char *time_out, size_t tn) {
    /* Parse: YYYY-MM-DDTHH:MM */
    int yr=0, mo=0, dy=0, hr=0, mn=0;
    sscanf(iso, "%d-%d-%dT%d:%d", &yr, &mo, &dy, &hr, &mn);

    /* Month abbreviation */
    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
    const char *mon = (mo >= 1 && mo <= 12) ? months[mo-1] : "???";
    snprintf(date_out, dn, "%s %d", mon, dy);

    /* Convert UTC to local time using mktime trick */
    struct tm t = {0};
    t.tm_year = yr - 1900;
    t.tm_mon  = mo - 1;
    t.tm_mday = dy;
    t.tm_hour = hr;
    t.tm_min  = mn;
    t.tm_isdst = -1;
    /* mktime treats as local — use gmtime offset to convert from UTC */
    time_t utc = mktime(&t);
    struct tm *local = localtime(&utc);
    int lhr = local->tm_hour;
    int lmn = local->tm_min;
    snprintf(time_out, tn, "%d:%02d %s",
             lhr % 12 == 0 ? 12 : lhr % 12, lmn,
             lhr < 12 ? "AM" : "PM");
}

/* ── MLB API: find next scheduled game within 14 days ───────────── */
static int fetch_next_game(CURL *curl) {
    /* Query a 14-day window starting tomorrow */
    char start[16], end[16], url[MAX_URL];
    date_offset_str(start, sizeof(start), 1);
    date_offset_str(end,   sizeof(end),   14);
    snprintf(url, sizeof(url),
        MLB_API_BASE "/schedule?sportId=1&teamId=%d&startDate=%s&endDate=%s&hydrate=team",
        g_app.team_id, start, end);

    char *body = http_get(curl, url);
    if (!body) return 0;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return 0;

    int found = 0;
    cJSON *dates = cJSON_GetObjectItem(root, "dates");
    if (cJSON_IsArray(dates)) {
        /* Walk dates in order — first game found is the next one */
        int nd = cJSON_GetArraySize(dates);
        for (int di = 0; di < nd && !found; di++) {
            cJSON *day   = cJSON_GetArrayItem(dates, di);
            cJSON *games = cJSON_GetObjectItem(day, "games");
            if (!cJSON_IsArray(games)) continue;
            int ng = cJSON_GetArraySize(games);
            for (int gi = 0; gi < ng && !found; gi++) {
                cJSON *game = cJSON_GetArrayItem(games, gi);

                /* Get game time */
                cJSON *gt = cJSON_GetObjectItem(game, "gameDate");
                if (!cJSON_IsString(gt)) continue;

                /* Get opponent name and home/away */
                cJSON *teams = cJSON_GetObjectItem(game, "teams");
                if (!teams) continue;
                cJSON *home = cJSON_GetObjectItem(teams, "home");
                cJSON *away = cJSON_GetObjectItem(teams, "away");
                if (!home || !away) continue;

                /* Determine which side we are */
                int is_home = 0;
                cJSON *ht = cJSON_GetObjectItem(home, "team");
                if (ht) {
                    cJSON *hid = cJSON_GetObjectItem(ht, "id");
                    if (cJSON_IsNumber(hid) && (int)hid->valuedouble == g_app.team_id)
                        is_home = 1;
                }
                cJSON *opp_team = cJSON_GetObjectItem(is_home ? away : home, "team");
                if (!opp_team) continue;
                cJSON *opp_name = cJSON_GetObjectItem(opp_team, "teamName");
                if (!cJSON_IsString(opp_name)) continue;

                /* Store results */
                strncpy(g_app.next_game_opponent, opp_name->valuestring,
                        sizeof(g_app.next_game_opponent)-1);
                g_app.next_game_home = is_home;
                format_game_time(gt->valuestring,
                                 g_app.next_game_date, sizeof(g_app.next_game_date),
                                 g_app.next_game_time, sizeof(g_app.next_game_time));
                found = 1;
                app_log("next game: %s %s vs %s @ %s %s",
                        is_home ? "home" : "away",
                        g_app.team_name, g_app.next_game_opponent,
                        g_app.next_game_date, g_app.next_game_time);
            }
        }
    }

    if (!found) {
        g_app.next_game_opponent[0] = '\0';
        g_app.next_game_date[0]     = '\0';
        g_app.next_game_time[0]     = '\0';
    }

    cJSON_Delete(root);
    return found;
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
            app_log("no game today — looking ahead");
            /* Clear any stale next-game info then re-fetch */
            g_app.next_game_opponent[0] = '\0';
            fetch_next_game(g_app.fetch_curl);
            g_app.last_poll_time = time(NULL);
            pthread_mutex_unlock(&g_app.lock);
            return;
        }
        /* Found a game today — clear next-game info */
        g_app.next_game_opponent[0] = '\0';
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

    /* Always update display when live or score changed */
    if (g_app.is_live || score_changed)
        display_show(msg);

    /* Play clip on scoring plays */
    if (my_team_scored)
        play_clip(g_app.score_clip_id);
    else if (score_changed)
        play_clip(g_app.notification_clip_id);

    /* Reset game_pk after final */
    if (strcmp(live.state, "Final") == 0) {
        if (g_app.display_persist_final) {
            /* Keep final score on display until midnight */
            time_t now = time(NULL);
            struct tm *tm_now = localtime(&now);
            int secs_until_midnight = (23 - tm_now->tm_hour) * 3600
                                    + (59 - tm_now->tm_min)  * 60
                                    + (59 - tm_now->tm_sec)  + 1;
            app_log("final: persisting display for %ds until midnight", secs_until_midnight);
            sleep(secs_until_midnight);
        } else {
            sleep(60);
        }
        g_app.current_game_pk = 0;
        g_app.my_score        = 0;
        g_app.opponent_score  = 0;
        memset(g_app.last_display_msg, 0, sizeof(g_app.last_display_msg));
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
    char method[8], path[256];
    sscanf(req, "%7s %255s", method, path);

    /* Read body for POSTs */
    char body_buf[4096] = {0};
    char *body_start = strstr(req, "\r\n\r\n");
    if (body_start && strcmp(method, "POST") == 0)
        strncpy(body_buf, body_start + 4, sizeof(body_buf)-1);

    /* Strip any leading prefix (e.g. /local/mlb_scores/api) down to the route */
    const char *route = strrchr(path, '/');
    if (!route) route = path;
    /* Check full suffix for routes that share a prefix like /config */
    #define ROUTE(r) (strstr(path, (r)) != NULL)

    /* ── GET /status ── */
    if (strcmp(method, "GET") == 0 && ROUTE("/status")) {
        pthread_mutex_lock(&g_app.lock);
        char ts[32] = "--";
        if (g_app.last_poll_time) {
            struct tm *tm = localtime(&g_app.last_poll_time);
            strftime(ts, sizeof(ts), "%H:%M:%S", tm);
        }
        char safe_last_play[MAX_MSG * 2];
        json_escape(g_app.last_play, safe_last_play, sizeof(safe_last_play));
        char resp[2048];
        snprintf(resp, sizeof(resp),
            "{\"enabled\":%s,\"game_pk\":%d,"
            "\"team_id\":%d,\"team_name\":\"%s\","
            "\"my_score\":%d,\"opponent_score\":%d,"
            "\"opponent_name\":\"%s\","
            "\"game_state\":\"%s\","
            "\"inning_state\":\"%s\",\"inning\":%d,\"outs\":%d,"
            "\"last_play\":\"%s\","
            "\"last_poll_time\":\"%s\","
            "\"is_live\":%s,"
            "\"next_game_opponent\":\"%s\","
            "\"next_game_date\":\"%s\","
            "\"next_game_time\":\"%s\","
            "\"next_game_home\":%s}",
            g_app.enabled ? "true" : "false",
            g_app.current_game_pk,
            g_app.team_id, g_app.team_name,
            g_app.my_score, g_app.opponent_score,
            g_app.opponent_name,
            g_app.game_state,
            g_app.inning_state, g_app.inning, g_app.outs,
            safe_last_play,
            ts,
            g_app.is_live ? "true" : "false",
            g_app.next_game_opponent,
            g_app.next_game_date,
            g_app.next_game_time,
            g_app.next_game_home ? "true" : "false");
        pthread_mutex_unlock(&g_app.lock);
        http_respond(fd, 200, "application/json", resp);
        return;
    }

    /* ── GET /config ── */
    if (strcmp(method, "GET") == 0 && ROUTE("/config")) {
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
    if (strcmp(method, "POST") == 0 && ROUTE("/config")) {
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
    if (strcmp(method, "POST") == 0 && ROUTE("/test_display")) {
        char tmsg[MAX_MSG];
        snprintf(tmsg, sizeof(tmsg), "MLB SCORES TEST: %s Score Display", g_app.team_name);
        memset(g_app.last_display_msg, 0, sizeof(g_app.last_display_msg));
        char disp_resp[512] = "";
        long http_code = display_show_ex(tmsg, disp_resp, sizeof(disp_resp));
        char out[768];
        snprintf(out, sizeof(out), "{\"message\":\"ok\",\"display_http\":%ld,\"display_resp\":\"%s\",\"sent_msg\":\"%s\"}", http_code, disp_resp, tmsg);
        http_respond(fd, 200, "application/json", out);
        return;
    }

    /* ── POST /test_audio ── */
    if (strcmp(method, "POST") == 0 && ROUTE("/test_audio")) {
        char clip_resp[512] = "";
        long clip_http = play_clip_ex(g_app.notification_clip_id, clip_resp, sizeof(clip_resp));
        char out[640];
        snprintf(out, sizeof(out),
            "{\"message\":\"ok\",\"clip_id\":%d,\"clip_http\":%ld,\"clip_resp\":\"%s\"}",
            g_app.notification_clip_id, clip_http, clip_resp);
        http_respond(fd, 200, "application/json", out);
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

        /* Include raw for diagnostics */
        cJSON_AddStringToObject(root, "raw", raw ? raw : "(empty)");

        if (raw) {
            /* Format: root.MediaClip.C<n>.Name=<name> */
            char *line = strtok(raw, "\n");
            while (line) {
                int idx; char name[128];
                if (sscanf(line, "root.MediaClip.C%d.Name=%127[^\r\n]", &idx, name) == 2) {
                    cJSON *clip = cJSON_CreateObject();
                    cJSON_AddNumberToObject(clip, "id",   idx);
                    cJSON_AddStringToObject(clip, "name", name);
                    cJSON_AddItemToArray(arr, clip);
                }
                line = strtok(NULL, "\n");
            }
            free(raw);
        }

        /* If no clips parsed, fall back to known working default */
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
        /* Build JSON array of all teams sorted by name */
        cJSON *root = cJSON_CreateObject();
        cJSON *arr  = cJSON_AddArrayToObject(root, "teams");
        for (int i = 0; i < NUM_TEAMS; i++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddNumberToObject(t, "id",   MLB_TEAMS[i].id);
            cJSON_AddStringToObject(t, "name", MLB_TEAMS[i].name);
            cJSON_AddStringToObject(t, "abbr", MLB_TEAMS[i].abbr);
            cJSON_AddStringToObject(t, "bg",   MLB_TEAMS[i].bg);
            cJSON_AddStringToObject(t, "fg",   MLB_TEAMS[i].fg);
            cJSON_AddItemToArray(arr, t);
        }
        char *out = cJSON_PrintUnformatted(root);
        http_respond(fd, 200, "application/json", out ? out : "{}");
        free(out);
        cJSON_Delete(root);
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
    GETINT("TeamId",              team_id)
    GETINT("PollIntervalSec",     poll_interval_sec)
    GETINT("NotificationClipId",  notification_clip_id)
    GETINT("ScoreClipId",         score_clip_id)
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
