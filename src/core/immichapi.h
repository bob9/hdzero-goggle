#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "core/lanhttp.h"

/**
 * Immich client: back up DVR recordings to a self-hosted Immich server.
 * Auth is an API key (created in Immich under Account Settings -> API
 * Keys); Immich has no LAN discovery protocol, so the server address is
 * provided manually or via /mnt/extsd/immich.txt (line 1 = API key,
 * line 2 = host[:port], port defaults to 2283).
 *
 * Upload status tracking: a successful upload writes a "<file>.immich"
 * sidecar next to the recording; the server's own duplicate detection
 * also reports already-uploaded files, which re-creates a missing sidecar.
 */

#define IMMICH_OK       0
#define IMMICH_DUP      1  // server already has this file (marked as uploaded)
#define IMMICH_ERR_NET  -1
#define IMMICH_ERR_AUTH -2
#define IMMICH_ERR_PROTO -4

bool immich_configured(void);
bool immich_server_reachable(void);
bool immich_token_from_sdcard(void);
void immich_settings_save(void);

/** True if the recording has an upload sidecar. */
bool immich_uploaded(const char *filepath);

/**
 * Upload one recording (blocking; run on a worker thread). state->bytes
 * tracks sent bytes; set state->cancel to abort. On IMMICH_OK/IMMICH_DUP
 * the sidecar is written.
 */
int immich_upload(const char *filepath, const char *filename, lan_stream_state_t *state);

#ifdef __cplusplus
}
#endif
