#include "page_immich.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <log/log.h>

#include "../conf/ui.h"

#include "core/common.hh"
#include "core/immichapi.h"
#include "core/settings.h"
#include "core/wifi.h"
#include "lang/language.h"
#include "record/record_definitions.h"
#include "ui/page_common.h"
#include "ui/ui_keyboard.h"
#include "ui/ui_style.h"
#include "util/filesystem.h"

#define IMMICH_FILES_DIR REC_diskPATH REC_packPATH
#define IMMICH_FILES_MAX 128

#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
#define LIST_ROWS  14
#define LIST_Y0    96
#define LIST_ROW_H 44
#define HINT_Y     760
#else
#define LIST_ROWS  10
#define LIST_Y0    76
#define LIST_ROW_H 34
#define HINT_Y     500
#endif

typedef enum {
    IM_ST_NO_WIFI = 0,
    IM_ST_NO_SD,
    IM_ST_SETUP,     // server address + API key entry
    IM_ST_TESTING,   // credential check in flight
    IM_ST_LIST,      // pick recordings to upload
    IM_ST_UPLOADING, // batch in progress
} immich_ui_state_t;

typedef enum {
    IM_INPUT_NONE = 0,
    IM_INPUT_HOST,
    IM_INPUT_KEY,
} immich_input_t;

typedef struct {
    char filename[80];
    long size_mb;
    bool uploaded;
    bool selected;
} immich_file_t;

typedef enum {
    IM_WORK_NONE = 0,
    IM_WORK_TEST,
    IM_WORK_UPLOAD,
} immich_work_t;

typedef struct {
    // One background job at a time; results are polled by the UI timer.
    pthread_t thread;
    volatile immich_work_t work;
    volatile bool finished;

    // WORK_TEST result
    volatile bool test_ok;

    // WORK_UPLOAD batch
    char names[IMMICH_FILES_MAX][80];
    int total;
    volatile int current; // batch index of the file in flight
    volatile int ok_count, dup_count, fail_count;
    volatile bool auth_failed;
    lan_stream_state_t io;
} immich_worker_t;

typedef struct {
    immich_ui_state_t state;
    immich_input_t pending_input;

    immich_file_t files[IMMICH_FILES_MAX];
    int file_count;
    int cur_sel;

    char summary[96]; // last batch outcome, shown on the list header line

    lv_obj_t *header;
    lv_obj_t *status;
    lv_obj_t *rows[LIST_ROWS];
    lv_obj_t *hint;
    lv_timer_t *timer;
} immich_page_t;

static immich_worker_t g_iwork;
static immich_page_t g_im;

static const lv_coord_t im_page_wh[] = {UI_PAGE_VIEW_SIZE};

/**
 * Background work: credentials test or the upload batch.
 */
static void *immich_thread(void *arg) {
    (void)arg;
    if (g_iwork.work == IM_WORK_TEST) {
        g_iwork.test_ok = immich_server_reachable();
    } else if (g_iwork.work == IM_WORK_UPLOAD) {
        if (!immich_server_reachable()) {
            g_iwork.fail_count = g_iwork.total;
        } else {
            for (int i = 0; i < g_iwork.total && !g_iwork.io.cancel; i++) {
                g_iwork.current = i;
                char path[300];
                snprintf(path, sizeof(path), "%s%s", IMMICH_FILES_DIR, g_iwork.names[i]);

                memset((void *)&g_iwork.io.bytes, 0, sizeof(g_iwork.io.bytes));
                g_iwork.io.bytes = 0;
                g_iwork.io.done = false;

                int rc = immich_upload(path, g_iwork.names[i], &g_iwork.io);
                if (rc == IMMICH_OK) {
                    g_iwork.ok_count++;
                } else if (rc == IMMICH_DUP) {
                    g_iwork.dup_count++;
                } else {
                    g_iwork.fail_count++;
                    if (rc == IMMICH_ERR_AUTH) {
                        g_iwork.auth_failed = true;
                        break;
                    }
                }
            }
        }
    }
    g_iwork.finished = true;
    return NULL;
}

