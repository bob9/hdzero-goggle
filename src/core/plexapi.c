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
    int req_len = snprintf(req, sizeof(req),
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
    if (req_len >= (int)sizeof(req) || write(fd, req, req_len) != req_len) {
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

int plex_find_movie_section(char *key, int key_size, char *title, int title_size) {
    plex_http_resp_t resp;
    int rc = plex_http_get(g_setting.plex.host, g_setting.plex.port, "/library/sections", &resp);
    if (rc != PLEX_OK) {
        plex_http_free(&resp);
        return rc;
    }

    rc = PLEX_ERR_NOMOVIE;
    const char *p = resp.body;
    while ((p = xml_next_elem(p, "Directory")) != NULL) {
        char type[16];
        if (xml_attr(p, "type", type, sizeof(type)) && strcmp(type, "movie") == 0) {
            if (xml_attr(p, "key", key, key_size)) {
                if (!xml_attr(p, "title", title, title_size)) {
                    snprintf(title, title_size, "Movies");
                }
                rc = PLEX_OK;
                break;
            }
        }
    }

    plex_http_free(&resp);
    return rc;
}

static int plex_parse_movies(const char *xml, plex_movie_t *list, int offset, int max, long *total_out) {
    const char *container = xml_next_elem(xml, "MediaContainer");
    if (container && total_out) {
        *total_out = xml_attr_long(container, "totalSize", -1);
    }

    int added = 0;
    const char *p = xml;
    while (offset + added < max && (p = xml_next_elem(p, "Video")) != NULL) {
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

        added++;
        p++;
    }
    return added;
}

int plex_load_movies(const char *section_key, plex_movie_t **out, int *out_count, volatile int *progress) {
    plex_movie_t *list = malloc(sizeof(plex_movie_t) * PLEX_MOVIES_MAX);
    if (!list) {
        return PLEX_ERR_NET;
    }

    int count = 0;
    long total = -1;
    do {
        char path[256];
        snprintf(path, sizeof(path),
                 "/library/sections/%s/all?type=1&sort=titleSort"
                 "&X-Plex-Container-Start=%d&X-Plex-Container-Size=%d",
                 section_key, count, PLEX_CHUNK);

        plex_http_resp_t resp;
        int rc = plex_http_get(g_setting.plex.host, g_setting.plex.port, path, &resp);
        if (rc != PLEX_OK) {
            plex_http_free(&resp);
            free(list);
            return rc;
        }

        int added = plex_parse_movies(resp.body, list, count, PLEX_MOVIES_MAX, total < 0 ? &total : NULL);
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
