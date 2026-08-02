#include "jellyfinapi.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <log/log.h>
#include <minIni.h>

#include "core/settings.h"
#include "ui/page_common.h" // SETTING_INI
#include "util/filesystem.h"

#define JF_DISCOVER_PORT 7359
#define JF_CHUNK         100
#define JF_TOKEN_FILE    "/mnt/extsd/jellyfintoken.txt"
#define JF_CACHE_SD      "/mnt/extsd/plexcache"
#define JF_CACHE_TMP     "/tmp/plexcache"

void jf_settings_save(void) {
    ini_puts("jellyfin", "host", g_setting.jellyfin.host, SETTING_INI);
    ini_putl("jellyfin", "port", g_setting.jellyfin.port, SETTING_INI);
    ini_puts("jellyfin", "token", g_setting.jellyfin.token, SETTING_INI);
    ini_puts("jellyfin", "user_id", g_setting.jellyfin.user_id, SETTING_INI);
}

/**
 * Auth headers. Jellyfin accepts the token via X-Emby-Token; the
 * MediaBrowser identity header is required for the auth endpoints.
 */
static const char *jf_device_id(void) {
    // Reuse the Plex client id as the stable device identifier
    extern void plex_settings_save(void);
    if (!g_setting.plex.client_id[0]) {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        snprintf(g_setting.plex.client_id, sizeof(g_setting.plex.client_id),
                 "hdzg-%08x%08x", (unsigned)rand(), (unsigned)rand());
        plex_settings_save();
    }
    return g_setting.plex.client_id;
}

static void jf_headers(char *buf, int size, bool with_token) {
    snprintf(buf, size,
             "X-Emby-Authorization: MediaBrowser Client=\"HDZero Goggle\", Device=\"HDZero Goggle\", DeviceId=\"%s\", Version=\"1.0\"%s%s%s\r\n",
             jf_device_id(),
             (with_token && g_setting.jellyfin.token[0]) ? ", Token=\"" : "",
             (with_token && g_setting.jellyfin.token[0]) ? g_setting.jellyfin.token : "",
             (with_token && g_setting.jellyfin.token[0]) ? "\"" : "");
}

static int jf_map_status(int status) {
    if (status < 0) {
        return PLEX_ERR_NET;
    }
    if (status == 401 || status == 403) {
        return PLEX_ERR_AUTH;
    }
    if (status != 200 && status != 204) {
        return PLEX_ERR_PROTO;
    }
    return PLEX_OK;
}

/**
 * Targeted JSON field extraction. Scans only the top level of one object
 * (depth-1 keys), skipping nested objects/arrays and respecting string
 * escapes - enough for Jellyfin's stable response shapes.
 */
static const char *json_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

static const char *json_skip_string(const char *p) { // p at opening quote
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
        }
        p++;
    }
    return *p ? p + 1 : p;
}

static const char *json_skip_value(const char *p) {
    p = json_skip_ws(p);
    if (*p == '"') {
        return json_skip_string(p);
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (*p == '{') ? '}' : ']';
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = json_skip_string(p);
                continue;
            }
            if (*p == open) {
                depth++;
            } else if (*p == close) {
                depth--;
                if (depth == 0) {
                    return p + 1;
                }
            }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') {
        p++;
    }
    return p;
}

/** Find `"key":` at depth 1 of the object starting at obj ('{'). Returns pointer to the value. */
static const char *json_field(const char *obj, const char *key) {
    const char *p = json_skip_ws(obj);
    if (*p != '{') {
        return NULL;
    }
    p++;
    size_t key_len = strlen(key);
    while (*p) {
        p = json_skip_ws(p);
        if (*p == '}' || !*p) {
            return NULL;
        }
        if (*p != '"') {
            return NULL;
        }
        const char *kstart = p + 1;
        const char *kend = json_skip_string(p) - 1; // at closing quote
        p = json_skip_ws(kend + 1);
        if (*p != ':') {
            return NULL;
        }
        p++;
        if ((size_t)(kend - kstart) == key_len && strncmp(kstart, key, key_len) == 0) {
            return json_skip_ws(p);
        }
        p = json_skip_value(p);
        p = json_skip_ws(p);
        if (*p == ',') {
            p++;
        }
    }
    return NULL;
}

