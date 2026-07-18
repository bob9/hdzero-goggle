#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Video frame rate of an mp4 file, rounded to the nearest integer;
// 0 if it cannot be determined. Pure file parsing, no vendor libraries.
int mp4probe_fps(const char *path);

#ifdef __cplusplus
}
#endif
