# QUAKE on HDZero Goggles

This branch adds a **QUAKE** entry to the goggle main menu that runs the real
Quake engine (WinQuake via
[quakegeneric](https://github.com/erysdren/quakegeneric)) full-screen on the
goggles.

DOOM is **not** in this build. The transmitter controller protocol below was
originally defined for the DOOM port and is unchanged, so the same EdgeTX
model, mixer setup and Lua script drive Quake without modification — which is
why the channel and bit names still say DOOM.

## Setup

1. Copy Quake's `id1` folder to `SDCARD/quake/id1/`. It needs `pak0.pak` —
   either the freely distributable shareware episode, or your registered
   `pak0.pak` + `pak1.pak` if you own the full game.
2. Flash this branch's firmware.
3. Open **QUAKE** in the main menu.

Saves and the Quake config are written to the SD card alongside the paks.

## Goggle controls (no extra hardware)

| Input                    | Action                          |
| ------------------------ | ------------------------------- |
| Dial rotate              | Turn left / right               |
| Dial click               | Fire                            |
| Right button short press | Toggle move forward             |
| Right button long press  | Jump (Enter in menus)           |
| Dial long press          | Leave the game (engine pauses)  |

Quake has no "use" action but does jump, so the right button long-press is
**jump**, and it doubles as Enter in the menus. Escape opens the in-game menu;
the Y switch confirms quit prompts.

## Playing with your EdgeTX radio (ESP-NOW)

In every variant the goggle side is the same: the stock goggle backpack
drops unknown MSP functions but forwards `MSP_ELRS_SET_OSD` (0x00B6)
verbatim to the goggles, so the button mask is tunnelled inside it
(subcommand `0xD0`). **Both backpacks stay stock.**

### Option A — custom ELRS TX module firmware (no extra hardware)

The `doom-controller-3.5.6` branch of the ExpressLRS fork adds
`DoomInputToMSPOut` to the TX module: while **AUX10 is high** (doom mode),
the module encodes the sticks into the button mask and sends it out its
backpack UART; the stock TX backpack broadcasts it over ESP-NOW.

```
sticks -> ELRS TX module (fork) -> TX backpack (stock) -> ESP-NOW -> goggle backpack (stock) -> DOOM
```

Mapping (fixed in firmware; remap anything with the radio mixer):
Mode 2 FPV-style: throttle = walk forward/back (middle = stop),
elevator = look up/down (QUAKE), aileron = turn,
rudder = strafe, AUX2 = fire, AUX3 = use, AUX4 = Enter, AUX5 = Escape,
AUX6 = Y (confirm y/n prompts), AUX7 = 6POS weapon select,
AUX10 = doom mode on/off.
Leaving doom mode releases all buttons.

#### Step 1 — create a separate game model

Always use a **dedicated model** for Doom. The Doom firmware reads fixed
channels (including ones your quad models may use for arming or modes), so
playing on a quad model could flip real functions - and vice versa, a
quad's switch positions could spray Doom inputs. A separate model keeps
the two worlds isolated and is one page-flip away.

1. Long-press **MDL** > **Model select** > pick an empty slot > press
   ENTER > **Create model**. Run the wizard (plane/none is fine - only
   the sticks matter) and name it `DOOM`.
2. In **MDL > Setup**, scroll to **Internal RF** and set **Mode = CRSF**.
   A fresh model often has the internal module off, and without this the
   ELRS Lua shows "no ExpressLRS - enable a CRSF internal or external
   module".
3. No receiver match/model ID fiddling is needed - Doom rides the
   backpack, not the RC link, so the module just has to be on and bound
   to your usual bind phrase.

#### Step 2 — mixer: every channel the game reads

EdgeTX channel numbers vs ELRS AUX names: **AUXn = CH(n+4)** - e.g. AUX2
is CH6. The firmware reads these channels and nothing else:

| EdgeTX channel | ELRS name | Source            | Doom action                    |
| -------------- | --------- | ----------------- | ------------------------------ |
| CH1            | -         | Aileron (stick)   | Turn left / right              |
| CH2            | -         | Elevator (stick)  | Look up / down (QUAKE) |
| CH3            | -         | Throttle (stick)  | Walk forward / back (middle = stop) |
| CH4            | -         | Rudder (stick)    | Strafe left / right            |
| CH5            | AUX1      | -                 | (unused, typically arm on quads) |
| CH6            | AUX2      | SH (momentary)    | Fire                           |
| CH7            | AUX3      | SD                | Use / open doors               |
| CH8            | AUX4      | SA                | Enter (Doom menus)             |
| CH9            | AUX5      | SB                | Escape (Doom menu open/close)  |
| CH10           | AUX6      | SC                | Y - confirm quit / y-n prompts |
| CH11           | AUX7      | 6POS button       | Weapon select 1-6 (Doom and Quake) |
| CH14           | AUX10     | SF                | **Doom mode on/off**           |

CH1-CH4 are already in the mixer from the model wizard (ELRS requires
AETR order) - leave them alone. Add the six switch lines: in
**MDL > Mixer**, highlight the channel, long-press ENTER > **Edit**, set
**Source** to the switch, leave Weight at 100. Repeat for CH6, CH7, CH8,
CH9, CH10 and CH14.

The suggested switches are just that - any switch works, the firmware
only reads channel numbers. A switch counts as pressed in its **high**
position (past mid): on a 3-position switch, middle and low are "off".
Sticks register past ~25% deflection.

#### Step 3 — verify, then play

From the main screen open the **Channel Monitor** (long-press PAGE) and
check: sticks move CH1-CH4, and each switch moves its channel - CH6, CH7,
CH8, CH9, CH10, CH14.

To play: radio on the DOOM model, open DOOM on the goggles, then flip the
CH14 (AUX10) switch - the sticks take over. At the title screen flip the
Enter (CH8) switch a few times to start a game. Flip CH14 off when done -
it releases every button, and your other models never see any of it.

### Option B — stock module + ESP32 dongle on AUX serial

If you don't want to flash the module, the stock ELRS TX module has no
Lua-to-backpack passthrough, so a small ESP32 dongle on the radio's AUX
serial port does the ESP-NOW sending:

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
| 10  | 0x0400 | Y (confirm quit and other y/n prompts) |

Inputs are ignored while the QUAKE page is not active. The dongle sends an
all-released mask if the radio goes quiet for 600ms.

## Notes

- The engine keeps running (paused) after you leave the page; re-entering
  resumes where you were.
- Sound is not implemented.
- The DVR/live video paths are untouched by this branch.

## Licensing

The **engine** is free software: id Software released the Quake source under
the **GPLv2** in 1999, and that is what `src/quake/quakegeneric` is.

The **game data is not**. `pak0.pak`/`pak1.pak` remain id Software/ZeniMax
copyright. The shareware `pak0.pak` is freely redistributable under id's
shareware terms (unmodified, non-commercial) but is not open source; the full
paks are commercial. No pak files are bundled in this repository — you supply
your own.

Because the GPLv2 engine is statically linked into the firmware binary, the
binary this branch produces is a **GPLv2** work, not MIT like the base
HDZero firmware. See `LICENSING.md`.
