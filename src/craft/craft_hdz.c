// A tiny Minecraft-style voxel world for the HDZero goggles, in the spirit
// of Notch's Minecraft4k: a 64x64x64 block world rendered by per-pixel ray
// marching on the CPU. Walk the terrain, dig blocks, place blocks.
//
// Runs on its own thread like the Doom engine; the UI polls frames via
// craft_hdz_frame_copy() and inputs arrive either from the goggle buttons
// (craft_hdz_action) or from the transmitter over ESP-NOW using the same
// button mask as DOOM (craft_hdz_msp_input).

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "craft_hdz.h"
#include "doom/doom_hdz.h" // DOOM_BTN_* button mask bits

#define REN_W 160
#define REN_H 100
#define SCALE 8

#define WORLD 64
#define EYE_HEIGHT 1.6f
#define MAX_DIST 28.0f

#define BLK_AIR 0
#define BLK_GRASS 1
#define BLK_DIRT 2
#define BLK_STONE 3
#define BLK_WOOD 4
#define BLK_LEAVES 5

#define PULSE_MS 140
#define ACT_INTERVAL_MS 280 // dig/place repeat while held

static pthread_mutex_t craft_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t craft_cond = PTHREAD_COND_INITIALIZER;
static pthread_t craft_thread;
static bool craft_started = false;
static bool craft_paused = false;

static uint8_t world[WORLD * WORLD * WORLD];

static float ppx, ppy, ppz; // player feet position
static float yaw;
static float vel_y;
static bool on_ground;

// inputs (guarded by craft_mutex)
static uint16_t espnow_mask;
static uint16_t pulse_mask;
static uint32_t pulse_deadline[16];
static bool forward_toggled;

static uint32_t frame[REN_W * REN_H];
static bool frame_ready = false;

static uint32_t ticks_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static inline uint8_t world_get(int x, int y, int z) {
    if (x < 0 || x >= WORLD || z < 0 || z >= WORLD || y >= WORLD)
        return BLK_AIR;
    if (y < 0)
        return BLK_STONE;
    return world[(y * WORLD + z) * WORLD + x];
}

static inline void world_set(int x, int y, int z, uint8_t b) {
    if (x < 0 || x >= WORLD || y < 1 || y >= WORLD || z < 0 || z >= WORLD)
        return; // keep the bottom layer so nobody digs into the void
    world[(y * WORLD + z) * WORLD + x] = b;
}

static int terrain_height(int x, int z) {
    float h = 26.0f + 6.0f * sinf(x * 0.23f) + 5.0f * cosf(z * 0.19f) +
              3.0f * sinf((x + z) * 0.11f) + 2.0f * cosf((x - z) * 0.31f);
    int hi = (int)h;
    if (hi < 4)
        hi = 4;
    if (hi > WORLD - 12)
        hi = WORLD - 12;
    return hi;
}

static void world_generate(void) {
    memset(world, BLK_AIR, sizeof(world));
    for (int x = 0; x < WORLD; x++) {
        for (int z = 0; z < WORLD; z++) {
            int h = terrain_height(x, z);
            for (int y = 0; y <= h; y++) {
                uint8_t b = BLK_STONE;
                if (y == h)
                    b = BLK_GRASS;
                else if (y >= h - 3)
                    b = BLK_DIRT;
                world[(y * WORLD + z) * WORLD + x] = b;
            }
        }
    }
    // a few deterministic trees on flat ground
    for (int x = 6; x < WORLD - 6; x += 11) {
        for (int z = 8; z < WORLD - 6; z += 13) {
            int h = terrain_height(x, z);
            if (terrain_height(x + 1, z) != h || terrain_height(x, z + 1) != h)
                continue;
            for (int y = h + 1; y <= h + 4; y++)
                world_set(x, y, z, BLK_WOOD);
            for (int dx = -2; dx <= 2; dx++)
                for (int dz = -2; dz <= 2; dz++)
                    for (int dy = 4; dy <= 6; dy++)
                        if (!(dx == 0 && dz == 0 && dy < 6))
                            if (world_get(x + dx, h + dy, z + dz) == BLK_AIR)
                                world_set(x + dx, h + dy, z + dz, BLK_LEAVES);
        }
    }
}

// ---- rendering ----

static const uint32_t block_rgb[6] = {
    0x000000,  // air (unused)
    0x4CAF50,  // grass
    0x9B7653,  // dirt
    0x8A8A8A,  // stone
    0x7A5220,  // wood
    0x2E8B57,  // leaves
};

