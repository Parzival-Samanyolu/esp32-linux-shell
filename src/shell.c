#include "shell.h"
#include "commands.h"
#include "config.h"
#include "dmesg.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "shell";

static volatile int s_client_count;

// ---------------------------------------------------------------------------
//  I/O helpers
// ---------------------------------------------------------------------------
int shell_send_all(int sock, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = send(sock, data + sent, len - sent, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += n;
    }
    return 0;
}

int shell_printf(int sock, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;   // truncated, send what fits
    return shell_send_all(sock, buf, n) == 0 ? n : -1;
}

int shell_read_line(int sock, char *buf, size_t maxlen)
{
    size_t idx = 0;
    while (idx < maxlen - 1) {
        unsigned char c;
        int r = recv(sock, &c, 1, 0);
        if (r <= 0) return -1;                 // disconnect / error

        if (c == 0xFF) {                       // telnet IAC: swallow 2 more bytes
            unsigned char t[2];
            recv(sock, t, 2, 0);
            continue;
        }
        if (c == '\r') continue;               // strip CR (handles \r\n)
        if (c == '\n') break;                  // end of line
        if (c == 0x7F || c == 0x08) {          // backspace / delete
            if (idx > 0) idx--;
            continue;
        }
        if (c == 0x03) return -1;              // Ctrl-C closes session
        buf[idx++] = (char)c;
    }
    buf[idx] = '\0';
    return (int)idx;
}

int shell_prompt_read(shell_ctx_t *ctx, const char *prompt, char *buf, size_t maxlen)
{
    int sock = ctx->sock;
    shell_send_all(sock, prompt, strlen(prompt));

    if (!ctx->char_mode) {
        // Line mode (nc): the client echoes locally; just read a line.
        return shell_read_line(sock, buf, maxlen);
    }

    // Char mode (telnet): server must echo each keystroke.
    size_t idx = 0;
    while (idx < maxlen - 1) {
        unsigned char c;
        int r = recv(sock, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == 0xFF) { unsigned char t[2]; recv(sock, t, 2, 0); continue; }  // IAC
        if (c == '\r' || c == '\n') {
            // swallow a trailing \n / \0 within a short window
            fd_set f; FD_ZERO(&f); FD_SET(sock, &f);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 15000 };
            if (select(sock + 1, &f, NULL, NULL, &tv) > 0) { unsigned char t; recv(sock, &t, 1, MSG_DONTWAIT); }
            shell_send_all(sock, "\r\n", 2);
            break;
        }
        if (c == 0x7F || c == 0x08) {                       // backspace
            if (idx > 0) { idx--; shell_send_all(sock, "\b \b", 3); }
            continue;
        }
        if (c == 0x04) { if (idx == 0) return -1; continue; } // Ctrl-D on empty = EOF
        if (c == 0x03) return -1;                             // Ctrl-C
        if (c >= 32 && c < 127) {
            buf[idx++] = (char)c;
            shell_send_all(sock, (const char *)&c, 1);        // echo
        }
    }
    buf[idx] = '\0';
    return (int)idx;
}

// ---------------------------------------------------------------------------
//  Character-mode line editor (arrow-key history + inline editing)
//  Active only when the client negotiated telnet char-at-a-time mode.
// ---------------------------------------------------------------------------
#define SH_HIST_MAX 20

typedef struct { char *items[SH_HIST_MAX]; int count; } history_t;

static void hist_add(history_t *h, const char *line)
{
    if (!*line) return;
    if (h->count > 0 && strcmp(h->items[h->count - 1], line) == 0) return;  // no dup
    if (h->count == SH_HIST_MAX) {
        free(h->items[0]);
        memmove(&h->items[0], &h->items[1], sizeof(char *) * (SH_HIST_MAX - 1));
        h->count--;
    }
    h->items[h->count++] = strdup(line);
}
static void hist_free(history_t *h)
{
    for (int i = 0; i < h->count; i++) free(h->items[i]);
}

// Detect a telnet-capable client: such clients open the connection by sending
// IAC negotiation. Plain `nc` stays silent -> we keep it in simple line mode
// (and never send it raw telnet bytes that would show as garbage).
static int shell_negotiate(int sock)
{
    // Proactively offer character-at-a-time mode (this is what real telnet
    // servers do). Telnet clients — PuTTY *and* BSD/macOS telnet — reply with
    // IAC and switch to char mode. Plain `nc` ignores it and stays in line
    // mode (use the `arrows` command there).
    const unsigned char neg[] = { 0xFF,0xFB,0x03,   // IAC WILL SUPPRESS-GO-AHEAD
                                  0xFF,0xFB,0x01 };  // IAC WILL ECHO
    shell_send_all(sock, (const char *)neg, sizeof(neg));

    int char_mode = 0;
    unsigned char buf[128];
    fd_set f; FD_ZERO(&f); FD_SET(sock, &f);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 600000 };
    while (select(sock + 1, &f, NULL, NULL, &tv) > 0) {
        int n = recv(sock, buf, sizeof(buf), MSG_DONTWAIT);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) if (buf[i] == 0xFF) char_mode = 1;  // telnet replied
        tv.tv_sec = 0; tv.tv_usec = 150000;   // keep draining the rest briefly
        FD_ZERO(&f); FD_SET(sock, &f);
    }
    return char_mode;
}

