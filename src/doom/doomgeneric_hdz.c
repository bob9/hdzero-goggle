// HDZero goggle platform layer for doomgeneric.
//
// The engine runs on its own thread; the LVGL UI (page_doom) polls
// doom_hdz_frame_copy_2x() from the main loop and pushes button events in
// via doom_hdz_action() (goggle dial/buttons) or doom_hdz_msp_input()
// (transmitter buttons forwarded over ESP-NOW -> backpack -> MSP).

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "doom_hdz.h"
#include "doomgeneric.h"
#include "doomkeys.h"

#define DOOM_W DOOMGENERIC_RESX
#define DOOM_H DOOMGENERIC_RESY

// How long a dial detent / button click counts as "held".
#define PULSE_MS 140

static pthread_mutex_t doom_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t doom_cond = PTHREAD_COND_INITIALIZER;
static pthread_t doom_thread;
static bool doom_started = false;
static bool doom_paused = false;
static char doom_wad[256];

static uint32_t doom_frame[DOOM_W * DOOM_H];
static bool frame_ready = false;

// Key event queue towards the engine (drained by DG_GetKey on the doom
// thread). Producers run on the UI thread and the ESP32 RX thread.
#define KQ_LEN 64
static struct {
    uint8_t key;
    uint8_t pressed;
} kq[KQ_LEN];
static int kq_count = 0, kq_read = 0;

static uint32_t pulse_deadline[256]; // ms tick when a pulsed key auto-releases
static bool key_held[256];           // toggled / ESP-NOW held keys
static uint16_t espnow_mask = 0;

static uint32_t ticks_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// doom_mutex must be held.
static void kq_push(uint8_t key, uint8_t pressed) {
    if (kq_count == KQ_LEN)
        return; // full: drop, the engine will catch up
    kq[(kq_read + kq_count) % KQ_LEN].key = key;
    kq[(kq_read + kq_count) % KQ_LEN].pressed = pressed;
    kq_count++;
}

// doom_mutex must be held.
static void key_pulse(uint8_t key) {
    if (!key_held[key] && pulse_deadline[key] == 0)
        kq_push(key, 1);
    pulse_deadline[key] = ticks_ms() + PULSE_MS;
}

// doom_mutex must be held.
static void key_set_held(uint8_t key, bool held) {
    if (key_held[key] == held)
        return;
    key_held[key] = held;
    kq_push(key, held ? 1 : 0);
}

// doom_mutex must be held.
static void release_all_keys(void) {
    for (int k = 0; k < 256; k++) {
        if (pulse_deadline[k]) {
            pulse_deadline[k] = 0;
            kq_push((uint8_t)k, 0);
        }
        if (key_held[k])
            key_set_held((uint8_t)k, false);
    }
    espnow_mask = 0;
}

//
// doomgeneric callbacks (engine thread)
//

void DG_Init() {
}

void DG_DrawFrame() {
    pthread_mutex_lock(&doom_mutex);
    memcpy(doom_frame, DG_ScreenBuffer, sizeof(doom_frame));
    frame_ready = true;
    pthread_mutex_unlock(&doom_mutex);
}

