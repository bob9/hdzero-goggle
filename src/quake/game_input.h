#pragma once

// Transmitter controller protocol.
//
// This is the wire format shared by the on-goggle games and the EdgeTX side
// (misc/doom_controller/doom.lua). It was defined for the DOOM port, and the
// DOOM_* names are kept deliberately: the bitmask on the wire is unchanged, so
// the same transmitter model, mixer setup and Lua script drive this build
// without modification. Renaming the macros would only desynchronise the code
// from the documentation and the radio side.

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Button mask payload: uint16 little-endian bitmask of held buttons. Send a
// new mask on every change; bits stay "held" until a mask without them
// arrives. Two transports, both landing in <game>_hdz_msp_input():
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
#define DOOM_BTN_Y        (1 << 10) // confirm y/n prompts (e.g. quit)

// Bits 11-13 carry a 3-bit weapon-slot field (0 = none, 1..6 = select the
// weapon slot), fed by a multi-position switch such as the RadioMaster
// 6-position button. A change of value presses the matching number key.
#define DOOM_WEAPON_SHIFT 11
#define DOOM_WEAPON_FIELD (7 << DOOM_WEAPON_SHIFT)

// Look up/down. Quake pitches; Doom's engine could not, which is why these
// sit above the original button range.
#define DOOM_BTN_LOOK_UP   (1 << 14)
#define DOOM_BTN_LOOK_DOWN (1 << 15)

#ifdef __cplusplus
}
#endif
