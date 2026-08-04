// HDZero goggle platform layer for quakegeneric (WinQuake).
//
// Same shape as the Doom port: the engine runs on its own thread via
// QG_Create()/QG_Tick(), frames arrive through QG_DrawFrame() as 8-bit
// palettized 320x240 which we expand to ARGB, and input comes either from
// the goggle buttons or the transmitter's ESP-NOW button mask.

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "game_input.h" // DOOM_BTN_* controller mask bits
#include "quake_hdz.h"
#include "quakegeneric/quakegeneric.h"

#define REN_W QUAKEGENERIC_RES_X
#define REN_H QUAKEGENERIC_RES_Y
#define SCALE 4

#define PULSE_MS 140
#define QUAKE_BASEDIR "/mnt/extsd/quake"

static pthread_mutex_t quake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t quake_cond = PTHREAD_COND_INITIALIZER;
static pthread_t quake_thread;
static bool quake_started = false;
static bool quake_paused = false;

static uint32_t palette_lut[256];
static uint32_t frame[REN_W * REN_H];
static bool frame_ready = false;

// key events towards the engine
#define KQ_LEN 64
static struct {
    int key;
    int down;
} kq[KQ_LEN];
static int kq_count = 0, kq_read = 0;

static bool key_held[512];
static uint32_t pulse_deadline[512];
static uint16_t espnow_mask = 0;
static bool forward_toggled = false;
static bool analog_present = false; // transmitter sends axis bytes
static float axis_turn = 0.0f, axis_pitch = 0.0f;

static const struct {
    uint16_t bit;
    int key;
} mask_keys[] = {
    {DOOM_BTN_FORWARD, K_UPARROW},
    {DOOM_BTN_BACK, K_DOWNARROW},
    {DOOM_BTN_TURN_L, K_LEFTARROW},
    {DOOM_BTN_TURN_R, K_RIGHTARROW},
    {DOOM_BTN_FIRE, K_CTRL},
    {DOOM_BTN_USE, K_SPACE}, // jump: quake has no "use" action
    {DOOM_BTN_ENTER, K_ENTER},
    {DOOM_BTN_ESCAPE, K_ESCAPE},
    {DOOM_BTN_STRAFE_L, ','},
    {DOOM_BTN_STRAFE_R, '.'},
    {DOOM_BTN_Y, 'y'},
    // look up/down via 'a'/'z' (+lookup/+lookdown in id's default.cfg).
    // Never send PGUP/PGDN here: stock default.cfg binds PGDN to +lookup
    // (DEL is lookdown), so PGDN would fight the 'z' lookdown and win.
    {DOOM_BTN_LOOK_UP, 'a'},
    {DOOM_BTN_LOOK_DOWN, 'z'},
};

static uint32_t ticks_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// quake_mutex must be held.
static void kq_push(int key, int down) {
    if (kq_count == KQ_LEN)
        return;
    kq[(kq_read + kq_count) % KQ_LEN].key = key;
    kq[(kq_read + kq_count) % KQ_LEN].down = down;
    kq_count++;
}

// quake_mutex must be held.
static void key_set_held(int key, bool held) {
    if (key_held[key] == held)
        return;
    key_held[key] = held;
    kq_push(key, held ? 1 : 0);
}

// quake_mutex must be held.
static void key_pulse(int key) {
    if (!key_held[key] && pulse_deadline[key] == 0)
        kq_push(key, 1);
    pulse_deadline[key] = ticks_ms() + PULSE_MS;
}

// quake_mutex must be held.
static void release_all_keys(void) {
    for (int k = 0; k < 512; k++) {
        if (pulse_deadline[k]) {
            pulse_deadline[k] = 0;
            kq_push(k, 0);
        }
        if (key_held[k])
            key_set_held(k, false);
    }
    espnow_mask = 0;
    forward_toggled = false;
}

//
// quakegeneric callbacks (engine thread)
//

void QG_Init(void) {
}

void QG_Quit(void) {
    // in-game quit: park the engine thread instead of exiting the app
    pthread_mutex_lock(&quake_mutex);
    quake_paused = true;
    release_all_keys();
    pthread_mutex_unlock(&quake_mutex);
    pthread_exit(NULL);
}

