#pragma once
#include "esp_err.h"

// Initialise the SPI bus and mount the SD card as FAT32 at SD_MOUNT.
esp_err_t sdcard_init(void);

// True if the card mounted successfully.
bool sdcard_mounted(void);

// Total / free bytes on the card (0 if unmounted).
void sdcard_usage(uint64_t *total_bytes, uint64_t *free_bytes);
