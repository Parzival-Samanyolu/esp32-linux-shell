#include "temp.h"
#include "dmesg.h"

#include <stdint.h>
#include "nvs.h"
#include "esp_err.h"

// ESP32-classic internal temperature sensor: a deprecated ROM function that
// returns degrees Fahrenheit off a chip-specific baseline. Noisy and offset by
// ±10-20 °C between chips, so we average it and apply a stored offset.
#if CONFIG_IDF_TARGET_ESP32
extern uint8_t temprature_sens_read(void);
#endif

static float s_offset = 0.0f;   // calibration offset in °C

static float raw_c(void)
{
#if CONFIG_IDF_TARGET_ESP32
    int sum = 0;
    for (int i = 0; i < 16; i++) sum += temprature_sens_read();
    float f = sum / 16.0f;              // averaged raw (Fahrenheit-ish)
    return (f - 32.0f) / 1.8f;          // -> Celsius
#else
    return 0.0f;
#endif
}

float temp_raw_c(void)  { return raw_c(); }
float temp_read_c(void) { return raw_c() + s_offset; }
float temp_offset(void) { return s_offset; }

void temp_init(void)
{
    nvs_handle_t h;
    if (nvs_open("cal", NVS_READONLY, &h) == ESP_OK) {
        int32_t milli = 0;
        if (nvs_get_i32(h, "temp_off", &milli) == ESP_OK) s_offset = milli / 1000.0f;
        nvs_close(h);
    }
}

void temp_calibrate(float real_c)
{
    s_offset = real_c - raw_c();
    nvs_handle_t h;
    if (nvs_open("cal", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "temp_off", (int32_t)(s_offset * 1000.0f));
        nvs_commit(h);
        nvs_close(h);
        dmesg_add("temp: calibrated, offset %+.1f C", s_offset);
    }
}
