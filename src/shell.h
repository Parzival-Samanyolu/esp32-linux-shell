#pragma once
#include <stddef.h>

// Per-client shell state, passed to every command handler.
typedef struct {
    int  sock;            // connected TCP socket
    char cwd[256];        // absolute path under SD_MOUNT (e.g. "/sdcard/foo")
    int  id;              // client index (for task naming)
    int  want_close;      // set by `exit`/`logout` to end the session
    int  char_mode;       // 1 if client negotiated character-at-a-time (telnet/PuTTY)
} shell_ctx_t;

// Start the TCP listener task on SHELL_PORT.
void shell_start(void);

// Reliable send of a raw buffer to a socket (loops on partial writes).
// Returns 0 on success, -1 on error/disconnect.
int shell_send_all(int sock, const char *data, size_t len);

// printf to a socket. Returns bytes sent, or -1 on error.
int shell_printf(int sock, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Read one line from the socket. Strips CR, handles \n and \r\n, backspace,
// and telnet IAC sequences. Returns line length, or -1 on disconnect.
int shell_read_line(int sock, char *buf, size_t maxlen);

// Print `prompt` and read one line, echoing correctly for the client type
// (server-echoes in char/telnet mode, relies on local echo in line mode).
// Used by language REPLs. Returns line length, or -1 on disconnect/EOF.
int shell_prompt_read(shell_ctx_t *ctx, const char *prompt, char *buf, size_t maxlen);

// Output capture (for the web dashboard command runner). shell_capture_begin()
// returns a negative sentinel "fd": put it in shell_ctx_t.sock, run cmd_execute(),
// then shell_capture_end() detaches and returns the accumulated output (caller
// must free() it). Any shell output to a sock < 0 is buffered, not sent, and any
// input read from a sock < 0 returns EOF. Returns -1 if no capture slot is free.
int   shell_capture_begin(void);
char *shell_capture_end(int fd, size_t *len_out);
