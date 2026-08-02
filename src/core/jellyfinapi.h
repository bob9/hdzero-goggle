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

/** Every episode of one series in airing order (kind = PLEX_ITEM_EPISODE). */
int jf_load_episodes(const char *series_id, plex_movie_t **out, int *out_count, volatile int *progress);

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

/**
 * Open a playback session (POST /Items/{id}/PlaybackInfo) before streaming.
 * Fills play_session with the id the SERVER issues - progress reports quoting
 * a self-invented id match no session - and runtime_ms with the exact runtime
 * (the library listing only carries whole minutes). Returns PLEX_OK on
 * success; the caller can fall back to a local id and the rounded runtime.
 */
int jf_playback_info(const char *item_id, int max_kbps, char *play_session, int session_size,
                     long long *runtime_ms);

/** Build the transcoded-TS stream request path for a movie. */
void jf_stream_path(char *buf, int size, const plex_movie_t *movie, int offset_s, int max_kbps,
                    const char *play_session);

/** Growing-file stream download against the Jellyfin server. */
int jf_stream_download(const char *path, const char *dest_file, lan_stream_state_t *state);

typedef enum {
    JF_PLAY_START = 0,
    JF_PLAY_PROGRESS,
    JF_PLAY_STOPPED,
} jf_play_event_t;

/**
 * Playback session reporting (POST /Sessions/Playing[...]). Jellyfin pauses
 * a transcode that runs ~3 min ahead of the last reported position, so a
 * client that never reports stalls every stream; regular progress keeps the
 * transcoder running (and feeds watched/resume state as a bonus).
 */
void jf_playback_report(const char *item_id, const char *play_session,
                        jf_play_event_t event, long long pos_ticks);

void jf_settings_save(void);

#ifdef __cplusplus
}
#endif