static bool json_str(const char *obj, const char *key, char *out, int out_size) {
    const char *v = json_field(obj, key);
    if (!v || *v != '"') {
        return false;
    }
    v++;
    int w = 0;
    while (*v && *v != '"' && w < out_size - 1) {
        if (*v == '\\' && v[1]) {
            v++;
            switch (*v) {
            case 'n':
            case 'r':
            case 't':
                out[w++] = ' ';
                break;
            case 'u':
                // \uXXXX: keep ASCII, replace the rest
                if (strlen(v) >= 5) {
                    unsigned code = 0;
                    sscanf(v + 1, "%4x", &code);
                    out[w++] = (code >= 32 && code < 127) ? (char)code : '?';
                    v += 4;
                } else {
                    out[w++] = '?';
                }
                break;
            default:
                out[w++] = *v;
                break;
            }
            v++;
        } else {
            out[w++] = *v++;
        }
    }
    out[w] = '\0';
    return true;
}

static long long json_ll(const char *obj, const char *key, long long fallback) {
    const char *v = json_field(obj, key);
    if (!v || (!(*v >= '0' && *v <= '9') && *v != '-')) {
        return fallback;
    }
    return atoll(v);
}

static bool json_true(const char *obj, const char *key) {
    const char *v = json_field(obj, key);
    return v && strncmp(v, "true", 4) == 0;
}

/** Position at the first object of array `"key": [ {...}, ... ]`; NULL if absent/empty. */
static const char *json_array_first(const char *obj, const char *key) {
    const char *v = json_field(obj, key);
    if (!v || *v != '[') {
        return NULL;
    }
    v = json_skip_ws(v + 1);
    return (*v == '{') ? v : NULL;
}

/** Advance from one array object to the next; NULL at array end. */
static const char *json_array_next(const char *item) {
    const char *p = json_skip_value(item);
    p = json_skip_ws(p);
    if (*p == ',') {
        p = json_skip_ws(p + 1);
    }
    return (*p == '{') ? p : NULL;
}

/**
 * Discovery: Jellyfin answers a UDP broadcast with a small JSON blob.
 */
int jf_discover(plex_server_t *out, int max_out) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return 0;
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    struct timeval tv = {.tv_usec = 300 * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const char *msg = "who is JellyfinServer?";
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(JF_DISCOVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    sendto(fd, msg, strlen(msg), 0, (struct sockaddr *)&dst, sizeof(dst));

    int count = 0;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed_ms > 1500 || count >= max_out) {
            break;
        }

        char buf[1024];
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src, &src_len);
        if (n <= 0) {
            continue;
        }
        buf[n] = '\0';

        plex_server_t srv = {.port = 8096, .backend = MEDIA_BACKEND_JELLYFIN};
        inet_ntop(AF_INET, &src.sin_addr, srv.host, sizeof(srv.host));
        if (!json_str(buf, "Name", srv.name, sizeof(srv.name))) {
            snprintf(srv.name, sizeof(srv.name), "Jellyfin Server");
        }
        // "Address" carries scheme://host:port; extract a nonstandard port
        char addr[96];
        if (json_str(buf, "Address", addr, sizeof(addr))) {
            char *port_sep = strrchr(addr, ':');
            if (port_sep && atoi(port_sep + 1) > 0) {
                srv.port = atoi(port_sep + 1);
            }
        }

        bool dup = false;
        for (int i = 0; i < count; i++) {
            if (strcmp(out[i].host, srv.host) == 0 && out[i].backend == MEDIA_BACKEND_JELLYFIN) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            out[count++] = srv;
            LOGI("jellyfin: discovered %s at %s:%d", srv.name, srv.host, srv.port);
        }
    }

    close(fd);
    return count;
}

