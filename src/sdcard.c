#include "sdcard.h"
#include "config.h"
#include "dmesg.h"

#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"

static const char *TAG = "sdcard";

static sdmmc_card_t *s_card;
static bool          s_mounted;

esp_err_t sdcard_init(void)
{
    esp_err_t ret;

    // ---- SPI bus ----------------------------------------------------------
    spi_bus_config_t bus = {
        .mosi_io_num     = SD_PIN_MOSI,
        .miso_io_num     = SD_PIN_MISO,
        .sclk_io_num     = SD_PIN_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };
    ret = spi_bus_initialize(SD_SPI_HOST, &bus, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        dmesg_add("mmc: spi bus init failed (%s)", esp_err_to_name(ret));
        return ret;
    }

    // SD cards in SPI mode need pull-ups on the data lines. Enable the ESP32's
    // internal pull-ups so modules without their own resistors still work —
    // without this, an unresponsive MISO yields ESP_ERR_TIMEOUT (0x107).
    gpio_set_pull_mode(SD_PIN_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_PIN_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_PIN_CLK,  GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_PIN_CS,   GPIO_PULLUP_ONLY);

    // ---- SD-over-SPI device ----------------------------------------------
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = SD_PIN_CS;
    dev.host_id = SD_SPI_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    // Breadboard jumper wires are noisy at 20MHz; 10MHz is a safe compromise.
    host.max_freq_khz = 10000;

    // ---- Mount FAT32 ------------------------------------------------------
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,   // don't wipe the user's card
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &dev, &mount, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(ret));
        dmesg_add("mmc: mount failed (%s)", esp_err_to_name(ret));
        s_mounted = false;
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD mounted. Size: %lluMB",
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024));
    dmesg_add("mmc: card %s, %lluMB",
              s_card->cid.name,
              ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024));
    return ESP_OK;
}

bool sdcard_mounted(void) { return s_mounted; }

void sdcard_usage(uint64_t *total_bytes, uint64_t *free_bytes)
{
    *total_bytes = 0;
    *free_bytes  = 0;
    if (!s_mounted) return;

    FATFS *fs;
    DWORD  free_clusters;
    // "0:" is the first mounted FATFS volume.
    if (f_getfree("0:", &free_clusters, &fs) == FR_OK) {
        uint64_t sector = fs->ssize;
        uint64_t total_sectors = (uint64_t)(fs->n_fatent - 2) * fs->csize;
        uint64_t free_sectors  = (uint64_t)free_clusters * fs->csize;
        *total_bytes = total_sectors * sector;
        *free_bytes  = free_sectors * sector;
    }
}
