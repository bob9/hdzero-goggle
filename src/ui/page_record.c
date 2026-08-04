#include "page_record.h"

#include <stdio.h>
#include <stdlib.h>

#include <minIni.h>

#include "../conf/ui.h"

#include "../core/common.hh"
#include "core/app_state.h"
#include "core/dvr.h"
#include "driver/rtc.h"
#include "lang/language.h"
#include "page_common.h"
#include "screenrec/screen_record.h"
#include "ui/ui_style.h"

#define STOP_DELAY_MAX 30
#define STOP_DELAY_STEP 5

static btn_group_t btn_group_record_mode;
static btn_group_t btn_group_format;
static btn_group_t btn_group_bitrate_scale;
static btn_group_t btn_group_rate_control;
static btn_group_t btn_group_record_osd;
static btn_group_t btn_group_file_naming;
static slider_group_t slider_group_stop_delay;
static slider_group_t slider_group_vbr_quality;
static slider_group_t slider_group_vbr_max_qp;
static lv_obj_t *label_screen_record = NULL;

enum {
    ROW_RECORD_MODE = 0,
    ROW_RECORD_FORMAT,
    ROW_RECORD_BITRATE,
    ROW_RECORD_BITRATE_WRAP, // 5 bitrate buttons wrap onto a second grid row
    ROW_RATE_CONTROL,
    ROW_VBR_QUALITY,
    ROW_VBR_MAX_QP,
    ROW_RECORD_OSD,
    ROW_NAMING_SCHEME,
    ROW_STOP_DELAY,
    ROW_SCREEN_RECORD,
    ROW_BACK,

    ROW_COUNT
};

static bool stop_delay_focused = false;
static bool stop_delay_changed = false;
static bool vbr_quality_focused = false;
static bool vbr_quality_changed = false;
static bool vbr_max_qp_focused = false;
static bool vbr_max_qp_changed = false;

static lv_coord_t col_dsc[] = {UI_RECORD_COLS};
static lv_coord_t row_dsc[] = {UI_RECORD_ROWS};

// The Record Bitrate buttons read low-to-high after Normal, but the stored
// enum values are frozen for backward compatibility, so the menu order and
// the setting value are not the same thing. These map between them.
static const setting_record_bitrate_scale_t bitrate_btn_to_setting[] = {
    SETTING_RECORD_BITRATE_SCALE_NORMAL,
    SETTING_RECORD_BITRATE_SCALE_QUARTER,
    SETTING_RECORD_BITRATE_SCALE_HALF,
    SETTING_RECORD_BITRATE_SCALE_HIGH,
    SETTING_RECORD_BITRATE_SCALE_MAX,
};
#define BITRATE_BTN_COUNT (int)(sizeof(bitrate_btn_to_setting) / sizeof(bitrate_btn_to_setting[0]))

static int bitrate_setting_to_btn(setting_record_bitrate_scale_t v) {
    for (int i = 0; i < BITRATE_BTN_COUNT; i++) {
        if (bitrate_btn_to_setting[i] == v)
            return i;
    }
    return 0; // unknown value in settings.ini -> Normal
}

static void update_stop_delay_label() {
    char buf[16];

    lv_slider_set_value(slider_group_stop_delay.slider, g_setting.record.stop_delay_seconds, LV_ANIM_OFF);
    if (g_setting.record.stop_delay_seconds == 0)
        snprintf(buf, sizeof(buf), "%s", _lang("Off"));
    else
        snprintf(buf, sizeof(buf), "%ds", g_setting.record.stop_delay_seconds);
    lv_label_set_text(slider_group_stop_delay.label, buf);
}

static void update_vbr_quality_label() {
    char buf[16];
    lv_slider_set_value(slider_group_vbr_quality.slider, g_setting.record.vbr_quality, LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "%d", g_setting.record.vbr_quality);
    lv_label_set_text(slider_group_vbr_quality.label, buf);
}

static void update_vbr_max_qp_label() {
    char buf[24];
    lv_slider_set_value(slider_group_vbr_max_qp.slider, g_setting.record.vbr_max_qp, LV_ANIM_OFF);
    if (g_setting.record.vbr_max_qp == VBR_MAX_QP_RECOMMENDED)
        snprintf(buf, sizeof(buf), "%d *", g_setting.record.vbr_max_qp);
    else
        snprintf(buf, sizeof(buf), "%d", g_setting.record.vbr_max_qp);
    lv_label_set_text(slider_group_vbr_max_qp.label, buf);
}