enum { EK_UP = 1000, EK_DOWN, EK_LEFT, EK_RIGHT, EK_HOME, EK_END, EK_DEL, EK_BS };

static int ek_raw(int sock)
{
    unsigned char c;
    while (1) {
        int r = recv(sock, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == 0xFF) { unsigned char t[2]; recv(sock, t, 2, 0); continue; }
        return c;
    }
}
static int ek_timeout(int sock, int ms)
{
    fd_set f; FD_ZERO(&f); FD_SET(sock, &f);
    struct timeval tv = { .tv_sec = 0, .tv_usec = ms * 1000 };
    if (select(sock + 1, &f, NULL, NULL, &tv) <= 0) return -2;
    return ek_raw(sock);
}
static int ek_read(int sock)
{
    int c = ek_raw(sock);
    if (c < 0) return -1;
    if (c == 0x7F || c == 0x08) return EK_BS;
    if (c != 0x1b) return c;
    int b1 = ek_timeout(sock, 60);
    if (b1 != '[' && b1 != 'O') return 0x1b;
    int b2 = ek_timeout(sock, 60);
    if (b2 >= '0' && b2 <= '9') {
        int b3 = ek_timeout(sock, 60); (void)b3;   // trailing '~'
        switch (b2) { case '1': case '7': return EK_HOME;
                      case '4': case '8': return EK_END; case '3': return EK_DEL; }
        return 0x1b;
    }
    switch (b2) {
        case 'A': return EK_UP;  case 'B': return EK_DOWN;
        case 'C': return EK_RIGHT; case 'D': return EK_LEFT;
        case 'H': return EK_HOME; case 'F': return EK_END;
    }
    return 0x1b;
}

// Repaint the current input line and place the cursor.
static void ed_line_redraw(int sock, const char *prompt, const char *buf, int cur)
{
    shell_send_all(sock, "\r", 1);
    shell_send_all(sock, prompt, strlen(prompt));
    shell_send_all(sock, buf, strlen(buf));
    shell_send_all(sock, "\x1b[K", 3);          // clear to end of line
    shell_send_all(sock, "\r", 1);
    int col = (int)strlen(prompt) + cur;
    if (col > 0) { char m[16]; int k = snprintf(m, sizeof(m), "\x1b[%dC", col);
                   shell_send_all(sock, m, k); }
}

static int shell_read_line_interactive(int sock, const char *prompt,
                                       char *buf, size_t maxlen, history_t *h)
{
    int len = 0, cur = 0;
    buf[0] = '\0';
    int hpos = h->count;                 // == count means "fresh line"
    char stash[256]; stash[0] = '\0';    // in-progress line saved while browsing

    shell_send_all(sock, prompt, strlen(prompt));

    while (1) {
        int k = ek_read(sock);
        if (k == -1) return -1;
        if (k == '\r' || k == '\n') {
            ek_timeout(sock, 15);        // swallow a trailing \n / \0
            shell_send_all(sock, "\r\n", 2);
            break;
        }
        if (k == 0x03) { shell_send_all(sock, "^C\r\n", 4); buf[0] = 0; return 0; } // Ctrl-C
        if (k == 0x04) { if (len == 0) return -1; continue; }                       // Ctrl-D
        if (k == EK_BS) {
            if (cur > 0) { memmove(buf+cur-1, buf+cur, len-cur); cur--; len--;
                           buf[len]=0; ed_line_redraw(sock,prompt,buf,cur); }
        } else if (k == EK_DEL) {
            if (cur < len) { memmove(buf+cur, buf+cur+1, len-cur-1); len--;
                             buf[len]=0; ed_line_redraw(sock,prompt,buf,cur); }
        } else if (k == EK_LEFT)  { if (cur>0){cur--; ed_line_redraw(sock,prompt,buf,cur);} }
        else if (k == EK_RIGHT) { if (cur<len){cur++; ed_line_redraw(sock,prompt,buf,cur);} }
        else if (k == EK_HOME)  { cur=0; ed_line_redraw(sock,prompt,buf,cur); }
        else if (k == EK_END)   { cur=len; ed_line_redraw(sock,prompt,buf,cur); }
        else if (k == EK_UP) {
            if (hpos > 0) {
                if (hpos == h->count) { strncpy(stash, buf, sizeof(stash)-1); stash[sizeof(stash)-1]=0; }
                hpos--;
                strncpy(buf, h->items[hpos], maxlen-1); buf[maxlen-1]=0;
                len = strlen(buf); cur = len; ed_line_redraw(sock,prompt,buf,cur);
            }
        } else if (k == EK_DOWN) {
            if (hpos < h->count) {
                hpos++;
                const char *src = (hpos == h->count) ? stash : h->items[hpos];
                strncpy(buf, src, maxlen-1); buf[maxlen-1]=0;
                len = strlen(buf); cur = len; ed_line_redraw(sock,prompt,buf,cur);
            }
        } else if (k >= 32 && k < 127) {
            if (len + 1 < (int)maxlen) {
                memmove(buf+cur+1, buf+cur, len-cur);
                buf[cur] = (char)k; cur++; len++; buf[len]=0;
                ed_line_redraw(sock, prompt, buf, cur);
            }
        }
    }
    buf[len] = '\0';
    hist_add(h, buf);
    return len;
}

