#include "page_craft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../conf/ui.h"

#include "../core/common.hh"
#include "craft/craft_hdz.h"
#include "lang/language.h"
#include "page_common.h"
#include "ui/ui_style.h"

static lv_coord_t col_dsc[] = {UI_RECORD_COLS};
static lv_coord_t row_dsc[] = {UI_RECORD_ROWS};

static lv_obj_t *status_label = NULL;
static lv_obj_t *overlay = NULL;
static lv_obj_t *canvas = NULL;
static lv_color_t *canvas_buf = NULL;
static bool craft_shown = false;

static void create_overlay(void) {
    overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    canvas_buf = malloc(CRAFT_FB_W * CRAFT_FB_H * sizeof(lv_color_t));
    memset(canvas_buf, 0, CRAFT_FB_W * CRAFT_FB_H * sizeof(lv_color_t));

    canvas = lv_canvas_create(overlay);
    lv_canvas_set_buffer(canvas, canvas_buf, CRAFT_FB_W, CRAFT_FB_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(canvas);
}

static lv_obj_t *page_craft_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[512];
    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_VIEW_SIZE);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, UI_PAGE_VIEW_SIZE);

    snprintf(buf, sizeof(buf), "%s:", _lang("MINECRAFT"));
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

    status_label = create_label_item(cont, "", 1, 0, 4);

    lv_obj_t *help = lv_label_create(cont);
    snprintf(buf, sizeof(buf), "%s\n%s\n%s\n%s",
             _lang("Goggle controls: dial turns, dial-click digs"),
             _lang("Right button short: toggle walk, long: place a block"),
             _lang("Long-press the dial to leave the world"),
             _lang("Transmitter play: same controller as DOOM (fire digs, use places, enter jumps)"));
    lv_label_set_text(help, buf);
    lv_obj_set_style_text_font(help, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(help, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(help, UI_PAGE_TEXT_PAD, 0);
    lv_label_set_long_mode(help, LV_LABEL_LONG_WRAP);
    lv_obj_set_grid_cell(help, LV_GRID_ALIGN_START, 1, 4,
                         LV_GRID_ALIGN_START, 1, 4);

    return page;
}

static void page_craft_enter(void) {
    if (!overlay)
        create_overlay();

    if (!craft_hdz_start()) {
        lv_label_set_text(status_label, "#FF0000 Failed to start#");
        return;
    }

    lv_label_set_text(status_label, "#00FF00 Running#");
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay);
    craft_shown = true;
}

static void page_craft_exit(void) {
    craft_shown = false;
    craft_hdz_pause();
    if (overlay)
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    if (status_label)
        lv_label_set_text(status_label, "");
}

static void page_craft_on_update(uint32_t delta_ms) {
    (void)delta_ms;
    if (!craft_shown || !canvas_buf)
        return;
    if (craft_hdz_frame_copy((uint32_t *)canvas_buf))
        lv_obj_invalidate(canvas);
}

static void page_craft_on_roller(uint8_t key) {
    if (!craft_shown)
        return;
    craft_hdz_action((key == DIAL_KEY_UP) ? CRAFT_ACT_TURN_RIGHT : CRAFT_ACT_TURN_LEFT);
}

static void page_craft_on_click(uint8_t key, int sel) {
    (void)key;
    (void)sel;
    if (!craft_shown)
        return;
    craft_hdz_action(CRAFT_ACT_BREAK);
}

static void page_craft_on_right_button(bool is_short) {
    if (!craft_shown)
        return;
    craft_hdz_action(is_short ? CRAFT_ACT_TOGGLE_FORWARD : CRAFT_ACT_PLACE);
}

page_pack_t pp_craft = {
    .p_arr = {
        .cur = 0,
        .max = 0,
    },
    .name = "MINECRAFT",
    .create = page_craft_create,
    .enter = page_craft_enter,
    .exit = page_craft_exit,
    .on_created = NULL,
    .on_update = page_craft_on_update,
    .on_roller = page_craft_on_roller,
    .on_click = page_craft_on_click,
    .on_right_button = page_craft_on_right_button,
};
