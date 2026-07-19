#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <log/log.h>
#include <minIni.h>

#include "core/common.hh"
#include "core/settings.h"
#include "ui/page_common.h"

#define WPA_CTRL_DIR   "/var/log/wpa_supplicant"
#define WPA_CTRL_PATH  WPA_CTRL_DIR "/wlan0"
#define WPA_LOCAL_PATH "/tmp/wifi_ctrl_app"

/**
 * Persist the remembered networks list.
 */
static void wifi_networks_save() {
    char key[32];
    ini_putl("wifi", "net_count", g_setting.wifi.net_count, SETTING_INI);
    for (int i = 0; i < WIFI_NETWORKS_MAX; i++) {
        snprintf(key, sizeof(key), "net%d_ssid", i);
        ini_puts("wifi", key, i < g_setting.wifi.net_count ? g_setting.wifi.networks[i].ssid : NULL, SETTING_INI);
        snprintf(key, sizeof(key), "net%d_passwd", i);
        ini_puts("wifi", key, i < g_setting.wifi.net_count ? g_setting.wifi.networks[i].passwd : NULL, SETTING_INI);
    }
}

int wifi_networks_find(const char *ssid) {
    for (int i = 0; i < g_setting.wifi.net_count; i++) {
        if (0 == strcmp(g_setting.wifi.networks[i].ssid, ssid)) {
            return i;
        }
    }
    return -1;
}

void wifi_networks_remember(const char *ssid, const char *passwd) {
    if (!ssid || !ssid[0]) {
        return;
    }

    int found = wifi_networks_find(ssid);
    if (found == 0 &&
        0 == strcmp(g_setting.wifi.networks[0].passwd, passwd)) {
        return;
    }

    // Drop the existing entry (or the oldest when the list is full)
    if (found < 0) {
        found = g_setting.wifi.net_count;
        if (found >= WIFI_NETWORKS_MAX) {
            found = WIFI_NETWORKS_MAX - 1;
        } else {
            g_setting.wifi.net_count++;
        }
    }

    // Most recently used network lives at the front
    for (int i = found; i > 0; i--) {
        g_setting.wifi.networks[i] = g_setting.wifi.networks[i - 1];
    }
    snprintf(g_setting.wifi.networks[0].ssid, WIFI_SSID_MAX, "%s", ssid);
    snprintf(g_setting.wifi.networks[0].passwd, WIFI_PASSWD_MAX, "%s", passwd);

    wifi_networks_save();
}

void wifi_networks_forget(int index) {
    if (index < 0 || index >= g_setting.wifi.net_count) {
        return;
    }

    for (int i = index; i < g_setting.wifi.net_count - 1; i++) {
        g_setting.wifi.networks[i] = g_setting.wifi.networks[i + 1];
    }
    g_setting.wifi.net_count--;
    memset(&g_setting.wifi.networks[g_setting.wifi.net_count], 0, sizeof(wifi_network_t));

    wifi_networks_save();
}

/**
 * Minimal wpa_supplicant control interface client (UNIX datagram socket).
 */
