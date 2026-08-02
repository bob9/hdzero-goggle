#include "plexapi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
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

#define PLEX_CONNECT_TIMEOUT_S 3
#define PLEX_IO_TIMEOUT_S      5
#define PLEX_BODY_MAX          (4 * 1024 * 1024)
#define PLEX_CHUNK             100 // movies per listing request

#define PLEX_CACHE_SD  "/mnt/extsd/plexcache"
#define PLEX_CACHE_TMP "/tmp/plexcache"

#define GDM_ADDR "239.0.0.250"
#define GDM_PORT 32414

typedef struct {
    int status; // HTTP status, or <0 on network error
    char *body; // malloc'd, NUL-terminated
    size_t body_len;
} plex_http_resp_t;

/**
 * The stable identifier Plex uses to recognize this client. Generated once
 * and persisted alongside the server settings.
 */
static const char *plex_client_id(void) {
    if (!g_setting.plex.client_id[0]) {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        snprintf(g_setting.plex.client_id, sizeof(g_setting.plex.client_id),
                 "hdzg-%08x%08x", (unsigned)rand(), (unsigned)rand());
        plex_settings_save();
    }
    return g_setting.plex.client_id;
}

void plex_settings_save(void) {
    ini_puts("plex", "host", g_setting.plex.host, SETTING_INI);
    ini_putl("plex", "port", g_setting.plex.port, SETTING_INI);
    ini_puts("plex", "token", g_setting.plex.token, SETTING_INI);
    ini_puts("plex", "client_id", g_setting.plex.client_id, SETTING_INI);
    ini_putl("plex", "backend", g_setting.plex.backend, SETTING_INI);
    ini_putl("plex", "quality", g_setting.plex.quality, SETTING_INI);
}

#define PLEX_TOKEN_FILE "/mnt/extsd/plextoken.txt"

// Trim surrounding whitespace/newlines from editors on any OS
static char *plex_trim(char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    return s;
}

bool plex_token_from_sdcard(void) {
    FILE *fp = fopen(PLEX_TOKEN_FILE, "r");
    if (!fp) {
        return false;
    }

    char token_line[PLEX_TOKEN_MAX * 2] = "";
    char host_line[PLEX_HOST_MAX * 2] = "";
    const bool got = fgets(token_line, sizeof(token_line), fp) != NULL;
    if (got) {
        // Optional second line: host[:port], for fully typing-free setup
        if (!fgets(host_line, sizeof(host_line), fp)) {
            host_line[0] = '\0';
        }
    }
    fclose(fp);
    if (!got) {
        return false;
    }

    char *token = plex_trim(token_line);
    if (!*token) {
        return false;
    }
    snprintf(g_setting.plex.token, sizeof(g_setting.plex.token), "%s", token);

    char *host = plex_trim(host_line);
    if (*host) {
        char *colon = strrchr(host, ':');
        int port = 32400;
        if (colon) {
            *colon = '\0';
            port = atoi(colon + 1);
        }
        snprintf(g_setting.plex.host, sizeof(g_setting.plex.host), "%s", host);
        g_setting.plex.port = port > 0 ? port : 32400;
    }

    plex_settings_save();
    LOGI("plex: adopted token%s from %s", *host ? " and server" : "", PLEX_TOKEN_FILE);
    return true;
}

static int plex_connect(const char *host, int port) {
    char port_str[8];
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;

    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    // Large receive window before connect(): the WiFi link's latency times
    // the kernel's small default window otherwise caps download throughput
    int rcvbuf = 512 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    // Bounded connect: the UI worker must never hang on a dead address
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno == EINPROGRESS) {
        fd_set wfds;
        struct timeval tv = {.tv_sec = PLEX_CONNECT_TIMEOUT_S};
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        rc = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (rc > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            rc = err ? -1 : 0;
        } else {
            rc = -1;
        }
    }
    if (rc < 0) {
        close(fd);
        return -1;
    }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    struct timeval io_tv = {.tv_sec = PLEX_IO_TIMEOUT_S};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_tv, sizeof(io_tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_tv, sizeof(io_tv));
    return fd;
}

