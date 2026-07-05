#include "python.h"
#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pocketpy.h"

// pocketpy exposes a single global VM and is not thread-safe, so all execution
// is serialised behind one mutex and one "current output socket".
static SemaphoreHandle_t s_lock;
static bool              s_inited;
static int               s_sock = -1;

// pocketpy prints with '\n'; translate to '\r\n' for terminals.
static void py_print_cb(const char *s)
{
    if (s_sock < 0 || !s) return;
    const char *p = s;
    while (*p) {
        const char *nl = strchr(p, '\n');
        if (!nl) { shell_send_all(s_sock, p, strlen(p)); break; }
        if (nl > p) shell_send_all(s_sock, p, nl - p);
        shell_send_all(s_sock, "\r\n", 2);
        p = nl + 1;
    }
}

void python_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

static void ensure_vm(void)
{
    if (!s_inited) {
        py_initialize();
        py_callbacks()->print = py_print_cb;
        s_inited = true;
    }
}

// Execute one chunk with the given compile mode, routing output to `sock`.
static void py_run(int sock, const char *src, const char *fname, enum py_CompileMode mode)
{
    if (!s_lock) python_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ensure_vm();
    s_sock = sock;
    bool ok = py_exec(src, fname, mode, NULL);
    if (!ok) {
        py_printexc();
        py_clearexc(NULL);
    }
    s_sock = -1;
    xSemaphoreGive(s_lock);
}

void python_run_file(shell_ctx_t *ctx, const char *fs_path)
{
    FILE *f = fopen(fs_path, "rb");
    if (!f) { shell_printf(ctx->sock, "python: %s: not found\r\n", fs_path); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 256 * 1024) { fclose(f); shell_printf(ctx->sock, "python: file too large\r\n"); return; }
    char *src = malloc(sz + 1);
    if (!src) { fclose(f); shell_printf(ctx->sock, "python: out of memory\r\n"); return; }
    fread(src, 1, sz, f);
    src[sz] = '\0';
    fclose(f);
    py_run(ctx->sock, src, fs_path, EXEC_MODE);
    free(src);
}

void python_repl(shell_ctx_t *ctx)
{
    shell_printf(ctx->sock,
        "pocketpy REPL (Python 3 subset). Type code; 'exit()' or Ctrl-D to quit.\r\n");

    char line[512];
    char block[4096];
    while (1) {
        int n = shell_prompt_read(ctx, ">>> ", line, sizeof(line));
        if (n < 0) break;                                  // EOF / disconnect
        if (n == 0) continue;
        if (!strcmp(line, "exit()") || !strcmp(line, "exit") || !strcmp(line, "quit()"))
            break;

        // A line ending in ':' opens a block — collect until a blank line.
        int len = (int)strlen(line);
        if (len > 0 && line[len - 1] == ':') {
            int bl = snprintf(block, sizeof(block), "%s\n", line);
            while (1) {
                int m = shell_prompt_read(ctx, "... ", line, sizeof(line));
                if (m <= 0) break;                         // blank line ends the block
                bl += snprintf(block + bl, sizeof(block) - bl, "%s\n", line);
                if (bl >= (int)sizeof(block) - 1) break;
            }
            py_run(ctx->sock, block, "<stdin>", EXEC_MODE);
        } else {
            // SINGLE_MODE auto-prints expression results, like the real REPL.
            py_run(ctx->sock, line, "<stdin>", SINGLE_MODE);
        }
    }
    shell_printf(ctx->sock, "\r\n");
}
