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

// The platform demuxer ends playback when its read position reaches the byte
// count the file had at open, so a stream that is still downloading stops
// early. Giving it a file that is already large sidesteps that entirely.
//
// The card is FAT32, which has no sparse files: these bytes are really
// written, at ~42ms/MB measured on the goggle's card. So the file is
// allocated once and then reused for every later stream rather than being
// deleted and recreated per movie.
#define PLEXSTREAM_PREALLOC_BYTES (512LL * 1024 * 1024)

/**
 * Start a session for a movie at the given offset. Spawns the download
 * thread and returns immediately; progress is read via plexstream_bytes().
 */
bool plexstream_begin(const plex_movie_t *movie, int offset_s, int max_kbps);

long plexstream_bytes(void);

// True while the stream file is being allocated and no bytes are flowing yet.
// Download-rate accounting has to skip this window or it reads as a stalled
// server (a one-off ~20s on a fresh card).
bool plexstream_preparing(void);

// Exact runtime the server reported for the streaming item, or 0 if it did
// not (Plex backend, or the PlaybackInfo call failed) - fall back to the
// library listing's whole-minute duration then.
long long plexstream_runtime_ms(void);
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
