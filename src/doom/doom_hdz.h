#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Doom framebuffer is 640x400; the UI shows it pixel-doubled at 1280x800.
#define DOOM_HDZ_FB_W 1280
#define DOOM_HDZ_FB_H 800

// Goggle-button actions (fallback controls).
typedef enum {
    DOOM_ACT_TURN_LEFT = 0,
    DOOM_ACT_TURN_RIGHT,
    DOOM_ACT_FIRE,
    DOOM_ACT_USE_ENTER,      // use / open doors, also Enter for Doom's menus
    DOOM_ACT_TOGGLE_FORWARD, // toggle auto-move forward
} doom_action_t;

// Button mask payload: uint16 little-endian bitmask of held buttons. Send a
// new mask on every change; bits stay "held" until a mask without them
// arrives. Two transports, both landing in doom_hdz_msp_input():
//  - MSP_ELRS_SET_OSD (0x00B6) subcommand 0xD0 over ESP-NOW: payload
//    {0xD0, mask_lo, mask_hi}. Works with the STOCK goggle backpack, which
//    forwards SET_OSD verbatim to the goggle UART.
//  - MSP_DOOM_INPUT (0x0D00) with payload {mask_lo, mask_hi}: direct UART
//    function for custom backpack builds.
#define DOOM_BTN_FORWARD  (1 << 0)
#define DOOM_BTN_BACK     (1 << 1)
#define DOOM_BTN_TURN_L   (1 << 2)
#define DOOM_BTN_TURN_R   (1 << 3)
#define DOOM_BTN_FIRE     (1 << 4)
#define DOOM_BTN_USE      (1 << 5)
#define DOOM_BTN_ENTER    (1 << 6)
#define DOOM_BTN_ESCAPE   (1 << 7)
#define DOOM_BTN_STRAFE_L (1 << 8)
#define DOOM_BTN_STRAFE_R (1 << 9)
#define DOOM_BTN_Y        (1 << 10) // confirm Doom's y/n prompts (e.g. quit)

// Bits 11-13 carry a 3-bit weapon-slot field (0 = none, 1..6 = select the
// weapon slot), fed by a multi-position switch such as the RadioMaster
// 6-position button. A change of value presses the matching number key.
#define DOOM_WEAPON_SHIFT 11
#define DOOM_WEAPON_FIELD (7 << DOOM_WEAPON_SHIFT)

// Look for a Doom IWAD in the SD card root; true if found.
bool doom_hdz_find_wad(char *buf, size_t len);

// Spawn the Doom engine thread (first call) or resume it. False on failure.
bool doom_hdz_start(const char *wad_path);

// Pause the engine (between tics) and release all held inputs.
void doom_hdz_pause(void);

bool doom_hdz_active(void);

// Copy the latest frame, pixel-doubled, into a DOOM_HDZ_FB_W x DOOM_HDZ_FB_H
// ARGB8888 buffer. Returns false if no new frame since the last copy.
bool doom_hdz_frame_copy_2x(uint32_t *dst);

// Goggle-button input.
void doom_hdz_action(doom_action_t act);

// MSP_DOOM_INPUT handler; safe to call from the ESP32 RX thread.
void doom_hdz_msp_input(const uint8_t *payload, uint16_t size);

#ifdef __cplusplus
}
#endif