static int wpa_request(const char *cmd, char *reply, size_t reply_size) {
    static int fd = -1;

    if (fd < 0) {
        fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0) {
            return -1;
        }

        struct sockaddr_un local = {.sun_family = AF_UNIX};
        snprintf(local.sun_path, sizeof(local.sun_path), "%s", WPA_LOCAL_PATH);
        unlink(local.sun_path);
        if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
            close(fd);
            fd = -1;
            return -1;
        }

        struct timeval tv = {.tv_sec = 0, .tv_usec = 300000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    // Drop any stale replies left over from a timed out request
    while (recv(fd, reply, reply_size - 1, MSG_DONTWAIT) > 0)
        ;

    struct sockaddr_un dest = {.sun_family = AF_UNIX};
    snprintf(dest.sun_path, sizeof(dest.sun_path), "%s", WPA_CTRL_PATH);
    if (sendto(fd, cmd, strlen(cmd), 0, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        return -1;
    }

    // Skip unsolicited event messages (they start with '<')
    for (int attempt = 0; attempt < 10; attempt++) {
        ssize_t len = recv(fd, reply, reply_size - 1, 0);
        if (len < 0) {
            return -1;
        }
        if (len > 0 && reply[0] == '<') {
            continue;
        }
        reply[len] = '\0';
        return (int)len;
    }

    return -1;
}

bool wifi_wpa_alive() {
    char reply[16];
    return wpa_request("PING", reply, sizeof(reply)) > 0 &&
           0 == strncmp(reply, "PONG", 4);
}

bool wifi_reconfigure() {
    char reply[16];
    return wpa_request("RECONFIGURE", reply, sizeof(reply)) > 0 &&
           0 == strncmp(reply, "OK", 2);
}

bool wifi_scan_trigger() {
    char reply[16];
    // FAIL-BUSY simply means a scan is already running
    return wpa_request("SCAN", reply, sizeof(reply)) > 0;
}

/**
 * Undo wpa_supplicant printf-style escaping of SSID text.
 */
static void wifi_unescape_ssid(const char *in, char *out, int out_size) {
    int o = 0;
    for (int i = 0; in[i] && o < out_size - 1; i++) {
        if (in[i] == '\\' && in[i + 1]) {
            i++;
            switch (in[i]) {
            case 'x': {
                char hex[3] = {in[i + 1], in[i + 1] ? in[i + 2] : 0, 0};
                if (hex[0] && hex[1]) {
                    out[o++] = (char)strtol(hex, NULL, 16);
                    i += 2;
                }
                break;
            }
            case 'n':
                out[o++] = ' ';
                break;
            case 't':
                out[o++] = ' ';
                break;
            case 'e':
                break;
            default:
                out[o++] = in[i];
                break;
            }
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

int wifi_scan_results(wifi_scan_entry_t *entries, int max) {
    static char reply[4096];
    int len = wpa_request("SCAN_RESULTS", reply, sizeof(reply));
    if (len <= 0) {
        return -1;
    }

    int count = 0;
    char *saveptr = NULL;
    char *line = strtok_r(reply, "\n", &saveptr);

    // First line is the header: bssid / frequency / signal level / flags / ssid
    while ((line = strtok_r(NULL, "\n", &saveptr))) {
        char *fields[5] = {0};
        int nfields = 0;
        char *fsave = NULL;
        for (char *f = strtok_r(line, "\t", &fsave); f && nfields < 5; f = strtok_r(NULL, "\t", &fsave)) {
            fields[nfields++] = f;
        }
        if (nfields < 5 || !fields[4][0]) {
            continue;
        }

        char ssid[WIFI_SSID_MAX];
        wifi_unescape_ssid(fields[4], ssid, sizeof(ssid));
        if (!ssid[0]) {
            continue;
        }

        int rssi = atoi(fields[2]);
        bool secured = strstr(fields[3], "WPA") || strstr(fields[3], "WEP");

        // Keep the strongest signal per unique SSID
        int existing = -1;
        for (int i = 0; i < count; i++) {
            if (0 == strcmp(entries[i].ssid, ssid)) {
                existing = i;
                break;
            }
        }
        if (existing >= 0) {
            if (rssi > entries[existing].rssi) {
                entries[existing].rssi = rssi;
                entries[existing].secured = secured;
            }
            continue;
        }

        if (count >= max) {
            continue;
        }

        snprintf(entries[count].ssid, WIFI_SSID_MAX, "%s", ssid);
        entries[count].rssi = rssi;
        entries[count].secured = secured;
        entries[count].saved = wifi_networks_find(ssid) >= 0;
        count++;
    }

    // Strongest signal first
    for (int i = 1; i < count; i++) {
        wifi_scan_entry_t key = entries[i];
        int j = i - 1;
        while (j >= 0 && entries[j].rssi < key.rssi) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }

    return count;
}

bool wifi_connected_ssid(char *buffer, int size) {
    static char reply[1024];
    if (wpa_request("STATUS", reply, sizeof(reply)) <= 0) {
        return false;
    }

    bool completed = strstr(reply, "wpa_state=COMPLETED") != NULL;
    char *ssid = strstr(reply, "\nssid=");
    if (!completed || !ssid) {
        return false;
    }

    ssid += 6;
    int i = 0;
    while (ssid[i] && ssid[i] != '\n' && i < size - 1) {
        buffer[i] = ssid[i];
        i++;
    }
    buffer[i] = '\0';
    return i > 0;
}
