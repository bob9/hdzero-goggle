#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "core/plexapi.h"

/**
 * A movie playback session: asks the Plex server for a universal transcode
 * to H.264/AAC MPEG-TS (remux-only when the source is already compatible),
 * and downloads the stream onto the SD card while the goggle's existing
 * TS player reads the growing file.
 */

#define PLEXSTREAM_FILE "/mnt/extsd/plexcache/stream.ts"

/**
 * Start a session for a movie at the given offset. Spawns the download
 * thread and returns immediately; progress is read via plexstream_bytes().
 */
bool plexstream_begin(const plex_movie_t *movie, int offset_s, int max_kbps);

long plexstream_bytes(void);
bool plexstream_failed(void);
bool plexstream_auth_failed(void);
bool plexstream_complete(void); // stream ended cleanly (short movie fully downloaded)

/**
 * Stop the session: cancel the download, tell the server to drop the
 * transcode, and delete the local stream file.
 */
void plexstream_stop(void);

#ifdef __cplusplus
}
#endif
