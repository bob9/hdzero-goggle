#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// Internal render resolution 160x100, shown 8x upscaled at 1280x800.
#define CRAFT_FB_W 1280
#define CRAFT_FB_H 800

// Goggle-button actions (fallback controls).
typedef enum {
    CRAFT_ACT_TURN_LEFT = 0,
    CRAFT_ACT_TURN_RIGHT,
    CRAFT_ACT_BREAK,          // dig the targeted block
    CRAFT_ACT_PLACE,          // place a block on the targeted face
    CRAFT_ACT_TOGGLE_FORWARD, // toggle auto-walk forward
} craft_action_t;

// Start the engine thread (first call) or resume it.
bool craft_hdz_start(void);

// Pause the engine and release all held inputs.
void craft_hdz_pause(void);

bool craft_hdz_active(void);

// Copy the latest frame, upscaled, into a CRAFT_FB_W x CRAFT_FB_H
// ARGB8888 buffer. Returns false if no new frame since the last copy.
bool craft_hdz_frame_copy(uint32_t *dst);

void craft_hdz_action(craft_action_t act);

// Shares the DOOM controller button mask (DOOM_BTN_* bits): forward/back,
// turn, strafe move the player; fire digs; use places; enter jumps.
// Safe to call from the ESP32 RX thread; ignored while inactive.
void craft_hdz_msp_input(const uint8_t *payload, uint16_t size);

#ifdef __cplusplus
}
#endif
