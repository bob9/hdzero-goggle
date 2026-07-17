# DOOM on HDZero Goggles

This branch adds a **DOOM** entry to the goggle main menu that runs the real
Doom engine ([doomgeneric](https://github.com/ozkl/doomgeneric)) full-screen
on the goggles.

## Setup

1. Copy `DOOM1.WAD` (the freely distributable shareware WAD) to the **root of
   the goggle SD card**. Also accepted: `DOOM.WAD`, `DOOM2.WAD`,
   `freedoom1.wad`, `freedoom2.wad`.
2. Flash this branch's firmware.
3. Open **DOOM** in the main menu. The game starts full-screen at 640x400,
   pixel-doubled to 1280x800.

Savegames and the Doom config file are written to the SD card.

## Goggle controls (fallback)

| Input                    | Action                          |
| ------------------------ | ------------------------------- |
| Dial rotate              | Turn left / right               |
| Dial click               | Fire                            |
| Right button short press | Toggle move forward             |
| Right button long press  | Use / open doors (Enter in menus) |
| Dial long press          | Leave the game (engine pauses)  |

Tip: in Doom's title menu, right-button **long** press is Enter — press it a
few times to start a game on the default skill.

## Playing with your transmitter (ESP-NOW)

The goggle's ESP32 backpack forwards any MSP packet it receives over ESP-NOW
to the goggle. This branch adds an MSP function for Doom input:

- **Function**: `0x0D00` (`MSP_DOOM_INPUT`)
- **Type**: MSPv2 command (`<`)
- **Payload**: 2 bytes, little-endian `uint16` bitmask of *currently held*
  buttons. Send a new mask on every change (edge-triggered on the goggle
  side, so buttons stay held until you clear their bit).

| Bit | Value  | Button       |
| --- | ------ | ------------ |
| 0   | 0x0001 | Move forward |
| 1   | 0x0002 | Move back    |
| 2   | 0x0004 | Turn left    |
| 3   | 0x0008 | Turn right   |
| 4   | 0x0010 | Fire         |
| 5   | 0x0020 | Use / open   |
| 6   | 0x0040 | Enter        |
| 7   | 0x0080 | Escape       |
| 8   | 0x0100 | Strafe left  |
| 9   | 0x0200 | Strafe right |

The sender is any ESP-NOW peer bound to the same backpack UID as the goggles
— e.g. an ELRS TX backpack driven by an EdgeTX Lua script mapping sticks and
switches to the bitmask, or a bare ESP32 dev board with a couple of buttons.
Movement is fully analog-free: mask bit set = key held, bit cleared = key
released, so stick deflection maps naturally to held direction bits.

Inputs are ignored while the DOOM page is not active.

## Notes

- The engine keeps running (paused) after you leave the page; re-entering
  resumes where you were.
- Sound is not implemented.
- The DVR/live video paths are untouched by this branch.