bool jf_server_reachable(const char *host, int port) {
    char *body = NULL;
    int status = lanhttp_request(host, port, "GET", "/System/Info/Public", NULL, NULL, &body, NULL);
    free(body);
    return status == 200;
}

/**
 * When signed in with an admin API key (SD-card file path) there is no
 * user context yet; adopt the first user so watched flags work.
 */
static void jf_ensure_user_id(void) {
    if (g_setting.jellyfin.user_id[0]) {
        return;
    }
    char headers[512];
    jf_headers(headers, sizeof(headers), true);
    char *body = NULL;
    int status = lanhttp_request(g_setting.jellyfin.host, g_setting.jellyfin.port,
                                 "GET", "/Users", headers, NULL, &body, NULL);
    if (status == 200 && body) {
        const char *p = json_skip_ws(body);
        if (*p == '[') {
            p = json_skip_ws(p + 1);
            if (*p == '{') {
                json_str(p, "Id", g_setting.jellyfin.user_id, sizeof(g_setting.jellyfin.user_id));
                jf_settings_save();
            }
        }
    }
    free(body);
}

static int jf_load_items(const char *base_query, plex_movie_t **out, int *out_count, volatile int *progress) {
    jf_ensure_user_id();

    plex_movie_t *list = malloc(sizeof(plex_movie_t) * PLEX_MOVIES_MAX);
    if (!list) {
        return PLEX_ERR_NET;
    }

    char headers[512];
    jf_headers(headers, sizeof(headers), true);

    int count = 0;
    long long total = -1;
    do {
        char path[512];
        snprintf(path, sizeof(path),
                 "%s&StartIndex=%d&Limit=%d%s%s",
                 base_query, count, JF_CHUNK,
                 g_setting.jellyfin.user_id[0] ? "&userId=" : "",
                 g_setting.jellyfin.user_id[0] ? g_setting.jellyfin.user_id : "");

        char *body = NULL;
        int status = lanhttp_request(g_setting.jellyfin.host, g_setting.jellyfin.port,
                                     "GET", path, headers, NULL, &body, NULL);
        int rc = jf_map_status(status);
        if (rc != PLEX_OK || !body) {
            free(body);
            free(list);
            return rc == PLEX_OK ? PLEX_ERR_NET : rc;
        }

        if (total < 0) {
            total = json_ll(body, "TotalRecordCount", -1);
        }

        int added = 0;
        for (const char *item = json_array_first(body, "Items");
             item && count + added < PLEX_MOVIES_MAX;
             item = json_array_next(item)) {
            plex_movie_t *m = &list[count + added];
            memset(m, 0, sizeof(*m));
            if (!json_str(item, "Id", m->rating_key, sizeof(m->rating_key))) {
                continue;
            }
            if (!json_str(item, "Name", m->title, sizeof(m->title))) {
                snprintf(m->title, sizeof(m->title), "Untitled");
            }
            m->year = (int)json_ll(item, "ProductionYear", 0);
            m->duration_min = (int)(json_ll(item, "RunTimeTicks", 0) / 600000000LL);
            char type[16] = "";
            json_str(item, "Type", type, sizeof(type));
            m->kind = strcmp(type, "Series") == 0    ? PLEX_ITEM_SERIES
                      : strcmp(type, "Episode") == 0 ? PLEX_ITEM_EPISODE
                                                     : PLEX_ITEM_MOVIE;
            m->season = (int16_t)json_ll(item, "ParentIndexNumber", 0);
            m->episode = (int16_t)json_ll(item, "IndexNumber", 0);
            const char *ud = json_field(item, "UserData");
            if (ud && *ud == '{') {
                m->watched = json_true(ud, "Played");
            }
            // Poster presence: any ImageTags object with a Primary tag
            const char *tags = json_field(item, "ImageTags");
            if (tags && *tags == '{') {
                json_str(tags, "Primary", m->thumb, sizeof(m->thumb));
            }
            added++;
        }
        free(body);
        if (added == 0) {
            break;
        }
        if (total < 0) {
            total = PLEX_MOVIES_MAX;
        }
        count += added;
        if (progress) {
            *progress = count;
        }
    } while (count < total && count < PLEX_MOVIES_MAX);

    if (count == 0) {
        free(list);
        *out = NULL;
        *out_count = 0;
        return PLEX_OK;
    }
    if (count < PLEX_MOVIES_MAX) {
        plex_movie_t *shrunk = realloc(list, sizeof(plex_movie_t) * count);
        if (shrunk) {
            list = shrunk;
        }
    }
    *out = list;
    *out_count = count;
    return PLEX_OK;
}

