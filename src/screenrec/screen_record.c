// Screen recorder.
//
// The goggle composites two things onto the panel: the LVGL UI, which the app
// draws into /dev/fb0, and the video layer beneath it (FPV feed, DVR playback,
// Plex). Grabbing fb0 would only get the UI, with a hole where the video is -
// useless for demos. So this uses the display engine's write-back path
// (DISP_CAPTURE_*) on /dev/disp, which hands back the *composited* screen,
// exactly what the wearer sees.
//
// From there the frame is already NV12, which is what the hardware H.264
// encoder wants, so it goes straight into VENC with no colour conversion, and
// the encoded stream is muxed to MP4 by the same ffpack wrapper the DVR uses.
//
// Everything runs on one worker thread. The UI only ever sets a flag.

#define LOG_TAG "screenrec"
#include <log/log.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

// sunxi_display2.h is a kernel header and spells its scalars u32/s32. The
// vendor's hwdisplay.h supplies those typedefs and then includes it, which is
// the sanctioned way in; including sunxi_display2.h directly does not compile.
#include <vo/hwdisplay.h>

#include "mm_common.h"
#include "mm_comm_venc.h"
#include "mm_comm_video.h"
#include "mpi_sys.h"
#include "mpi_venc.h"

// Relative so this does not put src/record on the include path - it holds
// appmsg.h/gogglemsg.h/version.h, which collide with src/player's.
#include "../record/ffpack.h"

#include "driver/beep.h"
#include "screen_record.h"
#include "util/sdcard.h"

#define DISP_DEVICE "/dev/disp"
#define SCREENREC_DIR "/mnt/extsd/DCIM/SCREENREC"
#define SCREENREC_PREFIX "screen_"

// 30fps is the sweet spot: smooth enough to show menu navigation and video
// playback, and the write-back path comfortably keeps up at 720p. The panel
// runs faster than this; frames are sampled, not every panel refresh is kept.
#define SCREENREC_FPS 30
#define SCREENREC_BITRATE (12 * 1024 * 1024)
#define SCREENREC_GOP (SCREENREC_FPS * 2)

// Stop before the card is truly full so the MP4 trailer can still be written.
#define SCREENREC_MIN_FREE_MB 100

#define SCREENREC_MAX_FRAME (1024 * 1024)

// ffpack takes pts in units of 10us.
#define PTS_UNITS_PER_SEC 100000

typedef enum {
    SR_IDLE = 0,
    SR_STARTING,
    SR_RUNNING,
    SR_STOPPING,
} sr_state_t;

static struct {
    pthread_mutex_t mutex;
    pthread_t thread;
    volatile sr_state_t state;
    volatile bool stop_requested;
    bool thread_valid;

    int disp_fd;
    bool capture_started;

    unsigned int phy_addr;
    void *vir_addr;
    unsigned int buf_size;

    VENC_CHN ve_chn;
    bool ve_recving;
    bool mpi_inited;

    FFPack_t *ff;
    int stream_index;

    int width;
    int height;

    time_t started_at;
    char filename[64];
    char error[96];
} sr = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .disp_fd = -1,
    .ve_chn = -1,
};

static void sr_set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sr.error, sizeof(sr.error), fmt, ap);
    va_end(ap);
    LOGE("%s", sr.error);
}

///////////////////////////////////////////////////////////////////////////////
// Display write-back capture
//
// The sunxi disp2 driver takes its ioctl arguments as an unsigned long[4]:
// arg[0] is the screen index, arg[1] onwards are command specific. Passing the
// struct pointer directly (the obvious thing) silently fails.

static int disp_ioctl(int fd, unsigned int cmd, unsigned long a1, unsigned long a2) {
    unsigned long args[4] = {0, a1, a2, 0}; // args[0] = screen 0
    return ioctl(fd, cmd, (void *)args);
}