static int plex_build_request(char *req, int size, const char *host, int port, const char *path) {
    int req_len = snprintf(req, size,
                           "GET %s HTTP/1.0\r\n"
                           "Host: %s:%d\r\n"
                           "Accept: application/xml\r\n"
                           "X-Plex-Client-Identifier: %s\r\n"
                           "X-Plex-Product: HDZero Goggle\r\n"
                           "X-Plex-Version: 1.0\r\n"
                           "X-Plex-Device: HDZero Goggle\r\n"
                           "X-Plex-Platform: Linux\r\n"
                           "%s%s%s"
                           "Connection: close\r\n"
                           "\r\n",
                           path, host, port, plex_client_id(),
                           g_setting.plex.token[0] ? "X-Plex-Token: " : "",
                           g_setting.plex.token[0] ? g_setting.plex.token : "",
                           g_setting.plex.token[0] ? "\r\n" : "");
    return (req_len >= size) ? -1 : req_len;
}

/**
 * HTTP/1.0 GET. Connection: close framing means the body simply ends at EOF,
 * so no chunked-transfer handling is needed. Response body is malloc'd.
 */
static int plex_http_get(const char *host, int port, const char *path, plex_http_resp_t *resp) {
    memset(resp, 0, sizeof(*resp));
    resp->status = PLEX_ERR_NET;

    int fd = plex_connect(host, port);
    if (fd < 0) {
        return PLEX_ERR_NET;
    }

    char req[1024];
    int req_len = plex_build_request(req, sizeof(req), host, port, path);
    if (req_len < 0 || write(fd, req, req_len) != req_len) {
        close(fd);
        return PLEX_ERR_NET;
    }

    size_t cap = 64 * 1024, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(fd);
        return PLEX_ERR_NET;
    }

    for (;;) {
        if (len + 8192 + 1 > cap) {
            if (cap * 2 > PLEX_BODY_MAX) {
                LOGE("plex: response exceeds %d bytes, aborting", PLEX_BODY_MAX);
                free(buf);
                close(fd);
                return PLEX_ERR_PROTO;
            }
            cap *= 2;
            char *nbuf = realloc(buf, cap);
            if (!nbuf) {
                free(buf);
                close(fd);
                return PLEX_ERR_NET;
            }
            buf = nbuf;
        }
        ssize_t n = read(fd, buf + len, 8192);
        if (n < 0) {
            free(buf);
            close(fd);
            return PLEX_ERR_NET;
        }
        if (n == 0) {
            break;
        }
        len += n;
    }
    close(fd);
    buf[len] = '\0';

    int status = 0;
    if (sscanf(buf, "HTTP/%*d.%*d %d", &status) != 1) {
        free(buf);
        return PLEX_ERR_PROTO;
    }

    char *body = strstr(buf, "\r\n\r\n");
    if (!body) {
        free(buf);
        return PLEX_ERR_PROTO;
    }
    body += 4;

    resp->status = status;
    resp->body_len = len - (body - buf);
    resp->body = malloc(resp->body_len + 1);
    if (!resp->body) {
        free(buf);
        return PLEX_ERR_NET;
    }
    memcpy(resp->body, body, resp->body_len);
    resp->body[resp->body_len] = '\0';
    free(buf);

    if (status == 401 || status == 403) {
        return PLEX_ERR_AUTH;
    }
    if (status != 200) {
        return PLEX_ERR_PROTO;
    }
    return PLEX_OK;
}

static void plex_http_free(plex_http_resp_t *resp) {
    free(resp->body);
    resp->body = NULL;
}

/**
 * XML helpers. Plex attribute values are double-quoted; elements of interest
 * are scanned by "<Name " prefix and bounded by the closing '>'.
 */
static const char *xml_next_elem(const char *p, const char *name) {
    size_t name_len = strlen(name);
    while ((p = strchr(p, '<')) != NULL) {
        if (strncmp(p + 1, name, name_len) == 0 &&
            (p[1 + name_len] == ' ' || p[1 + name_len] == '>' || p[1 + name_len] == '/')) {
            return p + 1;
        }
        p++;
    }
    return NULL;
}

