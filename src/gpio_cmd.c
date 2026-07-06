#include "gpio_cmd.h"
#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_rom_sys.h"

// PWM uses LEDC timer 1 / channel 2 in LOW-speed mode. The camera driver uses
// LEDC timer 0 / channel 0 (high-speed) for its XCLK, so these never collide.
#define PWM_TIMER    LEDC_TIMER_1
#define PWM_CHANNEL  LEDC_CHANNEL_2
#define PWM_MODE     LEDC_LOW_SPEED_MODE

// ---------------------------------------------------------------------------
//  Pin classification / guard
// ---------------------------------------------------------------------------
// Returns a human role for a pin, and whether it's usable for I/O.
static const char *pin_role(int p)
{
    switch (p) {
        case 5: case 18: case 19: case 21: case 22: case 23:
        case 25: case 26: case 32: case 33:
        case 34: case 35: case 36: case 39: return "camera";
        case 0:  return "sd-cs / strap";
        case 13: return "sd-mosi";
        case 14: return "sd-clk";
        case 27: return "sd-miso";
        case 16: case 17: return "psram";
        case 1:  return "uart-tx";
        case 3:  return "uart-rx";
        case 2: case 12: case 15: return "strapping";
        case 4:  return "LED (onboard blue)";
        default: return "free";
    }
}

static bool pin_is_camera_sd_psram(int p)
{
    const char *r = pin_role(p);
    return strcmp(r, "camera") == 0 || strncmp(r, "sd-", 3) == 0 || strcmp(r, "psram") == 0;
}

// Classify a pin for a requested use. rc: 0 = ok, 1 = ok-but-warn, -1 = blocked.
// Fills `why` with the reason (blocked) or warning text.
static int pin_check(int p, bool need_output, char *why, size_t whyn)
{
    if (!GPIO_IS_VALID_GPIO(p)) { snprintf(why, whyn, "GPIO%d is not a valid pin", p); return -1; }
    if (pin_is_camera_sd_psram(p)) {
        snprintf(why, whyn, "GPIO%d is in use by the %s — refused", p, pin_role(p));
        return -1;
    }
    if (need_output && !GPIO_IS_VALID_OUTPUT_GPIO(p)) {
        snprintf(why, whyn, "GPIO%d is input-only (34-39 can't drive output)", p);
        return -1;
    }
    if (p == 1 || p == 3) {
        snprintf(why, whyn, "GPIO%d is the UART0 console — driving it garbles the serial monitor", p);
        return 1;
    }
    if (p == 0 || p == 2 || p == 12 || p == 15) {
        snprintf(why, whyn, "GPIO%d is a boot-strapping pin — safe to toggle now, but don't hold it at boot", p);
        return 1;
    }
    why[0] = '\0';
    return 0;
}

// ---------------------------------------------------------------------------
//  low-level helpers
// ---------------------------------------------------------------------------
static void pin_output(int p)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << p,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

static void pin_input(int p)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << p,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

// Exposed for selftest: drive GPIO4 high then low and read it back. The pin
// must be INPUT_OUTPUT (not plain OUTPUT) for gpio_get_level() to reflect the
// driven level on the ESP32.
int gpio_led_loopback(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << ONBOARD_LED_PIN,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(ONBOARD_LED_PIN, 1);
    esp_rom_delay_us(50);
    int hi = gpio_get_level(ONBOARD_LED_PIN);
    gpio_set_level(ONBOARD_LED_PIN, 0);
    esp_rom_delay_us(50);
    int lo = gpio_get_level(ONBOARD_LED_PIN);
    return (hi == 1 && lo == 0) ? 1 : 0;
}

