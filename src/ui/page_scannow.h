#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ui/ui_main_menu.h"
#include <lvgl/lvgl.h>

#define HDZERO_CHANNEL_NUM (g_setting.source.hdzero_band == RACE_BAND ? 12 : 8)
// all-band scan/dial: 12 raceband-mode channels followed by L1-L8
#define SCAN_ALL_CH_NUM    20
#define SCAN_CHANNEL_NUM   (g_setting.source.dial_lowband ? SCAN_ALL_CH_NUM : HDZERO_CHANNEL_NUM)
#define ANALOG_CHANNEL_NUM 48

typedef enum {
    RACE_BAND = 0,
    LOW_BAND = 1,
} band_t;

int scan(void);
int scan_reinit(void);
void autoscan_exit(void);
void page_scannow_set_channel_label(void);

extern page_pack_t pp_scannow;

#ifdef __cplusplus
}
#endif