void QG_DrawFrame(void *pixels) {
    const uint8_t *src = (const uint8_t *)pixels;
    pthread_mutex_lock(&quake_mutex);
    for (int i = 0; i < REN_W * REN_H; i++)
        frame[i] = palette_lut[src[i]];
    frame_ready = true;
    pthread_mutex_unlock(&quake_mutex);
}

void QG_SetPalette(unsigned char palette[768]) {
    pthread_mutex_lock(&quake_mutex);
    for (int i = 0; i < 256; i++) {
        palette_lut[i] = 0xFF000000u |
                         ((uint32_t)palette[i * 3 + 0] << 16) |
                         ((uint32_t)palette[i * 3 + 1] << 8) |
                         (uint32_t)palette[i * 3 + 2];
    }
    pthread_mutex_unlock(&quake_mutex);
}

int QG_GetKey(int *down, int *key) {
    int have = 0;
    pthread_mutex_lock(&quake_mutex);

    uint32_t now = ticks_ms();
    for (int k = 0; k < 512; k++) {
        if (pulse_deadline[k] && (int32_t)(now - pulse_deadline[k]) >= 0) {
            pulse_deadline[k] = 0;
            if (!key_held[k])
                kq_push(k, 0);
        }
    }

    if (kq_count) {
        *key = kq[kq_read].key;
        *down = kq[kq_read].down;
        kq_read = (kq_read + 1) % KQ_LEN;
        kq_count--;
        have = 1;
    }
    pthread_mutex_unlock(&quake_mutex);
    return have;
}

void QG_GetMouseMove(int *x, int *y) {
    *x = 0;
    *y = 0;
}

void QG_GetJoyAxes(float *axes) {
    for (int i = 0; i < QUAKEGENERIC_JOY_MAX_AXES; i++)
        axes[i] = 0.0f;
    pthread_mutex_lock(&quake_mutex);
    axes[QUAKEGENERIC_JOY_AXIS_R] = axis_turn;  // yaw rate, proportional
    axes[QUAKEGENERIC_JOY_AXIS_U] = axis_pitch; // positional pitch
    pthread_mutex_unlock(&quake_mutex);
}

//
// engine thread
//

static void *quake_thread_fn(void *arg) {
    (void)arg;

    char *argv[] = {"quake", "-basedir", QUAKE_BASEDIR, NULL};
    QG_Create(3, argv);

    // axis R = turn (proportional yaw rate), axis U = look (positional
    // pitch; see the JOY_ABSOLUTE_AXIS branch in in_null.c). Sensitivities
    // are negative so stick right turns right and stick up looks up.
    extern void Cbuf_AddText(char *text);
    Cbuf_AddText("joystick 1\njoyadvanced 1\njoyadvaxisr 4\njoyadvaxisu 2\n"
                 "joypitchsensitivity -1\njoyadvancedupdate\n+mlook\n");

    uint32_t last = ticks_ms();
    for (;;) {
        pthread_mutex_lock(&quake_mutex);
        while (quake_paused)
            pthread_cond_wait(&quake_cond, &quake_mutex);
        pthread_mutex_unlock(&quake_mutex);

        uint32_t now = ticks_ms();
        double dt = (now - last) / 1000.0;
        last = now;
        if (dt > 0.1)
            dt = 0.1;
        if (dt <= 0.0)
            dt = 0.001;

        QG_Tick(dt);
        usleep(1000); // yield; Host_Frame paces itself off the durations
    }
    return NULL;
}

//
// UI-facing API
//

bool quake_hdz_find_pak(void) {
    static const char *paths[] = {
        QUAKE_BASEDIR "/id1/pak0.pak",
        QUAKE_BASEDIR "/id1/PAK0.PAK",
    };
    struct stat st;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
        if (stat(paths[i], &st) == 0 && st.st_size > 0)
            return true;
    return false;
}

bool quake_hdz_start(void) {
    pthread_mutex_lock(&quake_mutex);
    if (!quake_started) {
        quake_paused = false;
        if (pthread_create(&quake_thread, NULL, quake_thread_fn, NULL) != 0) {
            pthread_mutex_unlock(&quake_mutex);
            return false;
        }
        quake_started = true;
    } else {
        quake_paused = false;
        pthread_cond_signal(&quake_cond);
    }
    pthread_mutex_unlock(&quake_mutex);
    return true;
}

