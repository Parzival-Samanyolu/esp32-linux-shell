// ============================================================================
//  ESP32 WROVER-E Linux Shell — entry point
//  Boots NVS, PSRAM heap, SD card (FAT32), Wi-Fi, then the TCP shell server.
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"

#include "config.h"
#include "dmesg.h"
#include "wifi.h"
#include "sdcard.h"
#include "shell.h"
#include "camera.h"
#include "python.h"
#include "js.h"
#include "cc.h"
#include "fileserver.h"
#include "captive.h"
#include "temp.h"

static const char *TAG = "main";

void app_main(void)
{
    dmesg_init();
    dmesg_add("ESP32 Linux Shell v1.0 booting");
    dmesg_add("Xtensa LX6 dual-core @ 240MHz");

    // ---- NVS (needed by Wi-Fi) --------------------------------------------
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    dmesg_add("nvs: initialised");
    temp_init();   // load the saved temperature calibration offset

    // ---- Report PSRAM so we know the 8MB is live --------------------------
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram > 0) {
        dmesg_add("psram: %u KB detected", (unsigned)(psram / 1024));
        ESP_LOGI(TAG, "PSRAM total: %u bytes", (unsigned)psram);
    } else {
        dmesg_add("psram: NOT detected (check CONFIG_SPIRAM)");
        ESP_LOGW(TAG, "PSRAM not detected!");
    }

    // ---- SD card (FAT32 over SPI) -----------------------------------------
    if (sdcard_init() == ESP_OK) {
        dmesg_add("mmc: SD card mounted at %s (FAT32)", SD_MOUNT);
    } else {
        dmesg_add("mmc: SD card mount FAILED — file commands will error");
    }

    // ---- Wi-Fi (blocks until connected, then auto-reconnects) -------------
    // Bring Wi-Fi up BEFORE the camera. Wi-Fi init does bulk NVS/flash reads
    // (which disable the cache); if the camera's DMA is already running it can
    // touch PSRAM during that window and trip a "cache disabled" panic. Doing
    // Wi-Fi first means all those flash reads finish before the camera starts.
    wifi_init_sta();

    // ---- Real clock via SNTP (so `date` and file timestamps are real) -----
    setenv("TZ", "<+03>-3", 1);   // Turkey, UTC+3
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    dmesg_add("sntp: time sync started (pool.ntp.org, TZ=+03)");

    // ---- Camera (OV2640 on the DVP connector) -----------------------------
    if (camera_init() == ESP_OK) {
        int w = 0, h = 0;
        camera_max_resolution(&w, &h);
        dmesg_add("camera: ready (%s %dx%d)", camera_sensor_name(), w, h);
    } else {
        dmesg_add("camera: not available — 'photo'/'stream' will error");
    }

    // ---- Scripting: create the interpreter locks --------------------------
    python_init();
    js_init();
    cc_init();

    // ---- TCP shell server -------------------------------------------------
    shell_start();
    dmesg_add("shell: listening on tcp/%d", SHELL_PORT);

    // ---- Web server + captive portal (always on so /gui works out of the box) --
    fileserver_start();          // port 80: /gui desktop, /dash, file manager
    captive_portal_start();      // joining the hotspot pops open the desktop

    ESP_LOGI(TAG, "System ready. Connect with:  nc <IP> %d", SHELL_PORT);
}