static bool sr_open_display(void) {
    sr.disp_fd = open(DISP_DEVICE, O_RDWR);
    if (sr.disp_fd < 0) {
        sr_set_error("cannot open %s (%s)", DISP_DEVICE, strerror(errno));
        return false;
    }

    int w = disp_ioctl(sr.disp_fd, DISP_GET_SCN_WIDTH, 0, 0);
    int h = disp_ioctl(sr.disp_fd, DISP_GET_SCN_HEIGHT, 0, 0);
    if (w <= 0 || h <= 0) {
        sr_set_error("display reports no size (%dx%d)", w, h);
        return false;
    }

    // NV12 needs even dimensions in both axes; the chroma plane is half size.
    sr.width = w & ~1;
    sr.height = h & ~1;
    LOGI("screen is %dx%d, capturing %dx%d", w, h, sr.width, sr.height);
    return true;
}

// One write-back capture into our MMZ buffer. Returns false if the display
// engine never completed the capture.
static bool sr_capture_frame(void) {
    struct disp_capture_info info;
    const unsigned int y_size = (unsigned int)sr.width * sr.height;

    memset(&info, 0, sizeof(info));
    // A zero window means "the whole screen", which is what we want.
    info.window.x = 0;
    info.window.y = 0;
    info.window.width = sr.width;
    info.window.height = sr.height;

    info.out_frame.format = DISP_FORMAT_YUV420_SP_UVUV; // NV12
    info.out_frame.size[0].width = sr.width;
    info.out_frame.size[0].height = sr.height;
    info.out_frame.size[1].width = sr.width / 2;
    info.out_frame.size[1].height = sr.height / 2;
    info.out_frame.crop.x = 0;
    info.out_frame.crop.y = 0;
    info.out_frame.crop.width = sr.width;
    info.out_frame.crop.height = sr.height;
    info.out_frame.addr[0] = sr.phy_addr;
    info.out_frame.addr[1] = sr.phy_addr + y_size;

    if (disp_ioctl(sr.disp_fd, DISP_CAPTURE_COMMIT, (unsigned long)&info, 0) != 0) {
        LOGW("capture commit failed (%s)", strerror(errno));
        return false;
    }

    // QUERY returns 0 once the engine has finished writing back. Poll rather
    // than sleeping a fixed time so we track the panel, not the wall clock.
    for (int i = 0; i < 100; i++) {
        if (disp_ioctl(sr.disp_fd, DISP_CAPTURE_QUERY, 0, 0) == 0) {
            // Invalidate so the CPU-side copy the encoder reads is coherent
            // with what the display engine just DMA'd in.
            AW_MPI_SYS_MmzFlushCache(sr.phy_addr, sr.vir_addr, sr.buf_size);
            return true;
        }
        usleep(1000);
    }

    LOGW("capture did not complete in 100ms");
    return false;
}

///////////////////////////////////////////////////////////////////////////////
// Encoder

static bool sr_create_encoder(void) {
    VENC_CHN_ATTR_S attr;

    memset(&attr, 0, sizeof(attr));
    attr.VeAttr.Type = PT_H264;
    attr.VeAttr.MaxKeyInterval = SCREENREC_GOP;
    attr.VeAttr.SrcPicWidth = sr.width;
    attr.VeAttr.SrcPicHeight = sr.height;
    attr.VeAttr.PixelFormat = MM_PIXEL_FORMAT_YUV_SEMIPLANAR_420; // NV12
    attr.VeAttr.Field = VIDEO_FIELD_FRAME;

    attr.VeAttr.AttrH264e.bByFrame = TRUE;
    attr.VeAttr.AttrH264e.Profile = 2; // high
    attr.VeAttr.AttrH264e.PicWidth = sr.width;
    attr.VeAttr.AttrH264e.PicHeight = sr.height;

    attr.RcAttr.mRcMode = VENC_RC_MODE_H264CBR;
    attr.RcAttr.mAttrH264Cbr.mSrcFrmRate = SCREENREC_FPS;
    attr.RcAttr.mAttrH264Cbr.mBitRate = SCREENREC_BITRATE;

    // The DVR owns a channel of its own while it is recording, so take the
    // first free one rather than assuming channel 0.
    for (VENC_CHN chn = 0; chn < VENC_MAX_CHN_NUM; chn++) {
        ERRORTYPE ret = AW_MPI_VENC_CreateChn(chn, &attr);
        if (ret == SUCCESS) {
            sr.ve_chn = chn;
            LOGI("encoder on channel %d", chn);
            return true;
        }
        if (ret != ERR_VENC_EXIST) {
            sr_set_error("encoder rejected %dx%d (0x%x)", sr.width, sr.height, ret);
            return false;
        }
    }

    sr_set_error("no free encoder channel - stop the DVR first");
    return false;
}