// ---------------------------------------------------------------------------
//  gpio <sub> ...
// ---------------------------------------------------------------------------
static void gpio_info(shell_ctx_t *ctx)
{
    shell_printf(ctx->sock, "GPIO map (Deneyap Kart / WROVER-E):\r\n");
    shell_printf(ctx->sock, "  PIN  ROLE\r\n");
    static const int pins[] = {0,1,2,3,4,5,12,13,14,15,16,17,18,19,21,22,23,25,26,27,32,33,34,35,36,39};
    for (unsigned i = 0; i < sizeof(pins)/sizeof(pins[0]); i++) {
        int p = pins[i];
        const char *r = pin_role(p);
        const char *tag = (strcmp(r,"free")==0) ? "  <- free" :
                          (strcmp(r,"LED (onboard blue)")==0) ? "  <- onboard LED" : "";
        shell_printf(ctx->sock, "  %2d   %s%s\r\n", p, r, tag);
    }
    shell_printf(ctx->sock,
        "Free & safe for your own use: GPIO4 (onboard LED). Camera/SD/PSRAM pins are refused.\r\n"
        "Try: led blink   |   gpio write 4 1   |   pwm 4 128   |   gpio read 4\r\n");
}

void gpio_cmd(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 2) {
        shell_printf(ctx->sock,
            "usage: gpio info | read <pin> | write <pin> <0|1> | blink [pin] [count] [ms]\r\n");
        return;
    }
    const char *sub = argv[1];
    char why[128];

    if (!strcmp(sub, "info")) { gpio_info(ctx); return; }

    if (!strcmp(sub, "read")) {
        if (argc < 3) { shell_printf(ctx->sock, "usage: gpio read <pin>\r\n"); return; }
        int p = atoi(argv[2]);
        int rc = pin_check(p, false, why, sizeof(why));
        if (rc < 0) { shell_printf(ctx->sock, "%s\r\n", why); return; }
        if (rc > 0) shell_printf(ctx->sock, "warning: %s\r\n", why);
        pin_input(p);
        int v = gpio_get_level(p);
        shell_printf(ctx->sock, "GPIO%d = %d\r\n", p, v);
        return;
    }

    if (!strcmp(sub, "write")) {
        if (argc < 4) { shell_printf(ctx->sock, "usage: gpio write <pin> <0|1>\r\n"); return; }
        int p = atoi(argv[2]), v = atoi(argv[3]) ? 1 : 0;
        int rc = pin_check(p, true, why, sizeof(why));
        if (rc < 0) { shell_printf(ctx->sock, "%s\r\n", why); return; }
        if (rc > 0) shell_printf(ctx->sock, "warning: %s\r\n", why);
        pin_output(p);
        gpio_set_level(p, v);
        shell_printf(ctx->sock, "GPIO%d <- %d%s\r\n", p, v,
                     p == ONBOARD_LED_PIN ? "  (onboard LED)" : "");
        return;
    }

    if (!strcmp(sub, "blink")) {
        int p     = (argc >= 3) ? atoi(argv[2]) : ONBOARD_LED_PIN;
        int count = (argc >= 4) ? atoi(argv[3]) : 5;
        int ms    = (argc >= 5) ? atoi(argv[4]) : 200;
        if (count < 1) count = 1;
        if (count > 100) count = 100;
        if (ms < 20) ms = 20;
        if (ms > 2000) ms = 2000;
        int rc = pin_check(p, true, why, sizeof(why));
        if (rc < 0) { shell_printf(ctx->sock, "%s\r\n", why); return; }
        if (rc > 0) shell_printf(ctx->sock, "warning: %s\r\n", why);
        pin_output(p);
        shell_printf(ctx->sock, "Blinking GPIO%d %d times (%dms)...\r\n", p, count, ms);
        for (int i = 0; i < count; i++) {
            gpio_set_level(p, 1); vTaskDelay(pdMS_TO_TICKS(ms));
            gpio_set_level(p, 0); vTaskDelay(pdMS_TO_TICKS(ms));
        }
        shell_printf(ctx->sock, "done.\r\n");
        return;
    }

    shell_printf(ctx->sock, "gpio: unknown subcommand '%s'\r\n", sub);
}

