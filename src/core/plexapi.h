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
#define PLEX_PENDING     1  // pin not linked yet, keep polling
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
    int backend; // media_backend_t: which kind of server answered
} plex_server_t;

typedef enum {
    PLEX_ITEM_MOVIE = 0, // memset(0) default: plain playable movie
    PLEX_ITEM_SERIES,    // TV series: opens an episode list instead of playing
    PLEX_ITEM_EPISODE,   // playable, labeled SxEy
} plex_item_kind_t;

typedef struct {
    char rating_key[40]; // Plex ratingKey or Jellyfin item id
    char title[96];
    char thumb[96]; // server-side art path/tag, "" if none
    int year;       // 0 if unknown
    int duration_min;
    bool watched;
    uint8_t kind;   // plex_item_kind_t
    int16_t season; // episodes only, 0 if unknown
    int16_t episode;
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
 * Find the first library section of the given type ("movie" or "show").
 * Uses the server and token from g_setting.plex.
 */
int plex_find_section(const char *type, char *key, int key_size, char *title, int title_size);

/**
 * Fetch the entire movie listing of a section, sorted by title, into a
 * malloc'd array the caller owns. progress (may be NULL) is updated with the
 * number of movies fetched so far while the transfer is running.
 */
int plex_load_movies(const char *section_key, plex_movie_t **out, int *out_count, volatile int *progress);

/** TV series listing of a show section (kind = PLEX_ITEM_SERIES). */
int plex_load_shows(const char *section_key, plex_movie_t **out, int *out_count, volatile int *progress);

/** Every episode of one series in airing order (kind = PLEX_ITEM_EPISODE). */
int plex_load_episodes(const char *series_key, plex_movie_t **out, int *out_count, volatile int *progress);

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

/**
 * Streaming download of a server path into a local file (used for movie
 * playback: the transcoded MPEG-TS grows on the SD card while the player
 * reads it). Blocking - run on a worker thread. `state` fields are updated
 * live; set `cancel` from any thread to abort. The state type is shared
 * with the generic LAN client so both backends stream identically.
 */
#include "core/lanhttp.h"
typedef lan_stream_state_t plex_stream_state_t;

int plex_stream_download(const char *path, const char *dest_file, plex_stream_state_t *state);

/**
 * Simple GET against the configured server, response body discarded.
 * Used for fire-and-forget calls (stop a transcode session, timelines).
 */
int plex_server_request(const char *path);

/**
 * plex.tv PIN-link sign-in (the TV-app flow: show a 4-character code, the
 * user enters it at plex.tv/link on any device). Uses the on-device curl
 * (BearSSL) for the HTTPS leg, so it needs internet access.
 *
 * plex_pin_start requests a new code; plex_pin_poll returns PLEX_PENDING
 * until the user links, then PLEX_OK with the account token stored in
 * g_setting.plex.token (and saved).
 */
int plex_pin_start(int *pin_id, char *code, int code_size);
int plex_pin_poll(int pin_id);

#ifdef __cplusplus
}
#endif