static bool immich_start_work(immich_work_t work) {
    g_iwork.work = work;
    g_iwork.finished = false;
    g_iwork.test_ok = false;
    g_iwork.auth_failed = false;
    g_iwork.io.cancel = false;
    g_iwork.io.bytes = 0;
    g_iwork.current = 0;
    g_iwork.ok_count = g_iwork.dup_count = g_iwork.fail_count = 0;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&g_iwork.thread, &attr, immich_thread, NULL);
    pthread_attr_destroy(&attr);
    return rc == 0;
}

/**
 * DVR file scan (same filter as the Playback page: .ts/.mp4 over 5 MB).
 */
static void immich_scan_files(void) {
    g_im.file_count = 0;

    struct dirent **namelist;
    int count = scandir(IMMICH_FILES_DIR, &namelist, NULL, alphasort);
    if (count < 0) {
        return;
    }
    for (int i = 0; i < count; i++) {
        struct dirent *e = namelist[i];
        const char *dot = strrchr(e->d_name, '.');
        if (e->d_name[0] == '.' || !dot ||
            (strcasecmp(dot, "." REC_packTS) != 0 && strcasecmp(dot, "." REC_packMP4) != 0)) {
            continue;
        }
        char path[300];
        snprintf(path, sizeof(path), "%s%s", IMMICH_FILES_DIR, e->d_name);
        long size_mb = fs_filesize(path) >> 20;
        if (size_mb < 5 || g_im.file_count >= IMMICH_FILES_MAX) {
            continue;
        }

        immich_file_t *f = &g_im.files[g_im.file_count++];
        snprintf(f->filename, sizeof(f->filename), "%s", e->d_name);
        f->size_mb = size_mb;
        f->uploaded = immich_uploaded(path);
        f->selected = false;
    }
    for (int i = 0; i < count; i++) {
        free(namelist[i]);
    }
    free(namelist);
}

/**
 * UI helpers
 */