int jf_load_movies(plex_movie_t **out, int *out_count, volatile int *progress) {
    // One query: movies and TV series interleave alphabetically
    return jf_load_items("/Items?IncludeItemTypes=Movie,Series&Recursive=true&SortBy=SortName"
                         "&Fields=ProductionYear",
                         out, out_count, progress);
}

int jf_load_episodes(const char *series_id, plex_movie_t **out, int *out_count, volatile int *progress) {
    char base[160];
    // The Shows endpoint returns every season's episodes in airing order
    snprintf(base, sizeof(base), "/Shows/%s/Episodes?Fields=ProductionYear", series_id);
    return jf_load_items(base, out, out_count, progress);
}

static const char *jf_cache_dir(void) {
    return fs_file_exists("/mnt/extsd") ? JF_CACHE_SD : JF_CACHE_TMP;
}

bool jf_poster_cached(const plex_movie_t *movie, char *path_out, int path_size) {
    snprintf(path_out, path_size, "%s/jf_%s.jpg", jf_cache_dir(), movie->rating_key);
    return fs_file_exists(path_out);
}

int jf_fetch_poster(const plex_movie_t *movie, int width, int height, char *path_out, int path_size) {
    if (!movie->thumb[0]) {
        return PLEX_ERR_PROTO;
    }
    if (jf_poster_cached(movie, path_out, path_size)) {
        return PLEX_OK;
    }
    mkdir(jf_cache_dir(), 0755);

    char path[256];
    snprintf(path, sizeof(path),
             "/Items/%s/Images/Primary?fillWidth=%d&fillHeight=%d&quality=90&format=Jpg",
             movie->rating_key, width, height);

    char headers[512];
    jf_headers(headers, sizeof(headers), true);

    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.part", path_out);
    lan_stream_state_t st = {0};
    int rc = lanhttp_download(g_setting.jellyfin.host, g_setting.jellyfin.port, path, headers, tmp, &st, false);
    if (rc != 0 || st.bytes == 0 || rename(tmp, path_out) != 0) {
        unlink(tmp);
        return PLEX_ERR_NET;
    }
    return PLEX_OK;
}

int jf_quickconnect_start(char *code, int code_size, char *secret, int secret_size) {
    char headers[512];
    jf_headers(headers, sizeof(headers), false);

    char *body = NULL;
    int status = lanhttp_request(g_setting.jellyfin.host, g_setting.jellyfin.port,
                                 "POST", "/QuickConnect/Initiate", headers, "", &body, NULL);
    int rc = jf_map_status(status);
    if (rc != PLEX_OK || !body) {
        free(body);
        // 401 on initiate usually means Quick Connect is disabled server-side
        return rc == PLEX_ERR_AUTH ? PLEX_ERR_PROTO : rc;
    }
    bool ok = json_str(body, "Code", code, code_size) &&
              json_str(body, "Secret", secret, secret_size);
    free(body);
    if (!ok) {
        return PLEX_ERR_PROTO;
    }
    LOGI("jellyfin: quickconnect code %s", code);
    return PLEX_OK;
}

