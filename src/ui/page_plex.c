#include "page_plex.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <log/log.h>

#include "../conf/ui.h"

#include "core/common.hh"
#include "core/plexapi.h"
#include "core/settings.h"
#include "core/wifi.h"
#include "lang/language.h"
#include "ui/page_common.h"
#include "ui/ui_keyboard.h"
#include "ui/ui_style.h"

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
    PLEX_ST_SETUP,   // pick a discovered server / enter one manually
    PLEX_ST_TOKEN,   // keyboard entry of the X-Plex-Token
    PLEX_ST_LOADING, // connect + library fetch in flight
    PLEX_ST_BROWSE,  // poster wall
    PLEX_ST_ERROR,
} plex_ui_state_t;

typedef enum {
    PLEX_JOB_NONE = 0,
    PLEX_JOB_DISCOVER,
    PLEX_JOB_CONNECT,
    PLEX_JOB_POSTERS,
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

    // JOB_POSTERS input: the worker only ever sees this private copy of the
    // visible page, so the UI can free/replace its own movie list at will.
    plex_movie_t poster_movies[PLEX_PAGE_CNT];
    int poster_count;

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

            int rc = plex_server_reachable(g_setting.plex.host, g_setting.plex.port)
                         ? PLEX_OK
                         : PLEX_ERR_NET;
            if (rc == PLEX_OK) {
                rc = plex_find_movie_section(key, sizeof(key), title, sizeof(title));
            }
            if (rc == PLEX_OK) {
                rc = plex_load_movies(key, &movies, &count, &g_work.progress);
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
                    if (!wanted[i].thumb[0] || plex_poster_cached(&wanted[i], path, sizeof(path))) {
                        continue;
                    }
                    plex_fetch_poster(&wanted[i], POSTER_W, POSTER_H, path, sizeof(path));
                    fetched = true;
                    break;
                }
                if (!fetched) {
                    break; // page fully cached (or nothing fetchable)
                }
            }
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
        snprintf(buf, sizeof(buf), "%s (%s:%d)",
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
    char buf[128];
    snprintf(buf, sizeof(buf), "%s %s:%d", _lang("Plex - Connecting to"), g_setting.plex.host, g_setting.plex.port);
    plex_set_header(buf);
    plex_show_status(_lang("Connecting..."));
    plex_set_hint(_lang("Long press the Enter button to exit"));
    g_work.progress = 0;
    plex_post_job(PLEX_JOB_CONNECT);
}

static void plex_state_loading(void);

