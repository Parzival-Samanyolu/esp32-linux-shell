#pragma once
#include "shell.h"

// Full-screen nano-like editor over the TCP shell. Uses ANSI escapes and
// negotiates Telnet character-at-a-time mode so arrow keys / single keypresses
// arrive live (works in telnet/PuTTY; over raw nc wrap with `stty raw -echo`).
// Blocks until the user exits (^X); the file is on the SD card.
void editor_run(shell_ctx_t *ctx, const char *fs_path, const char *display_name);
