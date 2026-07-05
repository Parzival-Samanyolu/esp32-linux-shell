#pragma once
#include "shell.h"

// Create the JS interpreter lock (call once at boot). The Duktape heap is
// created lazily in PSRAM on first use.
void js_init(void);

// Interactive JavaScript REPL over the shell socket.
void js_repl(shell_ctx_t *ctx);

// Run a .js file from the SD card.
void js_run_file(shell_ctx_t *ctx, const char *fs_path);