// "Start" when idle, "REC 01:23  screen_0007.mp4" while running, or the reason
// the last attempt failed. Refreshed from on_update so the clock ticks.
static void update_screen_record_label() {
    char buf[96];

    if (label_screen_record == NULL) {
        return;
    }

    if (screen_record_is_active()) {
        const uint32_t s = screen_record_elapsed_s();
        snprintf(buf, sizeof(buf), "%s  %02u:%02u  %s", _lang("Stop"),
                 s / 60, s % 60, screen_record_filename());
    } else if (screen_record_last_error()[0]) {
        snprintf(buf, sizeof(buf), "%s  (%s)", _lang("Start"), screen_record_last_error());
    } else {
        snprintf(buf, sizeof(buf), "%s", _lang("Start"));
    }

    lv_label_set_text(label_screen_record, buf);
}

static void page_record_on_update(uint32_t delta_ms) {
    (void)delta_ms;
    update_screen_record_label();
}

static void update_visibility() {
    // The quality figure only means anything under VBR
    const bool vbr = (btn_group_rate_control.current == SETTING_RECORD_RC_VBR);
    slider_enable(&slider_group_vbr_quality, vbr);
    slider_enable(&slider_group_vbr_max_qp, vbr);
    if (vbr) {
        lv_obj_add_flag(pp_record.p_arr.panel[ROW_VBR_QUALITY], FLAG_SELECTABLE);
        lv_obj_add_flag(pp_record.p_arr.panel[ROW_VBR_MAX_QP], FLAG_SELECTABLE);
    } else {
        lv_obj_clear_flag(pp_record.p_arr.panel[ROW_VBR_QUALITY], FLAG_SELECTABLE);
        lv_obj_clear_flag(pp_record.p_arr.panel[ROW_VBR_MAX_QP], FLAG_SELECTABLE);
    }

    btn_group_enable(&btn_group_file_naming, rtc_has_battery() == 0);

    if (rtc_has_battery() == 0) {
        lv_obj_add_flag(pp_record.p_arr.panel[ROW_NAMING_SCHEME], FLAG_SELECTABLE);
    } else {
        lv_obj_clear_flag(pp_record.p_arr.panel[ROW_NAMING_SCHEME], FLAG_SELECTABLE);
    }
}

