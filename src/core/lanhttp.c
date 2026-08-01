#include "lanhttp.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <log/log.h>

#define LANHTTP_CONNECT_TIMEOUT_S 3
#define LANHTTP_IO_TIMEOUT_S      5
#define LANHTTP_BODY_MAX          (4 * 1024 * 1024)

static int lanhttp_connect(const char *host, int port) {
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

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno == EINPROGRESS) {
        fd_set wfds;
        struct timeval tv = {.tv_sec = LANHTTP_CONNECT_TIMEOUT_S};
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
    struct timeval io_tv = {.tv_sec = LANHTTP_IO_TIMEOUT_S};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_tv, sizeof(io_tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_tv, sizeof(io_tv));
    return fd;
}

static int lanhttp_send_request(int fd, const char *host, int port, const char *method,
                                const char *path, const char *extra_headers, const char *post_body) {
    char req[2048];
    size_t body_len = post_body ? strlen(post_body) : 0;
    int req_len = snprintf(req, sizeof(req),
                           "%s %s HTTP/1.0\r\n"
                           "Host: %s:%d\r\n"
                           "Accept: application/json\r\n"
                           "%s"
                           "%s"
                           "Connection: close\r\n"
                           "\r\n",
                           method, path, host, port,
                           extra_headers ? extra_headers : "",
                           post_body ? "Content-Type: application/json\r\n" : "");
    if (req_len >= (int)sizeof(req)) {
        return -1;
    }
    // Content-Length must precede the blank line; rebuild when posting
    if (post_body) {
        req_len = snprintf(req, sizeof(req),
                           "%s %s HTTP/1.0\r\n"
                           "Host: %s:%d\r\n"
                           "Accept: application/json\r\n"
                           "%s"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           method, path, host, port,
                           extra_headers ? extra_headers : "", body_len);
        if (req_len >= (int)sizeof(req)) {
            return -1;
        }
    }
    if (write(fd, req, req_len) != req_len) {
        return -1;
    }
    if (post_body && body_len > 0 && write(fd, post_body, body_len) != (ssize_t)body_len) {
        return -1;
    }
    return 0;
}

int lanhttp_request(const char *host, int port, const char *method, const char *path,
                    const char *extra_headers, const char *post_body,
                    char **body_out, size_t *body_len_out) {
    if (body_out) {
        *body_out = NULL;
    }
    if (body_len_out) {
        *body_len_out = 0;
    }

    int fd = lanhttp_connect(host, port);
    if (fd < 0) {
        return LANHTTP_ERR_NET;
    }
    if (lanhttp_send_request(fd, host, port, method, path, extra_headers, post_body) != 0) {
        close(fd);
        return LANHTTP_ERR_NET;
    }

    size_t cap = 64 * 1024, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(fd);
        return LANHTTP_ERR_NET;
    }
    for (;;) {
        if (len + 8192 + 1 > cap) {
            if (cap * 2 > LANHTTP_BODY_MAX) {
                free(buf);
                close(fd);
                return LANHTTP_ERR_PROTO;
            }
            cap *= 2;
            char *nbuf = realloc(buf, cap);
            if (!nbuf) {
                free(buf);
                close(fd);
                return LANHTTP_ERR_NET;
            }
            buf = nbuf;
        }
        ssize_t n = read(fd, buf + len, 8192);
        if (n < 0) {
            free(buf);
            close(fd);
            return LANHTTP_ERR_NET;
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
        return LANHTTP_ERR_PROTO;
    }
    char *body = strstr(buf, "\r\n\r\n");
    if (!body) {
        free(buf);
        return LANHTTP_ERR_PROTO;
    }
    body += 4;

    if (body_out) {
        size_t blen = len - (body - buf);
        *body_out = malloc(blen + 1);
        if (*body_out) {
            memcpy(*body_out, body, blen);
            (*body_out)[blen] = '\0';
            if (body_len_out) {
                *body_len_out = blen;
            }
        }
    }
    free(buf);
    return status;
}

