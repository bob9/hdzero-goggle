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
| Right button long press  | Use / open doors (Enter/Y in menus) |
| Dial long press          | Leave the game (engine pauses)    |

Tip: in Doom's title menu, right-button **long** press is Enter — press it a
few times to start a game on the default skill.

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
elevator = forward/back, aileron = turn, rudder = strafe, AUX2 = fire,
AUX3 = use, AUX4 = Enter, AUX5 = Escape, AUX6 = Y (confirm y/n prompts),
AUX10 = doom mode on/off.
Leaving doom mode releases all buttons.

#### EdgeTX model + mixer setup

Make a dedicated model so Doom switches never touch a quad model:

1. **MDL page > Model select**: create a new model, name it `DOOM`.
2. **MDL > Setup > Internal RF**: set **Mode = CRSF** (otherwise the ELRS
   Lua reports no module).
3. **MDL > Mixer**: CH1-CH4 already carry your sticks (AETR) from the
   wizard - leave them. Then add one line per button: highlight the
   channel, long-press ENTER > Edit, set **Source** to a switch, leave
   Weight at 100:

   | Channel | Source (suggested) | Doom action |
   | ------- | ------------------ | ------------------------ |
   | CH6     | SH (momentary)     | Fire                     |
   | CH7     | SD                 | Use / open doors         |
   | CH8     | SA                 | Enter (Doom menus)       |
   | CH9     | SB                 | Escape                   |
   | CH10    | SC                 | Y (confirm quit prompts) |
   | CH14    | SF                 | Doom mode on/off         |

   Any switches work - the firmware only reads the channel numbers. A
   switch counts as pressed in its **high** position (past mid), so on
   3-position switches the middle is off.
4. Check it: from the main screen open the **Channel Monitor** and flip
   each switch - CH6..CH14 should move. Sticks show on CH1-CH4.

To play: bind as normal, open DOOM on the goggles, then flip the CH14
switch - the sticks take over. At the title screen flip the Enter (CH8)
switch a few times to start a game. Flip CH14 back off when done - it
releases every button.

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

Inputs are ignored while the DOOM page is not active. The dongle sends an
all-released mask if the radio goes quiet for 600ms.

## Notes

- The engine keeps running (paused) after you leave the page; re-entering
  resumes where you were.
- Sound is not implemented.
- The DVR/live video paths are untouched by this branch.
