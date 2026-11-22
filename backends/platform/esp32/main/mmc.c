// Copyright 2024 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
#include "driver/sdmmc_host.h"
#include "blkcache.h"
#include "esp_check.h"
#include "ff.h"
#include "diskio_impl.h"
#include "sdmmc_cmd.h"

static const char *TAG = "mmc";

#define SD_DMA_BUF_SIZE (32 * 1024)

static sdmmc_card_t card;
static blkcache_handle_t *bc;
FATFS *fatfs;


static DSTATUS dio_init (unsigned char pdrv) {
	return 0;
}

static DSTATUS dio_status (unsigned char pdrv) {
	return 0;
}

static DRESULT dio_read (unsigned char pdrv, unsigned char *buff, uint32_t sector, unsigned count) {
	blkcache_read_sectors(bc, buff, sector, count);
	return RES_OK;
}

static DRESULT dio_write (unsigned char pdrv, const unsigned char *buff, uint32_t sector, unsigned count) {
	blkcache_write_sectors(bc, buff, sector, count);
	return RES_OK;
}

static DRESULT dio_ioctl (unsigned char pdrv, unsigned char cmd, void *buff) {
	if (cmd==CTRL_SYNC) {
		//todo
	} else if (cmd==GET_SECTOR_COUNT) {
		*((DWORD*) buff) = card.csd.capacity;
	} else if (cmd==GET_SECTOR_SIZE) {
		*((DWORD*) buff) = card.csd.sector_size;
	} else if (cmd==GET_BLOCK_SIZE) {
		return RES_ERROR;
	} else if (cmd==CTRL_TRIM) {
		return RES_ERROR;
	} else {
		return RES_ERROR;
	}
	return RES_OK;
}

bool sdcard_mount_blkcache(const char *mountpoint, int files) {
	/* The SD pins (GPIO 39..44, IO domain VDD_IO_5) are powered by LDO channel 4, which the
	 * board's panel_power device has already switched on at 3.3 V before the LCD init. The card
	 * therefore runs with 3.3 V signalling, no SD power control driver here. */
	sdmmc_host_t host = SDMMC_HOST_DEFAULT();
	host.slot = SDMMC_HOST_SLOT_0;
	host.max_freq_khz =	 SDMMC_FREQ_52M;
	/* Writes come from FATFS buffers that are not cache line aligned. Give the driver a fixed
	 * PSRAM bounce buffer (the SDMMC DMA reaches PSRAM if it is aligned to the 128 byte cache
	 * line), otherwise it allocates one per transfer in internal RAM, which is used up by the
	 * LCD bounce buffers and ScummVM. Reads land in the aligned blkcache blocks directly. */
	static void *s_sd_dma_buf;
	if (!s_sd_dma_buf) {
		s_sd_dma_buf = heap_caps_aligned_calloc(128, 1, SD_DMA_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	}
	host.dma_aligned_buffer = s_sd_dma_buf;
	host.unaligned_multi_block_rw_max_chunk_size = SD_DMA_BUF_SIZE / 512;
	const sdmmc_slot_config_t slot_config = {
		/* SD card is connected to Slot 0 pins. Slot 0 uses IO MUX, so not specifying the pins here */
		.cd = SDMMC_SLOT_NO_CD,
		.wp = SDMMC_SLOT_NO_WP,
		.width = 4,
		.flags = 0,
	};
	ESP_ERROR_CHECK(sdmmc_host_init());
	ESP_ERROR_CHECK(sdmmc_host_init_slot(host.slot, &slot_config));
	esp_err_t ret = sdmmc_card_init(&host, &card);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "No SD card or SD card init failed: %s", esp_err_to_name(ret));
		return false;
	}
	sdmmc_card_print_info(stdout, &card);

	blkcache_config_t bcfg={
		.blksize=1024*32,
		.blkcount=16,
		.read_sectors_cb=(read_sectors_t)sdmmc_read_sectors,
		.write_sectors_cb=(write_sectors_t)sdmmc_write_sectors,
		.arg=(void*)&card
	};
	blkcache_init(&bcfg, &bc);

	ff_diskio_impl_t discio={
		.init=dio_init,
		.status=dio_status,
		.read=dio_read,
		.write=dio_write,
		.ioctl=dio_ioctl
	};

	BYTE pdrv = 0xFF;
	if (ff_diskio_get_drive(&pdrv) != ESP_OK) {
		printf("Out of drive numbers\n");
	}
	printf("diskio: pdrv %hhd\n", pdrv);
	ff_diskio_register(pdrv, &discio);

	char drv[3]={'0'+pdrv, ':', 0};
	esp_vfs_fat_conf_t conf = {
		.base_path = mountpoint, //"/sdcard"
		.fat_drive = drv,
		.max_files = files,
	};
	ESP_ERROR_CHECK(esp_vfs_fat_register_cfg(&conf, &fatfs));

	FRESULT fr=f_mount(fatfs, drv, 1);
	if (fr!=FR_OK) {
		printf("f_mount failed %d\n", fr);
		return false;
	}
	return true;
}