static void xml_entity_decode(char *s) {
    static const struct {
        const char *ent;
        char ch;
    } tab[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&#39;", '\''}};
    char *w = s;
    while (*s) {
        if (*s == '&') {
            bool matched = false;
            for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
                size_t l = strlen(tab[i].ent);
                if (strncmp(s, tab[i].ent, l) == 0) {
                    *w++ = tab[i].ch;
                    s += l;
                    matched = true;
                    break;
                }
            }
            if (matched) {
                continue;
            }
        }
        *w++ = *s++;
    }
    *w = '\0';
}

static bool xml_attr(const char *tag, const char *name, char *out, int out_size) {
    const char *end = strchr(tag, '>');
    if (!end) {
        return false;
    }

    size_t name_len = strlen(name);
    const char *p = tag;
    while ((p = strstr(p, name)) != NULL && p < end) {
        // Attribute name must be preceded by whitespace and followed by ="
        if (p[-1] == ' ' && p[name_len] == '=' && p[name_len + 1] == '"') {
            const char *v = p + name_len + 2;
            const char *q = strchr(v, '"');
            if (!q || q > end) {
                return false;
            }
            int n = q - v;
            if (n >= out_size) {
                n = out_size - 1;
            }
            memcpy(out, v, n);
            out[n] = '\0';
            xml_entity_decode(out);
            return true;
        }
        p += name_len;
    }
    return false;
}

static long xml_attr_long(const char *tag, const char *name, long fallback) {
    char buf[24];
    if (!xml_attr(tag, name, buf, sizeof(buf))) {
        return fallback;
    }
    return atol(buf);
}

static void url_encode(const char *in, char *out, int out_size) {
    static const char hex[] = "0123456789ABCDEF";
    int w = 0;
    for (; *in && w < out_size - 4; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[w++] = c;
        } else {
            out[w++] = '%';
            out[w++] = hex[c >> 4];
            out[w++] = hex[c & 0xF];
        }
    }
    out[w] = '\0';
}

int plex_gdm_discover(plex_server_t *out, int max_out) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return 0;
    }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    struct timeval tv = {.tv_usec = 300 * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const char *msearch = "M-SEARCH * HTTP/1.1\r\n\r\n";
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(GDM_PORT),
    };

    // Multicast is the specified GDM target; broadcast covers setups that
    // filter multicast.
    inet_aton(GDM_ADDR, &dst.sin_addr);
    sendto(fd, msearch, strlen(msearch), 0, (struct sockaddr *)&dst, sizeof(dst));
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(fd, msearch, strlen(msearch), 0, (struct sockaddr *)&dst, sizeof(dst));

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

        plex_server_t srv = {.port = 32400};
        inet_ntop(AF_INET, &src.sin_addr, srv.host, sizeof(srv.host));

        char *line = strtok(buf, "\r\n");
        while (line) {
            if (strncasecmp(line, "Name:", 5) == 0) {
                const char *v = line + 5;
                while (*v == ' ') {
                    v++;
                }
                snprintf(srv.name, sizeof(srv.name), "%s", v);
            } else if (strncasecmp(line, "Port:", 5) == 0) {
                srv.port = atoi(line + 5);
            }
            line = strtok(NULL, "\r\n");
        }

        bool dup = false;
        for (int i = 0; i < count; i++) {
            if (strcmp(out[i].host, srv.host) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            if (!srv.name[0]) {
                snprintf(srv.name, sizeof(srv.name), "Plex Server");
            }
            out[count++] = srv;
            LOGI("plex: discovered %s at %s:%d", srv.name, srv.host, srv.port);
        }
    }

    close(fd);
    return count;
}

bool plex_server_reachable(const char *host, int port) {
    plex_http_resp_t resp;
    int rc = plex_http_get(host, port, "/identity", &resp);
    plex_http_free(&resp);
    return rc == PLEX_OK;
}

