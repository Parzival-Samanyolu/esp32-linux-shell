#include "htop.h"
#include "shell.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "temp.h"

#define BAR_WIDTH 20

// Render a [####....] percentage bar into out.
static void make_bar(char *out, size_t n, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = (pct * BAR_WIDTH) / 100;
    size_t p = 0;
    out[p++] = '[';
    for (int i = 0; i < BAR_WIDTH && p < n - 2; i++)
        out[p++] = (i < filled) ? '#' : '.';   // ASCII-safe blocks
    out[p++] = ']';
    out[p] = '\0';
}

static const char *state_str(eTaskState s)
{
    switch (s) {
        case eRunning:   return "RUNNING";
        case eReady:     return "READY";
        case eBlocked:   return "BLOCKED";
        case eSuspended: return "SUSPEND";
        case eDeleted:   return "DELETED";
        default:         return "UNKNOWN";
    }
}

static int cmp_tasks(const void *a, const void *b)
{
    const TaskStatus_t *ta = a, *tb = b;
    return (int)ta->xTaskNumber - (int)tb->xTaskNumber;
}

static void htop_render(shell_ctx_t *ctx)
{
    int sock = ctx->sock;

    // ---- Clear screen + home cursor (ANSI, works over raw TCP) -----------
    shell_printf(sock, "\033[2J\033[H");

    // ---- Uptime -----------------------------------------------------------
    uint64_t us = (uint64_t)esp_timer_get_time();
    uint32_t secs = us / 1000000ULL;
    uint32_t hh = secs / 3600, mm = (secs % 3600) / 60, ss = secs % 60;

    shell_printf(sock, "ESP32 htop - FreeRTOS Task Monitor\r\n");
    shell_printf(sock, "Uptime: %02u:%02u:%02u  |  Cores: 2 @ 240MHz  |  Temp: ~%.0f\xc2\xb0""C  |  Tasks: %u\r\n",
                 (unsigned)hh, (unsigned)mm, (unsigned)ss,
                 temp_read_c(), (unsigned)uxTaskGetNumberOfTasks());
    shell_printf(sock,
        "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81"
        "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81"
        "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\r\n");

    // ---- Memory (real values from the heap allocator) ---------------------
    size_t int_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t int_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_used  = int_total - int_free;
    int    int_pct   = int_total ? (int)((int_used * 100) / int_total) : 0;

    size_t ps_total  = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t ps_free   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t ps_used   = ps_total - ps_free;
    int    ps_pct    = ps_total ? (int)((ps_used * 100) / ps_total) : 0;

    char bar[BAR_WIDTH + 4];
    make_bar(bar, sizeof(bar), int_pct);
    shell_printf(sock, "Internal RAM:  %s  %3d%% used  %uKB/%uKB\r\n",
                 bar, int_pct, (unsigned)(int_used / 1024), (unsigned)(int_total / 1024));

    make_bar(bar, sizeof(bar), ps_pct);
    if (ps_total)
        shell_printf(sock, "PSRAM:         %s  %3d%% used  %uKB/%uKB\r\n",
                     bar, ps_pct, (unsigned)(ps_used / 1024), (unsigned)(ps_total / 1024));
    else
        shell_printf(sock, "PSRAM:         %s   n/a (not detected)\r\n", bar);

    shell_printf(sock,
        "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81"
        "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81"
        "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\r\n");

    // ---- Task table (real FreeRTOS data) ----------------------------------
    shell_printf(sock, " PID  NAME                 CORE  STATE     STACK FREE  PRIORITY\r\n");

    UBaseType_t count = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks = malloc(count * sizeof(TaskStatus_t));
    if (tasks) {
        uint32_t total_runtime = 0;
        count = uxTaskGetSystemState(tasks, count, &total_runtime);
        qsort(tasks, count, sizeof(TaskStatus_t), cmp_tasks);

        for (UBaseType_t i = 0; i < count; i++) {
            TaskStatus_t *t = &tasks[i];

            char core[12];
#if ( configNUM_CORES > 1 ) || defined(CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID)
            if (t->xCoreID == tskNO_AFFINITY)
                snprintf(core, sizeof(core), "*");
            else
                snprintf(core, sizeof(core), "%d", (int)t->xCoreID);
#else
            snprintf(core, sizeof(core), "-");
#endif
            // usStackHighWaterMark = minimum free stack ever seen (bytes).
            shell_printf(sock, "%4u  %-18.18s   %-3s  %-8s  %6uB      %u\r\n",
                         (unsigned)t->xTaskNumber,
                         t->pcTaskName,
                         core,
                         state_str(t->eCurrentState),
                         (unsigned)t->usStackHighWaterMark,
                         (unsigned)t->uxCurrentPriority);
        }
        free(tasks);
    } else {
        shell_printf(sock, " (out of memory reading task list)\r\n");
    }

    shell_printf(sock,
        "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81"
        "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81"
        "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\r\n");
    shell_printf(sock, "Press 'q' to quit\r\n");
}

void htop_run(shell_ctx_t *ctx)
{
    int sock = ctx->sock;

    // Hide cursor for a cleaner display.
    shell_printf(sock, "\033[?25l");

    while (1) {
        htop_render(ctx);

        // Wait ~1s while polling the socket for a 'q' keypress.
        for (int i = 0; i < 10; i++) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };  // 100ms

            int r = select(sock + 1, &rfds, NULL, NULL, &tv);
            if (r > 0 && FD_ISSET(sock, &rfds)) {
                unsigned char c;
                int n = recv(sock, &c, 1, 0);
                if (n <= 0) goto done;                 // disconnected
                if (c == 'q' || c == 'Q' || c == 0x03) goto done;  // q / Ctrl-C
            } else if (r < 0) {
                goto done;
            }
        }
    }

done:
    // Restore cursor, clear screen.
    shell_printf(sock, "\033[?25h\033[2J\033[H");
}
