#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "core/settings.h"

#define WIFI_SCAN_ENTRIES_MAX 24

typedef struct {
    char ssid[WIFI_SSID_MAX];
    int rssi;
    bool secured;
    bool saved;
} wifi_scan_entry_t;

/**
 * Remembered client networks (most recently used first).
 */
int wifi_networks_find(const char *ssid);
void wifi_networks_remember(const char *ssid, const char *passwd);
void wifi_networks_forget(int index);

/**
 * wpa_supplicant control interface.
 */
bool wifi_wpa_alive();
bool wifi_reconfigure();
bool wifi_scan_trigger();
int wifi_scan_results(wifi_scan_entry_t *entries, int max);
bool wifi_connected_ssid(char *buffer, int size);

#ifdef __cplusplus
}
#endif