static lv_obj_t *page_record_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[256];
    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_VIEW_SIZE);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, UI_PAGE_VIEW_SIZE);

    snprintf(buf, sizeof(buf), "%s:", _lang("Record Option"));
    create_text(NULL, section, false, buf, LV_MENU_ITEM_BUILDER_VARIANT_2);

    lv_obj_t *cont = lv_obj_create(section);
    lv_obj_set_size(cont, UI_PAGE_VIEW_SIZE);
    lv_obj_set_pos(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_GRID);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(cont, &style_context, LV_PART_MAIN);

    lv_obj_set_style_grid_column_dsc_array(cont, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(cont, row_dsc, 0);

    create_select_item(arr, cont);

    create_btn_group_item(&btn_group_record_mode, cont, 2, _lang("Record Mode"), _lang("Auto"), _lang("Manual"), "", "", ROW_RECORD_MODE);
    create_btn_group_item(&btn_group_format, cont, 2, _lang("Record Format"), "MP4", "TS", "", "", ROW_RECORD_FORMAT);
    // 5 buttons wrap onto a second grid row: stretch the selection panel over
    // both and take the absorbed row out of the navigation.
    create_btn_group_item2(&btn_group_bitrate_scale, cont, BITRATE_BTN_COUNT, _lang("Record Bitrate"),
                           _lang("Normal"), "1/4", "1/2", "1.5x", "2x", " ", ROW_RECORD_BITRATE);
    lv_obj_set_grid_cell(arr->panel[ROW_RECORD_BITRATE], LV_GRID_ALIGN_STRETCH, 0, 6,
                         LV_GRID_ALIGN_STRETCH, ROW_RECORD_BITRATE, 2);
    lv_obj_clear_flag(arr->panel[ROW_RECORD_BITRATE_WRAP], FLAG_SELECTABLE);
    create_btn_group_item(&btn_group_rate_control, cont, 2, _lang("Rate Control"), "CBR", "VBR", "", "", ROW_RATE_CONTROL);
    create_slider_item(&slider_group_vbr_quality, cont, _lang("VBR Quality"), VBR_QUALITY_MAX, g_setting.record.vbr_quality, ROW_VBR_QUALITY);
    create_slider_item(&slider_group_vbr_max_qp, cont, _lang("VBR Max QP"), VBR_MAX_QP_MAX, g_setting.record.vbr_max_qp, ROW_VBR_MAX_QP);
    create_btn_group_item(&btn_group_record_osd, cont, 2, _lang("Record OSD"), _lang("Yes"), _lang("No"), "", "", ROW_RECORD_OSD);
    create_btn_group_item(&btn_group_file_naming, cont, 3, _lang("Naming Scheme"), _lang("Digits"), _lang("Date"), "ELRS", "", ROW_NAMING_SCHEME);
    create_slider_item(&slider_group_stop_delay, cont, _lang("Auto DVR Stop Delay"), STOP_DELAY_MAX, g_setting.record.stop_delay_seconds, ROW_STOP_DELAY);

    create_label_item(cont, _lang("Screen Record"), 1, ROW_SCREEN_RECORD, 1);
    label_screen_record = create_label_item(cont, "", 2, ROW_SCREEN_RECORD, 3);
    update_screen_record_label();

    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    create_label_item(cont, buf, 1, ROW_BACK, 1);

    lv_obj_t *label2 = lv_label_create(cont);
    snprintf(buf, sizeof(buf), "%s.\n%s.\n%s.\n%s.",
             _lang("MP4 must be closed properly or the file corrupts - TS is recommended"),
             _lang("Bitrate: 1/4 and 1/2 save card space; 1.5x and 2x look better but need a fast card"),
             _lang("CBR spends the bitrate evenly, so file size is fixed and quality dips on busy scenes. VBR spends it where the picture is complex: steadier quality, smaller files, higher write peaks"),
             _lang("VBR only - Quality 0-13 (6 default). Max QP is the quality floor: lower gives a better worst case but larger files (40 * suggested)"));
    lv_label_set_text(label2, buf);
    lv_obj_set_style_text_font(label2, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_align(label2, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(label2, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(label2, UI_PAGE_TEXT_PAD, 0);
    lv_label_set_long_mode(label2, LV_LABEL_LONG_WRAP);
    lv_obj_set_grid_cell(label2, LV_GRID_ALIGN_START, 1, 4,
                         LV_GRID_ALIGN_START, ROW_BACK + 1, 3);

    btn_group_set_sel(&btn_group_record_mode, g_setting.record.mode_manual ? 1 : 0);
    btn_group_set_sel(&btn_group_format, g_setting.record.format_ts ? 1 : 0);
    btn_group_set_sel(&btn_group_bitrate_scale, bitrate_setting_to_btn(g_setting.record.bitrate_scale));
    btn_group_set_sel(&btn_group_rate_control, g_setting.record.rc_mode);
    update_vbr_quality_label();
    update_vbr_max_qp_label();
    btn_group_set_sel(&btn_group_record_osd, g_setting.record.osd ? 0 : 1);
    btn_group_set_sel(&btn_group_file_naming, g_setting.record.naming);
    update_stop_delay_label();

    update_visibility();

    return page;
}

static void page_record_exit_stop_delay() {
    lv_obj_add_style(slider_group_stop_delay.slider, &style_silder_main, LV_PART_MAIN);
    app_state_push(APP_STATE_SUBMENU);

    if (stop_delay_changed) {
        ini_putl("record", "stop_delay_seconds", g_setting.record.stop_delay_seconds, SETTING_INI);
        stop_delay_changed = false;
    }
    stop_delay_focused = false;
}

static void page_record_exit_vbr_quality() {
    lv_obj_add_style(slider_group_vbr_quality.slider, &style_silder_main, LV_PART_MAIN);
    app_state_push(APP_STATE_SUBMENU);

    if (vbr_quality_changed) {
        ini_putl("record", "vbr_quality", g_setting.record.vbr_quality, SETTING_INI);
        vbr_quality_changed = false;
    }
    vbr_quality_focused = false;
}

static void page_record_exit_vbr_max_qp() {
    lv_obj_add_style(slider_group_vbr_max_qp.slider, &style_silder_main, LV_PART_MAIN);
    app_state_push(APP_STATE_SUBMENU);

    if (vbr_max_qp_changed) {
        ini_putl("record", "vbr_max_qp", g_setting.record.vbr_max_qp, SETTING_INI);
        vbr_max_qp_changed = false;
    }
    vbr_max_qp_focused = false;
}

static void page_record_exit() {
    if (stop_delay_focused) {
        page_record_exit_stop_delay();
    }
    if (vbr_quality_focused) {
        page_record_exit_vbr_quality();
    }
    if (vbr_max_qp_focused) {
        page_record_exit_vbr_max_qp();
    }
}

static void page_record_on_roller(uint8_t key) {
    int value;

    if (vbr_max_qp_focused) {
        value = g_setting.record.vbr_max_qp;
        if (key == DIAL_KEY_UP && value > 0) {
            value--;
        } else if (key == DIAL_KEY_DOWN && value < VBR_MAX_QP_MAX) {
            value++;
        } else {
            return;
        }
        g_setting.record.vbr_max_qp = value;
        update_vbr_max_qp_label();
        vbr_max_qp_changed = true;
        return;
    }

    if (vbr_quality_focused) {
        value = g_setting.record.vbr_quality;
        if (key == DIAL_KEY_UP && value > VBR_QUALITY_MIN) {
            value--;
        } else if (key == DIAL_KEY_DOWN && value < VBR_QUALITY_MAX) {
            value++;
        } else {
            return;
        }
        g_setting.record.vbr_quality = value;
        update_vbr_quality_label();
        vbr_quality_changed = true;
        return;
    }

    if (!stop_delay_focused)
        return;

    value = g_setting.record.stop_delay_seconds;
    if (key == DIAL_KEY_UP && value > 0) {
        value -= STOP_DELAY_STEP;
    } else if (key == DIAL_KEY_DOWN && value < STOP_DELAY_MAX) {
        value += STOP_DELAY_STEP;
    } else {
        return;
    }

    g_setting.record.stop_delay_seconds = value;
    update_stop_delay_label();
    stop_delay_changed = true;
}

static void page_record_on_click(uint8_t key, int sel) {
    (void)key;

    if (stop_delay_focused) {
        page_record_exit_stop_delay();
        return;
    }
    if (vbr_quality_focused) {
        page_record_exit_vbr_quality();
        return;
    }
    if (vbr_max_qp_focused) {
        page_record_exit_vbr_max_qp();
        return;
    }

    if (sel == ROW_RECORD_MODE) {
        btn_group_toggle_sel(&btn_group_record_mode);
        g_setting.record.mode_manual = btn_group_get_sel(&btn_group_record_mode);
        settings_put_bool("record", "mode_manual", g_setting.record.mode_manual);
    } else if (sel == ROW_RECORD_FORMAT) {
        btn_group_toggle_sel(&btn_group_format);
        g_setting.record.format_ts = btn_group_get_sel(&btn_group_format);
        settings_put_bool("record", "format_ts", g_setting.record.format_ts);
        if (g_setting.record.format_ts)
            ini_puts("record", "type", "ts", REC_CONF);
        else
            ini_puts("record", "type", "mp4", REC_CONF);
    } else if (sel == ROW_RECORD_BITRATE) {
        btn_group_toggle_sel(&btn_group_bitrate_scale);
        g_setting.record.bitrate_scale = bitrate_btn_to_setting[btn_group_get_sel(&btn_group_bitrate_scale)];
        ini_putl("record", "bitrate_scale", g_setting.record.bitrate_scale, SETTING_INI);
    } else if (sel == ROW_RATE_CONTROL) {
        btn_group_toggle_sel(&btn_group_rate_control);
        g_setting.record.rc_mode = btn_group_get_sel(&btn_group_rate_control);
        ini_putl("record", "rc_mode", g_setting.record.rc_mode, SETTING_INI);
        update_visibility();
    } else if (sel == ROW_VBR_QUALITY) {
        vbr_quality_focused = true;
        vbr_quality_changed = false;
        app_state_push(APP_STATE_SUBMENU_ITEM_FOCUSED);
        lv_obj_add_style(slider_group_vbr_quality.slider, &style_silder_select, LV_PART_MAIN);
    } else if (sel == ROW_VBR_MAX_QP) {
        vbr_max_qp_focused = true;
        vbr_max_qp_changed = false;
        app_state_push(APP_STATE_SUBMENU_ITEM_FOCUSED);
        lv_obj_add_style(slider_group_vbr_max_qp.slider, &style_silder_select, LV_PART_MAIN);
    } else if (sel == ROW_RECORD_OSD) {
        btn_group_toggle_sel(&btn_group_record_osd);
        g_setting.record.osd = !btn_group_get_sel(&btn_group_record_osd);
        settings_put_bool("record", "osd", g_setting.record.osd);
    } else if (sel == ROW_NAMING_SCHEME) {
        if (rtc_has_battery() == 0) {
            btn_group_toggle_sel(&btn_group_file_naming);
            g_setting.record.naming = btn_group_get_sel(&btn_group_file_naming);
            ini_putl("record", "naming", g_setting.record.naming, SETTING_INI);
            if (g_setting.record.naming != SETTING_NAMING_ELRS) {
                dvr_clear_race_label();
            }
        }
    } else if (sel == ROW_STOP_DELAY) {
        stop_delay_focused = true;
        stop_delay_changed = false;
        app_state_push(APP_STATE_SUBMENU_ITEM_FOCUSED);
        lv_obj_add_style(slider_group_stop_delay.slider, &style_silder_select, LV_PART_MAIN);
    } else if (sel == ROW_SCREEN_RECORD) {
        screen_record_toggle();
        update_screen_record_label();
    }
}

page_pack_t pp_record = {
    .p_arr = {
        .cur = 0,
        .max = ROW_BACK + 1,
    },
    .name = "Record Option",
    .create = page_record_create,
    .enter = NULL,
    .exit = page_record_exit,
    .on_created = NULL,
    .on_update = page_record_on_update,
    .on_roller = page_record_on_roller,
    .on_click = page_record_on_click,
    .on_right_button = NULL,
};