int jf_quickconnect_poll(const char *secret) {
    char headers[512];
    jf_headers(headers, sizeof(headers), false);

    char path[192];
    snprintf(path, sizeof(path), "/QuickConnect/Connect?secret=%s", secret);
    char *body = NULL;
    int status = lanhttp_request(g_setting.jellyfin.host, g_setting.jellyfin.port,
                                 "GET", path, headers, NULL, &body, NULL);
    int rc = jf_map_status(status);
    if (rc != PLEX_OK || !body) {
        free(body);
        return rc == PLEX_OK ? PLEX_ERR_NET : rc;
    }
    bool authenticated = json_true(body, "Authenticated");
    free(body);
    if (!authenticated) {
        return PLEX_PENDING;
    }

    // Exchange the approved secret for a session token
    char post[192];
    snprintf(post, sizeof(post), "{\"Secret\":\"%s\"}", secret);
    body = NULL;
    status = lanhttp_request(g_setting.jellyfin.host, g_setting.jellyfin.port,
                             "POST", "/Users/AuthenticateWithQuickConnect", headers, post, &body, NULL);
    rc = jf_map_status(status);
    if (rc != PLEX_OK || !body) {
        free(body);
        return rc == PLEX_OK ? PLEX_ERR_NET : rc;
    }

    bool ok = json_str(body, "AccessToken", g_setting.jellyfin.token, sizeof(g_setting.jellyfin.token));
    const char *user = json_field(body, "User");
    if (user && *user == '{') {
        json_str(user, "Id", g_setting.jellyfin.user_id, sizeof(g_setting.jellyfin.user_id));
    }
    free(body);
    if (!ok) {
        return PLEX_ERR_PROTO;
    }
    jf_settings_save();
    LOGI("jellyfin: quickconnect linked, token stored");
    return PLEX_OK;
}

static char *jf_trim(char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    return s;
}

bool jf_token_from_sdcard(void) {
    FILE *fp = fopen(JF_TOKEN_FILE, "r");
    if (!fp) {
        return false;
    }
    char token_line[PLEX_TOKEN_MAX * 2] = "";
    char host_line[PLEX_HOST_MAX * 2] = "";
    const bool got = fgets(token_line, sizeof(token_line), fp) != NULL;
    if (got && !fgets(host_line, sizeof(host_line), fp)) {
        host_line[0] = '\0';
    }
    fclose(fp);
    if (!got) {
        return false;
    }

    char *token = jf_trim(token_line);
    if (!*token) {
        return false;
    }
    snprintf(g_setting.jellyfin.token, sizeof(g_setting.jellyfin.token), "%s", token);
    g_setting.jellyfin.user_id[0] = '\0'; // re-resolve for the new token

    char *host = jf_trim(host_line);
    if (*host) {
        char *colon = strrchr(host, ':');
        int port = 8096;
        if (colon) {
            *colon = '\0';
            port = atoi(colon + 1);
        }
        snprintf(g_setting.jellyfin.host, sizeof(g_setting.jellyfin.host), "%s", host);
        g_setting.jellyfin.port = port > 0 ? port : 8096;
    }

    jf_settings_save();
    LOGI("jellyfin: adopted token%s from %s", *host ? " and server" : "", JF_TOKEN_FILE);
    return true;
}

