#pragma once
#include "shell.h"

// Create the interpreter lock (call once at boot, before any client). The
// pocketpy VM itself is initialised lazily on first use.
void python_init(void);

// Interactive Python REPL over the shell socket (blocks until exit()/Ctrl-D).
void python_repl(shell_ctx_t *ctx);

// Run a .py file from the SD card.
void python_run_file(shell_ctx_t *ctx, const char *fs_path);
