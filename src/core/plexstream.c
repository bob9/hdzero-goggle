#include "plexstream.h"

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

typedef struct {
    plex_stream_state_t io;
    pthread_t thread;
    bool active;
    int backend;
    char path[768];
    char session[24];
} plexstream_t;

static plexstream_t g_stream;

static void *plexstream_thread(void *arg) {
    (void)arg;
    if (g_stream.backend == MEDIA_BACKEND_JELLYFIN) {
        jf_stream_download(g_stream.path, PLEXSTREAM_FILE, &g_stream.io);
    } else {
        plex_stream_download(g_stream.path, PLEXSTREAM_FILE, &g_stream.io);
    }
    return NULL;
}

bool plexstream_begin(const plex_movie_t *movie, int offset_s, int max_kbps) {
    if (g_stream.active) {
        plexstream_stop();
    }

    mkdir("/mnt/extsd/plexcache", 0755);
    unlink(PLEXSTREAM_FILE);

    g_stream.backend = g_setting.plex.backend;
    snprintf(g_stream.session, sizeof(g_stream.session), "hdzg%08x", (unsigned)time(NULL));

    if (g_stream.backend == MEDIA_BACKEND_JELLYFIN) {
        jf_stream_path(g_stream.path, sizeof(g_stream.path), movie, offset_s, max_kbps);
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
                 "&maxVideoBitrate=%d&videoQuality=100&videoResolution=1920x1080"
                 "&directPlay=0&directStream=1&subtitles=none&fastSeek=1"
                 "&offset=%d&session=%s",
                 movie->rating_key, max_kbps, offset_s, g_stream.session);
    }

    memset(&g_stream.io, 0, sizeof(g_stream.io));
    if (pthread_create(&g_stream.thread, NULL, plexstream_thread, NULL) != 0) {
        LOGE("plexstream: thread create failed");
        return false;
    }
    g_stream.active = true;
    LOGI("plexstream: session %s started for %s at %ds (backend %d)",
         g_stream.session, movie->rating_key, offset_s, g_stream.backend);
    return true;
}

long plexstream_bytes(void) {
    return g_stream.io.bytes;
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
    g_stream.active = false;

    if (g_stream.backend != MEDIA_BACKEND_JELLYFIN) {
        // Best effort: release the server's transcoder promptly (Jellyfin
        // reaps its transcode when the connection drops)
        char path[128];
        snprintf(path, sizeof(path), "/video/:/transcode/universal/stop?session=%s", g_stream.session);
        plex_server_request(path);
    }

    unlink(PLEXSTREAM_FILE);
    LOGI("plexstream: session %s stopped", g_stream.session);
}
