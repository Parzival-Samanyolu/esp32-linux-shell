#pragma once
#include "shell.h"

// Run the live task/memory monitor. Refreshes ~1Hz, uses ANSI escapes only,
// and returns when the client presses 'q'. Works over raw TCP (netcat/PuTTY).
void htop_run(shell_ctx_t *ctx);
