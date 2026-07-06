#pragma once
#include "shell.h"

// `qr [text]` — print a scannable QR code in the terminal. With no argument it
// encodes the Wi-Fi hotspot login so a phone can scan it to join.
void do_qr(shell_ctx_t *ctx, int argc, char **argv);
