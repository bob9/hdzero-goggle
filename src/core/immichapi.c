#include "immichapi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <log/log.h>
#include <minIni.h>

#include "core/settings.h"
#include "ui/page_common.h" // SETTING_INI
#include "util/filesystem.h"

#define IMMICH_TOKEN_FILE "/mnt/extsd/immich.txt"

void immich_settings_save(void) {
    ini_puts("immich", "host", g_setting.immich.host, SETTING_INI);
    ini_putl("immich", "port", g_setting.immich.port, SETTING_INI);
    ini_puts("immich", "api_key", g_setting.immich.api_key, SETTING_INI);
}

bool immich_configured(void) {
    return g_setting.immich.host[0] && g_setting.immich.api_key[0];
}

static void immich_headers(char *buf, int size) {
    snprintf(buf, size, "x-api-key: %s\r\n", g_setting.immich.api_key);
}

/** Tiny flat-JSON string lookup - Immich's replies here are small and flat. */
static bool immich_json_str(const char *body, const char *key, char *out, int out_size) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(body, needle);
    if (!p) {
        return false;
    }
    p += strlen(needle);
    while (*p == ' ') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    int w = 0;
    while (*p && *p != '"' && w < out_size - 1) {
        if (*p == '\\' && p[1]) {
            p++;
        }
        out[w++] = *p++;
    }
    out[w] = '\0';
    return true;
}

bool immich_server_reachable(void) {
    char headers[160];
    immich_headers(headers, sizeof(headers));
    char *body = NULL;
    int status = lanhttp_request(g_setting.immich.host, g_setting.immich.port,
                                 "GET", "/api/server/ping", headers, NULL, &body, NULL);
    bool ok = (status == 200) && body && strstr(body, "pong");
    free(body);
    return ok;
}

static char *immich_trim(char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    return s;
}

bool immich_token_from_sdcard(void) {
    FILE *fp = fopen(IMMICH_TOKEN_FILE, "r");
    if (!fp) {
        return false;
    }
    char key_line[IMMICH_KEY_MAX * 2] = "";
    char host_line[PLEX_HOST_MAX * 2] = "";
    const bool got = fgets(key_line, sizeof(key_line), fp) != NULL;
    if (got && !fgets(host_line, sizeof(host_line), fp)) {
        host_line[0] = '\0';
    }
    fclose(fp);
    if (!got) {
        return false;
    }

    char *key = immich_trim(key_line);
    if (!*key) {
        return false;
    }
    snprintf(g_setting.immich.api_key, sizeof(g_setting.immich.api_key), "%s", key);

    char *host = immich_trim(host_line);
    if (*host) {
        char *colon = strrchr(host, ':');
        int port = 2283;
        if (colon) {
            *colon = '\0';
            port = atoi(colon + 1);
        }
        snprintf(g_setting.immich.host, sizeof(g_setting.immich.host), "%s", host);
        g_setting.immich.port = port > 0 ? port : 2283;
    }

    immich_settings_save();
    LOGI("immich: adopted api key%s from %s", *host ? " and server" : "", IMMICH_TOKEN_FILE);
    return true;
}

static void immich_sidecar_path(const char *filepath, char *out, int size) {
    snprintf(out, size, "%s.immich", filepath);
}

bool immich_uploaded(const char *filepath) {
    char sidecar[300];
    immich_sidecar_path(filepath, sidecar, sizeof(sidecar));
    return fs_file_exists(sidecar);
}

static void immich_mark_uploaded(const char *filepath, const char *asset_id) {
    char sidecar[300];
    immich_sidecar_path(filepath, sidecar, sizeof(sidecar));
    FILE *fp = fopen(sidecar, "w");
    if (fp) {
        fprintf(fp, "%s\n", asset_id[0] ? asset_id : "uploaded");
        fclose(fp);
    }
}

int immich_upload(const char *filepath, const char *filename, lan_stream_state_t *state) {
    struct stat st;
    if (stat(filepath, &st) != 0) {
        return IMMICH_ERR_NET;
    }

    char created[40], modified[40];
    struct tm tm_utc;
    gmtime_r(&st.st_mtime, &tm_utc);
    strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%S.000Z", &tm_utc);
    snprintf(modified, sizeof(modified), "%s", created);

    // Stable per-file id so a retried upload is recognized by the server
    char device_asset_id[160];
    snprintf(device_asset_id, sizeof(device_asset_id), "%s-%lld", filename, (long long)st.st_size);
    char device_id[64];
    snprintf(device_id, sizeof(device_id), "hdzero-goggle-%s",
             g_setting.plex.client_id[0] ? g_setting.plex.client_id : "0");

    const char *fields[][2] = {
        {"deviceAssetId", device_asset_id},
        {"deviceId", device_id},
        {"fileCreatedAt", created},
        {"fileModifiedAt", modified},
        {"isFavorite", "false"},
    };

    char headers[160];
    immich_headers(headers, sizeof(headers));

    char *body = NULL;
    int status = lanhttp_post_file(g_setting.immich.host, g_setting.immich.port,
                                   "/api/assets", headers,
                                   fields, 5, "assetData", filename, filepath,
                                   state, &body);
    if (status < 0) {
        free(body);
        return IMMICH_ERR_NET;
    }
    if (status == 401 || status == 403) {
        free(body);
        return IMMICH_ERR_AUTH;
    }
    if (status < 200 || status >= 300) {
        LOGE("immich: upload of %s rejected, status %d: %.120s", filename, status, body ? body : "");
        free(body);
        return IMMICH_ERR_PROTO;
    }

    char asset_status[24] = "";
    char asset_id[80] = "";
    if (body) {
        immich_json_str(body, "status", asset_status, sizeof(asset_status));
        immich_json_str(body, "id", asset_id, sizeof(asset_id));
    }
    free(body);

    immich_mark_uploaded(filepath, asset_id);
    if (strcmp(asset_status, "duplicate") == 0) {
        LOGI("immich: %s already on server (duplicate)", filename);
        return IMMICH_DUP;
    }
    LOGI("immich: %s uploaded (%s)", filename, asset_id);
    return IMMICH_OK;
}
