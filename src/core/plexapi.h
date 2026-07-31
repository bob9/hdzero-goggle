#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/**
 * Minimal Plex Media Server client for browsing a local server's movie
 * library. All calls are blocking with socket timeouts and must run off the
 * LVGL thread (page_plex drives them from a worker thread).
 */

#define PLEX_OK          0
#define PLEX_ERR_NET     -1 // unreachable / connection or read failure
#define PLEX_ERR_AUTH    -2 // 401/403 - token required or wrong
#define PLEX_ERR_NOMOVIE -3 // no movie library section on the server
#define PLEX_ERR_PROTO   -4 // unexpected response payload

#define PLEX_SERVERS_MAX 8
#define PLEX_MOVIES_MAX  2048

typedef struct {
    char name[64];
    char host[40];
    int port;
} plex_server_t;

typedef struct {
    char rating_key[16];
    char title[96];
    char thumb[96]; // server-side art path, "" if none
    int year;       // 0 if unknown
    int duration_min;
    bool watched;
} plex_movie_t;

/**
 * Discover Plex servers on the LAN via GDM multicast.
 * Blocks for ~1.5 seconds. Returns the number of servers found.
 */
int plex_gdm_discover(plex_server_t *out, int max_out);

/**
 * True if a Plex server answers /identity at host:port (no auth required).
 */
bool plex_server_reachable(const char *host, int port);

/**
 * Find the first movie library section.
 * Uses the server and token from g_setting.plex.
 */
int plex_find_movie_section(char *key, int key_size, char *title, int title_size);

/**
 * Fetch the entire movie listing of a section, sorted by title, into a
 * malloc'd array the caller owns. progress (may be NULL) is updated with the
 * number of movies fetched so far while the transfer is running.
 */
int plex_load_movies(const char *section_key, plex_movie_t **out, int *out_count, volatile int *progress);

/**
 * Ensure the poster for a movie is present in the local cache, downloading a
 * server-scaled JPEG if needed. Returns PLEX_OK and the cache file path, or
 * an error. Movies without art return PLEX_ERR_PROTO.
 */
int plex_fetch_poster(const plex_movie_t *movie, int width, int height, char *path_out, int path_size);

/**
 * Cache path a poster would live at; returns true if it is already cached.
 */
bool plex_poster_cached(const plex_movie_t *movie, char *path_out, int path_size);

/**
 * Persist g_setting.plex to the settings ini.
 */
void plex_settings_save(void);

/**
 * Adopt a token from /mnt/extsd/plextoken.txt if present (first line,
 * whitespace trimmed) - the no-typing alternative to keyboard entry.
 * Returns true if a token was read and saved.
 */
bool plex_token_from_sdcard(void);

#ifdef __cplusplus
}
#endif
