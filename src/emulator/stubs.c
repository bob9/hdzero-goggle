#include "media.h"

#include "adec2ao.h"
#include "awdmx.h"
#include "vdec2vo.h"
#include "wav_test.h"

typedef struct
{
    Vdec2VoContext_t *vv;
    Adec2AoContext_t *aa;
    AwdmxContext_t *dmx;
    uint32_t state;
    pthread_mutex_t mutex;
    int playingTime; // ms
} PlayContext_t;

static int play_start(PlayContext_t *playCtx) { return -1; }
static int play_pause(PlayContext_t *playCtx) { return -1; }
static int play_stop(PlayContext_t *playCtx) { return -1; }
static int play_seekto(PlayContext_t *playCtx, int seekTime) { return -1; }
static void play_moveStatus(PlayContext_t *playCtx) {}
static void play_onDemuxEof(void *context) {}
void *thread_media(void *params) { return NULL; }
void media_control(media_t *media, player_cmd_t *cmd) {}
media_t *media_instantiate(char *filename, notify_cb_t notify) { return NULL; }
void media_exit(media_t *media) {}
int media_retimed_hz(media_t *media) { return 0; }
int wav_test_play(const char *path) { return -1; }
int wav_test_record(const char *path, int seconds) { return -1; }

// Screen recording needs the display engine's write-back path and the hardware
// encoder; neither exists on a desktop host.
#include "screenrec/screen_record.h"

bool screen_record_start(void) { return false; }
void screen_record_stop(void) {}
void screen_record_toggle(void) {}
bool screen_record_is_active(void) { return false; }
uint32_t screen_record_elapsed_s(void) { return 0; }
const char *screen_record_filename(void) { return ""; }
const char *screen_record_last_error(void) { return "Not available in the emulator"; }

#if HDZGOGGLE
void Display_HDZ(int mode, int is_43) {}
#elif HDZBOXPRO
void Display_720P90(int mode) {}
void Display_720P60_50(int mode, uint8_t is_43) {}
void Display_1080P30(int mode) {}
void Display_1080P24(int mode) {}
#endif
