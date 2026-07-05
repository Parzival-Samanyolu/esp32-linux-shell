#include "js.h"
#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

#include "duktape.h"

// One Duktape heap, serialised behind a mutex (Duktape isn't thread-safe).
static SemaphoreHandle_t s_lock;
static duk_context      *s_ctx;
static int               s_sock = -1;

// ---- Duktape heap in PSRAM (keeps scarce internal DRAM free) ---------------
static void *js_alloc(void *u, duk_size_t n)
{ return n ? heap_caps_malloc(n, MALLOC_CAP_SPIRAM) : NULL; }
static void *js_realloc(void *u, void *p, duk_size_t n)
{
    if (n == 0) { free(p); return NULL; }
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
}
static void js_free(void *u, void *p) { free(p); }
static void js_fatal(void *u, const char *msg)
{
    if (s_sock >= 0) shell_printf(s_sock, "\r\njs: FATAL: %s\r\n", msg ? msg : "?");
}

// ---- print() / console.log(): route to the socket, '\n' -> '\r\n' ----------
static duk_ret_t js_print(duk_context *ctx)
{
    int n = duk_get_top(ctx);
    for (int i = 0; i < n; i++) {
        if (i && s_sock >= 0) shell_send_all(s_sock, " ", 1);
        const char *s = duk_safe_to_string(ctx, i);
        if (s && s_sock >= 0) shell_send_all(s_sock, s, strlen(s));
    }
    if (s_sock >= 0) shell_send_all(s_sock, "\r\n", 2);
    return 0;
}

static void ensure_ctx(void)
{
    if (s_ctx) return;
    s_ctx = duk_create_heap(js_alloc, js_realloc, js_free, NULL, js_fatal);
    if (!s_ctx) return;
    // Global print()
    duk_push_c_function(s_ctx, js_print, DUK_VARARGS);
    duk_put_global_string(s_ctx, "print");
    // console.log()
    duk_push_object(s_ctx);
    duk_push_c_function(s_ctx, js_print, DUK_VARARGS);
    duk_put_prop_string(s_ctx, -2, "log");
    duk_put_global_string(s_ctx, "console");
}

void js_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

// Evaluate a chunk; if `repl`, echo the result value like a JS console.
static void js_run(int sock, const char *code, bool repl)
{
    if (!s_lock) js_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ensure_ctx();
    s_sock = sock;
    if (!s_ctx) {
        shell_printf(sock, "js: could not create heap\r\n");
    } else if (duk_peval_string(s_ctx, code) != 0) {
        shell_printf(sock, "%s\r\n", duk_safe_to_string(s_ctx, -1));   // error text
        duk_pop(s_ctx);
    } else {
        if (repl) {
            const char *r = duk_safe_to_string(s_ctx, -1);
            if (r && strcmp(r, "undefined") != 0) shell_printf(sock, "%s\r\n", r);
        }
        duk_pop(s_ctx);
    }
    s_sock = -1;
    xSemaphoreGive(s_lock);
}

void js_run_file(shell_ctx_t *ctx, const char *fs_path)
{
    FILE *f = fopen(fs_path, "rb");
    if (!f) { shell_printf(ctx->sock, "js: %s: not found\r\n", fs_path); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 256 * 1024) { fclose(f); shell_printf(ctx->sock, "js: file too large\r\n"); return; }
    char *src = malloc(sz + 1);
    if (!src) { fclose(f); shell_printf(ctx->sock, "js: out of memory\r\n"); return; }
    fread(src, 1, sz, f);
    src[sz] = '\0';
    fclose(f);
    js_run(ctx->sock, src, false);
    free(src);
}

void js_repl(shell_ctx_t *ctx)
{
    shell_printf(ctx->sock, "Duktape JavaScript REPL. Type JS; 'exit' or Ctrl-D to quit.\r\n");
    char line[512];
    while (1) {
        int n = shell_prompt_read(ctx, "js> ", line, sizeof(line));
        if (n < 0) break;
        if (n == 0) continue;
        if (!strcmp(line, "exit") || !strcmp(line, "exit()") || !strcmp(line, "quit()")) break;
        js_run(ctx->sock, line, true);
    }
    shell_printf(ctx->sock, "\r\n");
}