int lanhttp_post_file(const char *host, int port, const char *path,
                      const char *extra_headers,
                      const char *fields[][2], int field_count,
                      const char *file_field, const char *filename, const char *filepath,
                      lan_stream_state_t *state, char **resp_body) {
    static const char *BOUNDARY = "hdzgboundary7f3a9c1e";

    state->bytes = 0;
    state->done = false;
    state->result = LANHTTP_ERR_NET;
    if (resp_body) {
        *resp_body = NULL;
    }

    FILE *in = fopen(filepath, "rb");
    if (!in) {
        state->done = true;
        return LANHTTP_ERR_NET;
    }
    fseek(in, 0, SEEK_END);
    long file_size = ftell(in);
    fseek(in, 0, SEEK_SET);

    // Assemble everything except the file bytes so Content-Length is exact
    char preamble[2048];
    int pre_len = 0;
    for (int i = 0; i < field_count; i++) {
        pre_len += snprintf(preamble + pre_len, sizeof(preamble) - pre_len,
                            "--%s\r\n"
                            "Content-Disposition: form-data; name=\"%s\"\r\n\r\n"
                            "%s\r\n",
                            BOUNDARY, fields[i][0], fields[i][1]);
    }
    pre_len += snprintf(preamble + pre_len, sizeof(preamble) - pre_len,
                        "--%s\r\n"
                        "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
                        "Content-Type: application/octet-stream\r\n\r\n",
                        BOUNDARY, file_field, filename);
    char epilogue[64];
    int epi_len = snprintf(epilogue, sizeof(epilogue), "\r\n--%s--\r\n", BOUNDARY);
    if (pre_len >= (int)sizeof(preamble)) {
        fclose(in);
        state->done = true;
        return LANHTTP_ERR_NET;
    }

    int fd = lanhttp_connect(host, port);
    if (fd < 0) {
        fclose(in);
        state->done = true;
        return LANHTTP_ERR_NET;
    }

    char req[1024];
    int req_len = snprintf(req, sizeof(req),
                           "POST %s HTTP/1.0\r\n"
                           "Host: %s:%d\r\n"
                           "Accept: application/json\r\n"
                           "%s"
                           "Content-Type: multipart/form-data; boundary=%s\r\n"
                           "Content-Length: %ld\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           path, host, port,
                           extra_headers ? extra_headers : "",
                           BOUNDARY, (long)pre_len + file_size + epi_len);
    if (req_len >= (int)sizeof(req) ||
        write(fd, req, req_len) != req_len ||
        write(fd, preamble, pre_len) != pre_len) {
        fclose(in);
        close(fd);
        state->done = true;
        return LANHTTP_ERR_NET;
    }

    char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (state->cancel) {
            fclose(in);
            close(fd);
            state->result = LANHTTP_ERR_NET;
            state->done = true;
            return LANHTTP_ERR_NET;
        }
        ssize_t sent = 0;
        while (sent < (ssize_t)n) {
            ssize_t w = write(fd, buf + sent, n - sent);
            if (w <= 0) {
                LOGE("lanhttp: upload write failed at %ld bytes", state->bytes);
                fclose(in);
                close(fd);
                state->done = true;
                return LANHTTP_ERR_NET;
            }
            sent += w;
        }
        state->bytes += n;
    }
    fclose(in);
    if (write(fd, epilogue, epi_len) != epi_len) {
        close(fd);
        state->done = true;
        return LANHTTP_ERR_NET;
    }

    // Small JSON response
    size_t cap = 16 * 1024, len = 0;
    char *resp = malloc(cap);
    if (!resp) {
        close(fd);
        state->done = true;
        return LANHTTP_ERR_NET;
    }
    for (;;) {
        if (len + 2048 + 1 > cap) {
            break; // response larger than any expected asset reply; keep what we have
        }
        ssize_t r = read(fd, resp + len, 2048);
        if (r <= 0) {
            break;
        }
        len += r;
    }
    close(fd);
    resp[len] = '\0';

    int status = 0;
    char *body = strstr(resp, "\r\n\r\n");
    if (sscanf(resp, "HTTP/%*d.%*d %d", &status) != 1 || !body) {
        free(resp);
        state->done = true;
        return LANHTTP_ERR_PROTO;
    }
    if (resp_body) {
        *resp_body = strdup(body + 4);
    }
    free(resp);

    state->result = (status >= 200 && status < 300) ? 0 : LANHTTP_ERR_PROTO;
    state->done = true;
    LOGI("lanhttp: upload of %s finished, status %d, %ld bytes", filename, status, state->bytes);
    return status;
}

int lanhttp_download(const char *host, int port, const char *path,
                     const char *extra_headers, const char *dest_file,
                     lan_stream_state_t *state) {
    state->bytes = 0;
    state->done = false;
    state->result = LANHTTP_ERR_NET;

    int fd = lanhttp_connect(host, port);
    if (fd < 0) {
        state->done = true;
        return LANHTTP_ERR_NET;
    }
    if (lanhttp_send_request(fd, host, port, "GET", path, extra_headers, NULL) != 0) {
        close(fd);
        state->done = true;
        return LANHTTP_ERR_NET;
    }

    char head[4096];
    size_t head_len = 0;
    char *body_start = NULL;
    while (head_len < sizeof(head) - 1) {
        ssize_t n = read(fd, head + head_len, sizeof(head) - 1 - head_len);
        if (n <= 0) {
            close(fd);
            state->done = true;
            return LANHTTP_ERR_NET;
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
        LOGE("lanhttp: stream status %d", status);
        close(fd);
        state->done = true;
        state->result = (status == 401 || status == 403) ? LANHTTP_ERR_AUTH : LANHTTP_ERR_PROTO;
        return state->result;
    }

    FILE *out = fopen(dest_file, "wb");
    if (!out) {
        close(fd);
        state->done = true;
        return LANHTTP_ERR_NET;
    }

    size_t body_in_head = head_len - (body_start - head);
    if (body_in_head > 0) {
        fwrite(body_start, 1, body_in_head, out);
        state->bytes = body_in_head;
    }

    char buf[64 * 1024];
    int quiet_reads = 0;
    int rc = 0;
    while (!state->cancel) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            quiet_reads = 0;
            if (fwrite(buf, 1, n, out) != (size_t)n) {
                LOGE("lanhttp: stream write failed (disk full?)");
                rc = LANHTTP_ERR_NET;
                break;
            }
            state->bytes += n;
            if ((state->bytes & ((256 * 1024) - 1)) < (long)sizeof(buf)) {
                fflush(out);
            }
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                if (++quiet_reads >= 12) {
                    LOGE("lanhttp: stream stalled for 60s, giving up");
                    rc = LANHTTP_ERR_NET;
                    break;
                }
                continue;
            }
            rc = LANHTTP_ERR_NET;
            break;
        }
    }

    fflush(out);
    fclose(out);
    close(fd);
    state->result = state->cancel ? 0 : rc;
    state->done = true;
    LOGI("lanhttp: stream ended, %ld bytes, rc=%d%s", state->bytes, rc, state->cancel ? " (cancelled)" : "");
    return state->result;
}
