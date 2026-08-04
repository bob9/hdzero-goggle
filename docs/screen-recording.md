# Screen recording

Records what is actually on the goggle display — the menus, the OSD, the FPV
feed, DVR playback, Plex — to an MP4 on the SD card. Intended for capturing demo
footage without pointing a phone down the lens.

## Using it

**From the menu:** `Record Option` → `Screen Record`. Click to start; the row
turns into `Stop  01:23  screen_0007.mp4` and counts up. Click again to stop.

**From a button:** `Input` → assign **Screen Record** to any button action. This
is the useful one — start the recording, then navigate wherever you like while
it keeps rolling.

A short beep marks both the start and the stop.

Files land in `/mnt/extsd/DCIM/SCREENREC/` as `screen_0001.mp4`, `screen_0002.mp4`
and so on. They are numbered rather than timestamped because goggles without an
RTC battery boot at the epoch and would otherwise produce a heap of files with
the same name.

## What gets captured

The composited screen: both the LVGL user interface and the video layer beneath
it, exactly as the wearer sees them.

This matters because the two are separate pieces of hardware. The app draws the
UI into `/dev/fb0`; the FPV feed and video playback live on a different display
layer entirely. Grabbing the framebuffer — the obvious approach — would produce
menus floating over a black hole where the video should be. So the recorder uses
the display engine's **write-back capture** path (`DISP_CAPTURE_*` on
`/dev/disp`), which returns the composited output after both layers are mixed.

Recording is 720p (or whatever the panel reports) at 30fps, H.264 high profile,
12 Mbps CBR, muxed to MP4 with the same `ffpack` wrapper the DVR uses.

## Stopping matters

MP4 needs its index (`moov` atom) written when the file is closed. Stop the
recording before powering off, or the file will not play. The recorder stops
itself when the card drops below 100 MB free, so there is always room to finish
the file properly.

## Notes and limits

- **The DVR can be recording at the same time**, but the two share the hardware
  encoder. If the DVR already holds every encoder channel, screen recording
  reports `no free encoder channel - stop the DVR first`.
- **Frames are sampled, not every panel refresh is kept.** At 90Hz you get every
  third frame. Motion in the recording is smooth but not a frame-exact capture
  of the panel.
- **Write-back capture is a display-engine feature.** If a particular panel or
  display mode does not support it, starting reports `this display does not
  support screen capture` rather than producing a broken file.
- The recorder writes to the SD card continuously; a slow card will drop frames
  before it drops the recording.

## Where the code lives

| File | What it does |
| --- | --- |
| `src/screenrec/screen_record.c` | The worker: capture, encode, mux. One thread; the UI only sets a flag. |
| `src/screenrec/screen_record.h` | Public API — `start`/`stop`/`toggle`/status. |
| `src/ui/page_record.c` | The `Screen Record` menu row and its live status. |
| `src/ui/page_input.c` | The assignable **Screen Record** button action. |
| `src/emulator/stubs.c` | No-ops for the desktop emulator build. |
