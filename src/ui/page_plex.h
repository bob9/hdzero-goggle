#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "ui/ui_main_menu.h"

extern page_pack_t pp_plex;

/**
 * True while a streamed movie owns the fullscreen player. Both this page and
 * the recordings page share APP_STATE_PLAYBACK, so the input layer has to ask
 * who the player belongs to before routing a key to one of them.
 */
bool plex_playback_active(void);

/** Deliver a key to the streaming player (exit, pause, seek). */
void plex_playback_key(uint8_t key);

#ifdef __cplusplus
}
#endif
