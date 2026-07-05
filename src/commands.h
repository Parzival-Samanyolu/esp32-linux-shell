#pragma once
#include "shell.h"

// Print the shell banner to a new client.
void cmd_banner(shell_ctx_t *ctx);

// Emit the prompt (root@esp32:<cwd>#).
void cmd_prompt(shell_ctx_t *ctx);

// Build the prompt string (without sending) — used by the line editor.
void cmd_prompt_build(shell_ctx_t *ctx, char *out, size_t n);

// Parse and execute one command line (already CR-stripped).
void cmd_execute(shell_ctx_t *ctx, char *line);

// Resolve a user path argument against ctx->cwd into an absolute FS path.
// Handles absolute ("/..."), home ("~"), relative, "." and "..".
void cmd_resolve_path(shell_ctx_t *ctx, const char *arg, char *out, size_t n);

// Map an absolute FS path (/sdcard/...) to the virtual display path (/...).
void cmd_display_path(const char *fs_path, char *out, size_t n);
