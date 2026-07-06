#pragma once
#include "shell.h"

// Extra Unix-style file/text commands, split out of commands.c to keep it lean.
// All reuse cmd_resolve_path() for SD-relative paths.
void ux_grep(shell_ctx_t *ctx, int argc, char **argv);   // grep [-i] [-n] <pat> <file...>
void ux_wc(shell_ctx_t *ctx, int argc, char **argv);      // wc <file>
void ux_head(shell_ctx_t *ctx, int argc, char **argv);    // head [-n N] <file>
void ux_tail(shell_ctx_t *ctx, int argc, char **argv);    // tail [-n N] <file>
void ux_cp(shell_ctx_t *ctx, int argc, char **argv);      // cp <src> <dst>
void ux_mv(shell_ctx_t *ctx, int argc, char **argv);      // mv <src> <dst>
void ux_find(shell_ctx_t *ctx, int argc, char **argv);    // find [path] [name-substr]
void ux_du(shell_ctx_t *ctx, int argc, char **argv);      // du [path]
void ux_hexdump(shell_ctx_t *ctx, int argc, char **argv); // hexdump <file>
void ux_history(shell_ctx_t *ctx);                        // history

// Record a command line into the shared recent-history ring (called from
// cmd_execute for every non-empty command).
void ux_history_add(const char *line);
