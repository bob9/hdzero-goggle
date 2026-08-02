#include "plexstream.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <log/log.h>

#include "core/jellyfinapi.h"
#include "core/settings.h"
#include "util/hwlog.h"

typedef struct {
    plex_stream_state_t io;
    pthread_t thread;
    pthread_t reporter;
    bool reporter_active;
    bool active;
    bool preparing; // allocating the stream file; no bytes flow yet
    int backend;
    int offset_s;
    time_t started_at;
    char item_id[64];
    char path[768];
    char session[48];    // server-issued PlaySessionId is a 32-char GUID
    long long runtime_ms; // exact runtime from the server, 0 if unknown
} plexstream_t;

static plexstream_t g_stream;

// Make sure the stream file is already at its full size before any of it is
// downloaded, so the demuxer never sees a short file. Costs a one-off ~20s
// on a fresh card (FAT32 writes the blocks for real); every later stream
// finds the file already there and returns immediately.
static void plexstream_ensure_prealloc(void) {
    struct stat st;
    if (stat(PLEXSTREAM_FILE, &st) == 0 && st.st_size >= PLEXSTREAM_PREALLOC_BYTES) {
        return;
    }

    int fd = open(PLEXSTREAM_FILE, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        LOGE("plexstream: cannot create %s", PLEXSTREAM_FILE);
        return;
    }
    time_t const t0 = time(NULL);
    if (ftruncate(fd, (off_t)PLEXSTREAM_PREALLOC_BYTES) != 0) {
        LOGE("plexstream: preallocate failed");
        close(fd);
        unlink(PLEXSTREAM_FILE); // a partial file would be worse than none
        return;
    }
    close(fd);
    hwlog("plex: preallocated %lldMB in %lds", PLEXSTREAM_PREALLOC_BYTES / (1024 * 1024),
          (long)(time(NULL) - t0));
}

static void *plexstream_thread(void *arg) {
    (void)arg;
    plexstream_ensure_prealloc();
    g_stream.preparing = false;
    if (g_stream.backend == MEDIA_BACKEND_JELLYFIN) {
        jf_stream_download(g_stream.path, PLEXSTREAM_FILE, &g_stream.io);
    } else {
        plex_stream_download(g_stream.path, PLEXSTREAM_FILE, &g_stream.io);
    }
    // Fsync'd to SD: if playback later runs out of file, this line says
    // when and why the feed stopped (result 0 + no cancel = server closed)
    hwlog("plex: stream ended bytes=%ld result=%d%s",
          g_stream.io.bytes, g_stream.io.result, g_stream.io.cancel ? " (cancelled)" : "");
    return NULL;
}

// Keeps Jellyfin's transcoder alive: the server pauses any transcode that
// runs ~3 min ahead of the last reported playback position, and this client
// downloads far ahead of real time. Wall-clock elapsed tracks the viewer
// closely enough to keep the throttler satisfied.
static void *plexstream_reporter_thread(void *arg) {
    (void)arg;
    jf_playback_report(g_stream.item_id, g_stream.session, JF_PLAY_START,
                       (long long)g_stream.offset_s * 10000000LL);
    int ticks = 0;
    while (!g_stream.io.cancel && !g_stream.io.done) {
        sleep(1);
        if (++ticks < 10) {
            continue;
        }
        ticks = 0;
        long long pos_s = g_stream.offset_s + (long long)(time(NULL) - g_stream.started_at);
        jf_playback_report(g_stream.item_id, g_stream.session, JF_PLAY_PROGRESS,
                           pos_s * 10000000LL);
    }
    long long pos_s = g_stream.offset_s + (long long)(time(NULL) - g_stream.started_at);
    jf_playback_report(g_stream.item_id, g_stream.session, JF_PLAY_STOPPED,
                       pos_s * 10000000LL);
    return NULL;
}

