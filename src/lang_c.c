#include "cc.h"
#include "shell.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// PicoC's platform.h requires a host to be selected; match the component's
// UNIX_HOST selection so the Picoc struct layout is consistent.
#define UNIX_HOST 1
// The PicoC headers trip a couple of pedantic warnings we build with -Werror;
// silence them just around these includes (third-party headers).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcomment"
#include "picoc.h"
#include "picoc_esp32.h"
#pragma GCC diagnostic pop

// PicoC uses a single global VM state per run; serialise access.
static SemaphoreHandle_t s_lock;
static int               s_sock = -1;

// Per-character output sink; translate '\n' -> '\r\n' for terminals.
static void cc_putc(char c)
{
    if (s_sock < 0) return;
    if (c == '\n') shell_send_all(s_sock, "\r\n", 2);
    else           shell_send_all(s_sock, &c, 1);
}

void cc_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

void cc_run_file(shell_ctx_t *ctx, const char *fs_path)
{
    if (!s_lock) cc_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sock = ctx->sock;
    picoc_set_output(cc_putc);

    // The Picoc state is large; keep it off the (small) task stack.
    Picoc *pc = malloc(sizeof(Picoc));
    if (!pc) {
        shell_printf(ctx->sock, "cc: out of memory\r\n");
        s_sock = -1;
        xSemaphoreGive(s_lock);
        return;
    }

    // ~96 KB interpreter heap — lands in PSRAM (over the internal-alloc threshold).
    PicocInitialize(pc, 96 * 1024);
    PicocIncludeAllSystemHeaders(pc);

    if (PicocPlatformSetExitPoint(pc) == 0) {   // setjmp: errors longjmp back here
        PicocPlatformScanFile(pc, fs_path);     // parse + run top-level (defines main)
        PicocCallMain(pc, 0, NULL);             // then call main()
        shell_printf(ctx->sock, "\r\n[program exited with %d]\r\n", pc->PicocExitValue);
    } else {
        shell_printf(ctx->sock, "\r\n[program aborted]\r\n");
    }

    PicocCleanup(pc);
    free(pc);
    s_sock = -1;
    xSemaphoreGive(s_lock);
}