// Ask the server to open a playback session before streaming. Two things
// come back that are worth having: the PlaySessionId the server itself
// issues (progress reports have to quote it or they match no session, and
// the transcode throttler is driven off those reports), and the exact
// runtime, which the library listing only carries rounded to whole minutes.
int jf_playback_info(const char *item_id, int max_kbps, char *play_session, int session_size,
                     long long *runtime_ms) {
    char path[256];
    snprintf(path, sizeof(path), "/Items/%s/PlaybackInfo?userId=%s",
             item_id, g_setting.jellyfin.user_id);

    char headers[512];
    jf_headers(headers, sizeof(headers), true);

    // The profile mirrors what jf_stream_path actually requests, so the
    // server plans the same transcode it is about to be asked for.
    char body[640];
    snprintf(body, sizeof(body),
             "{\"DeviceProfile\":{\"MaxStreamingBitrate\":%d000,"
             "\"DirectPlayProfiles\":[],"
             "\"TranscodingProfiles\":[{\"Container\":\"ts\",\"Type\":\"Video\","
             "\"VideoCodec\":\"h264\",\"AudioCodec\":\"aac\",\"Protocol\":\"http\","
             "\"Context\":\"Streaming\",\"MaxAudioChannels\":\"2\"}]}}",
             max_kbps);

    char *resp = NULL;
    int status = lanhttp_request(g_setting.jellyfin.host, g_setting.jellyfin.port,
                                 "POST", path, headers, body, &resp, NULL);
    int rc = jf_map_status(status);
    if (rc != PLEX_OK || !resp) {
        free(resp);
        return rc == PLEX_OK ? PLEX_ERR_PROTO : rc;
    }

    bool const got_session = json_str(resp, "PlaySessionId", play_session, session_size);

    if (runtime_ms) {
        const char *src = json_array_first(resp, "MediaSources");
        long long const ticks = src ? json_ll(src, "RunTimeTicks", 0) : 0;
        *runtime_ms = ticks / 10000LL; // 100ns ticks -> ms
    }
    free(resp);

    if (!got_session) {
        return PLEX_ERR_PROTO;
    }
    LOGI("jellyfin: playback session %s, runtime %lldms", play_session, runtime_ms ? *runtime_ms : 0);
    return PLEX_OK;
}

void jf_stream_path(char *buf, int size, const plex_movie_t *movie, int offset_s, int max_kbps,
                    const char *play_session) {
    // Continuous transcoded TS over one HTTP response; the token travels
    // as api_key since the download path sends no custom headers.
    // Stream copy must be explicitly DISABLED (Jellyfin defaults it on):
    // copied source video can be interlaced or high-level H.264, which
    // wedges the goggle's hardware decoder solid. A forced transcode
    // yields normalized progressive H.264 the player is known to handle.
    // The bitrate implies the resolution tier (1080p/720p/480p).
    // Constrain the output to the exact shape the goggle's player is
    // proven with (matching the Plex transcode profile): stereo AAC -
    // 5.1 output would be new territory for the audio decoder - <=30 fps,
    // and H.264 high@4.0.
    snprintf(buf, size,
             "/Videos/%s/stream.ts?VideoCodec=h264&AudioCodec=aac"
             "&VideoBitrate=%d000&AudioBitrate=192000&MaxWidth=%d"
             "&MaxAudioChannels=2&MaxFramerate=30&Profile=high&Level=40"
             "&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false"
             "&SubtitleMethod=None&StartTimeTicks=%lld&DeviceId=%s"
             "&PlaySessionId=%s&api_key=%s",
             movie->rating_key, max_kbps,
             max_kbps >= 8000 ? 1920 : (max_kbps >= 4000 ? 1280 : 854),
             (long long)offset_s * 10000000LL,
             jf_device_id(), play_session, g_setting.jellyfin.token);
}

int jf_stream_download(const char *path, const char *dest_file, lan_stream_state_t *state) {
    return lanhttp_download(g_setting.jellyfin.host, g_setting.jellyfin.port, path, NULL, dest_file, state, true);
}

void jf_playback_report(const char *item_id, const char *play_session,
                        jf_play_event_t event, long long pos_ticks) {
    static const char *paths[] = {
        "/Sessions/Playing",
        "/Sessions/Playing/Progress",
        "/Sessions/Playing/Stopped",
    };
    char headers[512];
    jf_headers(headers, sizeof(headers), true);

    char body[512];
    snprintf(body, sizeof(body),
             "{\"ItemId\":\"%s\",\"PlaySessionId\":\"%s\",\"PositionTicks\":%lld,"
             "\"PlayMethod\":\"Transcode\",\"CanSeek\":true,\"IsPaused\":false}",
             item_id, play_session, pos_ticks);

    int status = lanhttp_request(g_setting.jellyfin.host, g_setting.jellyfin.port,
                                 "POST", paths[event], headers, body, NULL, NULL);
    if (status < 200 || status >= 300) {
        LOGI("jellyfin: playback report %d returned %d", event, status);
    }
}