int plex_find_section(const char *type, char *key, int key_size, char *title, int title_size) {
    plex_http_resp_t resp;
    int rc = plex_http_get(g_setting.plex.host, g_setting.plex.port, "/library/sections", &resp);
    if (rc != PLEX_OK) {
        plex_http_free(&resp);
        return rc;
    }

    rc = PLEX_ERR_NOMOVIE;
    const char *p = resp.body;
    while ((p = xml_next_elem(p, "Directory")) != NULL) {
        char t[16];
        if (xml_attr(p, "type", t, sizeof(t)) && strcmp(t, type) == 0) {
            if (xml_attr(p, "key", key, key_size)) {
                if (!xml_attr(p, "title", title, title_size)) {
                    snprintf(title, title_size, "%s", strcmp(type, "show") == 0 ? "TV Shows" : "Movies");
                }
                rc = PLEX_OK;
                break;
            }
        }
    }

    plex_http_free(&resp);
    return rc;
}

// Movies and episodes arrive as <Video>, series listings as <Directory>
static int plex_parse_items(const char *xml, const char *elem, int kind,
                            plex_movie_t *list, int offset, int max, long *total_out) {
    const char *container = xml_next_elem(xml, "MediaContainer");
    if (container && total_out) {
        *total_out = xml_attr_long(container, "totalSize", -1);
    }

    int added = 0;
    const char *p = xml;
    while (offset + added < max && (p = xml_next_elem(p, elem)) != NULL) {
        plex_movie_t *m = &list[offset + added];
        memset(m, 0, sizeof(*m));

        if (!xml_attr(p, "ratingKey", m->rating_key, sizeof(m->rating_key))) {
            p++;
            continue;
        }
        if (!xml_attr(p, "title", m->title, sizeof(m->title))) {
            snprintf(m->title, sizeof(m->title), "Untitled");
        }
        xml_attr(p, "thumb", m->thumb, sizeof(m->thumb));
        m->year = (int)xml_attr_long(p, "year", 0);
        m->duration_min = (int)(xml_attr_long(p, "duration", 0) / 60000);
        m->watched = xml_attr_long(p, "viewCount", 0) > 0;
        m->kind = kind;
        m->season = (int16_t)xml_attr_long(p, "parentIndex", 0);
        m->episode = (int16_t)xml_attr_long(p, "index", 0);

        added++;
        p++;
    }
    return added;
}

