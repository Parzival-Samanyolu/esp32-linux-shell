#pragma once
#include "shell.h"

// Fun / toy commands. cowsay + fortune work on any terminal; cmatrix and snake
// need a char-mode (telnet/PuTTY, or `arrows`) terminal and degrade gracefully.
void fun_cowsay(shell_ctx_t *ctx, int argc, char **argv);
void fun_fortune(shell_ctx_t *ctx);
void fun_cmatrix(shell_ctx_t *ctx);
void fun_snake(shell_ctx_t *ctx);
