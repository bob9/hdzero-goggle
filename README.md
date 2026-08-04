# Custom HDZero Goggles Firmware — Cinema + Games build

> **You are on the `games` branch.** This is the standard Cinema build **plus**
> DOOM, QUAKE and Minecraft. Games are deliberately kept out of the standard
> release — for that, use `main`.

This build is the latest HDZero goggle firmware, plus ripples' custom firmware
features, plus the **Cinema** media client (Plex, Jellyfin and Immich).

Built from the official [hd-zero/hdzero-goggle](https://github.com/hd-zero/hdzero-goggle)
firmware and [ripples' custom firmware](https://github.com/bolagnaise/hdzero-goggle),
both of which are fully merged into this build.

If you'd like to support ripples' upstream work, you can send a donation here:
https://www.paypal.com/paypalme/ripleyjb

## Release variants

| Release | Contents |
| --- | --- |
| **Standard** (`main`, `v9.6.0`) | Everything documented below except Games. This is the normal install. |
| **Games** (`games`, `v9.6.0-games`) | ← **this branch.** The standard build **plus** DOOM, QUAKE and Minecraft, playable from the transmitter. |
| **Screen recorder** (`screen-recorder`) | The standard build **plus** on-goggle screen recording to the SD card, for capturing demo footage without filming through the lens. |

---

# Features

Everything in this section is in addition to stock HDZero firmware.

## Cinema — Plex, Jellyfin and Immich

A media client that turns the goggle into a private big screen for your own
library. Reachable from the main menu as **Plex** and **Immich**.

- **Plex movie library browsing** from a Plex Media Server on your LAN, with a
  poster wall and background poster prefetch.
- **TV shows** with series browsing and episode drill-down.
- **Playback** of server-remuxed TS, streamed to the SD card and played through
  the goggle's stock hardware-decoded player.
- **Selectable stream quality** — 1080p, 720p or 480p — plus **automatic quality
  step-down** when the server can't keep up with the stream.
- **Resume where you left off**, using the server's stored playback position.
- **Three ways to sign in**:
  - the smart-TV flow — the goggle shows a code, you enter it at `plex.tv/link`;
  - a `plextoken.txt` (or `jellyfintoken.txt`) file in the root of the SD card,
    picked up automatically at boot — works with no internet;
  - typing a token directly with the on-screen keyboard.
- **Full Jellyfin support** behind the same movie UI, with playback-session
  reporting so the server tracks watched state, and transcoding constrained to
  the envelope the goggle's decoder is known to handle.
- **Immich** — selective backup of your DVR recordings to a self-hosted Immich
  server, authenticated with an API key.
- **Long-press Func** exits playback and returns to the library.
- **WiFi throughput tuning** for streaming: XR819 power-save disabled and raised
  TCP windows.

## DVR recording

- **Selectable rate control** — CBR or VBR, each at three quality points, with
  on-page explanations of what the setting does.
- **VBR quality and maximum QP** are directly settable.
- **Five record bitrate steps** — Normal, 1/4, 1/2, 1.5x and 2x.
- **Correct colour** in recordings: full colour range is declared in the H.264
  VUI, so players no longer crush or wash out the levels.
- **Reliable record OSD** — the record indicator bit is re-asserted and read back
  at record start.
- **Opt-in ELRS race naming** — recordings can be named from the race label sent
  over ELRS.

## DVR playback

- **True 90Hz and 60Hz playback** on both the Goggle and Goggle 2, routed through
  the live video path.
- **Panel refresh matched to the clip**, with the frame rate probed from the
  `.ts` PES timestamps.
- **Auto-hiding control bar** — fades after 4 seconds and wakes on dial input.
- **Faster video start** — the baseband is kept warm and prewarmed in the
  background while you browse the playback list.
- **Receiver off during playback**, so the raster stays black behind the video.
- **On-screen playback FPS counter** for diagnostics.

## WiFi

- **Laptop-style network selection** — pick a network from a scan list, with
  remembered networks reconnecting automatically.
- Scanning recovers from a stale `wpa_supplicant` socket instead of hanging.

## ELRS and VTX control

- **VTX Control** — a master switch governing whether the goggle may send VTX
  commands at all.
- **Send VTX is a deliberate action**, assignable to a button (default: right
  long-press). Changing channels never auto-sends.
- **Configurable "VTX Sent" banner** with a live preview of the style.
- **ELRS backpack fix** — readiness-gated power-on plus a self-healing watchdog.

## Clock and RTC

- **Time survives power-off** — written to every RTC device present, with the
  running clock persisted so battery-less units reboot near the right time.
- **Set-clock rollers track the live clock** and load the current time on entry.
- The clock page stays live while merely highlighted in the menu.
- Correct clock-battery detection on goggles that have one installed.

## Power

- **Shutdown** is its own menu entry beside Go Sleep. It quiesces the recorder,
  unmounts the SD card, and shows a safe-to-power-off screen.

## Image and source

- **Separate analog and HDZero image settings**, with switchable source profiles.
- The source is confirmed before the input switches, and the HDZero loading state
  is shown and animated rather than leaving a blank screen.

## Firmware update safety

- **Firmware is validated before any flash is touched** — a corrupt or truncated
  update file can no longer brick the goggle.
- **Honest update results** — no false `FAILED` on a successful OTA, and no false
  success when the app partition flash actually fails.

## Storage and diagnostics

- The automatic SD integrity check is skipped when the FAT is already clean,
  cutting boot time.
- **Hardware transition log** written to `hwlog.txt` on the SD card.
- Analog RSSI calibration sampling fixed.
- **A power cut can no longer factory-reset your settings.**

## Emulator

- Faithful RTC behaviour and macOS host support for on-desktop development.

## Games (this branch only)

Three full-screen games run natively on the goggle, reachable from the main
menu. See [`docs/doom.md`](docs/doom.md) for the complete setup and mixer guide.

- **DOOM** — the real Doom engine ([doomgeneric](https://github.com/ozkl/doomgeneric)),
  640x400 pixel-doubled to 1280x800. Copy `DOOM1.WAD` (shareware) to the root of
  the SD card; `DOOM.WAD`, `DOOM2.WAD` and the Freedoom WADs also work. Savegames
  and config are written to the SD card.
- **QUAKE** — WinQuake, sharing the Doom controller.
- **MINECRAFT** — a voxel block world using the same controller.

**Goggle-only controls** — no extra hardware: dial rotate turns, dial click
fires, right button short-press toggles move-forward, right button long-press is
use/Enter, and dial long-press leaves the game.

**Transmitter controls** — play with your EdgeTX radio over ESP-NOW, with both
backpacks left stock: the button mask is tunnelled inside `MSP_ELRS_SET_OSD`,
which the stock backpack forwards verbatim. Supports Mode 2 FPV-style sticks
(throttle walks, elevator looks up/down), analog turn with positional pitch, and
a weapon selector on a multi-position switch.

---

## Environment Setup

The firmware can either be built in a [devcontainer](https://containers.dev/) or natively on a linux machine.

Note: decompressing the repository in Windows system may damage some files and prevent correct builds.

### Devcontainer Setup

This repository supports the [vscode devcontainer](https://code.visualstudio.com/docs/devcontainers/containers) integration.
To get started, install docker, vscode and the devcontainer extension.
A [prompt](https://code.visualstudio.com/docs/devcontainers/create-dev-container#_add-configuration-files-to-a-repository) to reopen this repository in a container should appear.

### Native Setup

CMake is required for generating the build files.
A bash script is supplied to take care of the bootstrap process:

```
~/hdzero-goggle$ ./setup.sh
```

## Building Firmware

In either of the above scenarios the firmware can be built via make.
An appropiate vscode build task ships with this repository as well.

Compiling HDZero Goggles:
```
~/hdzero-goggle$ cd build_goggle
~/hdzero-goggle/build_goggle$ make clean all -j $(nproc)
```

The firmware is generated as hdzero-goggle/build_goggle/out/HDZERO_GOGGLE-77-206-<VERSION>-<commit>.bin
where `<VERSION>` is the contents of the `VERSION` file at the repository root.
Tagged releases omit the commit suffix.

Compiling HDZero BoxPro:
```
~/hdzero-goggle$ cd build_boxpro
~/hdzero-goggle/build_boxpro$ make clean all -j $(nproc)
```

The firmware is generated as hdzero-goggle/build_boxpro/out/HDZERO_BOXPRO-77-211-<VERSION>-<commit>.bin
Tagged releases omit the commit suffix.

### Building the firmware using nix

The nix build system can be used to build the firmware on any linux system.  
Make sure that nix [is installed](https://nixos.org/download/), and the [flakes feature](https://wiki.nixos.org/wiki/Flakes) is enabled.  
No bootstrapping or installation of any tools is required.

Use this command to build the firmware

```shellSession
nix build .#goggle-app
```

After this succeeds, the firmware can be found under `./result` in the current directory.


## Loading the Firmware

Firmware can be either flashed via goggle menu or alternatively be executed via the SD Card with a custom development script.  An example of this development script is provided below.  The goggles automatically checks to see if the develop.sh script exists in the root of the SD Card and if found develop.sh is then executed.

The following files must be placed in the root of SD Card in this example. This script will then check to see if HDZGOGGLE binary has been found during bootup and if found then executed.

Otherwise, if the HDZGOOGLE binary is not detected, the goggles will continue to load the built-in executable which was previously flashed.

SD Card File Hierarchy:

```
/develop.sh
/HDZGOGGLE
```

Development script (develop.sh):

```
#!/bin/sh

# Load via SD Card if found
if [ -e /mnt/extsd/HDZGOGGLE ]; then
	/mnt/extsd/HDZGOGGLE &
else
	/mnt/app/app/HDZGOGGLE &
fi
```

## Building the Emulator

Goggle source code can be built natively on the host machine and used for debugging.

### Library required

Requires build-essential tools and SDL2 development libraries (libsdl2-dev for debian) to be already installed.

```
sudo apt-get install build-essential libsdl2-dev
```

### Build and Run

Emulator support for both Goggle and BoxPro is supported by setting the appropriate compilation switches.

```
~/hdzero-goggle$ mkdir build_emu
~/hdzero-goggle$ cd build_emu
~/hdzero-goggle/build_emu$ cmake .. -DEMULATOR_BUILD=ON -DCMAKE_BUILD_TYPE=Debug -DHDZ_GOGGLE=ON -DHDZ_BOXPRO=OFF -DHDZ_GOGGLE2=OFF
~/hdzero-goggle/build_emu$ make -j $(nproc)
~/hdzero-goggle/build_emu$ ./HDZGOGGLE
```

### Emulator Keys

`a` = right button press
`w` = wheel up
`s` = wheel down
`d` = wheel center press
Use `F11` to toggle full screen where applicable.

## Support and Developer Channels

Join the official Discord server here:

https://discord.gg/kGsnEDMb2V

Or the official Facebook group:

https://www.facebook.com/groups/hdzero
