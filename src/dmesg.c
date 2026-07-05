#include "dmesg.h"
#include "shell.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

#define DMESG_LINES   64
#define DMESG_LINELEN 128

static char             s_buf[DMESG_LINES][DMESG_LINELEN];
static int              s_head;   // next write slot
static int              s_count;  // number of valid lines
static SemaphoreHandle_t s_lock;

void dmesg_init(void)
{
    s_head = 0;
    s_count = 0;
    s_lock = xSemaphoreCreateMutex();
}

void dmesg_add(const char *fmt, ...)
{
    if (!s_lock) return;

    // Seconds.microseconds since boot, like a real kernel log prefix.
    uint64_t us   = (uint64_t)esp_timer_get_time();
    uint32_t sec  = (uint32_t)(us / 1000000ULL);
    uint32_t frac = (uint32_t)(us % 1000000ULL);

    char msg[DMESG_LINELEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_buf[s_head], DMESG_LINELEN, "[%5u.%06u] %.100s",
             (unsigned)sec, (unsigned)frac, msg);
    s_head = (s_head + 1) % DMESG_LINES;
    if (s_count < DMESG_LINES) s_count++;
    xSemaphoreGive(s_lock);
}

void dmesg_dump(int sock)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int start = (s_head - s_count + DMESG_LINES) % DMESG_LINES;
    for (int i = 0; i < s_count; i++) {
        int idx = (start + i) % DMESG_LINES;
        shell_printf(sock, "%s\r\n", s_buf[idx]);
    }
    xSemaphoreGive(s_lock);
}