// ---------------------------------------------------------------------------
//  pwm <pin> <0-255>
// ---------------------------------------------------------------------------
void gpio_pwm(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 3) { shell_printf(ctx->sock, "usage: pwm <pin> <0-255>\r\n"); return; }
    int p = atoi(argv[1]);
    int duty = atoi(argv[2]);
    if (duty < 0) duty = 0;
    if (duty > 255) duty = 255;

    char why[128];
    int rc = pin_check(p, true, why, sizeof(why));
    if (rc < 0) { shell_printf(ctx->sock, "%s\r\n", why); return; }
    if (rc > 0) shell_printf(ctx->sock, "warning: %s\r\n", why);

    ledc_timer_config_t tcfg = {
        .speed_mode      = PWM_MODE,
        .timer_num       = PWM_TIMER,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tcfg);
    ledc_channel_config_t ccfg = {
        .gpio_num   = p,
        .speed_mode = PWM_MODE,
        .channel    = PWM_CHANNEL,
        .timer_sel  = PWM_TIMER,
        .duty       = duty,
        .hpoint     = 0,
    };
    ledc_channel_config(&ccfg);
    ledc_set_duty(PWM_MODE, PWM_CHANNEL, duty);
    ledc_update_duty(PWM_MODE, PWM_CHANNEL);
    shell_printf(ctx->sock, "PWM GPIO%d = %d/255 (%d%%)%s\r\n", p, duty, duty * 100 / 255,
                 p == ONBOARD_LED_PIN ? "  (onboard LED brightness)" : "");
}

// ---------------------------------------------------------------------------
//  led on|off|blink  — convenience for the onboard LED (GPIO4)
// ---------------------------------------------------------------------------
void gpio_led(shell_ctx_t *ctx, int argc, char **argv)
{
    const char *a = (argc >= 2) ? argv[1] : "blink";
    pin_output(ONBOARD_LED_PIN);
    if (!strcmp(a, "on")) {
        gpio_set_level(ONBOARD_LED_PIN, 1);
        shell_printf(ctx->sock, "Onboard LED (GPIO4) ON\r\n");
    } else if (!strcmp(a, "off")) {
        gpio_set_level(ONBOARD_LED_PIN, 0);
        shell_printf(ctx->sock, "Onboard LED (GPIO4) OFF\r\n");
    } else {
        shell_printf(ctx->sock, "Blinking onboard LED (GPIO4) 5x...\r\n");
        for (int i = 0; i < 5; i++) {
            gpio_set_level(ONBOARD_LED_PIN, 1); vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(ONBOARD_LED_PIN, 0); vTaskDelay(pdMS_TO_TICKS(200));
        }
        shell_printf(ctx->sock, "done.\r\n");
    }
}

// ---------------------------------------------------------------------------
//  rgb <r> <g> <b>  — drive all three onboard RGB channels (opt-in)
// ---------------------------------------------------------------------------
void gpio_rgb(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 4) {
        shell_printf(ctx->sock, "usage: rgb <r> <g> <b>   (each 0 or 1)\r\n");
        return;
    }
    int r = atoi(argv[1]) ? 1 : 0, g = atoi(argv[2]) ? 1 : 0, b = atoi(argv[3]) ? 1 : 0;
    shell_printf(ctx->sock,
        "note: red=GPIO3 and green=GPIO1 are the UART0 console pins; the serial\r\n"
        "monitor may show noise while they're driven. Blue=GPIO4 is safe.\r\n");
    pin_output(3); pin_output(1); pin_output(ONBOARD_LED_PIN);
    gpio_set_level(3, r);
    gpio_set_level(1, g);
    gpio_set_level(ONBOARD_LED_PIN, b);
    shell_printf(ctx->sock, "RGB = (%d,%d,%d)\r\n", r, g, b);
}