static int plex_load_list(const char *base_path, const char *elem, int kind,
                          plex_movie_t **out, int *out_count, volatile int *progress) {
    plex_movie_t *list = malloc(sizeof(plex_movie_t) * PLEX_MOVIES_MAX);
    if (!list) {
        return PLEX_ERR_NET;
    }

    int count = 0;
    long total = -1;
    do {
        char path[256];
        snprintf(path, sizeof(path),
                 "%s%cX-Plex-Container-Start=%d&X-Plex-Container-Size=%d",
                 base_path, strchr(base_path, '?') ? '&' : '?', count, PLEX_CHUNK);

        plex_http_resp_t resp;
        int rc = plex_http_get(g_setting.plex.host, g_setting.plex.port, path, &resp);
        if (rc != PLEX_OK) {
            plex_http_free(&resp);
            free(list);
            return rc;
        }

        int added = plex_parse_items(resp.body, elem, kind, list, count, PLEX_MOVIES_MAX, total < 0 ? &total : NULL);
        plex_http_free(&resp);
        if (added == 0) {
            break;
        }
        if (total < 0) {
            // Server did not report totalSize; keep paging until a short page
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

int plex_load_movies(const char *section_key, plex_movie_t **out, int *out_count, volatile int *progress) {
    char base[128];
    snprintf(base, sizeof(base), "/library/sections/%s/all?type=1&sort=titleSort", section_key);
    return plex_load_list(base, "Video", PLEX_ITEM_MOVIE, out, out_count, progress);
}

int plex_load_shows(const char *section_key, plex_movie_t **out, int *out_count, volatile int *progress) {
    char base[128];
    snprintf(base, sizeof(base), "/library/sections/%s/all?type=2&sort=titleSort", section_key);
    return plex_load_list(base, "Directory", PLEX_ITEM_SERIES, out, out_count, progress);
}

int plex_load_episodes(const char *series_key, plex_movie_t **out, int *out_count, volatile int *progress) {
    char base[128];
    // allLeaves walks seasons for us; default order is airing order
    snprintf(base, sizeof(base), "/library/metadata/%s/allLeaves", series_key);
    return plex_load_list(base, "Video", PLEX_ITEM_EPISODE, out, out_count, progress);
}

int plex_server_request(const char *path) {
    plex_http_resp_t resp;
    int rc = plex_http_get(g_setting.plex.host, g_setting.plex.port, path, &resp);
    plex_http_free(&resp);
    return rc;
}

int plex_stream_download(const char *path, const char *dest_file, plex_stream_state_t *state) {
    state->bytes = 0;
    state->done = false;
    state->result = PLEX_ERR_NET;

    int fd = plex_connect(g_setting.plex.host, g_setting.plex.port);
    if (fd < 0) {
        state->done = true;
        return PLEX_ERR_NET;
    }

    char req[1536];
    int req_len = plex_build_request(req, sizeof(req), g_setting.plex.host, g_setting.plex.port, path);
    if (req_len < 0 || write(fd, req, req_len) != req_len) {
        close(fd);
        state->done = true;
        return PLEX_ERR_NET;
    }

    // Parse the response header, then stream the body straight to disk
    char head[4096];
    size_t head_len = 0;
    char *body_start = NULL;
    int head_quiet = 0;
    while (head_len < sizeof(head) - 1) {
        ssize_t n = read(fd, head + head_len, sizeof(head) - 1 - head_len);
        // A cold transcode can take longer than the socket timeout to emit
        // its headers; that is not a network failure (see lanhttp_download)
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (++head_quiet >= 12) {
                LOGE("plex: no response headers after 60s");
                close(fd);
                state->done = true;
                return PLEX_ERR_NET;
            }
            continue;
        }
        if (n <= 0) {
            close(fd);
            state->done = true;
            return PLEX_ERR_NET;
        }
        head_len += n;
        head[head_len] = '\0';
        if ((body_start = strstr(head, "\r\n\r\n")) != NULL) {
            body_start += 4;
            break;
        }
    }
    int status = 0;
    if (!body_start || sscanf(head, "HTTP/%*d.%*d %d", &status) != 1 || status != 200) {
        LOGE("plex: stream http status %d", status);
        close(fd);
        state->done = true;
        state->result = (status == 401 || status == 403) ? PLEX_ERR_AUTH : PLEX_ERR_PROTO;
        return state->result;
    }

    // Overwrite from the front rather than truncating, so a preallocated
    // stream file keeps its size (see plexstream.h).
    FILE *out = fopen(dest_file, "r+b");
    if (!out) {
        out = fopen(dest_file, "wb");
    }
    if (!out) {
        close(fd);
        state->done = true;
        return PLEX_ERR_NET;
    }

    size_t body_in_head = head_len - (body_start - head);
    if (body_in_head > 0) {
        fwrite(body_start, 1, body_in_head, out);
        state->bytes = body_in_head;
    }

    char buf[64 * 1024];
    int quiet_reads = 0; // the transcoder legitimately pauses; only a long
                         // silence (~60s) is a real failure
    int rc = PLEX_OK;
    while (!state->cancel) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            quiet_reads = 0;
            if (fwrite(buf, 1, n, out) != (size_t)n) {
                LOGE("plex: stream write failed (disk full?)");
                rc = PLEX_ERR_NET;
                break;
            }
            state->bytes += n;
            // Keep the on-disk frontier honest for the concurrent reader
            if ((state->bytes & ((256 * 1024) - 1)) < (long)sizeof(buf)) {
                fflush(out);
            }
        } else if (n == 0) {
            break; // transcode complete
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                if (++quiet_reads >= 12) { // 12 x 5s recv timeout
                    LOGE("plex: stream stalled for 60s, giving up");
                    rc = PLEX_ERR_NET;
                    break;
                }
                continue;
            }
            rc = PLEX_ERR_NET;
            break;
        }
    }

    fflush(out);
    fclose(out);
    close(fd);
    state->result = state->cancel ? PLEX_OK : rc;
    state->done = true;
    LOGI("plex: stream ended, %ld bytes, rc=%d%s", state->bytes, rc, state->cancel ? " (cancelled)" : "");
    return state->result;
}

/**
 * plex.tv PIN-link flow. The firmware's bundled curl (BearSSL) handles the
 * HTTPS leg to plex.tv, exactly like the online firmware downloader does.
 * -k mirrors the existing downloader scripts (no CA bundle on the device).
 */
