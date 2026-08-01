#include "page_plex.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <log/log.h>

#include "../conf/ui.h"

#include <sys/statvfs.h>

#include "core/app_state.h"
#include "core/common.hh"
#include "core/jellyfinapi.h"
#include "core/plexapi.h"
#include "core/plexstream.h"
#include "core/settings.h"
#include "core/wifi.h"
#include "lang/language.h"
#include "player/media.h"
#include "ui/page_common.h"
#include "ui/ui_keyboard.h"
#include "ui/ui_player.h"
#include "ui/ui_style.h"
#include "util/hwlog.h"

extern media_t *media; // ui_player's active playback handle

/**
 * Poster wall layout: 5x2 portrait posters per page, sized per UI variant.
 */
#define PLEX_COLS     5
#define PLEX_ROWS     2
#define PLEX_PAGE_CNT (PLEX_COLS * PLEX_ROWS)

#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
#define POSTER_W   160
#define POSTER_H   240
#define GRID_GAP_X 32
#define GRID_GAP_Y 64
#define GRID_X0    40
#define GRID_Y0    100
#define ROW_H      44
#define HINT_Y     760
#else
#define POSTER_W   104
#define POSTER_H   156
#define GRID_GAP_X 24
#define GRID_GAP_Y 48
#define GRID_X0    30
#define GRID_Y0    80
#define ROW_H      34
#define HINT_Y     500
#endif

#define PLEX_MENU_ROWS_MAX (PLEX_SERVERS_MAX + 2)

typedef enum {
    PLEX_ST_NO_WIFI = 0,
    PLEX_ST_SETUP,     // pick a discovered server / enter one manually
    PLEX_ST_LINK,      // plex.tv/link code sign-in (keyboard/SD-file fallbacks live here too)
    PLEX_ST_LOADING,   // connect + library fetch in flight
    PLEX_ST_BROWSE,    // poster wall
    PLEX_ST_BUFFERING, // stream download building the playback head start
    PLEX_ST_ERROR,
} plex_ui_state_t;

// Server-side transcode caps, indexed by stream_quality_t (g_setting.plex.quality).
// The bitrate implies the resolution tier in both backends' stream URLs.
static const int plex_quality_kbps[] = {12000, 6000, 2500};
static const char *plex_quality_name[] = {"1080p", "720p", "480p"};
#define PLEX_QUALITY_COUNT 3

static int plex_quality(void) {
    int q = g_setting.plex.quality;
    return (q < 0 || q >= PLEX_QUALITY_COUNT) ? 0 : q;
}

// Below this sustained download rate no tier is watchable; the server's
// transcoder (or the WiFi link) is not keeping up
#define PLEX_MIN_STREAM_BPS (300 * 1024)
#define PLEX_BUFFER_START      (8 * 1024 * 1024) // head start before the player opens the file
#define PLEX_MIN_FREE_SD_BYTES (2LL * 1024 * 1024 * 1024)

typedef enum {
    PLEX_JOB_NONE = 0,
    PLEX_JOB_DISCOVER,
    PLEX_JOB_CONNECT,
    PLEX_JOB_EPISODES,
    PLEX_JOB_POSTERS,
    PLEX_JOB_PIN,
} plex_job_t;

typedef enum {
    PLEX_INPUT_NONE = 0,
    PLEX_INPUT_HOST,
    PLEX_INPUT_TOKEN,
} plex_input_t;

typedef struct {
    // Worker plumbing; every field below `lock` is guarded by it.
    pthread_t worker;
    bool worker_started;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    plex_job_t job;
    int gen; // bumped whenever pending work becomes irrelevant; stale results are dropped

    // JOB_DISCOVER results
    bool discover_done;
    plex_server_t servers[PLEX_SERVERS_MAX];
    int server_count;

    // JOB_CONNECT results
    bool connect_done;
    int connect_rc;
    char section_title[64];
    plex_movie_t *res_movies;
    int res_count;

    // JOB_EPISODES input + results
    char episodes_key[40];
    bool episodes_done;
    int episodes_rc;
    plex_movie_t *res_episodes;
    int res_episode_count;

    // JOB_POSTERS input: the worker only ever sees this private copy of the
    // visible page, so the UI can free/replace its own movie list at will.
    plex_movie_t poster_movies[PLEX_PAGE_CNT];
    int poster_count;

    // Background poster prefetch: a worker-owned copy of the whole grid
    // list, walked once whenever the visible page is fully cached, so later
    // pages open instantly instead of downloading on arrival.
    plex_movie_t *prefetch;
    int prefetch_count;
    int prefetch_pos;

    // JOB_PIN results
    bool pin_code_ready; // a fresh plex.tv/link code is in pin_code
    bool pin_linked;     // token obtained and saved by plexapi
    bool pin_failed;     // could not reach plex.tv (or code expired)
    char pin_code[12];

    volatile int progress; // movies fetched so far, for the loading label
} plex_worker_state_t;

typedef struct {
    plex_ui_state_t state;
    plex_input_t pending_input;

    // Adopted library (owned by the UI thread)
    plex_movie_t *movies;
    int movie_count;
    int cur_sel;
    char section_title[64];

    // Setup / error menu rows
    plex_server_t rows_servers[PLEX_SERVERS_MAX];
    int rows_server_count;
    int row_sel;
    int row_count;

    bool detail_open;
    bool link_failed; // last plex.tv code request/poll failed or expired
    bool playing;     // fullscreen player active; keys route to mplayer

    // Episode drill-down: the library grid is parked here while the grid
    // shows one series' episodes
    bool loading_episodes; // the in-flight LOADING state is an episode fetch
    bool in_series;
    char series_title[96];
    plex_movie_t *lib_movies;
    int lib_movie_count;
    int lib_cur_sel;
    char lib_section_title[64];

    // Buffering: download rate sampling + automatic quality step-down
    int stream_tier; // quality tier in use; starts at the setting, steps down on trouble
    long buf_last_bytes;
    uint32_t buf_last_ms;
    long buf_rate;        // bytes/sec over the last sample window
    uint32_t buf_slow_ms; // time spent below the playable-rate floor

    // Widgets
    lv_obj_t *header;
    lv_obj_t *status;
    lv_obj_t *rows[PLEX_MENU_ROWS_MAX];
    lv_obj_t *cell[PLEX_PAGE_CNT];   // frame + placeholder + focus ring
    lv_obj_t *poster[PLEX_PAGE_CNT]; // the artwork itself
    lv_obj_t *poster_label[PLEX_PAGE_CNT];
    char poster_src[PLEX_PAGE_CNT][300]; // currently applied image path per cell
    lv_obj_t *hint;
    lv_obj_t *detail;
    lv_timer_t *timer;
} plex_page_state_t;

static plex_worker_state_t g_work = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};
static plex_page_state_t g_plex;

static const lv_coord_t page_wh[] = {UI_PAGE_VIEW_SIZE};

/**
 * Active backend indirection: the saved server is either Plex or Jellyfin;
 * everything downstream (posters, sign-in, streaming) branches here.
 */
static bool backend_is_jf(void) {
    return g_setting.plex.backend == MEDIA_BACKEND_JELLYFIN;
}

static const char *active_host(void) {
    return backend_is_jf() ? g_setting.jellyfin.host : g_setting.plex.host;
}

static int active_port(void) {
    return backend_is_jf() ? g_setting.jellyfin.port : g_setting.plex.port;
}

static const char *active_token(void) {
    return backend_is_jf() ? g_setting.jellyfin.token : g_setting.plex.token;
}

static const char *active_name(void) {
    return backend_is_jf() ? "Jellyfin" : "Plex";
}

