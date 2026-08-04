# Licensing of this build

**Short version: the firmware binary produced by this branch is GPLv2, not MIT.**

The base HDZero goggle firmware is MIT licensed (see `LICENSE`), and that still
covers the HDZero and custom firmware code on its own. But this branch also
statically links the **Quake engine**, which id Software released under the
**GNU General Public License version 2** in 1999 (`src/quake/quakegeneric`,
© 1996-1997 Id Software, Inc.).

GPLv2 and MIT are compatible in that direction — MIT code may be incorporated
into a GPL work — but the resulting combined binary must be distributed under
the GPLv2. So:

- **`LICENSE` (MIT)** applies to the HDZero and custom firmware source in this
  repository, taken by itself.
- **The built firmware image** (`HDZERO_GOGGLE-*.bin` and the `HDZGOGGLE`
  executable inside it) is a **GPLv2** combined work, because the Quake engine
  is linked into it.

If you redistribute the firmware binary, GPLv2 requires that the complete
corresponding source be available to recipients. This repository is public, so
that obligation is met by pointing to it.

Building and flashing the firmware for your own use triggers no obligation at
all — the GPL attaches when you convey the binary to someone else.

## Game data is separate, and is not included

No game data ships in this repository. `pak0.pak` and `pak1.pak` are id
Software/ZeniMax copyright and are **not** open source:

- The **shareware** `pak0.pak` is freely redistributable under id's shareware
  terms (unmodified, non-commercial). Freely available is not the same as
  freely licensed — it is not an open-source licence.
- The **registered** `pak0.pak` + `pak1.pak` are commercial and must be
  purchased.

Users supply their own data files on the SD card. Do not bundle either into a
firmware image you distribute.

## A note on the vendor libraries

The firmware also links proprietary Allwinner media libraries (`libsoftwinner`
and friends) that ship with the device's Linux system. Whether GPLv2's system
library exception covers them is genuinely arguable rather than settled. This
is the same situation any GPL application on this SoC is in, and it is noted
here for completeness rather than as a resolved question.

Nothing here is legal advice.

## The standard build is unaffected

The `main` branch contains no GPL-licensed code. That firmware is MIT, plainly.