static int plex_curl_pin(const char *method_args, char *out, int out_size) {
    char cmd[768];
    snprintf(cmd, sizeof(cmd),
             "curl -ks -m 15 %s "
             "-H 'Accept: application/xml' "
             "-H 'X-Plex-Product: HDZero Goggle' "
             "-H 'X-Plex-Version: 1.0' "
             "-H 'X-Plex-Client-Identifier: %s' 2>/dev/null",
             method_args, plex_client_id());

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return PLEX_ERR_NET;
    }
    size_t len = fread(out, 1, out_size - 1, fp);
    int status = pclose(fp);
    out[len] = '\0';
    if (status != 0 || len == 0) {
        return PLEX_ERR_NET;
    }
    return PLEX_OK;
}

int plex_pin_start(int *pin_id, char *code, int code_size) {
    char body[4096];
    int rc = plex_curl_pin("-X POST 'https://plex.tv/api/v2/pins?strong=false'", body, sizeof(body));
    if (rc != PLEX_OK) {
        return rc;
    }

    const char *pin = xml_next_elem(body, "pin");
    if (!pin) {
        return PLEX_ERR_PROTO;
    }
    long id = xml_attr_long(pin, "id", -1);
    if (id < 0 || !xml_attr(pin, "code", code, code_size)) {
        return PLEX_ERR_PROTO;
    }
    *pin_id = (int)id;
    LOGI("plex: pin %ld code %s", id, code);
    return PLEX_OK;
}

int plex_pin_poll(int pin_id) {
    char args[128], body[4096];
    snprintf(args, sizeof(args), "'https://plex.tv/api/v2/pins/%d'", pin_id);
    int rc = plex_curl_pin(args, body, sizeof(body));
    if (rc != PLEX_OK) {
        return rc;
    }

    const char *pin = xml_next_elem(body, "pin");
    if (!pin) {
        return PLEX_ERR_PROTO;
    }

    char token[PLEX_TOKEN_MAX] = "";
    if (!xml_attr(pin, "authToken", token, sizeof(token)) || !token[0]) {
        return PLEX_PENDING;
    }

    snprintf(g_setting.plex.token, sizeof(g_setting.plex.token), "%s", token);
    plex_settings_save();
    LOGI("plex: pin linked, token stored");
    return PLEX_OK;
}

static const char *plex_cache_dir(void) {
    return fs_file_exists("/mnt/extsd") ? PLEX_CACHE_SD : PLEX_CACHE_TMP;
}

bool plex_poster_cached(const plex_movie_t *movie, char *path_out, int path_size) {
    snprintf(path_out, path_size, "%s/%s.jpg", plex_cache_dir(), movie->rating_key);
    return fs_file_exists(path_out);
}

int plex_fetch_poster(const plex_movie_t *movie, int width, int height, char *path_out, int path_size) {
    if (!movie->thumb[0]) {
        return PLEX_ERR_PROTO;
    }
    if (plex_poster_cached(movie, path_out, path_size)) {
        return PLEX_OK;
    }

    mkdir(plex_cache_dir(), 0755);

    char thumb_enc[256];
    url_encode(movie->thumb, thumb_enc, sizeof(thumb_enc));

    char path[512];
    snprintf(path, sizeof(path),
             "/photo/:/transcode?width=%d&height=%d&minSize=1&upscale=1&url=%s",
             width, height, thumb_enc);

    plex_http_resp_t resp;
    int rc = plex_http_get(g_setting.plex.host, g_setting.plex.port, path, &resp);
    if (rc != PLEX_OK) {
        plex_http_free(&resp);
        return rc;
    }

    // Write to a temp name first so a half-written file is never mistaken
    // for a valid cached poster.
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.part", path_out);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        plex_http_free(&resp);
        return PLEX_ERR_NET;
    }
    size_t written = fwrite(resp.body, 1, resp.body_len, fp);
    fclose(fp);
    plex_http_free(&resp);

    if (written != resp.body_len || rename(tmp, path_out) != 0) {
        unlink(tmp);
        return PLEX_ERR_NET;
    }
    return PLEX_OK;
}
