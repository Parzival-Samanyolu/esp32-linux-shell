#pragma once
#include "shell.h"

// Create the C-interpreter lock (call once at boot).
void cc_init(void);

// Interpret a .c file from the SD card (PicoC). Runs main() if present.
void cc_run_file(shell_ctx_t *ctx, const char *fs_path);
