#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Quake renders 320x240, shown 4x upscaled at 1280x960.
#define QUAKE_FB_W 1280
#define QUAKE_FB_H 960

typedef enum {
    QUAKE_ACT_TURN_LEFT = 0,
    QUAKE_ACT_TURN_RIGHT,
    QUAKE_ACT_FIRE,
    QUAKE_ACT_JUMP_ENTER, // jump in game, Enter/confirm in menus
    QUAKE_ACT_TOGGLE_FORWARD,
} quake_action_t;

// True if the shareware/registered game data is present on the SD card
// (/mnt/extsd/quake/id1/pak0.pak).
bool quake_hdz_find_pak(void);

bool quake_hdz_start(void);
void quake_hdz_pause(void);
bool quake_hdz_active(void);

// Copy the latest frame, upscaled, into a QUAKE_FB_W x QUAKE_FB_H
// ARGB8888 buffer. Returns false if no new frame since the last copy.
bool quake_hdz_frame_copy(uint32_t *dst);

void quake_hdz_action(quake_action_t act);

// Shares the DOOM controller button mask (DOOM_BTN_* bits): forward/back,
// turn, strafe move; fire shoots; use jumps; enter/escape/Y drive menus.
// Safe to call from the ESP32 RX thread; ignored while inactive.
void quake_hdz_msp_input(const uint8_t *payload, uint16_t size);

#ifdef __cplusplus
}
#endif