static inline uint32_t shade(uint32_t rgb, float f) {
    if (f < 0.0f)
        f = 0.0f;
    if (f > 1.0f)
        f = 1.0f;
    uint32_t r = (uint32_t)(((rgb >> 16) & 0xFF) * f);
    uint32_t g = (uint32_t)(((rgb >> 8) & 0xFF) * f);
    uint32_t b = (uint32_t)((rgb & 0xFF) * f);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// March a ray through the voxel grid (Amanatides & Woo). Returns the block
// type or 0; fills the hit cell, the cell before it, and the distance.
static uint8_t raycast(float ox, float oy, float oz,
                       float dx, float dy, float dz,
                       int *hx, int *hy, int *hz,
                       int *prevx, int *prevy, int *prevz,
                       float *dist, int *side) {
    int mx = (int)floorf(ox), my = (int)floorf(oy), mz = (int)floorf(oz);
    float ddx = (dx == 0.0f) ? 1e30f : fabsf(1.0f / dx);
    float ddy = (dy == 0.0f) ? 1e30f : fabsf(1.0f / dy);
    float ddz = (dz == 0.0f) ? 1e30f : fabsf(1.0f / dz);
    int sx = (dx < 0) ? -1 : 1, sy = (dy < 0) ? -1 : 1, sz = (dz < 0) ? -1 : 1;
    float tx = ((dx < 0) ? (ox - mx) : (mx + 1.0f - ox)) * ddx;
    float ty = ((dy < 0) ? (oy - my) : (my + 1.0f - oy)) * ddy;
    float tz = ((dz < 0) ? (oz - mz) : (mz + 1.0f - oz)) * ddz;
    int px = mx, py = my, pz = mz;
    float t = 0.0f;

    for (int i = 0; i < 100; i++) {
        px = mx;
        py = my;
        pz = mz;
        if (tx < ty && tx < tz) {
            mx += sx;
            t = tx;
            tx += ddx;
            *side = 0;
        } else if (ty < tz) {
            my += sy;
            t = ty;
            ty += ddy;
            *side = 1;
        } else {
            mz += sz;
            t = tz;
            tz += ddz;
            *side = 2;
        }
        if (t > MAX_DIST)
            break;
        uint8_t b = world_get(mx, my, mz);
        if (b != BLK_AIR) {
            *hx = mx;
            *hy = my;
            *hz = mz;
            *prevx = px;
            *prevy = py;
            *prevz = pz;
            *dist = t;
            return b;
        }
    }
    return BLK_AIR;
}

static void render_frame(void) {
    float cy = cosf(yaw), sy = sinf(yaw);
    float ex = ppx, ey = ppy + EYE_HEIGHT, ez = ppz;

    for (int y = 0; y < REN_H; y++) {
        float vdir = -((float)y / REN_H - 0.5f) * 1.05f;
        for (int x = 0; x < REN_W; x++) {
            float hdir = ((float)x / REN_W - 0.5f) * 1.6f;
            float dx = sy + cy * hdir;
            float dz = cy - sy * hdir;
            float dy = vdir;

            int hx, hy, hz, px, pz2, py2, side = 0;
            float dist;
            uint8_t b = raycast(ex, ey, ez, dx, dy, dz,
                                &hx, &hy, &hz, &px, &py2, &pz2, &dist, &side);
            uint32_t c;
            if (b == BLK_AIR) {
                // sky: light horizon to blue zenith
                float k = vdir * 2.0f;
                if (k < 0.0f)
                    k = 0.0f;
                if (k > 1.0f)
                    k = 1.0f;
                uint32_t r = 200 - (uint32_t)(k * 100);
                uint32_t g = 225 - (uint32_t)(k * 80);
                c = 0xFF000000u | (r << 16) | (g << 8) | 255;
            } else {
                uint32_t rgb = block_rgb[b];
                // grass shows dirt on its sides
                if (b == BLK_GRASS && side != 1)
                    rgb = 0x7A8B4A;
                float bright = (side == 1) ? ((dy < 0) ? 1.0f : 0.55f)
                                           : ((side == 0) ? 0.8f : 0.65f);
                // cheap texture: dim a checker of the hit face
                float u, v;
                if (side == 0) {
                    u = ez + dz * dist;
                    v = ey + dy * dist;
                } else if (side == 1) {
                    u = ex + dx * dist;
                    v = ez + dz * dist;
                } else {
                    u = ex + dx * dist;
                    v = ey + dy * dist;
                }
                int iu = (int)floorf(u * 4.0f), iv = (int)floorf(v * 4.0f);
                if ((iu ^ iv) & 1)
                    bright *= 0.88f;
                float fog = 1.0f - dist / MAX_DIST;
                c = shade(rgb, bright * (0.35f + 0.65f * fog));
            }
            frame[y * REN_W + x] = c;
        }
    }

    // crosshair
    for (int i = -3; i <= 3; i++) {
        frame[(REN_H / 2) * REN_W + REN_W / 2 + i] ^= 0x00FFFFFF;
        if (i != 0)
            frame[(REN_H / 2 + i) * REN_W + REN_W / 2] ^= 0x00FFFFFF;
    }
}

// ---- simulation ----

static bool cell_free(float x, float y, float z) {
    int fy = (int)floorf(y);
    return world_get((int)floorf(x), fy, (int)floorf(z)) == BLK_AIR &&
           world_get((int)floorf(x), fy + 1, (int)floorf(z)) == BLK_AIR;
}

static uint16_t effective_mask(void) {
    // caller holds craft_mutex
    uint32_t now = ticks_ms();
    uint16_t m = espnow_mask;
    for (int b = 0; b < 16; b++) {
        if (pulse_deadline[b]) {
            if ((int32_t)(now - pulse_deadline[b]) >= 0)
                pulse_deadline[b] = 0;
            else
                m |= (1 << b);
        }
    }
    m |= pulse_mask;
    pulse_mask = 0;
    if (forward_toggled)
        m |= DOOM_BTN_FORWARD;
    return m;
}

static void simulate(float dt, uint16_t mask, uint32_t now) {
    static uint32_t last_dig = 0, last_place = 0;

    if (mask & DOOM_BTN_TURN_L)
        yaw -= 2.1f * dt;
    if (mask & DOOM_BTN_TURN_R)
        yaw += 2.1f * dt;

    float fwd = 0.0f, strafe = 0.0f;
    if (mask & DOOM_BTN_FORWARD)
        fwd += 1.0f;
    if (mask & DOOM_BTN_BACK)
        fwd -= 1.0f;
    if (mask & DOOM_BTN_STRAFE_R)
        strafe += 1.0f;
    if (mask & DOOM_BTN_STRAFE_L)
        strafe -= 1.0f;

    float speed = 4.0f;
    float mx = (sinf(yaw) * fwd + cosf(yaw) * strafe) * speed * dt;
    float mz = (cosf(yaw) * fwd - sinf(yaw) * strafe) * speed * dt;

    // horizontal movement with 1-block auto step-up
    float nx = ppx + mx, nz = ppz + mz;
    if (cell_free(nx, ppy + 0.05f, nz)) {
        ppx = nx;
        ppz = nz;
    } else if (world_get((int)floorf(nx), (int)floorf(ppy + 0.05f) + 1, (int)floorf(nz)) == BLK_AIR &&
               world_get((int)floorf(nx), (int)floorf(ppy + 0.05f) + 2, (int)floorf(nz)) == BLK_AIR &&
               on_ground) {
        ppy = floorf(ppy) + 1.02f;
        ppx = nx;
        ppz = nz;
    }

    // gravity and jumping
    if ((mask & DOOM_BTN_ENTER) && on_ground)
        vel_y = 7.0f;
    vel_y -= 18.0f * dt;
    if (vel_y < -20.0f)
        vel_y = -20.0f;
    ppy += vel_y * dt;
    on_ground = false;
    if (world_get((int)floorf(ppx), (int)floorf(ppy), (int)floorf(ppz)) != BLK_AIR) {
        ppy = floorf(ppy) + 1.0f;
        if (vel_y < 0)
            vel_y = 0;
        on_ground = true;
    }
    if (world_get((int)floorf(ppx), (int)floorf(ppy + EYE_HEIGHT + 0.2f), (int)floorf(ppz)) != BLK_AIR && vel_y > 0)
        vel_y = 0;

    // dig / place the targeted block
    if (mask & (DOOM_BTN_FIRE | DOOM_BTN_USE)) {
        int hx, hy, hz, px, py2, pz2, side;
        float dist;
        uint8_t b = raycast(ppx, ppy + EYE_HEIGHT, ppz, sinf(yaw), 0.0f, cosf(yaw),
                            &hx, &hy, &hz, &px, &py2, &pz2, &dist, &side);
        if (b != BLK_AIR && dist < 6.0f) {
            if ((mask & DOOM_BTN_FIRE) && now - last_dig > ACT_INTERVAL_MS) {
                last_dig = now;
                world_set(hx, hy, hz, BLK_AIR);
            }
            if ((mask & DOOM_BTN_USE) && now - last_place > ACT_INTERVAL_MS) {
                last_place = now;
                // don't place a block inside the player
                if (!(px == (int)floorf(ppx) && pz2 == (int)floorf(ppz) &&
                      (py2 == (int)floorf(ppy) || py2 == (int)floorf(ppy) + 1)))
                    world_set(px, py2, pz2, BLK_DIRT);
            }
        }
    }
}

static void *craft_thread_fn(void *arg) {
    (void)arg;
    uint32_t last = ticks_ms();

    for (;;) {
        pthread_mutex_lock(&craft_mutex);
        while (craft_paused)
            pthread_cond_wait(&craft_cond, &craft_mutex);
        uint16_t mask = effective_mask();
        pthread_mutex_unlock(&craft_mutex);

        uint32_t now = ticks_ms();
        float dt = (now - last) / 1000.0f;
        last = now;
        if (dt > 0.1f)
            dt = 0.1f;

        simulate(dt, mask, now);
        render_frame();

        pthread_mutex_lock(&craft_mutex);
        frame_ready = true;
        pthread_mutex_unlock(&craft_mutex);

        // pace to ~30fps; render dominates on slow frames anyway
        uint32_t took = ticks_ms() - now;
        if (took < 33)
            usleep((33 - took) * 1000);
    }
    return NULL;
}

// ---- UI-facing API ----

bool craft_hdz_start(void) {
    pthread_mutex_lock(&craft_mutex);
    if (!craft_started) {
        world_generate();
        ppx = WORLD / 2 + 0.5f;
        ppz = WORLD / 2 + 0.5f;
        ppy = terrain_height(WORLD / 2, WORLD / 2) + 1.0f;
        yaw = 0.8f;
        vel_y = 0;
        craft_paused = false;
        if (pthread_create(&craft_thread, NULL, craft_thread_fn, NULL) != 0) {
            pthread_mutex_unlock(&craft_mutex);
            return false;
        }
        craft_started = true;
    } else {
        craft_paused = false;
        pthread_cond_signal(&craft_cond);
    }
    pthread_mutex_unlock(&craft_mutex);
    return true;
}

void craft_hdz_pause(void) {
    pthread_mutex_lock(&craft_mutex);
    if (craft_started) {
        craft_paused = true;
        espnow_mask = 0;
        pulse_mask = 0;
        forward_toggled = false;
        memset(pulse_deadline, 0, sizeof(pulse_deadline));
    }
    pthread_mutex_unlock(&craft_mutex);
}

bool craft_hdz_active(void) {
    pthread_mutex_lock(&craft_mutex);
    bool active = craft_started && !craft_paused;
    pthread_mutex_unlock(&craft_mutex);
    return active;
}

bool craft_hdz_frame_copy(uint32_t *dst) {
    pthread_mutex_lock(&craft_mutex);
    if (!frame_ready) {
        pthread_mutex_unlock(&craft_mutex);
        return false;
    }

    for (int y = 0; y < REN_H; y++) {
        const uint32_t *src = &frame[y * REN_W];
        uint32_t *row = dst + (size_t)y * SCALE * CRAFT_FB_W;
        for (int x = 0; x < REN_W; x++) {
            uint32_t px = src[x];
            uint32_t *d = row + x * SCALE;
            for (int i = 0; i < SCALE; i++)
                d[i] = px;
        }
        for (int i = 1; i < SCALE; i++)
            memcpy(row + (size_t)i * CRAFT_FB_W, row, CRAFT_FB_W * sizeof(uint32_t));
    }

    frame_ready = false;
    pthread_mutex_unlock(&craft_mutex);
    return true;
}

static void bit_pulse(uint16_t bit) {
    // caller holds craft_mutex
    int idx = 0;
    while (((bit >> idx) & 1) == 0 && idx < 15)
        idx++;
    pulse_deadline[idx] = ticks_ms() + PULSE_MS;
}

void craft_hdz_action(craft_action_t act) {
    pthread_mutex_lock(&craft_mutex);
    if (!craft_started || craft_paused) {
        pthread_mutex_unlock(&craft_mutex);
        return;
    }
    switch (act) {
    case CRAFT_ACT_TURN_LEFT:
        bit_pulse(DOOM_BTN_TURN_L);
        break;
    case CRAFT_ACT_TURN_RIGHT:
        bit_pulse(DOOM_BTN_TURN_R);
        break;
    case CRAFT_ACT_BREAK:
        pulse_mask |= DOOM_BTN_FIRE;
        break;
    case CRAFT_ACT_PLACE:
        pulse_mask |= DOOM_BTN_USE;
        break;
    case CRAFT_ACT_TOGGLE_FORWARD:
        forward_toggled = !forward_toggled;
        break;
    }
    pthread_mutex_unlock(&craft_mutex);
}

void craft_hdz_msp_input(const uint8_t *payload, uint16_t size) {
    if (size < 2)
        return;
    pthread_mutex_lock(&craft_mutex);
    if (craft_started && !craft_paused)
        espnow_mask = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    pthread_mutex_unlock(&craft_mutex);
}