static bool active_token_from_sdcard(void) {
    return backend_is_jf() ? jf_token_from_sdcard() : plex_token_from_sdcard();
}

static bool active_poster_cached(const plex_movie_t *m, char *path, int size) {
    return backend_is_jf() ? jf_poster_cached(m, path, size) : plex_poster_cached(m, path, size);
}

static int active_fetch_poster(const plex_movie_t *m, int w, int h, char *path, int size) {
    return backend_is_jf() ? jf_fetch_poster(m, w, h, path, size) : plex_fetch_poster(m, w, h, path, size);
}

static int plex_title_cmp(const void *a, const void *b) {
    return strcasecmp(((const plex_movie_t *)a)->title, ((const plex_movie_t *)b)->title);
}

// Hand the worker its own copy of the grid list for background poster
// prefetching; replaces (and frees) any previous copy.
static void plex_prefetch_set(const plex_movie_t *list, int count) {
    plex_movie_t *copy = NULL;
    if (list && count > 0) {
        copy = malloc(sizeof(plex_movie_t) * count);
        if (copy) {
            memcpy(copy, list, sizeof(plex_movie_t) * count);
        }
    }
    pthread_mutex_lock(&g_work.lock);
    free(g_work.prefetch);
    g_work.prefetch = copy;
    g_work.prefetch_count = copy ? count : 0;
    g_work.prefetch_pos = 0;
    pthread_mutex_unlock(&g_work.lock);
}

/**
 * Worker thread: every network call lives here, never on the LVGL thread.
 */
static void *plex_worker_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_work.lock);
        while (g_work.job == PLEX_JOB_NONE) {
            pthread_cond_wait(&g_work.cond, &g_work.lock);
        }
        plex_job_t job = g_work.job;
        g_work.job = PLEX_JOB_NONE;
        int gen = g_work.gen;
        pthread_mutex_unlock(&g_work.lock);

        switch (job) {
        case PLEX_JOB_DISCOVER: {
            plex_server_t servers[PLEX_SERVERS_MAX];
            int n = plex_gdm_discover(servers, PLEX_SERVERS_MAX);
            if (n < PLEX_SERVERS_MAX) {
                n += jf_discover(servers + n, PLEX_SERVERS_MAX - n);
            }

            pthread_mutex_lock(&g_work.lock);
            if (gen == g_work.gen) {
                memcpy(g_work.servers, servers, sizeof(servers));
                g_work.server_count = n;
                g_work.discover_done = true;
            }
            pthread_mutex_unlock(&g_work.lock);
            break;
        }

        case PLEX_JOB_CONNECT: {
            char key[32] = "";
            char title[64] = "";
            plex_movie_t *movies = NULL;
            int count = 0;
            int rc;

            if (backend_is_jf()) {
                rc = jf_server_reachable(g_setting.jellyfin.host, g_setting.jellyfin.port)
                         ? PLEX_OK
                         : PLEX_ERR_NET;
                if (rc == PLEX_OK) {
                    if (!g_setting.jellyfin.token[0]) {
                        rc = PLEX_ERR_AUTH; // no token yet: go to Quick Connect
                    } else {
                        snprintf(title, sizeof(title), "Library");
                        rc = jf_load_movies(&movies, &count, &g_work.progress);
                    }
                }
            } else {
                rc = plex_server_reachable(g_setting.plex.host, g_setting.plex.port)
                         ? PLEX_OK
                         : PLEX_ERR_NET;
                if (rc == PLEX_OK) {
                    // Movies and TV shows live in separate sections; load
                    // whichever exist and interleave them alphabetically
                    plex_movie_t *shows = NULL;
                    int show_count = 0;
                    char skey[32], stitle[64] = "";
                    int mrc = plex_find_section("movie", key, sizeof(key), title, sizeof(title));
                    if (mrc == PLEX_OK) {
                        mrc = plex_load_movies(key, &movies, &count, &g_work.progress);
                    }
                    int trc = plex_find_section("show", skey, sizeof(skey), stitle, sizeof(stitle));
                    if (trc == PLEX_OK) {
                        trc = plex_load_shows(skey, &shows, &show_count, &g_work.progress);
                    }

                    if (mrc == PLEX_ERR_AUTH || trc == PLEX_ERR_AUTH) {
                        rc = PLEX_ERR_AUTH;
                        free(movies);
                        free(shows);
                        movies = NULL;
                        count = 0;
                    } else if (mrc != PLEX_OK && trc != PLEX_OK) {
                        rc = (mrc == PLEX_ERR_NOMOVIE && trc == PLEX_ERR_NOMOVIE)
                                 ? PLEX_ERR_NOMOVIE
                                 : PLEX_ERR_NET;
                    } else {
                        if (show_count > 0) {
                            plex_movie_t *merged = realloc(movies, sizeof(plex_movie_t) * (count + show_count));
                            if (merged) {
                                memcpy(merged + count, shows, sizeof(plex_movie_t) * show_count);
                                movies = merged;
                                count += show_count;
                                qsort(movies, count, sizeof(plex_movie_t), plex_title_cmp);
                            }
                        }
                        free(shows);
                        if (mrc == PLEX_OK && trc == PLEX_OK) {
                            snprintf(title, sizeof(title), "%s", _lang("Movies & TV"));
                        } else if (trc == PLEX_OK) {
                            snprintf(title, sizeof(title), "%s", stitle);
                        }
                    }
                }
            }

            pthread_mutex_lock(&g_work.lock);
            if (gen == g_work.gen) {
                g_work.connect_rc = rc;
                snprintf(g_work.section_title, sizeof(g_work.section_title), "%s", title);
                g_work.res_movies = movies;
                g_work.res_count = count;
                g_work.connect_done = true;
            } else {
                free(movies);
            }
            pthread_mutex_unlock(&g_work.lock);
            break;
        }

        case PLEX_JOB_EPISODES: {
            char skey[40];
            pthread_mutex_lock(&g_work.lock);
            snprintf(skey, sizeof(skey), "%s", g_work.episodes_key);
            pthread_mutex_unlock(&g_work.lock);

            plex_movie_t *episodes = NULL;
            int ep_count = 0;
            int rc = backend_is_jf()
                         ? jf_load_episodes(skey, &episodes, &ep_count, &g_work.progress)
                         : plex_load_episodes(skey, &episodes, &ep_count, &g_work.progress);

            pthread_mutex_lock(&g_work.lock);
            if (gen == g_work.gen) {
                g_work.episodes_rc = rc;
                g_work.res_episodes = episodes;
                g_work.res_episode_count = ep_count;
                g_work.episodes_done = true;
            } else {
                free(episodes);
            }
            pthread_mutex_unlock(&g_work.lock);
            break;
        }

        case PLEX_JOB_POSTERS: {
            // Download one missing poster at a time, re-reading the wanted
            // page between downloads so scrolling redirects the effort.
            for (;;) {
                plex_movie_t wanted[PLEX_PAGE_CNT];
                int count;

                pthread_mutex_lock(&g_work.lock);
                bool stale = (gen != g_work.gen) || (g_work.job != PLEX_JOB_NONE);
                memcpy(wanted, g_work.poster_movies, sizeof(wanted));
                count = g_work.poster_count;
                pthread_mutex_unlock(&g_work.lock);
                if (stale) {
                    break;
                }

                bool fetched = false;
                for (int i = 0; i < count; i++) {
                    char path[300];
                    if (!wanted[i].thumb[0] || active_poster_cached(&wanted[i], path, sizeof(path))) {
                        continue;
                    }
                    active_fetch_poster(&wanted[i], POSTER_W, POSTER_H, path, sizeof(path));
                    fetched = true;
                    break;
                }
                if (!fetched) {
                    // Visible page fully cached: advance the background
                    // prefetch by one poster (single pass, cursor persists)
                    plex_movie_t item;
                    bool have = false;
                    pthread_mutex_lock(&g_work.lock);
                    if (gen == g_work.gen && g_work.job == PLEX_JOB_NONE &&
                        g_work.prefetch && g_work.prefetch_pos < g_work.prefetch_count) {
                        item = g_work.prefetch[g_work.prefetch_pos++];
                        have = true;
                    }
                    pthread_mutex_unlock(&g_work.lock);
                    if (!have) {
                        break; // everything cached (or nothing fetchable)
                    }
                    char path[300];
                    if (item.thumb[0] && !active_poster_cached(&item, path, sizeof(path))) {
                        active_fetch_poster(&item, POSTER_W, POSTER_H, path, sizeof(path));
                    }
                }
            }
            break;
        }

        case PLEX_JOB_PIN: {
            char code[12] = "";
            char secret[128] = "";
            int pin_id = 0;
            bool jf = backend_is_jf();
            int rc = jf ? jf_quickconnect_start(code, sizeof(code), secret, sizeof(secret))
                        : plex_pin_start(&pin_id, code, sizeof(code));

            pthread_mutex_lock(&g_work.lock);
            if (gen != g_work.gen) {
                pthread_mutex_unlock(&g_work.lock);
                break;
            }
            if (rc == PLEX_OK) {
                snprintf(g_work.pin_code, sizeof(g_work.pin_code), "%s", code);
                g_work.pin_code_ready = true;
            } else {
                g_work.pin_failed = true;
            }
            pthread_mutex_unlock(&g_work.lock);
            if (rc != PLEX_OK) {
                break;
            }

            // Poll until linked, superseded, or the code's 15 minute life ends
            for (int elapsed_s = 0; elapsed_s < 900; elapsed_s += 3) {
                for (int i = 0; i < 12; i++) { // 3s in responsive chunks
                    usleep(250 * 1000);
                    pthread_mutex_lock(&g_work.lock);
                    bool stale = (gen != g_work.gen) || (g_work.job != PLEX_JOB_NONE);
                    pthread_mutex_unlock(&g_work.lock);
                    if (stale) {
                        goto pin_done;
                    }
                }

                rc = jf ? jf_quickconnect_poll(secret) : plex_pin_poll(pin_id);
                if (rc == PLEX_PENDING || rc == PLEX_ERR_NET) {
                    continue; // transient network blips keep polling
                }

                pthread_mutex_lock(&g_work.lock);
                if (gen == g_work.gen) {
                    if (rc == PLEX_OK) {
                        g_work.pin_linked = true;
                    } else {
                        g_work.pin_failed = true;
                    }
                }
                pthread_mutex_unlock(&g_work.lock);
                goto pin_done;
            }

            // Code expired without being linked
            pthread_mutex_lock(&g_work.lock);
            if (gen == g_work.gen) {
                g_work.pin_failed = true;
            }
            pthread_mutex_unlock(&g_work.lock);
        pin_done:
            break;
        }

        default:
            break;
        }
    }
    return NULL;
}

