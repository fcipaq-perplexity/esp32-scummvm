#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

// Mounts the SD card (SDMMC slot 0, 4 bit) with FAT at mountpoint. False if there is no card.
bool sdcard_mount_blkcache(const char *mountpoint, int files);

#ifdef __cplusplus
}
#endif