// Pull one encoded frame, if the encoder has one ready, and feed it to the
// muxer. Returns false only on a fatal error.
static bool sr_drain_encoder(uint8_t *frame_buf, uint64_t pts_base_us) {
    VENC_STREAM_S stream;
    VENC_PACK_S pack;

    memset(&stream, 0, sizeof(stream));
    memset(&pack, 0, sizeof(pack));
    stream.mPackCount = 1;
    stream.mpPack = &pack;

    // Short timeout: this is a poll, not a wait. The capture loop must keep
    // its cadence whether or not a frame has popped out yet.
    if (AW_MPI_VENC_GetStream(sr.ve_chn, &stream, 50) < 0) {
        return true;
    }

    int len = 0;
    if (pack.mLen0 && pack.mpAddr0) {
        if (len + (int)pack.mLen0 <= SCREENREC_MAX_FRAME) {
            memcpy(frame_buf, pack.mpAddr0, pack.mLen0);
            len += pack.mLen0;
        }
    }
    if (pack.mLen1 && pack.mpAddr1) {
        if (len + (int)pack.mLen1 <= SCREENREC_MAX_FRAME) {
            memcpy(frame_buf + len, pack.mpAddr1, pack.mLen1);
            len += pack.mLen1;
        }
    }

    const bool key = (pack.mDataType.enH264EType == H264E_NALU_ISLICE);
    const uint64_t pts_us = pack.mPTS;

    AW_MPI_VENC_ReleaseStream(sr.ve_chn, &stream);

    if (len <= 0) {
        return true;
    }

    // ffpack wants 10us units, counted from the first frame.
    const uint64_t pts = (pts_us > pts_base_us) ? (pts_us - pts_base_us) / 10 : 0;
    if (ffpack_input(sr.ff, sr.stream_index, frame_buf, len, key, pts) == -5) {
        sr_set_error("write failed - SD card removed?");
        return false;
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// Output file

static bool sr_open_output(void) {
    // Numbered, not timestamped: goggles without an RTC battery boot at epoch
    // and would otherwise produce a pile of identically named files.
    int index = 0;
    char path[160];

    mkdir("/mnt/extsd/DCIM", 0777);
    mkdir(SCREENREC_DIR, 0777);

    for (index = 1; index < 10000; index++) {
        snprintf(path, sizeof(path), "%s/%s%04d.mp4", SCREENREC_DIR, SCREENREC_PREFIX, index);
        if (access(path, F_OK) != 0) {
            break;
        }
    }
    if (index >= 10000) {
        sr_set_error("too many screen recordings on the card");
        return false;
    }
    snprintf(sr.filename, sizeof(sr.filename), "%s%04d.mp4", SCREENREC_PREFIX, index);

    sr.ff = ffpack_openFile(path, NULL);
    if (sr.ff == NULL) {
        sr_set_error("cannot create %s", sr.filename);
        return false;
    }

    // The encoder only has SPS/PPS to hand once the channel exists, and the
    // MP4 header needs them up front.
    VencHeaderData header;
    memset(&header, 0, sizeof(header));
    AW_MPI_VENC_GetH264SpsPpsInfo(sr.ve_chn, &header);

    FFStreamParameters_t param;
    memset(&param, 0, sizeof(param));
    param.mediaType = AVMEDIA_TYPE_VIDEO;
    param.codecId = AV_CODEC_ID_H264;
    param.video.width = sr.width;
    param.video.height = sr.height;
    param.video.fps = SCREENREC_FPS;
    param.spsData = header.pBuffer;
    param.spsLen = header.nLength;

    sr.stream_index = ffpack_newVideoStream(sr.ff, -1, &param);
    if (sr.stream_index < 0) {
        sr_set_error("cannot describe the video stream");
        return false;
    }

    if (ffpack_start(sr.ff) != 0) {
        sr_set_error("cannot write the MP4 header");
        return false;
    }

    LOGI("recording to %s", path);
    return true;
}

static bool sr_disk_has_room(void) {
    struct statvfs vfs;
    if (statvfs("/mnt/extsd", &vfs) != 0) {
        return false;
    }
    const uint64_t free_mb = ((uint64_t)vfs.f_bavail * vfs.f_bsize) / (1024 * 1024);
    return free_mb > SCREENREC_MIN_FREE_MB;
}

///////////////////////////////////////////////////////////////////////////////
// Teardown - every step guarded so a half-built session unwinds cleanly.

static void sr_teardown(void) {
    if (sr.capture_started) {
        disp_ioctl(sr.disp_fd, DISP_CAPTURE_STOP, 0, 0);
        sr.capture_started = false;
    }
    if (sr.disp_fd >= 0) {
        close(sr.disp_fd);
        sr.disp_fd = -1;
    }
    if (sr.ve_recving) {
        AW_MPI_VENC_StopRecvPic(sr.ve_chn);
        sr.ve_recving = false;
    }
    if (sr.ve_chn >= 0) {
        AW_MPI_VENC_ResetChn(sr.ve_chn);
        AW_MPI_VENC_DestroyChn(sr.ve_chn);
        sr.ve_chn = -1;
    }
    if (sr.ff) {
        // Writes the moov atom; without this the file is unplayable.
        ffpack_close(sr.ff);
        sr.ff = NULL;
    }
    if (sr.vir_addr) {
        AW_MPI_SYS_MmzFree(sr.phy_addr, sr.vir_addr);
        sr.vir_addr = NULL;
        sr.phy_addr = 0;
    }
    if (sr.mpi_inited) {
        AW_MPI_SYS_Exit();
        sr.mpi_inited = false;
    }
}

///////////////////////////////////////////////////////////////////////////////
// Worker

static void *sr_worker(void *arg) {
    (void)arg;

    uint8_t *frame_buf = NULL;
    uint64_t pts_base_us = 0;
    bool have_pts_base = false;

    if (!sr_open_display()) {
        goto done;
    }

    MPP_SYS_CONF_S sys_conf;
    memset(&sys_conf, 0, sizeof(sys_conf));
    sys_conf.nAlignWidth = 32;
    AW_MPI_SYS_SetConf(&sys_conf);
    if (AW_MPI_SYS_Init() != SUCCESS) {
        sr_set_error("media system would not start");
        goto done;
    }
    sr.mpi_inited = true;

    sr.buf_size = (unsigned int)sr.width * sr.height * 3 / 2;
    if (AW_MPI_SYS_MmzAlloc_Cached(&sr.phy_addr, &sr.vir_addr, sr.buf_size) != SUCCESS ||
        sr.vir_addr == NULL) {
        sr_set_error("out of contiguous memory for a %dx%d frame", sr.width, sr.height);
        sr.vir_addr = NULL;
        goto done;
    }

    if (!sr_create_encoder()) {
        goto done;
    }
    if (AW_MPI_VENC_StartRecvPic(sr.ve_chn) != SUCCESS) {
        sr_set_error("encoder would not start");
        goto done;
    }
    sr.ve_recving = true;

    if (!sr_open_output()) {
        goto done;
    }

    if (disp_ioctl(sr.disp_fd, DISP_CAPTURE_START, 0, 0) != 0) {
        sr_set_error("this display does not support screen capture");
        goto done;
    }
    sr.capture_started = true;

    frame_buf = malloc(SCREENREC_MAX_FRAME);
    if (frame_buf == NULL) {
        sr_set_error("out of memory");
        goto done;
    }

    sr.started_at = time(NULL);
    sr.state = SR_RUNNING;
    beep_dur(BEEP_SHORT);
    LOGI("screen recording started");

    const uint64_t frame_interval_us = 1000000 / SCREENREC_FPS;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    uint32_t frame_no = 0;
    while (!sr.stop_requested) {
        // Check the card every second rather than every frame - statvfs on a
        // FAT card is not free.
        if ((frame_no % SCREENREC_FPS) == 0 && !sr_disk_has_room()) {
            sr_set_error("SD card full - recording stopped");
            break;
        }
        frame_no++;

        if (sr_capture_frame()) {
            VIDEO_FRAME_INFO_S frame;
            memset(&frame, 0, sizeof(frame));
            frame.VFrame.mWidth = sr.width;
            frame.VFrame.mHeight = sr.height;
            frame.VFrame.mField = VIDEO_FIELD_FRAME;
            frame.VFrame.mPixelFormat = MM_PIXEL_FORMAT_YUV_SEMIPLANAR_420;
            frame.VFrame.mPhyAddr[0] = sr.phy_addr;
            frame.VFrame.mPhyAddr[1] = sr.phy_addr + (unsigned int)sr.width * sr.height;
            frame.VFrame.mpVirAddr[0] = sr.vir_addr;
            frame.VFrame.mpVirAddr[1] = (uint8_t *)sr.vir_addr + (size_t)sr.width * sr.height;
            frame.VFrame.mStride[0] = sr.width;
            frame.VFrame.mStride[1] = sr.width;
            frame.VFrame.mpts = (uint64_t)frame_no * frame_interval_us;

            if (!have_pts_base) {
                pts_base_us = frame.VFrame.mpts;
                have_pts_base = true;
            }

            // A dropped frame here is a hiccup, not a failure; the encoder is
            // simply behind. Skipping keeps the timeline honest.
            AW_MPI_VENC_SendFrame(sr.ve_chn, &frame, 0);
        }

        if (!sr_drain_encoder(frame_buf, pts_base_us)) {
            break;
        }

        // Absolute-deadline pacing so encoder jitter does not accumulate into
        // a slow-motion recording.
        next.tv_nsec += frame_interval_us * 1000;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    // Flush whatever the encoder still holds so the tail of the recording is
    // not lost.
    for (int i = 0; i < SCREENREC_FPS && sr_drain_encoder(frame_buf, pts_base_us); i++) {
    }

    LOGI("screen recording stopped after %u frames", frame_no);

done:
    free(frame_buf);
    sr_teardown();

    pthread_mutex_lock(&sr.mutex);
    sr.state = SR_IDLE;
    sr.started_at = 0;
    sr.stop_requested = false;
    pthread_mutex_unlock(&sr.mutex);

    beep_dur(BEEP_SHORT);
    return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// Public API

bool screen_record_start(void) {
    pthread_mutex_lock(&sr.mutex);

    if (sr.state != SR_IDLE) {
        pthread_mutex_unlock(&sr.mutex);
        return false;
    }

    // Join the previous worker before starting another, or its detached stack
    // leaks on every start/stop cycle.
    if (sr.thread_valid) {
        pthread_join(sr.thread, NULL);
        sr.thread_valid = false;
    }

    sr.error[0] = 0;
    sr.filename[0] = 0;

    if (!sdcard_mounted()) {
        snprintf(sr.error, sizeof(sr.error), "No SD card");
        pthread_mutex_unlock(&sr.mutex);
        LOGE("%s", sr.error);
        return false;
    }
    if (!sr_disk_has_room()) {
        snprintf(sr.error, sizeof(sr.error), "SD card is full");
        pthread_mutex_unlock(&sr.mutex);
        LOGE("%s", sr.error);
        return false;
    }

    sr.stop_requested = false;
    sr.state = SR_STARTING;

    if (pthread_create(&sr.thread, NULL, sr_worker, NULL) != 0) {
        sr.state = SR_IDLE;
        snprintf(sr.error, sizeof(sr.error), "Could not start the recorder");
        pthread_mutex_unlock(&sr.mutex);
        return false;
    }
    sr.thread_valid = true;

    pthread_mutex_unlock(&sr.mutex);
    return true;
}

void screen_record_stop(void) {
    pthread_mutex_lock(&sr.mutex);
    if (sr.state == SR_IDLE) {
        pthread_mutex_unlock(&sr.mutex);
        return;
    }
    sr.stop_requested = true;
    sr.state = SR_STOPPING;
    pthread_mutex_unlock(&sr.mutex);
}

void screen_record_toggle(void) {
    if (screen_record_is_active()) {
        screen_record_stop();
    } else {
        screen_record_start();
    }
}

bool screen_record_is_active(void) {
    return sr.state != SR_IDLE;
}

uint32_t screen_record_elapsed_s(void) {
    const time_t started = sr.started_at;
    if (started == 0) {
        return 0;
    }
    const time_t now = time(NULL);
    return (now > started) ? (uint32_t)(now - started) : 0;
}

const char *screen_record_filename(void) {
    return sr.filename;
}

const char *screen_record_last_error(void) {
    return sr.error;
}