static void plex_state_token(void) {
    // The no-typing path first: a plextoken.txt on the SD card signs in
    // without ever showing the keyboard.
    if (plex_token_from_sdcard()) {
        plex_state_loading();
        return;
    }

    plex_enter_state_common();
    g_plex.state = PLEX_ST_TOKEN;
    plex_set_header(_lang("Plex - Sign In"));
    plex_show_status(_lang("This server requires a Plex token.\nEasiest: on your computer, save the token into a file named\nplextoken.txt on this SD card - it is picked up automatically,\neven if you insert the card right now.\n\nOr type the token below, then click the Func button to submit.\n(Find your token in any Plex Web URL as X-Plex-Token.)"));
    plex_set_hint(_lang("Func button: Click to submit, Hold to erase"));
    g_plex.pending_input = PLEX_INPUT_TOKEN;
    keyboard_set_text(g_setting.plex.token);
    keyboard_open();
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
    if (plex_poster_cached(m, path, sizeof(path))) {
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

    snprintf(buf, sizeof(buf), "Plex - %s  |  %d %s  |  %s %d/%d",
             g_plex.section_title, g_plex.movie_count, _lang("movies"), _lang("page"), page + 1, pages);
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
        if (m->year > 0) {
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
    plex_set_hint(_lang("Click for details. Func button: Click to refresh, Hold to forget server. Long press the Enter button to exit"));
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
    if (m->year > 0) {
        snprintf(title, sizeof(title), "%s (%d)", m->title, m->year);
    } else {
        snprintf(title, sizeof(title), "%s", m->title);
    }
    snprintf(text, sizeof(text), "%s: %dh %02dm\n%s\n\n%s",
             _lang("Duration"), m->duration_min / 60, m->duration_min % 60,
             m->watched ? _lang("Watched") : _lang("Unwatched"),
             _lang("Streaming playback arrives in a later update."));
    lv_label_set_text(lv_msgbox_get_title(g_plex.detail), title);
    lv_label_set_text(lv_msgbox_get_text(g_plex.detail), text);
    lv_obj_clear_flag(g_plex.detail, LV_OBJ_FLAG_HIDDEN);
    g_plex.detail_open = true;
}

/**
 * Poll timer: applies worker results and fills in posters as they land.
 */
static void plex_timer_cb(lv_timer_t *timer) {
    (void)timer;

    bool discover_done = false, connect_done = false;
    int connect_rc = PLEX_OK;
    plex_movie_t *movies = NULL;
    int count = 0;
    char section_title[64] = "";

    pthread_mutex_lock(&g_work.lock);
    if (g_work.discover_done) {
        discover_done = true;
        g_work.discover_done = false;
        memcpy(g_plex.rows_servers, g_work.servers, sizeof(g_work.servers));
        g_plex.rows_server_count = g_work.server_count;
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
                g_plex.movies = movies;
                g_plex.movie_count = count;
                g_plex.cur_sel = 0;
                snprintf(g_plex.section_title, sizeof(g_plex.section_title), "%s", section_title);
                plex_state_browse();
            }
            break;
        case PLEX_ERR_AUTH:
            free(movies);
            plex_state_token();
            break;
        case PLEX_ERR_NOMOVIE:
            free(movies);
            plex_state_error(_lang("No movie library was found on this server."));
            break;
        default:
            free(movies);
            plex_state_error(_lang("Could not reach the Plex server.\nCheck that it is running and on the same network."));
            break;
        }
        return;
    }

    if (g_plex.state == PLEX_ST_TOKEN) {
        // Sign in the moment an SD card carrying plextoken.txt shows up
        if (plex_token_from_sdcard()) {
            plex_state_loading();
            return;
        }
    }

    if (g_plex.state == PLEX_ST_LOADING) {
        int progress = g_work.progress;
        if (progress > 0) {
            char buf[96];
            snprintf(buf, sizeof(buf), "%s %d %s...", _lang("Loading library"), progress, _lang("movies"));
            plex_show_status(buf);
        }
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
static void plex_save_server(const char *host, int port) {
    snprintf(g_setting.plex.host, sizeof(g_setting.plex.host), "%s", host);
    g_setting.plex.port = port > 0 ? port : 32400;
    plex_settings_save();
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
        int port = 32400;
        if (colon) {
            *colon = '\0';
            port = atoi(colon + 1);
        }
        plex_save_server(buf, port);
        plex_state_loading();
        break;
    }
    case PLEX_INPUT_TOKEN:
        snprintf(g_setting.plex.token, sizeof(g_setting.plex.token), "%s", buf);
        plex_settings_save();
        plex_state_loading();
        break;
    default:
        break;
    }
}

static void plex_setup_row_activate(void) {
    if (g_plex.row_sel < g_plex.rows_server_count) {
        plex_server_t *srv = &g_plex.rows_servers[g_plex.row_sel];
        plex_save_server(srv->host, srv->port);
        plex_state_loading();
    } else if (g_plex.row_sel == g_plex.rows_server_count) {
        plex_state_setup(true); // rescan
    } else {
        g_plex.pending_input = PLEX_INPUT_HOST;
        plex_show_status(_lang("Type the server IP address (optionally :port),\nthen click the Func button to submit."));
        keyboard_set_text(g_setting.plex.host);
        keyboard_open();
    }
}

static void plex_key(uint8_t key) {
    if (keyboard_active()) {
        if (key == DIAL_KEY_UP || key == DIAL_KEY_DOWN) {
            keyboard_scroll(key);
        } else if (key == DIAL_KEY_CLICK) {
            keyboard_press();
        }
        return;
    }

    if (g_plex.detail_open) {
        if (key == DIAL_KEY_CLICK || key == RIGHT_KEY_CLICK ||
            key == DIAL_KEY_UP || key == DIAL_KEY_DOWN) {
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

    case PLEX_ST_TOKEN:
        // The keyboard is normally up in this state; if it was dismissed,
        // any click returns to server selection.
        if (key == DIAL_KEY_CLICK) {
            plex_state_setup(false);
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

    // A pre-provisioned SD card (plextoken.txt with optional server line)
    // completes setup without any on-goggle input.
    if (!g_setting.plex.token[0] || !g_setting.plex.host[0]) {
        plex_token_from_sdcard();
    }

    if (g_plex.movies && g_plex.movie_count > 0) {
        plex_state_browse();
    } else if (g_setting.plex.host[0]) {
        plex_state_loading();
    } else {
        g_plex.rows_server_count = 0;
        plex_state_setup(true);
    }
}

static void page_plex_exit(void) {
    plex_invalidate_pending_work();

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
            plex_state_loading(); // refresh library
        } else {
            // Forget server + token, back to discovery
            g_setting.plex.host[0] = '\0';
            g_setting.plex.token[0] = '\0';
            plex_settings_save();
            free(g_plex.movies);
            g_plex.movies = NULL;
            g_plex.movie_count = 0;
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
