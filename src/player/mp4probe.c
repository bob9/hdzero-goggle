// Minimal MP4 box parser: reads the video track's frame rate straight from
// the file (moov/trak/mdia/mdhd + stts), so the player can set up the
// display before the vendor demuxer ever touches the file. No decoding, no
// vendor libraries - just box walking.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mp4probe.h"

#define RD32(p) (((uint32_t)(p)[0] << 24) | ((uint32_t)(p)[1] << 16) | ((uint32_t)(p)[2] << 8) | (uint32_t)(p)[3])

typedef struct {
    FILE *f;
    long end; // absolute end offset of the current container
} boxctx_t;

// Read the next box header at the current position; returns type and the
// absolute offset of the payload plus its size. False at container end.
static bool next_box(boxctx_t *c, char type[5], long *payload, long *psize) {
    long pos = ftell(c->f);
    if (pos < 0 || pos + 8 > c->end)
        return false;

    uint8_t hdr[8];
    if (fread(hdr, 1, 8, c->f) != 8)
        return false;

    uint64_t size = RD32(hdr);
    memcpy(type, hdr + 4, 4);
    type[4] = 0;
    long hlen = 8;

    if (size == 1) { // 64-bit size
        uint8_t big[8];
        if (fread(big, 1, 8, c->f) != 8)
            return false;
        size = ((uint64_t)RD32(big) << 32) | RD32(big + 4);
        hlen = 16;
    } else if (size == 0) { // to end of file/container
        size = c->end - pos;
    }
    if (size < (uint64_t)hlen || pos + (long)size > c->end)
        return false;

    *payload = pos + hlen;
    *psize = (long)size - hlen;
    return true;
}

static void box_skip(boxctx_t *c, long payload, long psize) {
    fseek(c->f, payload + psize, SEEK_SET);
}

// Parse one trak: true if it is a video track, filling timescale and the
// stts first-entry sample delta.
static bool parse_trak(FILE *f, long payload, long psize,
                       uint32_t *timescale, uint32_t *delta) {
    bool is_video = false;
    uint32_t ts = 0, dl = 0;

    boxctx_t trak = {f, payload + psize};
    fseek(f, payload, SEEK_SET);
    char t[5];
    long pl, sz;
    while (next_box(&trak, t, &pl, &sz)) {
        if (strcmp(t, "mdia") == 0) {
            boxctx_t mdia = {f, pl + sz};
            fseek(f, pl, SEEK_SET);
            char t2[5];
            long pl2, sz2;
            while (next_box(&mdia, t2, &pl2, &sz2)) {
                if (strcmp(t2, "mdhd") == 0 && sz2 >= 20) {
                    uint8_t b[24];
                    long n = sz2 < (long)sizeof(b) ? sz2 : (long)sizeof(b);
                    if (fread(b, 1, n, f) == (size_t)n) {
                        ts = (b[0] == 1) ? RD32(b + 20) : RD32(b + 12);
                    }
                } else if (strcmp(t2, "hdlr") == 0 && sz2 >= 12) {
                    uint8_t b[12];
                    if (fread(b, 1, 12, f) == 12 && memcmp(b + 8, "vide", 4) == 0)
                        is_video = true;
                } else if (strcmp(t2, "minf") == 0) {
                    boxctx_t minf = {f, pl2 + sz2};
                    fseek(f, pl2, SEEK_SET);
                    char t3[5];
                    long pl3, sz3;
                    while (next_box(&minf, t3, &pl3, &sz3)) {
                        if (strcmp(t3, "stbl") == 0) {
                            boxctx_t stbl = {f, pl3 + sz3};
                            fseek(f, pl3, SEEK_SET);
                            char t4[5];
                            long pl4, sz4;
                            while (next_box(&stbl, t4, &pl4, &sz4)) {
                                if (strcmp(t4, "stts") == 0 && sz4 >= 16) {
                                    uint8_t b[16];
                                    if (fread(b, 1, 16, f) == 16 && RD32(b + 4) >= 1)
                                        dl = RD32(b + 12); // first entry sample delta
                                }
                                box_skip(&stbl, pl4, sz4);
                            }
                        }
                        box_skip(&minf, pl3, sz3);
                    }
                }
                box_skip(&mdia, pl2, sz2);
            }
        }
        box_skip(&trak, pl, sz);
    }

    if (is_video && ts && dl) {
        *timescale = ts;
        *delta = dl;
        return true;
    }
    return false;
}

// Returns the video frame rate of an mp4 file rounded to the nearest
// integer, or 0 if it cannot be determined.
int mp4probe_fps(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    int fps = 0;
    boxctx_t top = {f, fsize};
    char t[5];
    long pl, sz;
    while (next_box(&top, t, &pl, &sz)) {
        if (strcmp(t, "moov") == 0) {
            boxctx_t moov = {f, pl + sz};
            fseek(f, pl, SEEK_SET);
            char t2[5];
            long pl2, sz2;
            while (next_box(&moov, t2, &pl2, &sz2)) {
                if (strcmp(t2, "trak") == 0) {
                    uint32_t ts, dl;
                    if (parse_trak(f, pl2, sz2, &ts, &dl)) {
                        fps = (int)((ts + dl / 2) / dl);
                        break;
                    }
                }
                box_skip(&moov, pl2, sz2);
            }
            break;
        }
        box_skip(&top, pl, sz);
    }

    fclose(f);
    return fps;
}
