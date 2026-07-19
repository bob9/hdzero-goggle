#include "time.h"

#include <sys/time.h>
#include <time.h>

#ifdef EMULATOR_BUILD
// Emulates the wall-clock jump settimeofday() causes on hardware when the
// user sets the clock; rtc_set_clock adjusts this offset.
int64_t g_time_jump_offset_ms = 0;
#endif

uint32_t time_ms() {
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);

    uint64_t now_ms = (tv_now.tv_sec * 1000000 + tv_now.tv_usec) / 1000;
#ifdef EMULATOR_BUILD
    now_ms += g_time_jump_offset_ms;
#endif

    static uint64_t start_ms = 0;
    if (start_ms == 0) {
        start_ms = now_ms;
    }

    return now_ms - start_ms;
}

uint32_t time_s() {
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);

    const uint64_t now_s = tv_now.tv_sec;

    static uint64_t start_s = 0;
    if (start_s == 0) {
        start_s = now_s;
    }

    return now_s - start_s;
}