static void immich_rows_hide(void) {
    for (int i = 0; i < LIST_ROWS; i++) {
        lv_obj_add_flag(g_im.rows[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void immich_show_status(const char *text) {
    lv_label_set_text(g_im.status, text);
    lv_obj_clear_flag(g_im.status, LV_OBJ_FLAG_HIDDEN);
}

static void immich_state_common(void) {
    immich_rows_hide();
    lv_obj_add_flag(g_im.status, LV_OBJ_FLAG_HIDDEN);
    g_im.pending_input = IM_INPUT_NONE;
    if (keyboard_active()) {
        keyboard_close();
    }
}

static void immich_paint_list(void) {
    char buf[160];
    int page = g_im.cur_sel / LIST_ROWS;
    int pages = (g_im.file_count + LIST_ROWS - 1) / LIST_ROWS;
    int selected = 0;
    for (int i = 0; i < g_im.file_count; i++) {
        selected += g_im.files[i].selected;
    }

    snprintf(buf, sizeof(buf), "Immich - %d %s, %d %s  |  %s %d/%d%s%s",
             g_im.file_count, _lang("recordings"), selected, _lang("selected"),
             _lang("page"), page + 1, pages > 0 ? pages : 1,
             g_im.summary[0] ? "  |  " : "", g_im.summary);
    lv_label_set_text(g_im.header, buf);

    for (int i = 0; i < LIST_ROWS; i++) {
        int seq = page * LIST_ROWS + i;
        if (seq >= g_im.file_count) {
            lv_obj_add_flag(g_im.rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        immich_file_t *f = &g_im.files[seq];
        snprintf(buf, sizeof(buf), "[%s] %s  %ldMB%s",
                 f->selected ? "x" : "  ",
                 f->filename, f->size_mb,
                 f->uploaded ? _lang("  - uploaded") : "");
        lv_label_set_text(g_im.rows[i], buf);
        lv_obj_set_style_text_color(g_im.rows[i],
                                    lv_color_hex(seq == g_im.cur_sel  ? 0xFFFF00
                                                 : f->uploaded       ? 0x00C000
                                                                     : TEXT_COLOR_DEFAULT),
                                    0);
        lv_obj_clear_flag(g_im.rows[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void immich_state_list(void) {
    immich_state_common();
    g_im.state = IM_ST_LIST;
    lv_label_set_text(g_im.hint,
                      _lang("Click: select/deselect. Func button: Click to upload selected, Hold to change server. Long press the Enter button to exit"));
    if (g_im.cur_sel >= g_im.file_count) {
        g_im.cur_sel = 0;
    }
    if (g_im.file_count == 0) {
        immich_show_status(_lang("No recordings found on the SD card."));
    }
    immich_paint_list();
}

static void immich_state_setup(void) {
    immich_state_common();
    g_im.state = IM_ST_SETUP;
    lv_label_set_text(g_im.header, _lang("Immich - Server Setup"));
    immich_show_status(_lang("Back up recordings to your Immich server.\n\nEasiest: on your computer, create immich.txt on this SD card:\n  line 1: your API key (Immich -> Account Settings -> API Keys)\n  line 2: server-ip:2283\nIt is picked up automatically, even inserted right now.\n\nOr click the dial to type the server address, then the API key."));
    lv_label_set_text(g_im.hint, _lang("Click the dial to type. Long press the Enter button to exit"));
}

static void immich_state_testing(void) {
    immich_state_common();
    g_im.state = IM_ST_TESTING;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s %s:%d...", _lang("Immich - Checking"), g_setting.immich.host, g_setting.immich.port);
    lv_label_set_text(g_im.header, "Immich");
    immich_show_status(buf);
    immich_start_work(IM_WORK_TEST);
}

static void immich_state_uploading(void) {
    immich_state_common();
    g_im.state = IM_ST_UPLOADING;
    lv_label_set_text(g_im.header, _lang("Immich - Uploading"));
    immich_show_status(_lang("Starting upload..."));
    lv_label_set_text(g_im.hint, _lang("Click the dial to cancel after the current file"));
}

static void immich_begin_upload(void) {
    g_iwork.total = 0;
    for (int i = 0; i < g_im.file_count && g_iwork.total < IMMICH_FILES_MAX; i++) {
        if (g_im.files[i].selected) {
            snprintf(g_iwork.names[g_iwork.total], sizeof(g_iwork.names[0]), "%s", g_im.files[i].filename);
            g_iwork.total++;
        }
    }
    if (g_iwork.total == 0) {
        // Nothing ticked: upload the highlighted file
        if (g_im.file_count == 0) {
            return;
        }
        snprintf(g_iwork.names[0], sizeof(g_iwork.names[0]), "%s", g_im.files[g_im.cur_sel].filename);
        g_iwork.total = 1;
    }

    immich_state_uploading();
    immich_start_work(IM_WORK_UPLOAD);
}

/**
 * Poll timer
 */
static void immich_timer_cb(lv_timer_t *timer) {
    (void)timer;

    if (g_im.state == IM_ST_SETUP) {
        if (!keyboard_active() && immich_token_from_sdcard()) {
            immich_state_testing();
        }
        return;
    }

    if (g_im.state == IM_ST_TESTING && g_iwork.finished) {
        if (g_iwork.test_ok) {
            immich_scan_files();
            immich_state_list();
        } else {
            immich_state_setup();
            immich_show_status(_lang("Could not reach the Immich server with these details.\nCheck the address and API key, then try again.\n\nClick the dial to re-enter them, or fix immich.txt on the SD card."));
        }
        return;
    }

    if (g_im.state == IM_ST_UPLOADING) {
        if (g_iwork.finished) {
            snprintf(g_im.summary, sizeof(g_im.summary), "%d %s, %d %s, %d %s",
                     g_iwork.ok_count, _lang("uploaded"),
                     g_iwork.dup_count, _lang("already on server"),
                     g_iwork.fail_count, _lang("failed"));
            immich_scan_files(); // refresh sidecar status
            for (int i = 0; i < g_im.file_count; i++) {
                g_im.files[i].selected = false;
            }
            immich_state_list();
            if (g_iwork.auth_failed) {
                immich_show_status(_lang("The server rejected the API key.\nFunc hold: re-enter server details."));
            }
            return;
        }

        int cur = g_iwork.current;
        if (cur < g_iwork.total) {
            char buf[192];
            long sent_mb = g_iwork.io.bytes >> 20;
            snprintf(buf, sizeof(buf), "%s %d/%d:\n%s\n%ld MB %s",
                     _lang("Uploading file"), cur + 1, g_iwork.total,
                     g_iwork.names[cur], sent_mb, _lang("sent"));
            immich_show_status(buf);
        }
    }
}

/**
 * Input
 */
static void immich_commit_keyboard(void) {
    char buf[128] = "";
    keyboard_get_text(buf, sizeof(buf));
    keyboard_close();

    immich_input_t input = g_im.pending_input;
    g_im.pending_input = IM_INPUT_NONE;

    switch (input) {
    case IM_INPUT_HOST: {
        if (!buf[0]) {
            immich_state_setup();
            return;
        }
        char *colon = strrchr(buf, ':');
        int port = 2283;
        if (colon) {
            *colon = '\0';
            port = atoi(colon + 1);
        }
        snprintf(g_setting.immich.host, sizeof(g_setting.immich.host), "%s", buf);
        g_setting.immich.port = port > 0 ? port : 2283;
        immich_settings_save();
        // Next: the API key
        g_im.pending_input = IM_INPUT_KEY;
        immich_show_status(_lang("Now type your Immich API key\n(Immich -> Account Settings -> API Keys -> New),\nthen click the Func button to submit."));
        keyboard_set_text(g_setting.immich.api_key);
        keyboard_open();
        break;
    }
    case IM_INPUT_KEY:
        if (buf[0]) {
            snprintf(g_setting.immich.api_key, sizeof(g_setting.immich.api_key), "%s", buf);
            immich_settings_save();
        }
        if (immich_configured()) {
            immich_state_testing();
        } else {
            immich_state_setup();
        }
        break;
    default:
        break;
    }
}

static void immich_key(uint8_t key) {
    if (keyboard_active()) {
        if (key == DIAL_KEY_UP || key == DIAL_KEY_DOWN) {
            keyboard_scroll(key);
        } else if (key == DIAL_KEY_CLICK) {
            keyboard_press();
        }
        return;
    }

    switch (g_im.state) {
    case IM_ST_SETUP:
        if (key == DIAL_KEY_CLICK) {
            g_im.pending_input = IM_INPUT_HOST;
            immich_show_status(_lang("Type the Immich server address (ip or ip:port,\ndefault port 2283), then click the Func button."));
            keyboard_set_text(g_setting.immich.host);
            keyboard_open();
        }
        break;

    case IM_ST_LIST:
        if (!g_im.file_count) {
            break;
        }
        if (key == DIAL_KEY_UP) {
            g_im.cur_sel = (g_im.cur_sel + 1) % g_im.file_count;
            immich_paint_list();
        } else if (key == DIAL_KEY_DOWN) {
            g_im.cur_sel = (g_im.cur_sel + g_im.file_count - 1) % g_im.file_count;
            immich_paint_list();
        } else if (key == DIAL_KEY_CLICK) {
            g_im.files[g_im.cur_sel].selected = !g_im.files[g_im.cur_sel].selected;
            immich_paint_list();
        }
        break;

    case IM_ST_UPLOADING:
        if (key == DIAL_KEY_CLICK) {
            g_iwork.io.cancel = true;
            immich_show_status(_lang("Cancelling after the current file..."));
        }
        break;

    default:
        break;
    }
}

/**
 * Page plumbing
 */
static lv_obj_t *page_immich_create(lv_obj_t *parent, panel_arr_t *arr) {
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

    g_im.header = lv_label_create(cont);
    lv_obj_set_style_text_font(g_im.header, UI_PAGE_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_im.header, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_pos(g_im.header, 10, 10);
    lv_label_set_text(g_im.header, "Immich");

    g_im.status = lv_label_create(cont);
    lv_obj_set_style_text_font(g_im.status, UI_PAGE_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_im.status, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_label_set_long_mode(g_im.status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_im.status, im_page_wh[0] - 40);
    lv_obj_set_pos(g_im.status, 10, 56);
    lv_obj_add_flag(g_im.status, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < LIST_ROWS; i++) {
        g_im.rows[i] = lv_label_create(cont);
        lv_obj_set_style_text_font(g_im.rows[i], UI_PAGE_LABEL_FONT, 0);
        lv_obj_set_style_text_color(g_im.rows[i], lv_color_hex(TEXT_COLOR_DEFAULT), 0);
        lv_label_set_long_mode(g_im.rows[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(g_im.rows[i], im_page_wh[0] - 40);
        lv_obj_set_pos(g_im.rows[i], 20, LIST_Y0 + i * LIST_ROW_H);
        lv_obj_add_flag(g_im.rows[i], LV_OBJ_FLAG_HIDDEN);
    }

    g_im.hint = lv_label_create(cont);
    lv_obj_set_style_text_font(g_im.hint, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_color(g_im.hint, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_label_set_long_mode(g_im.hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_im.hint, im_page_wh[0] - 20);
    lv_obj_set_pos(g_im.hint, 10, HINT_Y);
    lv_label_set_text(g_im.hint, "");

    return page;
}

static void page_immich_enter(void) {
    if (!g_im.timer) {
        g_im.timer = lv_timer_create(immich_timer_cb, 300, NULL);
    }
    g_im.summary[0] = '\0';

    char ssid[64];
    if (!wifi_connected_ssid(ssid, sizeof(ssid))) {
        immich_state_common();
        g_im.state = IM_ST_NO_WIFI;
        lv_label_set_text(g_im.header, "Immich");
        immich_show_status(_lang("WiFi is not connected.\nJoin your home network from the WiFi menu first."));
        lv_label_set_text(g_im.hint, _lang("Long press the Enter button to exit"));
        return;
    }
    if (!fs_file_exists(REC_diskPATH)) {
        immich_state_common();
        g_im.state = IM_ST_NO_SD;
        lv_label_set_text(g_im.header, "Immich");
        immich_show_status(_lang("No SD card found - the recordings live there."));
        lv_label_set_text(g_im.hint, _lang("Long press the Enter button to exit"));
        return;
    }

    if (!immich_configured()) {
        immich_token_from_sdcard(); // pre-provisioned card completes setup silently
    }
    if (immich_configured()) {
        immich_state_testing();
    } else {
        immich_state_setup();
    }
}

static void page_immich_exit(void) {
    g_iwork.io.cancel = true; // let any in-flight batch wind down
    if (g_im.timer) {
        lv_timer_del(g_im.timer);
        g_im.timer = NULL;
    }
    if (keyboard_active()) {
        keyboard_close();
    }
    g_im.pending_input = IM_INPUT_NONE;
}

static void page_immich_on_roller(uint8_t key) {
    immich_key(key);
}

static void page_immich_on_click(uint8_t key, int sel) {
    (void)sel;
    immich_key(key);
}

static void page_immich_on_right_button(bool is_short) {
    if (keyboard_active()) {
        if (is_short) {
            immich_commit_keyboard();
        } else {
            keyboard_clear_text();
        }
        return;
    }

    if (g_im.state == IM_ST_LIST) {
        if (is_short) {
            immich_begin_upload();
        } else {
            immich_state_setup();
        }
    }
}

page_pack_t pp_immich = {
    .name = "Immich",
    .create = page_immich_create,
    .enter = page_immich_enter,
    .exit = page_immich_exit,
    .on_created = NULL,
    .on_update = NULL,
    .on_roller = page_immich_on_roller,
    .on_click = page_immich_on_click,
    .on_right_button = page_immich_on_right_button,
};
