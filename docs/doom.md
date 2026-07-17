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

## Goggle controls (no extra hardware)

| Input                    | Action                            |
| ------------------------ | --------------------------------- |
| Dial rotate              | Turn left / right                 |
| Dial click               | Fire                              |
| Right button short press | Toggle move forward               |
| Right button long press  | Use / open doors (Enter in menus) |
| Dial long press          | Leave the game (engine pauses)    |

Tip: in Doom's title menu, right-button **long** press is Enter — press it a
few times to start a game on the default skill.

## Playing with your EdgeTX radio (ESP-NOW)

**No firmware changes on the radio, ELRS module, or either backpack.** The
stock goggle backpack drops unknown MSP functions but forwards
`MSP_ELRS_SET_OSD` (0x00B6) verbatim to the goggles, so the button mask is
tunnelled inside it (subcommand `0xD0`). The stock ELRS TX module has no
Lua-to-backpack passthrough, so the sender is a small ESP32 dongle on the
radio's AUX serial port instead:

```
EdgeTX Lua tool  ->  AUX serial  ->  ESP32 dongle  ->  ESP-NOW  ->  goggle backpack  ->  DOOM
```

Both parts live in `misc/doom_controller/`:

1. **`doom.lua`** — copy to `SCRIPTS/TOOLS/` on the radio SD card. Set a free
   serial port to mode **Lua** (SYS > Hardware > Serial ports). Open the tool
   to enter "Doom mode": elevator = forward/back, aileron = turn,
   rudder = strafe, SH = fire, SD = use, ENTER/EXIT keys = Doom menu keys.
2. **`doom_dongle/doom_dongle.ino`** — flash to any ESP32 dev board with the
   Arduino IDE. Edit `MY_UID` to your ELRS **bind UID** (6 numbers, shown in
   the ExpressLRS Lua / backpack web UI — the goggle backpack only listens to
   senders bearing its bound UID). Wire the radio serial TX pin to ESP32
   GPIO16 plus GND, power from 5V or USB.

### Wire protocol (for other senders)

Any ESP-NOW peer with the goggles' bind UID can drive the game by sending an
MSPv2 command, function `0x00B6`, payload `{0xD0, mask_lo, mask_hi}` — a
little-endian `uint16` bitmask of *currently held* buttons. Send a new mask
on every change; bits stay held until a mask without them arrives. (A direct
`MSP_DOOM_INPUT 0x0D00` UART function also exists for custom backpack
builds.)

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

Inputs are ignored while the DOOM page is not active. The dongle sends an
all-released mask if the radio goes quiet for 600ms.

## Notes

- The engine keeps running (paused) after you leave the page; re-entering
  resumes where you were.
- Sound is not implemented.
- The DVR/live video paths are untouched by this branch.