void quake_hdz_pause(void) {
    pthread_mutex_lock(&quake_mutex);
    if (quake_started) {
        quake_paused = true;
        axis_turn = axis_pitch = 0.0f;
        release_all_keys();
    }
    pthread_mutex_unlock(&quake_mutex);
}

bool quake_hdz_active(void) {
    pthread_mutex_lock(&quake_mutex);
    bool active = quake_started && !quake_paused;
    pthread_mutex_unlock(&quake_mutex);
    return active;
}

bool quake_hdz_frame_copy(uint32_t *dst) {
    pthread_mutex_lock(&quake_mutex);
    if (!frame_ready) {
        pthread_mutex_unlock(&quake_mutex);
        return false;
    }

    for (int y = 0; y < REN_H; y++) {
        const uint32_t *src = &frame[y * REN_W];
        uint32_t *row = dst + (size_t)y * SCALE * QUAKE_FB_W;
        for (int x = 0; x < REN_W; x++) {
            uint32_t px = src[x];
            uint32_t *d = row + x * SCALE;
            d[0] = px;
            d[1] = px;
            d[2] = px;
            d[3] = px;
        }
        for (int i = 1; i < SCALE; i++)
            memcpy(row + (size_t)i * QUAKE_FB_W, row, QUAKE_FB_W * sizeof(uint32_t));
    }

    frame_ready = false;
    pthread_mutex_unlock(&quake_mutex);
    return true;
}

void quake_hdz_action(quake_action_t act) {
    pthread_mutex_lock(&quake_mutex);
    if (!quake_started || quake_paused) {
        pthread_mutex_unlock(&quake_mutex);
        return;
    }
    switch (act) {
    case QUAKE_ACT_TURN_LEFT:
        key_pulse(K_LEFTARROW);
        break;
    case QUAKE_ACT_TURN_RIGHT:
        key_pulse(K_RIGHTARROW);
        break;
    case QUAKE_ACT_FIRE:
        key_pulse(K_CTRL);
        break;
    case QUAKE_ACT_JUMP_ENTER:
        key_pulse(K_SPACE);
        key_pulse(K_ENTER);
        key_pulse('y');
        break;
    case QUAKE_ACT_TOGGLE_FORWARD:
        forward_toggled = !forward_toggled;
        key_set_held(K_UPARROW, forward_toggled);
        break;
    }
    pthread_mutex_unlock(&quake_mutex);
}

void quake_hdz_msp_input(const uint8_t *payload, uint16_t size) {
    if (size < 2)
        return;
    pthread_mutex_lock(&quake_mutex);
    if (!quake_started || quake_paused) {
        pthread_mutex_unlock(&quake_mutex);
        return;
    }
    uint16_t mask = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    if (size >= 4) {
        analog_present = true;
        axis_turn = (float)(int8_t)payload[2] / 127.0f;
        axis_pitch = (float)(int8_t)payload[3] / 127.0f;
    }
    if (analog_present)
        mask &= ~(DOOM_BTN_TURN_L | DOOM_BTN_TURN_R |
                  DOOM_BTN_LOOK_UP | DOOM_BTN_LOOK_DOWN);
    uint16_t changed = mask ^ espnow_mask;
    if (changed & DOOM_WEAPON_FIELD) {
        int slot = (mask >> DOOM_WEAPON_SHIFT) & 7;
        if (slot >= 1 && slot <= 6)
            key_pulse('0' + slot);
    }
    for (size_t i = 0; i < sizeof(mask_keys) / sizeof(mask_keys[0]); i++) {
        if (changed & mask_keys[i].bit) {
            bool down = (mask & mask_keys[i].bit) != 0;
            if (mask_keys[i].bit == DOOM_BTN_FORWARD && forward_toggled && !down) {
                // don't release a forward that the goggle toggle holds
            } else {
                key_set_held(mask_keys[i].key, down);
            }
        }
    }
    espnow_mask = mask;
    pthread_mutex_unlock(&quake_mutex);
}