bool plexstream_begin(const plex_movie_t *movie, int offset_s, int max_kbps) {
    if (g_stream.active) {
        plexstream_stop();
    }

    // The stream file is deliberately not deleted between movies: it is
    // preallocated once and overwritten from the front each time, which is
    // what keeps the demuxer from seeing a short file (see plexstream.h).
    mkdir("/mnt/extsd/plexcache", 0755);

    g_stream.backend = g_setting.plex.backend;
    g_stream.offset_s = offset_s;
    g_stream.started_at = time(NULL);
    snprintf(g_stream.item_id, sizeof(g_stream.item_id), "%s", movie->rating_key);
    snprintf(g_stream.session, sizeof(g_stream.session), "hdzg%08x", (unsigned)time(NULL));
    g_stream.runtime_ms = 0;

    if (g_stream.backend == MEDIA_BACKEND_JELLYFIN) {
        // Prefer the server's own session id and runtime; the locally
        // generated id above stands in if the call fails, which only costs
        // the throttle-avoidance reports and leaves playback working.
        if (jf_playback_info(g_stream.item_id, max_kbps, g_stream.session,
                             sizeof(g_stream.session), &g_stream.runtime_ms) != PLEX_OK) {
            LOGE("plexstream: PlaybackInfo failed, using a local session id");
        }
        jf_stream_path(g_stream.path, sizeof(g_stream.path), movie, offset_s, max_kbps, g_stream.session);
    } else {
        // Universal transcode to a single continuous MPEG-TS HTTP stream.
        // directStream=1 lets compatible H.264 video pass through as a cheap
        // remux; anything else (HEVC, high bitrates) is transcoded to H.264.
        snprintf(g_stream.path, sizeof(g_stream.path),
                 "/video/:/transcode/universal/start.ts"
                 "?path=%%2Flibrary%%2Fmetadata%%2F%s"
                 "&mediaIndex=0&partIndex=0"
                 "&protocol=http&container=mpegts"
                 "&videoCodec=h264&audioCodec=aac&audioBoost=100"
                 "&maxVideoBitrate=%d&videoQuality=100&videoResolution=%s"
                 "&directPlay=0&directStream=1&subtitles=none&fastSeek=1"
                 "&offset=%d&session=%s",
                 movie->rating_key, max_kbps,
                 max_kbps >= 8000 ? "1920x1080" : (max_kbps >= 4000 ? "1280x720" : "854x480"),
                 offset_s, g_stream.session);
    }

    memset(&g_stream.io, 0, sizeof(g_stream.io));
    g_stream.preparing = true;
    if (pthread_create(&g_stream.thread, NULL, plexstream_thread, NULL) != 0) {
        LOGE("plexstream: thread create failed");
        return false;
    }
    g_stream.reporter_active =
        g_stream.backend == MEDIA_BACKEND_JELLYFIN &&
        pthread_create(&g_stream.reporter, NULL, plexstream_reporter_thread, NULL) == 0;
    g_stream.active = true;
    LOGI("plexstream: session %s started for %s at %ds (backend %d)",
         g_stream.session, movie->rating_key, offset_s, g_stream.backend);
    return true;
}

long plexstream_bytes(void) {
    return g_stream.io.bytes;
}

bool plexstream_preparing(void) {
    return g_stream.active && g_stream.preparing;
}

long long plexstream_runtime_ms(void) {
    return g_stream.runtime_ms;
}

bool plexstream_failed(void) {
    return g_stream.io.done && g_stream.io.result != PLEX_OK;
}

bool plexstream_auth_failed(void) {
    return g_stream.io.done && g_stream.io.result == PLEX_ERR_AUTH;
}

bool plexstream_complete(void) {
    return g_stream.io.done && g_stream.io.result == PLEX_OK;
}

void plexstream_stop(void) {
    if (!g_stream.active) {
        return;
    }

    g_stream.io.cancel = true;
    pthread_join(g_stream.thread, NULL);
    if (g_stream.reporter_active) {
        pthread_join(g_stream.reporter, NULL);
        g_stream.reporter_active = false;
    }
    g_stream.active = false;

    if (g_stream.backend != MEDIA_BACKEND_JELLYFIN) {
        // Best effort: release the server's transcoder promptly (Jellyfin
        // reaps its transcode when the connection drops)
        char path[128];
        snprintf(path, sizeof(path), "/video/:/transcode/universal/stop?session=%s", g_stream.session);
        plex_server_request(path);
    }

    LOGI("plexstream: session %s stopped", g_stream.session);
}
