#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

/**
 * Minimal plain-HTTP client for LAN media servers (HTTP/1.0, Connection:
 * close framing). Blocking with socket timeouts - callers run off the LVGL
 * thread.
 */

typedef struct {
    volatile bool cancel;
    volatile long bytes;
    volatile bool done;
    volatile int result; // 0 ok, <0 failure (LANHTTP_ERR_*)
} lan_stream_state_t;

#define LANHTTP_ERR_NET   -1
#define LANHTTP_ERR_AUTH  -2
#define LANHTTP_ERR_PROTO -4

/**
 * Request with optional POST body (JSON) and optional extra header lines
 * (each "\r\n"-terminated). On HTTP 200, *body_out is a malloc'd
 * NUL-terminated response body. Returns the HTTP status, or <0 on network
 * failure.
 */
int lanhttp_request(const char *host, int port, const char *method, const char *path,
                    const char *extra_headers, const char *post_body,
                    char **body_out, size_t *body_len_out);

/**
 * Stream a GET response body into dest_file while updating state, so a
 * reader can consume the file while it is still growing.
 *
 * keep_existing overwrites an existing dest_file from the front instead of
 * truncating it, which preserves a preallocated stream file. Pass false for
 * anything else: a leftover tail past the new content would corrupt it.
 */
int lanhttp_download(const char *host, int port, const char *path,
                     const char *extra_headers, const char *dest_file,
                     lan_stream_state_t *state, bool keep_existing);

/**
 * Multipart/form-data POST streaming a file from disk (for multi-GB
 * uploads). `fields` are simple name/value string pairs sent before the
 * file part. state->bytes tracks sent file bytes; set cancel to abort.
 * On HTTP 2xx, *resp_body (optional) is the malloc'd response. Returns
 * the HTTP status or <0 on network failure.
 */
int lanhttp_post_file(const char *host, int port, const char *path,
                      const char *extra_headers,
                      const char *fields[][2], int field_count,
                      const char *file_field, const char *filename, const char *filepath,
                      lan_stream_state_t *state, char **resp_body);

#ifdef __cplusplus
}
#endif
