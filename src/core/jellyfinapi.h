#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "core/lanhttp.h"
#include "core/plexapi.h" // shared plex_server_t / plex_movie_t item structs + PLEX_* codes

/**
 * Jellyfin adapter: same surface as plexapi so page_plex can drive either
 * backend behind the one poster-wall UI. Uses g_setting.jellyfin.
 */

/** UDP discovery ("who is JellyfinServer?" broadcast). Blocks ~1.5 s. */
int jf_discover(plex_server_t *out, int max_out);

bool jf_server_reachable(const char *host, int port);

/** Full movie listing, sorted by title. Caller owns the malloc'd array. */
int jf_load_movies(plex_movie_t **out, int *out_count, volatile int *progress);

int jf_fetch_poster(const plex_movie_t *movie, int width, int height, char *path_out, int path_size);
bool jf_poster_cached(const plex_movie_t *movie, char *path_out, int path_size);

/**
 * Quick Connect: Jellyfin's fully-local code sign-in. start returns the
 * on-screen code + a secret; poll with the secret returns PLEX_PENDING
 * until the user approves it in any signed-in Jellyfin app, then PLEX_OK
 * with the token and user id stored and saved.
 */
int jf_quickconnect_start(char *code, int code_size, char *secret, int secret_size);
int jf_quickconnect_poll(const char *secret);

/** Adopt token (line 1) and optional host[:port] (line 2) from /mnt/extsd/jellyfintoken.txt. */
bool jf_token_from_sdcard(void);

/** Build the transcoded-TS stream request path for a movie. */
void jf_stream_path(char *buf, int size, const plex_movie_t *movie, int offset_s, int max_kbps);

/** Growing-file stream download against the Jellyfin server. */
int jf_stream_download(const char *path, const char *dest_file, lan_stream_state_t *state);

void jf_settings_save(void);

#ifdef __cplusplus
}
#endif