// ---------------------------------------------------------------------------
//  Per-client task
// ---------------------------------------------------------------------------
static void shell_client_task(void *arg)
{
    shell_ctx_t *ctx = (shell_ctx_t *)arg;

    dmesg_add("shell: client %d connected", ctx->id);
    ESP_LOGI(TAG, "client %d connected on sock %d", ctx->id, ctx->sock);

    ctx->char_mode = shell_negotiate(ctx->sock);
    cmd_banner(ctx);

    history_t hist = { 0 };
    char *line = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (!line) line = malloc(1024);           // fallback to internal RAM

    char prompt[300];
    while (line) {
        cmd_prompt_build(ctx, prompt, sizeof(prompt));
        int n;
        if (ctx->char_mode)
            n = shell_read_line_interactive(ctx->sock, prompt, line, 1024, &hist);
        else {
            shell_send_all(ctx->sock, prompt, strlen(prompt));
            n = shell_read_line(ctx->sock, line, 1024);
        }
        if (n < 0) break;                      // disconnected
        cmd_execute(ctx, line);
        if (ctx->want_close) break;
    }
    hist_free(&hist);

    free(line);
    close(ctx->sock);
    dmesg_add("shell: client %d disconnected", ctx->id);
    ESP_LOGI(TAG, "client %d disconnected", ctx->id);
    s_client_count--;
    free(ctx);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
//  Listener task
// ---------------------------------------------------------------------------
static void shell_listener_task(void *arg)
{
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(SHELL_PORT),
    };

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() failed: %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_sock, 4) != 0) {
        ESP_LOGE(TAG, "listen() failed: %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "listening on port %d", SHELL_PORT);

    int next_id = 0;
    while (1) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int sock = accept(listen_sock, (struct sockaddr *)&src, &sl);
        if (sock < 0) {
            ESP_LOGW(TAG, "accept() failed: %d", errno);
            continue;
        }

        if (s_client_count >= SHELL_MAX_CLIENTS) {
            const char *busy = "Too many connections. Try again later.\r\n";
            shell_send_all(sock, busy, strlen(busy));
            close(sock);
            continue;
        }

        // Keepalive so dead clients get reaped.
        int ka = 1;
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));

        shell_ctx_t *ctx = calloc(1, sizeof(shell_ctx_t));
        if (!ctx) { close(sock); continue; }
        ctx->sock = sock;
        ctx->id   = next_id++;
        strncpy(ctx->cwd, SD_MOUNT, sizeof(ctx->cwd) - 1);   // start at "home"

        s_client_count++;
        char name[24];
        snprintf(name, sizeof(name), "shell_client_%d", ctx->id);
        // Pin clients to core 1 (Wi-Fi/lwip live on core 0).
        // Generous stack: the C interpreter (PicoC) recurses on the C stack for
        // interpreted function calls (~3-4KB per level), so give real headroom.
        if (xTaskCreatePinnedToCore(shell_client_task, name, 40960, ctx, 5,
                                    NULL, 1) != pdPASS) {
            ESP_LOGE(TAG, "failed to spawn client task");
            s_client_count--;
            close(sock);
            free(ctx);
        }
    }
}

void shell_start(void)
{
    xTaskCreatePinnedToCore(shell_listener_task, "shell_listener",
                            4096, NULL, 5, NULL, 1);
}