void DG_SleepMs(uint32_t ms) {
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs() {
    return ticks_ms();
}

int DG_GetKey(int *pressed, unsigned char *doomKey) {
    int have = 0;

    pthread_mutex_lock(&doom_mutex);

    // auto-release expired pulses first so a detent registers as a short hold
    uint32_t now = ticks_ms();
    for (int k = 0; k < 256; k++) {
        if (pulse_deadline[k] && (int32_t)(now - pulse_deadline[k]) >= 0) {
            pulse_deadline[k] = 0;
            if (!key_held[k])
                kq_push((uint8_t)k, 0);
        }
    }

    if (kq_count) {
        *doomKey = kq[kq_read].key;
        *pressed = kq[kq_read].pressed;
        kq_read = (kq_read + 1) % KQ_LEN;
        kq_count--;
        have = 1;
    }

    pthread_mutex_unlock(&doom_mutex);
    return have;
}

void DG_SetWindowTitle(const char *title) {
    (void)title;
}

//
// engine thread
//

static void *doom_thread_fn(void *arg) {
    (void)arg;

    // config, savegames and IWAD lookups all land on the SD card
    setenv("HOME", "/mnt/extsd", 0);
    setenv("DOOMWADDIR", "/mnt/extsd", 0);

    char *argv[] = {"doom", "-iwad", doom_wad, NULL};
    doomgeneric_Create(3, argv);

    for (;;) {
        pthread_mutex_lock(&doom_mutex);
        while (doom_paused)
            pthread_cond_wait(&doom_cond, &doom_mutex);
        pthread_mutex_unlock(&doom_mutex);

        doomgeneric_Tick();
    }
    return NULL;
}

//
// UI-facing API
//

bool doom_hdz_find_wad(char *buf, size_t len) {
    static const char *names[] = {
        "DOOM1.WAD", "doom1.wad", "DOOM.WAD", "doom.wad",
        "DOOM2.WAD", "doom2.wad", "freedoom1.wad", "freedoom2.wad"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[256];
        struct stat st;
        snprintf(path, sizeof(path), "/mnt/extsd/%s", names[i]);
        if (stat(path, &st) == 0 && st.st_size > 0) {
            snprintf(buf, len, "%s", path);
            return true;
        }
    }
    return false;
}

bool doom_hdz_start(const char *wad_path) {
    pthread_mutex_lock(&doom_mutex);
    if (!doom_started) {
        snprintf(doom_wad, sizeof(doom_wad), "%s", wad_path);
        doom_paused = false;
        if (pthread_create(&doom_thread, NULL, doom_thread_fn, NULL) != 0) {
            pthread_mutex_unlock(&doom_mutex);
            return false;
        }
        doom_started = true;
    } else {
        doom_paused = false;
        pthread_cond_signal(&doom_cond);
    }
    pthread_mutex_unlock(&doom_mutex);
    return true;
}

void doom_hdz_pause(void) {
    pthread_mutex_lock(&doom_mutex);
    if (doom_started) {
        doom_paused = true;
        release_all_keys();
    }
    pthread_mutex_unlock(&doom_mutex);
}

bool doom_hdz_active(void) {
    pthread_mutex_lock(&doom_mutex);
    bool active = doom_started && !doom_paused;
    pthread_mutex_unlock(&doom_mutex);
    return active;
}

bool doom_hdz_frame_copy_2x(uint32_t *dst) {
    pthread_mutex_lock(&doom_mutex);
    if (!frame_ready) {
        pthread_mutex_unlock(&doom_mutex);
        return false;
    }

    for (int y = 0; y < DOOM_H; y++) {
        const uint32_t *src = &doom_frame[y * DOOM_W];
        uint32_t *d = dst + (size_t)(2 * y) * DOOM_HDZ_FB_W;
        for (int x = 0; x < DOOM_W; x++) {
            uint32_t px = src[x] | 0xFF000000u;
            d[2 * x] = px;
            d[2 * x + 1] = px;
        }
        memcpy(d + DOOM_HDZ_FB_W, d, DOOM_HDZ_FB_W * sizeof(uint32_t));
    }

    frame_ready = false;
    pthread_mutex_unlock(&doom_mutex);
    return true;
}

void doom_hdz_action(doom_action_t act) {
    pthread_mutex_lock(&doom_mutex);
    if (!doom_started || doom_paused) {
        pthread_mutex_unlock(&doom_mutex);
        return;
    }

    switch (act) {
    case DOOM_ACT_TURN_LEFT:
        key_pulse(KEY_LEFTARROW);
        break;
    case DOOM_ACT_TURN_RIGHT:
        key_pulse(KEY_RIGHTARROW);
        break;
    case DOOM_ACT_FIRE:
        key_pulse(KEY_FIRE);
        break;
    case DOOM_ACT_USE_ENTER:
        key_pulse(KEY_USE);
        key_pulse(KEY_ENTER);
        break;
    case DOOM_ACT_TOGGLE_FORWARD:
        key_set_held(KEY_UPARROW, !key_held[KEY_UPARROW]);
        break;
    }
    pthread_mutex_unlock(&doom_mutex);
}

void doom_hdz_msp_input(const uint8_t *payload, uint16_t size) {
    static const struct {
        uint16_t bit;
        uint8_t key;
    } map[] = {
        {DOOM_BTN_FORWARD, KEY_UPARROW},
        {DOOM_BTN_BACK, KEY_DOWNARROW},
        {DOOM_BTN_TURN_L, KEY_LEFTARROW},
        {DOOM_BTN_TURN_R, KEY_RIGHTARROW},
        {DOOM_BTN_FIRE, KEY_FIRE},
        {DOOM_BTN_USE, KEY_USE},
        {DOOM_BTN_ENTER, KEY_ENTER},
        {DOOM_BTN_ESCAPE, KEY_ESCAPE},
        {DOOM_BTN_STRAFE_L, KEY_STRAFE_L},
        {DOOM_BTN_STRAFE_R, KEY_STRAFE_R},
    };

    if (size < 2)
        return;

    pthread_mutex_lock(&doom_mutex);
    if (!doom_started || doom_paused) {
        pthread_mutex_unlock(&doom_mutex);
        return;
    }

    uint16_t mask = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    uint16_t changed = mask ^ espnow_mask;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (changed & map[i].bit)
            key_set_held(map[i].key, mask & map[i].bit);
    }
    espnow_mask = mask;
    pthread_mutex_unlock(&doom_mutex);
}
