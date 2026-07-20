#include "sdcard.h"

#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

static int g_sdcard_free_size = 0;

bool sdcard_mounted() {
    struct stat mountpoint;
    struct stat mountpoint_parent;

    // Fetch mountpoint and mountpoint parent dev_id
    if (stat("/mnt/extsd", &mountpoint) == 0 &&
        stat("/mnt", &mountpoint_parent) == 0) {
        // iff the dev ids _do not_ match there is a filesystem mounted
        return (mountpoint.st_dev != mountpoint_parent.st_dev);
    }

    return false;
}

bool sdcard_inserted() {
    return access(SD_BLOCK_DEVICE, F_OK) == 0;
}

void sdcard_update_free_size() {
    struct statfs info;
    if (statfs("/mnt/extsd", &info) == 0)
        g_sdcard_free_size = (info.f_bsize * info.f_bavail) >> 20; // in MB
    else
        g_sdcard_free_size = 0;
}

bool sdcard_is_full() {
    return g_sdcard_free_size < 103;
}

/*
return in MB
*/
int sdcard_free_size() {
    return g_sdcard_free_size;
}
// FAT dirty-bit inspection: FAT16/32 volumes carry a clean-shutdown flag
// in the second FAT entry - set dirty while mounted for writing, cleared
// by a clean unmount (a Mac/PC eject, or fsck itself). If it reads clean,
// a full integrity check adds nothing. Anything unreadable or non-FAT
// reports dirty so the caller stays on the safe path.
bool sdcard_filesystem_dirty() {
    int fd = open("/dev/mmcblk0p1", O_RDONLY);
    if (fd < 0)
        fd = open("/dev/mmcblk0", O_RDONLY);
    if (fd < 0)
        return true;

    bool dirty = true;
    uint8_t bs[512];
    if (read(fd, bs, sizeof(bs)) == (ssize_t)sizeof(bs)) {
        uint16_t const bytes_per_sec = bs[11] | (bs[12] << 8);
        uint16_t const reserved = bs[14] | (bs[15] << 8);
        uint16_t const fatsz16 = bs[22] | (bs[23] << 8);
        if (bytes_per_sec >= 512 && reserved) {
            uint8_t e[8];
            if (pread(fd, e, sizeof(e), (off_t)reserved * bytes_per_sec) == (ssize_t)sizeof(e)) {
                if (fatsz16 == 0) { // FAT32
                    uint32_t const fat1 = e[4] | (e[5] << 8) | (e[6] << 16) | ((uint32_t)e[7] << 24);
                    dirty = !(fat1 & 0x08000000);
                } else { // FAT16
                    uint16_t const fat1 = e[2] | (e[3] << 8);
                    dirty = !(fat1 & 0x8000);
                }
            }
        }
    }
    close(fd);
    return dirty;
}
