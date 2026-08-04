#pragma once

// Screen recorder: captures what is actually on the goggle display - the LVGL
// UI *and* the video layer beneath it, composited - and writes an MP4 to the
// SD card. Meant for capturing demo footage without filming through the lens.
//
// This header is safe to include from the emulator build; src/emulator/stubs.c
// supplies no-op implementations there.

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// Start recording. Returns false if it could not start (no SD card, capture
// unsupported, encoder busy); screen_record_last_error() then says why.
bool screen_record_start(void);

// Stop and finalise the file. Safe to call when not recording.
void screen_record_stop(void);

// Start if idle, stop if recording. This is what the button action calls.
void screen_record_toggle(void);

bool screen_record_is_active(void);

// Seconds since the current recording started; 0 when idle.
uint32_t screen_record_elapsed_s(void);

// Basename of the file being written, or "" when idle.
const char *screen_record_filename(void);

// Human-readable reason the last start failed, or "" if there was none.
const char *screen_record_last_error(void);

#ifdef __cplusplus
}
#endif
