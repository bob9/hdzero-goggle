#include "page_shutdown.h"

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#include <log/log.h>

#include "../conf/ui.h"

#include "core/common.hh"
#include "lang/language.h"
#include "page_common.h"
#include "ui/ui_style.h"
#include "util/sdcard.h"
#include "util/system.h"

// Safe shutdown: once triggered the goggles swallow all input and the
// full-screen notice is the last thing they display. Sits beside Go Sleep
// in the main menu - both end the session, so they belong together.
//
// The point is the clean unmount: it clears the FAT dirty bit, which is what
// lets the next boot skip the SD integrity check (see sdcard_filesystem_dirty
// and the auto-check in page_storage.c). Pulling the battery instead leaves
// the card dirty and the check runs.
static bool shutdown_active = false;
static lv_obj_t *shutdown_label = NULL;

static void *page_shutdown_thread(void *arg) {
    (void)arg;

    // The same prologue the SD integrity check uses to quiesce the
    // recorder before an unmount, plus the live stream.
    system_exec("/mnt/app/app/record/gogglecmd -live quit");
    system_exec("/mnt/app/app/record/gogglecmd -rec quit");
    system_exec("/mnt/app/app/record/gogglecmd -sds quit");
    sleep(2);
    system_exec("killall rtspLive 2>/dev/null");
    system_exec("sync");

    bool safe = true;
    if (sdcard_mounted()) {
        safe = system_exec("umount /mnt/extsd") == 0;
        if (!safe) {
            // busy card: stop the writers the hard way and retry once
            system_exec("killall record sdstat 2>/dev/null");
            sleep(1);
            system_exec("sync");
            safe = system_exec("umount /mnt/extsd") == 0;
        }
    }

    pthread_mutex_lock(&lvgl_mutex);
    if (shutdown_label) {
        lv_label_set_text(shutdown_label,
                          safe ? "SD card released - it is now safe\nto turn the goggles off."
                               : "Could not release the SD card.\nStop recording, then power off.");
    }
    pthread_mutex_unlock(&lvgl_mutex);
    pthread_exit(NULL);
}

lv_obj_t *page_shutdown_create(lv_obj_t *parent, panel_arr_t *arr) {
    (void)arr;
    char buf[192];
    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_VIEW_SIZE);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, UI_PAGE_VIEW_SIZE);

    snprintf(buf, sizeof(buf), "%s:", _lang("Shutdown"));
    create_text(NULL, section, false, buf, LV_MENU_ITEM_BUILDER_VARIANT_2);

    lv_obj_t *cont = lv_menu_cont_create(section);
    lv_obj_set_width(cont, LV_PCT(100));

    lv_obj_t *desc_label = lv_label_create(cont);
    snprintf(buf, sizeof(buf), "%s.\n\n%s.\n%s.\n\n%s.",
             _lang("Stops recording and releases the SD card so the goggles can be "
                   "switched off without corrupting it"),
             _lang("Click the Enter Button to start"),
             _lang("Wait for the safe-to-power-off notice, then switch the goggles "
                   "off yourself - this does not power them down"),
             _lang("Releasing the card cleanly lets the next boot skip the SD card "
                   "integrity check, so the goggles start up faster. Pulling the "
                   "battery instead leaves the card dirty and the check runs"));
    lv_label_set_text(desc_label, buf);
    // Explicit width: the menu container is content-sized, so without this the
    // longer lines run off the panel instead of wrapping.
    lv_obj_set_width(desc_label, LV_PCT(96));
    lv_obj_set_style_text_font(desc_label, UI_PAGE_TEXT_FONT, 0);
    lv_obj_set_style_text_color(desc_label, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(desc_label, UI_PAGE_TEXT_PAD, 0);
    lv_label_set_long_mode(desc_label, LV_LABEL_LONG_WRAP);

    return page;
}

static void page_shutdown_enter() {
    LOGI("page_shutdown_enter");

    if (shutdown_active) {
        return;
    }
    shutdown_active = true;

    lv_obj_t *cover = lv_obj_create(lv_scr_act());
    lv_obj_set_size(cover, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(cover, 0, 0);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x010101), 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_radius(cover, 0, 0);

    shutdown_label = lv_label_create(cover);
    lv_label_set_text(shutdown_label, "Shutting down...");
    lv_obj_set_style_text_font(shutdown_label, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_align(shutdown_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(shutdown_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(shutdown_label);

    pthread_t tid;
    if (pthread_create(&tid, NULL, page_shutdown_thread, NULL) == 0) {
        pthread_detach(tid);
    }
}

page_pack_t pp_shutdown = {
    .create = page_shutdown_create,
    .enter = page_shutdown_enter,
    .exit = NULL,
    .on_created = NULL,
    .on_update = NULL,
    .on_roller = NULL,
    .on_click = NULL,
    .on_right_button = NULL,
    .name = "Shutdown",
};