static void plex_post_job(plex_job_t job) {
    pthread_mutex_lock(&g_work.lock);
    if (!g_work.worker_started) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&g_work.worker, &attr, plex_worker_thread, NULL) == 0) {
            g_work.worker_started = true;
        }
        pthread_attr_destroy(&attr);
    }
    g_work.job = job;
    pthread_cond_signal(&g_work.cond);
    pthread_mutex_unlock(&g_work.lock);
}

static void plex_invalidate_pending_work(void) {
    pthread_mutex_lock(&g_work.lock);
    g_work.gen++;
    g_work.job = PLEX_JOB_NONE;
    g_work.discover_done = false;
    g_work.connect_done = false;
    g_work.pin_code_ready = false;
    g_work.pin_linked = false;
    g_work.pin_failed = false;
    free(g_work.res_movies);
    g_work.res_movies = NULL;
    g_work.res_count = 0;
    pthread_mutex_unlock(&g_work.lock);
}

/**
 * UI helpers
 */
static void plex_rows_hide(void) {
    for (int i = 0; i < PLEX_MENU_ROWS_MAX; i++) {
        lv_obj_add_flag(g_plex.rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    g_plex.row_count = 0;
    g_plex.row_sel = 0;
}

static void plex_grid_hide(void) {
    for (int i = 0; i < PLEX_PAGE_CNT; i++) {
        lv_obj_add_flag(g_plex.cell[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_plex.poster[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_plex.poster_label[i], LV_OBJ_FLAG_HIDDEN);
        g_plex.poster_src[i][0] = '\0';
    }
}

static void plex_detail_close(void) {
    lv_obj_add_flag(g_plex.detail, LV_OBJ_FLAG_HIDDEN);
    g_plex.detail_open = false;
}

static void plex_rows_paint(void) {
    for (int i = 0; i < g_plex.row_count; i++) {
        lv_obj_set_style_text_color(g_plex.rows[i],
                                    lv_color_hex(i == g_plex.row_sel ? 0xFFFF00 : TEXT_COLOR_DEFAULT), 0);
    }
}

static void plex_row_set(int index, const char *text) {
    lv_label_set_text(g_plex.rows[index], text);
    lv_obj_clear_flag(g_plex.rows[index], LV_OBJ_FLAG_HIDDEN);
}

static void plex_show_status(const char *text) {
    lv_label_set_text(g_plex.status, text);
    lv_obj_clear_flag(g_plex.status, LV_OBJ_FLAG_HIDDEN);
}

static void plex_set_header(const char *text) {
    lv_label_set_text(g_plex.header, text);
}

static void plex_set_hint(const char *text) {
    lv_label_set_text(g_plex.hint, text);
}

/**
 * State transitions. Each builder resets the shared widgets for its state.
 */
static void plex_enter_state_common(void) {
    plex_rows_hide();
    plex_grid_hide();
    plex_detail_close();
    lv_obj_add_flag(g_plex.status, LV_OBJ_FLAG_HIDDEN);
    g_plex.pending_input = PLEX_INPUT_NONE;
    if (keyboard_active()) {
        keyboard_close();
    }
}

static void plex_state_no_wifi(void) {
    plex_enter_state_common();
    g_plex.state = PLEX_ST_NO_WIFI;
    plex_set_header("Plex");
    plex_show_status(_lang("WiFi is not connected.\nJoin your home network from the WiFi menu first."));
    plex_set_hint(_lang("Long press the Enter button to exit"));
}

static void plex_state_setup(bool trigger_scan) {
    plex_enter_state_common();
    g_plex.state = PLEX_ST_SETUP;
    plex_set_header(_lang("Plex - Select Server"));
    plex_set_hint(_lang("Scroll to highlight, click to select. Long press the Enter button to exit"));

    char buf[128];
    int row = 0;
    for (int i = 0; i < g_plex.rows_server_count && row < PLEX_SERVERS_MAX; i++, row++) {
        snprintf(buf, sizeof(buf), "[%s] %s (%s:%d)",
                 g_plex.rows_servers[i].backend == MEDIA_BACKEND_JELLYFIN ? "Jellyfin" : "Plex",
                 g_plex.rows_servers[i].name, g_plex.rows_servers[i].host, g_plex.rows_servers[i].port);
        plex_row_set(row, buf);
    }
    snprintf(buf, sizeof(buf), "> %s", _lang(trigger_scan ? "Scanning..." : "Scan again"));
    plex_row_set(row++, buf);
    snprintf(buf, sizeof(buf), "> %s", _lang("Enter server address manually"));
    plex_row_set(row++, buf);
    g_plex.row_count = row;
    g_plex.row_sel = 0;
    plex_rows_paint();

    if (trigger_scan) {
        plex_post_job(PLEX_JOB_DISCOVER);
    }
}

static void plex_state_loading(void) {
    plex_invalidate_pending_work(); // a fresh connect obsoletes older results
    plex_enter_state_common();
    g_plex.state = PLEX_ST_LOADING;
    g_plex.loading_episodes = false;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s - %s %s:%d", active_name(), _lang("Connecting to"), active_host(), active_port());
    plex_set_header(buf);
    plex_show_status(_lang("Connecting..."));
    plex_set_hint(_lang("Long press the Enter button to exit"));
    g_work.progress = 0;
    plex_post_job(PLEX_JOB_CONNECT);
}

static void plex_state_browse(void);

// Drill into a TV series: fetch its episode list, then swap the grid to it
static void plex_state_load_episodes(const plex_movie_t *m) {
    plex_invalidate_pending_work();
    pthread_mutex_lock(&g_work.lock);
    snprintf(g_work.episodes_key, sizeof(g_work.episodes_key), "%s", m->rating_key);
    pthread_mutex_unlock(&g_work.lock);
    snprintf(g_plex.series_title, sizeof(g_plex.series_title), "%s", m->title);

    plex_enter_state_common();
    g_plex.state = PLEX_ST_LOADING;
    g_plex.loading_episodes = true;
    char buf[160];
    snprintf(buf, sizeof(buf), "%s - %s", active_name(), m->title);
    plex_set_header(buf);
    plex_show_status(_lang("Loading episodes..."));
    plex_set_hint(_lang("Long press the Enter button to exit"));
    g_work.progress = 0;
    plex_post_job(PLEX_JOB_EPISODES);
}

// Leave the episode grid, restoring the parked library grid
static void plex_series_back(void) {
    if (!g_plex.in_series) {
        return;
    }
    free(g_plex.movies);
    g_plex.movies = g_plex.lib_movies;
    g_plex.movie_count = g_plex.lib_movie_count;
    g_plex.cur_sel = g_plex.lib_cur_sel;
    snprintf(g_plex.section_title, sizeof(g_plex.section_title), "%s", g_plex.lib_section_title);
    g_plex.lib_movies = NULL;
    g_plex.lib_movie_count = 0;
    g_plex.in_series = false;
    plex_prefetch_set(g_plex.movies, g_plex.movie_count);
    plex_state_browse();
}

static void plex_state_loading(void);

static void plex_state_link(void) {
    // The zero-interaction path first: a token file on the SD card signs
    // in without showing anything.
    if (active_token_from_sdcard()) {
        plex_state_loading();
        return;
    }

    plex_enter_state_common();
    g_plex.state = PLEX_ST_LINK;
    g_plex.link_failed = false;
    char buf[96];
    snprintf(buf, sizeof(buf), "%s - %s", active_name(), _lang("Sign In"));
    plex_set_header(buf);
    plex_show_status(backend_is_jf()
                         ? _lang("Requesting a Quick Connect code from the server...")
                         : _lang("Requesting a sign-in code from plex.tv..."));
    plex_set_hint(_lang("Click the dial to type a token instead. Long press the Enter button to exit"));
    plex_post_job(PLEX_JOB_PIN);
}

static void plex_link_show_code(const char *code) {
    char buf[512];
    if (backend_is_jf()) {
        snprintf(buf, sizeof(buf),
                 "%s\n\n      %s\n\n%s",
                 _lang("In any signed-in Jellyfin app or web page:\nclick your user icon -> Quick Connect,\nand enter this code:"),
                 code,
                 _lang("This screen continues automatically once approved.\n(Alternatives: put jellyfintoken.txt on the SD card,\nor click the dial to type an API key.)"));
    } else {
        snprintf(buf, sizeof(buf),
                 "%s\n\n      plex.tv/link\n\n%s\n\n      %s\n\n%s",
                 _lang("On your phone or computer, go to:"),
                 _lang("sign in to your Plex account, and enter this code:"),
                 code,
                 _lang("This screen continues automatically once linked.\n(Alternatives: put plextoken.txt on the SD card,\nor click the dial to type a token.)"));
    }
    plex_show_status(buf);
}

static void plex_state_error(const char *msg) {
    plex_enter_state_common();
    g_plex.state = PLEX_ST_ERROR;
    plex_set_header("Plex");
    plex_show_status(msg);
    char buf[96];
    snprintf(buf, sizeof(buf), "> %s", _lang("Retry"));
    plex_row_set(0, buf);
    snprintf(buf, sizeof(buf), "> %s", _lang("Choose a different server"));
    plex_row_set(1, buf);
    g_plex.row_count = 2;
    g_plex.row_sel = 0;
    plex_rows_paint();
    plex_set_hint(_lang("Long press the Enter button to exit"));
}

static void plex_apply_poster(int i, const plex_movie_t *m) {
    char path[280];
    if (active_poster_cached(m, path, sizeof(path))) {
        char src[300];
        snprintf(src, sizeof(src), "A:%s", path);
        if (strcmp(src, g_plex.poster_src[i]) != 0) {
            lv_img_set_src(g_plex.poster[i], src);
            snprintf(g_plex.poster_src[i], sizeof(g_plex.poster_src[i]), "%s", src);
        }
        lv_obj_clear_flag(g_plex.poster[i], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_plex.poster[i], LV_OBJ_FLAG_HIDDEN);
        g_plex.poster_src[i][0] = '\0';
    }
}

static void plex_grid_paint(void) {
    char buf[192];
    int page = g_plex.cur_sel / PLEX_PAGE_CNT;
    int pages = (g_plex.movie_count + PLEX_PAGE_CNT - 1) / PLEX_PAGE_CNT;

    snprintf(buf, sizeof(buf), "%s - %s  |  %d %s  |  %s %d/%d",
             active_name(), g_plex.section_title, g_plex.movie_count,
             _lang(g_plex.in_series ? "episodes" : "titles"), _lang("page"), page + 1, pages);
    plex_set_header(buf);

    for (int i = 0; i < PLEX_PAGE_CNT; i++) {
        int seq = page * PLEX_PAGE_CNT + i;
        if (seq >= g_plex.movie_count) {
            lv_obj_add_flag(g_plex.cell[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_plex.poster[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_plex.poster_label[i], LV_OBJ_FLAG_HIDDEN);
            g_plex.poster_src[i][0] = '\0';
            continue;
        }

        plex_movie_t *m = &g_plex.movies[seq];
        if (m->kind == PLEX_ITEM_EPISODE && m->season > 0) {
            snprintf(buf, sizeof(buf), "S%dE%d %s", m->season, m->episode, m->title);
        } else if (m->year > 0) {
            snprintf(buf, sizeof(buf), "%s (%d)", m->title, m->year);
        } else {
            snprintf(buf, sizeof(buf), "%s", m->title);
        }
        lv_label_set_text(g_plex.poster_label[i], buf);
        lv_obj_clear_flag(g_plex.poster_label[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_plex.cell[i], LV_OBJ_FLAG_HIDDEN);

        plex_apply_poster(i, m);

        bool focused = (seq == g_plex.cur_sel);
        lv_obj_remove_style(g_plex.cell[i], focused ? &style_pb_dark : &style_pb, LV_PART_MAIN);
        lv_obj_add_style(g_plex.cell[i], focused ? &style_pb : &style_pb_dark, LV_PART_MAIN);
        lv_obj_set_style_text_color(g_plex.poster_label[i],
                                    lv_color_hex(focused ? 0xFFFF00 : TEXT_COLOR_DEFAULT), 0);
    }

    // Hand the worker its own copy of the visible page to download
    pthread_mutex_lock(&g_work.lock);
    g_work.poster_count = 0;
    for (int i = 0; i < PLEX_PAGE_CNT; i++) {
        int seq = page * PLEX_PAGE_CNT + i;
        if (seq < g_plex.movie_count) {
            g_work.poster_movies[g_work.poster_count++] = g_plex.movies[seq];
        }
    }
    pthread_mutex_unlock(&g_work.lock);
    plex_post_job(PLEX_JOB_POSTERS);
}

static void plex_state_browse(void) {
    plex_enter_state_common();
    g_plex.state = PLEX_ST_BROWSE;
    plex_set_hint(_lang(g_plex.in_series
                            ? "Click for details. Func button: back to the library. Long press the Enter button to exit"
                            : "Click for details. Func button: Click to refresh, Hold to forget server. Long press the Enter button to exit"));
    if (g_plex.cur_sel >= g_plex.movie_count) {
        g_plex.cur_sel = 0;
    }
    plex_grid_paint();
}

static void plex_detail_open(void) {
    if (!g_plex.movie_count) {
        return;
    }
    plex_movie_t *m = &g_plex.movies[g_plex.cur_sel];
    char title[128], text[256];
    if (m->kind == PLEX_ITEM_EPISODE && m->season > 0) {
        snprintf(title, sizeof(title), "S%dE%d - %s", m->season, m->episode, m->title);
    } else if (m->year > 0) {
        snprintf(title, sizeof(title), "%s (%d)", m->title, m->year);
    } else {
        snprintf(title, sizeof(title), "%s", m->title);
    }
    if (m->kind == PLEX_ITEM_SERIES) {
        snprintf(text, sizeof(text), "%s\n%s\n\n%s",
                 _lang("TV series"),
                 m->watched ? _lang("Watched") : _lang("Unwatched"),
                 _lang("Click the dial to see the episodes.\nFunc button to go back."));
    } else {
        snprintf(text, sizeof(text), "%s: %dh %02dm\n%s\n%s: %s\n\n%s",
                 _lang("Duration"), m->duration_min / 60, m->duration_min % 60,
                 m->watched ? _lang("Watched") : _lang("Unwatched"),
                 _lang("Quality"), plex_quality_name[plex_quality()],
                 _lang("Click the dial to play. Scroll to change quality.\nFunc button to go back."));
    }
    lv_label_set_text(lv_msgbox_get_title(g_plex.detail), title);
    lv_label_set_text(lv_msgbox_get_text(g_plex.detail), text);
    lv_obj_clear_flag(g_plex.detail, LV_OBJ_FLAG_HIDDEN);
    g_plex.detail_open = true;
}

/**
 * Playback: server transcodes/remuxes to MPEG-TS, the download thread grows
 * a file on the SD card, and the existing TS player reads it.
 */
static bool plex_sd_has_space(void) {
    struct statvfs vfs;
    if (statvfs("/mnt/extsd", &vfs) != 0) {
        return false;
    }
    return (long long)vfs.f_bavail * vfs.f_bsize >= PLEX_MIN_FREE_SD_BYTES;
}

static void plex_start_playback(void) {
    if (!g_plex.movie_count) {
        return;
    }
    plex_movie_t *m = &g_plex.movies[g_plex.cur_sel];
    if (m->kind == PLEX_ITEM_SERIES) {
        return; // series items open an episode list, they don't play
    }

    plex_detail_close();
    if (!plex_sd_has_space()) {
        plex_show_status(_lang("Playback needs an SD card with at least 2 GB free.\nStreaming buffers the movie onto the card while it plays."));
        return;
    }
    g_plex.stream_tier = plex_quality();
    if (!plexstream_begin(m, 0, plex_quality_kbps[g_plex.stream_tier])) {
        plex_show_status(_lang("Could not start the stream."));
        return;
    }
    g_plex.buf_last_bytes = 0;
    g_plex.buf_last_ms = lv_tick_get();
    g_plex.buf_rate = -1;
    g_plex.buf_slow_ms = 0;

    plex_enter_state_common();
    g_plex.state = PLEX_ST_BUFFERING;
    char buf[160];
    snprintf(buf, sizeof(buf), "%s - %s %s", active_name(), _lang("Preparing"), m->title);
    plex_set_header(buf);
    plex_show_status(_lang("Asking the server for the stream..."));
    plex_set_hint(_lang("Click to cancel"));
}

// Restart the buffering stream one quality tier lower (the caller has
// already stopped the old one). Returns false if the restart failed.
static bool plex_stream_step_down(const char *reason) {
    g_plex.stream_tier++;
    if (!plexstream_begin(&g_plex.movies[g_plex.cur_sel], 0, plex_quality_kbps[g_plex.stream_tier])) {
        return false;
    }
    g_plex.buf_last_bytes = 0;
    g_plex.buf_last_ms = lv_tick_get();
    g_plex.buf_rate = -1;
    g_plex.buf_slow_ms = 0;
    char buf[192];
    snprintf(buf, sizeof(buf), "%s\n%s %s...", reason, _lang("Retrying at"), plex_quality_name[g_plex.stream_tier]);
    plex_show_status(buf);
    return true;
}

static void plex_launch_player(void) {
    plex_movie_t *m = &g_plex.movies[g_plex.cur_sel];
    // Breadcrumb straight to the SD card (hwlog fsyncs each line): the
    // app's normal log does not survive the hard freezes seen in the
    // field, so mark what the player is about to swallow
    hwlog("plex: launch item=%s bytes=%ld tier=%d",
          m->rating_key, plexstream_bytes(), g_plex.stream_tier);
    g_plex.playing = true;
    app_state_push(APP_STATE_PLAYBACK);
    mplayer_file(PLEXSTREAM_FILE);
    // The growing TS misreports its length; show the movie's real runtime
    media_override_duration(media, (uint32_t)m->duration_min * 60000u);
}

static void plex_end_playback(void) {
    g_plex.playing = false;
    plexstream_stop();
    app_state_push(APP_STATE_SUBMENU);
    plex_state_browse();
}

/**
 * Poll timer: applies worker results and fills in posters as they land.
 */
static void plex_timer_cb(lv_timer_t *timer) {
    (void)timer;

    // The player owns the page while a movie runs. Without this guard the
    // BUFFERING branch below kept re-launching the player every tick on
    // top of the live decode - double media_init wedged the whole device
    // (seen in field hwlogs as three launches in 1.5 s, then a freeze).
    if (g_plex.playing) {
        return;
    }

    bool discover_done = false, connect_done = false;
    int connect_rc = PLEX_OK;
    plex_movie_t *movies = NULL;
    int count = 0;
    char section_title[64] = "";

    bool episodes_done = false;
    int episodes_rc = PLEX_OK;
    plex_movie_t *episodes = NULL;
    int episode_count = 0;

    bool pin_code_ready = false, pin_linked = false, pin_failed = false;
    char pin_code[12] = "";

    pthread_mutex_lock(&g_work.lock);
    if (g_work.discover_done) {
        discover_done = true;
        g_work.discover_done = false;
        memcpy(g_plex.rows_servers, g_work.servers, sizeof(g_work.servers));
        g_plex.rows_server_count = g_work.server_count;
    }
    if (g_work.pin_code_ready) {
        pin_code_ready = true;
        g_work.pin_code_ready = false;
        snprintf(pin_code, sizeof(pin_code), "%s", g_work.pin_code);
    }
    if (g_work.pin_linked) {
        pin_linked = true;
        g_work.pin_linked = false;
    }
    if (g_work.pin_failed) {
        pin_failed = true;
        g_work.pin_failed = false;
    }
    if (g_work.connect_done) {
        connect_done = true;
        g_work.connect_done = false;
        connect_rc = g_work.connect_rc;
        movies = g_work.res_movies;
        count = g_work.res_count;
        snprintf(section_title, sizeof(section_title), "%s", g_work.section_title);
        g_work.res_movies = NULL;
        g_work.res_count = 0;
    }
    if (g_work.episodes_done) {
        episodes_done = true;
        g_work.episodes_done = false;
        episodes_rc = g_work.episodes_rc;
        episodes = g_work.res_episodes;
        episode_count = g_work.res_episode_count;
        g_work.res_episodes = NULL;
        g_work.res_episode_count = 0;
    }
    pthread_mutex_unlock(&g_work.lock);

    if (discover_done && g_plex.state == PLEX_ST_SETUP) {
        plex_state_setup(false);
        if (g_plex.rows_server_count == 0) {
            plex_show_status(_lang("No Plex server found on this network.\nEnter the server address manually."));
        }
    }

    if (connect_done) {
        switch (connect_rc) {
        case PLEX_OK:
            if (count == 0) {
                free(movies);
                plex_state_error(_lang("The movie library on this server is empty."));
            } else {
                free(g_plex.movies);
                if (g_plex.in_series) { // fresh library replaces any parked grid
                    free(g_plex.lib_movies);
                    g_plex.lib_movies = NULL;
                    g_plex.lib_movie_count = 0;
                    g_plex.in_series = false;
                }
                g_plex.movies = movies;
                g_plex.movie_count = count;
                g_plex.cur_sel = 0;
                snprintf(g_plex.section_title, sizeof(g_plex.section_title), "%s", section_title);
                plex_prefetch_set(g_plex.movies, g_plex.movie_count);
                plex_state_browse();
            }
            break;
        case PLEX_ERR_AUTH:
            free(movies);
            plex_state_link();
            break;
        case PLEX_ERR_NOMOVIE:
            free(movies);
            plex_state_error(_lang("No movie or TV library was found on this server."));
            break;
        default:
            free(movies);
            plex_state_error(_lang("Could not reach the Plex server.\nCheck that it is running and on the same network."));
            break;
        }
        return;
    }

    if (episodes_done) {
        if (episodes_rc == PLEX_OK && episode_count > 0) {
            if (!g_plex.in_series) {
                g_plex.lib_movies = g_plex.movies;
                g_plex.lib_movie_count = g_plex.movie_count;
                g_plex.lib_cur_sel = g_plex.cur_sel;
                snprintf(g_plex.lib_section_title, sizeof(g_plex.lib_section_title), "%s", g_plex.section_title);
            } else {
                free(g_plex.movies); // series-to-series switch keeps the parked library
            }
            g_plex.in_series = true;
            g_plex.movies = episodes;
            g_plex.movie_count = episode_count;
            g_plex.cur_sel = 0;
            snprintf(g_plex.section_title, sizeof(g_plex.section_title), "%s", g_plex.series_title);
            plex_prefetch_set(g_plex.movies, g_plex.movie_count);
            plex_state_browse();
        } else {
            free(episodes);
            if (episodes_rc == PLEX_ERR_AUTH) {
                plex_state_link();
            } else {
                plex_state_error(_lang("Could not load the episodes for this series."));
            }
        }
        return;
    }

    if (g_plex.state == PLEX_ST_LINK) {
        // plex.tv/link progress
        if (pin_linked) {
            plex_state_loading();
            return;
        }
        if (pin_code_ready && !keyboard_active()) {
            plex_link_show_code(pin_code);
        }
        if (pin_failed) {
            g_plex.link_failed = true;
            if (!keyboard_active()) {
                plex_show_status(backend_is_jf()
                                     ? _lang("Could not get a Quick Connect code.\nEnable Quick Connect in the Jellyfin dashboard, or:\n- put jellyfintoken.txt (API key) on the SD card\n- click the dial to type an API key\n- scroll to request a new code")
                                     : _lang("Could not sign in via plex.tv (no internet, or the code expired).\n\nAlternatives that work offline:\n- put plextoken.txt on the SD card (picked up automatically)\n- click the dial to type a token\n- scroll to request a new plex.tv code"));
            }
        }
        // Sign in the moment an SD card carrying a token file shows up
        if (!keyboard_active() && active_token_from_sdcard()) {
            plex_state_loading();
            return;
        }
    }

    if (g_plex.state == PLEX_ST_LOADING) {
        int progress = g_work.progress;
        if (progress > 0) {
            char buf[96];
            snprintf(buf, sizeof(buf), "%s %d %s...", _lang("Loading library"), progress,
                     _lang(g_plex.loading_episodes ? "episodes" : "titles"));
            plex_show_status(buf);
        }
    }

    if (g_plex.state == PLEX_ST_BUFFERING) {
        if (plexstream_auth_failed()) {
            plexstream_stop();
            plex_state_link();
            return;
        }
        if (plexstream_failed()) {
            plexstream_stop();
            // Step a quality tier down: a server whose transcoder can't
            // keep up (or crashed) often manages the lighter encode.
            if (g_plex.movie_count && g_plex.stream_tier < PLEX_QUALITY_COUNT - 1 &&
                plex_stream_step_down(_lang("The server is struggling with this title."))) {
                return;
            }
            plex_state_error(_lang("The server could not stream this movie.\nCheck that the server can transcode it (a 4K/HEVC movie\nneeds hardware transcoding enabled on the server)."));
            return;
        }
        long bytes = plexstream_bytes();
        if (bytes >= PLEX_BUFFER_START || (bytes > 0 && plexstream_complete())) {
            plex_launch_player();
            return;
        }

        // Sample the download rate every ~2 s so a struggling server is
        // visible instead of a silently frozen byte count
        uint32_t now = lv_tick_get();
        if (now - g_plex.buf_last_ms >= 2000) {
            long dt = (long)(now - g_plex.buf_last_ms);
            g_plex.buf_rate = (bytes - g_plex.buf_last_bytes) * 1000 / dt;
            g_plex.buf_last_bytes = bytes;
            g_plex.buf_last_ms = now;
            g_plex.buf_slow_ms = (g_plex.buf_rate < PLEX_MIN_STREAM_BPS) ? g_plex.buf_slow_ms + dt : 0;

            // A server stuck below a watchable rate won't improve by
            // waiting: drop a tier instead of buffering forever
            if (g_plex.buf_slow_ms >= 20000 && g_plex.movie_count &&
                g_plex.stream_tier < PLEX_QUALITY_COUNT - 1) {
                plexstream_stop();
                if (plex_stream_step_down(_lang("The server is converting too slowly."))) {
                    return;
                }
            }
        }
        char buf[160];
        if (g_plex.buf_rate < 0) {
            snprintf(buf, sizeof(buf), "%s %ld MB...", _lang("Buffering"), bytes / (1024 * 1024));
        } else if (g_plex.buf_rate < PLEX_MIN_STREAM_BPS) {
            snprintf(buf, sizeof(buf), "%s %ld MB (%ld KB/s)\n%s", _lang("Buffering"),
                     bytes / (1024 * 1024), g_plex.buf_rate / 1024,
                     _lang("The server is converting slower than the video plays.\nHardware transcoding on the server fixes this."));
        } else {
            snprintf(buf, sizeof(buf), "%s %ld MB (%.1f MB/s)...", _lang("Buffering"),
                     bytes / (1024 * 1024), (double)g_plex.buf_rate / (1024 * 1024));
        }
        plex_show_status(buf);
    }

    if (g_plex.state == PLEX_ST_BROWSE) {
        // Fill any visible cells whose posters have arrived since last paint
        int page = g_plex.cur_sel / PLEX_PAGE_CNT;
        for (int i = 0; i < PLEX_PAGE_CNT; i++) {
            int seq = page * PLEX_PAGE_CNT + i;
            if (seq >= g_plex.movie_count || g_plex.poster_src[i][0]) {
                continue;
            }
            plex_apply_poster(i, &g_plex.movies[seq]);
        }
    }
}

/**
 * Input handling
 */
static void plex_save_server(const char *host, int port, int backend) {
    g_setting.plex.backend = backend;
    if (backend == MEDIA_BACKEND_JELLYFIN) {
        snprintf(g_setting.jellyfin.host, sizeof(g_setting.jellyfin.host), "%s", host);
        g_setting.jellyfin.port = port > 0 ? port : 8096;
        jf_settings_save();
    } else {
        snprintf(g_setting.plex.host, sizeof(g_setting.plex.host), "%s", host);
        g_setting.plex.port = port > 0 ? port : 32400;
    }
    plex_settings_save(); // persists the backend choice either way
}

static void plex_commit_keyboard_text(void) {
    char buf[128] = "";
    keyboard_get_text(buf, sizeof(buf));
    keyboard_close();

    plex_input_t input = g_plex.pending_input;
    g_plex.pending_input = PLEX_INPUT_NONE;

    switch (input) {
    case PLEX_INPUT_HOST: {
        if (!buf[0]) {
            plex_state_setup(false);
            return;
        }
        char *colon = strrchr(buf, ':');
        int port = 0;
        if (colon) {
            *colon = '\0';
            port = atoi(colon + 1);
        }
        // Jellyfin's default port marks the server kind; anything else is Plex
        plex_save_server(buf, port, port == 8096 ? MEDIA_BACKEND_JELLYFIN : MEDIA_BACKEND_PLEX);
        plex_state_loading();
        break;
    }
    case PLEX_INPUT_TOKEN:
        if (!buf[0]) {
            plex_state_link();
            return;
        }
        if (backend_is_jf()) {
            snprintf(g_setting.jellyfin.token, sizeof(g_setting.jellyfin.token), "%s", buf);
            g_setting.jellyfin.user_id[0] = '\0';
            jf_settings_save();
        } else {
            snprintf(g_setting.plex.token, sizeof(g_setting.plex.token), "%s", buf);
            plex_settings_save();
        }
        plex_state_loading();
        break;
    default:
        break;
    }
}

static void plex_setup_row_activate(void) {
    if (g_plex.row_sel < g_plex.rows_server_count) {
        plex_server_t *srv = &g_plex.rows_servers[g_plex.row_sel];
        plex_save_server(srv->host, srv->port, srv->backend);
        plex_state_loading();
    } else if (g_plex.row_sel == g_plex.rows_server_count) {
        plex_state_setup(true); // rescan
    } else {
        g_plex.pending_input = PLEX_INPUT_HOST;
        plex_show_status(_lang("Type the server IP address (optionally :port),\nthen click the Func button to submit.\nUse :8096 for a Jellyfin server (:32400 or none = Plex)."));
        keyboard_set_text(active_host());
        keyboard_open();
    }
}

static void plex_key(uint8_t key) {
    if (g_plex.playing) {
        if (mplayer_on_key(key)) {
            plex_end_playback();
        }
        return;
    }

    if (g_plex.state == PLEX_ST_BUFFERING) {
        if (key == DIAL_KEY_CLICK || key == DIAL_KEY_PRESS) {
            plexstream_stop();
            plex_state_browse();
        }
        return;
    }

    if (keyboard_active()) {
        if (key == DIAL_KEY_UP || key == DIAL_KEY_DOWN) {
            keyboard_scroll(key);
        } else if (key == DIAL_KEY_CLICK) {
            keyboard_press();
        }
        return;
    }

    if (g_plex.detail_open) {
        plex_movie_t *m = g_plex.movie_count ? &g_plex.movies[g_plex.cur_sel] : NULL;
        if (key == DIAL_KEY_CLICK) {
            if (m && m->kind == PLEX_ITEM_SERIES) {
                plex_detail_close();
                plex_state_load_episodes(m);
            } else {
                plex_start_playback();
            }
        } else if (key == DIAL_KEY_UP || key == DIAL_KEY_DOWN) {
            if (m && m->kind == PLEX_ITEM_SERIES) {
                plex_detail_close(); // no quality to cycle on a series
            } else {
                int dir = (key == DIAL_KEY_UP) ? 1 : PLEX_QUALITY_COUNT - 1;
                g_setting.plex.quality = (plex_quality() + dir) % PLEX_QUALITY_COUNT;
                plex_settings_save();
                plex_detail_open(); // repaint with the new quality
            }
        } else if (key == RIGHT_KEY_CLICK) {
            plex_detail_close();
        }
        return;
    }

    switch (g_plex.state) {
    case PLEX_ST_SETUP:
    case PLEX_ST_ERROR:
        if (key == DIAL_KEY_UP) {
            g_plex.row_sel = (g_plex.row_sel + 1) % g_plex.row_count;
            plex_rows_paint();
        } else if (key == DIAL_KEY_DOWN) {
            g_plex.row_sel = (g_plex.row_sel + g_plex.row_count - 1) % g_plex.row_count;
            plex_rows_paint();
        } else if (key == DIAL_KEY_CLICK) {
            if (g_plex.state == PLEX_ST_SETUP) {
                plex_setup_row_activate();
            } else if (g_plex.row_sel == 0) {
                plex_state_loading(); // retry
            } else {
                plex_state_setup(true); // choose a different server
            }
        }
        break;

    case PLEX_ST_BROWSE:
        if (key == DIAL_KEY_UP) {
            g_plex.cur_sel = (g_plex.cur_sel + 1) % g_plex.movie_count;
            plex_grid_paint();
        } else if (key == DIAL_KEY_DOWN) {
            g_plex.cur_sel = (g_plex.cur_sel + g_plex.movie_count - 1) % g_plex.movie_count;
            plex_grid_paint();
        } else if (key == DIAL_KEY_CLICK) {
            plex_detail_open();
        }
        break;

    case PLEX_ST_LINK:
        if (key == DIAL_KEY_CLICK) {
            // Manual token entry as fallback (the pin worker keeps polling)
            g_plex.pending_input = PLEX_INPUT_TOKEN;
            plex_show_status(backend_is_jf()
                                 ? _lang("Type a Jellyfin API key, then click the Func button.\n(Create one in Dashboard -> API Keys.)")
                                 : _lang("Type the token, then click the Func button to submit.\n(Find it in any Plex Web URL as X-Plex-Token.)"));
            keyboard_set_text(active_token());
            keyboard_open();
        } else if ((key == DIAL_KEY_UP || key == DIAL_KEY_DOWN) && g_plex.link_failed) {
            plex_state_link(); // request a fresh plex.tv code
        }
        break;

    default:
        break;
    }
}

/**
 * Page plumbing
 */
static lv_obj_t *page_plex_create(lv_obj_t *parent, panel_arr_t *arr) {
    (void)arr;
    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_VIEW_SIZE);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, UI_PAGE_VIEW_SIZE);
#if HDZBOXPRO
    lv_obj_set_style_pad_top(section, 68, 0);
#endif

    lv_obj_t *cont = lv_obj_create(section);
    lv_obj_set_size(cont, UI_PAGE_VIEW_SIZE);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(cont, &style_context, LV_PART_MAIN);

    g_plex.header = lv_label_create(cont);
    lv_obj_set_style_text_font(g_plex.header, UI_PAGE_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_plex.header, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_pos(g_plex.header, 10, 10);
    lv_label_set_text(g_plex.header, "Plex");

    g_plex.status = lv_label_create(cont);
    lv_obj_set_style_text_font(g_plex.status, UI_PAGE_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_plex.status, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_label_set_long_mode(g_plex.status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_plex.status, page_wh[0] - 40);
    lv_obj_set_pos(g_plex.status, 10, 56);
    lv_obj_add_flag(g_plex.status, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < PLEX_MENU_ROWS_MAX; i++) {
        g_plex.rows[i] = lv_label_create(cont);
        lv_obj_set_style_text_font(g_plex.rows[i], UI_PAGE_TEXT_FONT, 0);
        lv_obj_set_style_text_color(g_plex.rows[i], lv_color_hex(TEXT_COLOR_DEFAULT), 0);
        lv_obj_set_pos(g_plex.rows[i], 20, GRID_Y0 + 60 + i * ROW_H);
        lv_obj_add_flag(g_plex.rows[i], LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < PLEX_PAGE_CNT; i++) {
        int col = i % PLEX_COLS;
        int row = i / PLEX_COLS;
        int x = GRID_X0 + col * (POSTER_W + GRID_GAP_X);
        int y = GRID_Y0 + row * (POSTER_H + GRID_GAP_Y);

        // Frame behind the artwork: dark placeholder + focus ring
        g_plex.cell[i] = lv_obj_create(cont);
        lv_obj_set_size(g_plex.cell[i], POSTER_W, POSTER_H);
        lv_obj_set_pos(g_plex.cell[i], x, y);
        lv_obj_clear_flag(g_plex.cell[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_style(g_plex.cell[i], &style_pb_dark, LV_PART_MAIN);
        lv_obj_set_style_bg_color(g_plex.cell[i], lv_color_make(24, 24, 24), 0);
        lv_obj_set_style_bg_opa(g_plex.cell[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(g_plex.cell[i], LV_OBJ_FLAG_HIDDEN);

        g_plex.poster[i] = lv_img_create(cont);
        lv_obj_set_size(g_plex.poster[i], POSTER_W, POSTER_H);
        lv_obj_set_pos(g_plex.poster[i], x, y);
        lv_obj_add_flag(g_plex.poster[i], LV_OBJ_FLAG_HIDDEN);

        g_plex.poster_label[i] = lv_label_create(cont);
        lv_obj_set_style_text_font(g_plex.poster_label[i], UI_PAGE_LABEL_FONT, 0);
        lv_obj_set_style_text_color(g_plex.poster_label[i], lv_color_hex(TEXT_COLOR_DEFAULT), 0);
        lv_label_set_long_mode(g_plex.poster_label[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(g_plex.poster_label[i], POSTER_W + GRID_GAP_X - 8);
        lv_obj_set_style_text_align(g_plex.poster_label[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(g_plex.poster_label[i], x - (GRID_GAP_X - 8) / 2, y + POSTER_H + 6);
        lv_obj_add_flag(g_plex.poster_label[i], LV_OBJ_FLAG_HIDDEN);
    }

    g_plex.hint = lv_label_create(cont);
    lv_obj_set_style_text_font(g_plex.hint, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_color(g_plex.hint, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_label_set_long_mode(g_plex.hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_plex.hint, page_wh[0] - 20);
    lv_obj_set_pos(g_plex.hint, 10, HINT_Y);
    lv_label_set_text(g_plex.hint, "");

    g_plex.detail = create_msgbox_item("Movie", "None");
    lv_obj_add_flag(g_plex.detail, LV_OBJ_FLAG_HIDDEN);

    return page;
}

static void page_plex_enter(void) {
    plex_invalidate_pending_work();

    if (!g_plex.timer) {
        g_plex.timer = lv_timer_create(plex_timer_cb, 250, NULL);
    }

    char ssid[64];
    if (!wifi_connected_ssid(ssid, sizeof(ssid))) {
        plex_state_no_wifi();
        return;
    }

    // A pre-provisioned SD card (plextoken.txt / jellyfintoken.txt with an
    // optional server line) completes setup without any on-goggle input.
    if (!active_token()[0] || !active_host()[0]) {
        if (plex_token_from_sdcard()) {
            g_setting.plex.backend = MEDIA_BACKEND_PLEX;
            plex_settings_save();
        } else if (jf_token_from_sdcard()) {
            g_setting.plex.backend = MEDIA_BACKEND_JELLYFIN;
            plex_settings_save();
        }
    }

    if (g_plex.movies && g_plex.movie_count > 0) {
        plex_state_browse();
    } else if (active_host()[0]) {
        plex_state_loading();
    } else {
        g_plex.rows_server_count = 0;
        plex_state_setup(true);
    }
}

static void page_plex_exit(void) {
    plex_invalidate_pending_work();
    plex_prefetch_set(NULL, 0); // stop background poster downloads

    // Safety net: never leave a download or transcode session running
    g_plex.playing = false;
    plexstream_stop();

    if (g_plex.timer) {
        lv_timer_del(g_plex.timer);
        g_plex.timer = NULL;
    }
    if (keyboard_active()) {
        keyboard_close();
    }
    plex_detail_close();
    g_plex.pending_input = PLEX_INPUT_NONE;
}

static void page_plex_on_roller(uint8_t key) {
    plex_key(key);
}

static void page_plex_on_click(uint8_t key, int sel) {
    (void)sel;
    plex_key(key);
}

static void page_plex_on_right_button(bool is_short) {
    if (g_plex.playing) {
        if (mplayer_on_key(is_short ? RIGHT_KEY_CLICK : RIGHT_KEY_PRESS)) {
            plex_end_playback();
        }
        return;
    }
    if (g_plex.state == PLEX_ST_BUFFERING) {
        return;
    }

    if (keyboard_active()) {
        if (is_short) {
            plex_commit_keyboard_text();
        } else {
            keyboard_clear_text();
        }
        return;
    }

    if (g_plex.detail_open) {
        plex_detail_close();
        return;
    }

    if (g_plex.state == PLEX_ST_BROWSE) {
        if (is_short) {
            if (g_plex.in_series) {
                plex_series_back();
            } else {
                plex_state_loading(); // refresh library
            }
        } else {
            // Forget server + token (both backends), back to discovery
            g_setting.plex.host[0] = '\0';
            g_setting.plex.token[0] = '\0';
            g_setting.plex.backend = MEDIA_BACKEND_PLEX;
            plex_settings_save();
            g_setting.jellyfin.host[0] = '\0';
            g_setting.jellyfin.token[0] = '\0';
            g_setting.jellyfin.user_id[0] = '\0';
            jf_settings_save();
            free(g_plex.movies);
            g_plex.movies = NULL;
            g_plex.movie_count = 0;
            if (g_plex.in_series) {
                free(g_plex.lib_movies);
                g_plex.lib_movies = NULL;
                g_plex.lib_movie_count = 0;
                g_plex.in_series = false;
            }
            plex_prefetch_set(NULL, 0);
            g_plex.rows_server_count = 0;
            plex_state_setup(true);
        }
    } else if (g_plex.state == PLEX_ST_SETUP && is_short) {
        plex_state_setup(true);
    }
}

page_pack_t pp_plex = {
    .name = "Plex",
    .create = page_plex_create,
    .enter = page_plex_enter,
    .exit = page_plex_exit,
    .on_created = NULL,
    .on_update = NULL,
    .on_roller = page_plex_on_roller,
    .on_click = page_plex_on_click,
    .on_right_button = page_plex_on_right_button,
};
