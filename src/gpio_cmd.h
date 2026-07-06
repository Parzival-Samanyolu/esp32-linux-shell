#pragma once
#include "shell.h"

// GPIO / onboard-LED control. The Deneyap Kart's onboard LED is GPIO4 (blue
// channel of the RGB LED); red=GPIO3 and green=GPIO1 share the UART0 console.
// A pin guard refuses pins used by the camera, SD card or PSRAM.
void gpio_cmd(shell_ctx_t *ctx, int argc, char **argv);   // "gpio read|write|blink|info ..."
void gpio_pwm(shell_ctx_t *ctx, int argc, char **argv);   // "pwm <pin> <0-255>"
void gpio_led(shell_ctx_t *ctx, int argc, char **argv);   // "led on|off|blink"
void gpio_rgb(shell_ctx_t *ctx, int argc, char **argv);   // "rgb <r> <g> <b>"

// Drive/read the onboard LED (GPIO4) — also used by selftest's GPIO loopback.
#define ONBOARD_LED_PIN  4

// Drive GPIO4 high then low and read it back. Returns 1 if the level followed.
int gpio_led_loopback(void);
