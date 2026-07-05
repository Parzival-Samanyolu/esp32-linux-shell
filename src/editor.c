#include "editor.h"
#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lwip/sockets.h"

// Assumed client terminal geometry (raw TCP can't reliably query it).
#define SCR_ROWS 24
#define SCR_COLS 80
#define TEXT_ROWS (SCR_ROWS - 3)   // title + status + help take 3 lines

// Editor key codes above the ASCII range.
enum {
    K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL, K_PGUP, K_PGDN
};

typedef struct { char *chars; int len; } erow;

typedef struct {
    int   sock;
    int   cx, cy;          // cursor: column, row (into file)
    int   rowoff, coloff;  // viewport scroll
    int   numrows;
    int   rowcap;
    erow *rows;
    int   dirty;
    char  filename[128];
    char  fs_path[256];
    char  status[96];
} editor_t;

// ---- append buffer (one syscall per repaint, no flicker) -------------------
typedef struct { char *b; int len, cap; } abuf;
static void ab_append(abuf *a, const char *s, int len)
{
    if (a->len + len > a->cap) {
        int nc = (a->cap ? a->cap * 2 : 1024);
        while (nc < a->len + len) nc *= 2;
        char *nb = realloc(a->b, nc);
        if (!nb) return;
        a->b = nb; a->cap = nc;
    }
    memcpy(a->b + a->len, s, len);
    a->len += len;
}
static void ab_str(abuf *a, const char *s) { ab_append(a, s, strlen(s)); }
static void ab_free(abuf *a) { free(a->b); }

// ---- row helpers -----------------------------------------------------------
static void ed_insert_row(editor_t *E, int at, const char *s, int len)
{
    if (at < 0 || at > E->numrows) return;
    if (E->numrows + 1 > E->rowcap) {
        E->rowcap = E->rowcap ? E->rowcap * 2 : 32;
        E->rows = realloc(E->rows, sizeof(erow) * E->rowcap);
    }
    memmove(&E->rows[at + 1], &E->rows[at], sizeof(erow) * (E->numrows - at));
    E->rows[at].chars = malloc(len + 1);
    memcpy(E->rows[at].chars, s, len);
    E->rows[at].chars[len] = '\0';
    E->rows[at].len = len;
    E->numrows++;
}

static void ed_free_row(erow *r) { free(r->chars); }

static void ed_del_row(editor_t *E, int at)
{
    if (at < 0 || at >= E->numrows) return;
    ed_free_row(&E->rows[at]);
    memmove(&E->rows[at], &E->rows[at + 1], sizeof(erow) * (E->numrows - at - 1));
    E->numrows--;
}

static void ed_row_insert_char(erow *r, int at, int c)
{
    if (at < 0 || at > r->len) at = r->len;
    r->chars = realloc(r->chars, r->len + 2);
    memmove(&r->chars[at + 1], &r->chars[at], r->len - at + 1);
    r->chars[at] = c;
    r->len++;
}

static void ed_row_del_char(erow *r, int at)
{
    if (at < 0 || at >= r->len) return;
    memmove(&r->chars[at], &r->chars[at + 1], r->len - at);
    r->len--;
}

static void ed_row_append(erow *r, const char *s, int len)
{
    r->chars = realloc(r->chars, r->len + len + 1);
    memcpy(&r->chars[r->len], s, len);
    r->len += len;
    r->chars[r->len] = '\0';
}

// ---- editing operations ----------------------------------------------------
static void ed_insert_char(editor_t *E, int c)
{
    if (E->cy == E->numrows) ed_insert_row(E, E->numrows, "", 0);
    ed_row_insert_char(&E->rows[E->cy], E->cx, c);
    E->cx++;
    E->dirty = 1;
}

static void ed_insert_newline(editor_t *E)
{
    if (E->cx == 0) {
        ed_insert_row(E, E->cy, "", 0);
    } else {
        erow *r = &E->rows[E->cy];
        ed_insert_row(E, E->cy + 1, &r->chars[E->cx], r->len - E->cx);
        r = &E->rows[E->cy];               // realloc may have moved it
        r->len = E->cx;
        r->chars[r->len] = '\0';
    }
    E->cy++;
    E->cx = 0;
    E->dirty = 1;
}

static void ed_del_char(editor_t *E)   // backspace
{
    if (E->cy == E->numrows) return;
    if (E->cx == 0 && E->cy == 0) return;
    erow *r = &E->rows[E->cy];
    if (E->cx > 0) {
        ed_row_del_char(r, E->cx - 1);
        E->cx--;
    } else {
        E->cx = E->rows[E->cy - 1].len;
        ed_row_append(&E->rows[E->cy - 1], r->chars, r->len);
        ed_del_row(E, E->cy);
        E->cy--;
    }
    E->dirty = 1;
}

// ---- file load / save ------------------------------------------------------
static void ed_load(editor_t *E)
{
    FILE *f = fopen(E->fs_path, "rb");
    if (!f) return;                        // new file
    char *line = malloc(1024);
    int   cap = 1024, len = 0;
    int   c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\r') continue;
        if (c == '\n') { ed_insert_row(E, E->numrows, line, len); len = 0; continue; }
        if (len + 1 >= cap) { cap *= 2; line = realloc(line, cap); }
        line[len++] = c;
    }
    if (len > 0) ed_insert_row(E, E->numrows, line, len);
    free(line);
    fclose(f);
}

static int ed_save(editor_t *E)
{
    FILE *f = fopen(E->fs_path, "wb");
    if (!f) return -1;
    int total = 0;
    for (int i = 0; i < E->numrows; i++) {
        fwrite(E->rows[i].chars, 1, E->rows[i].len, f);
        fputc('\n', f);
        total += E->rows[i].len + 1;
    }
    fclose(f);
    E->dirty = 0;
    return total;
}

// ---- input -----------------------------------------------------------------
static int ed_read_raw(editor_t *E)
{
    unsigned char c;
    while (1) {
        int n = recv(E->sock, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == 0xFF) {                   // telnet IAC: swallow 2 bytes
            unsigned char t[2];
            recv(E->sock, t, 2, 0);
            continue;
        }
        return c;
    }
}

// Read with a short timeout; returns -2 if nothing arrived (for ESC parsing).
static int ed_read_timeout(editor_t *E, int ms)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(E->sock, &fds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = ms * 1000 };
    int r = select(E->sock + 1, &fds, NULL, NULL, &tv);
    if (r <= 0) return -2;
    return ed_read_raw(E);
}

static int ed_read_key(editor_t *E)
{
    int c = ed_read_raw(E);
    if (c != 0x1b) return c;

    int b1 = ed_read_timeout(E, 60);
    if (b1 < 0) return 0x1b;
    if (b1 != '[' && b1 != 'O') return 0x1b;

    int b2 = ed_read_timeout(E, 60);
    if (b2 < 0) return 0x1b;

    if (b2 >= '0' && b2 <= '9') {
        int b3 = ed_read_timeout(E, 60);   // trailing '~'
        (void)b3;
        switch (b2) {
            case '1': case '7': return K_HOME;
            case '4': case '8': return K_END;
            case '3': return K_DEL;
            case '5': return K_PGUP;
            case '6': return K_PGDN;
        }
        return 0x1b;
    }
    switch (b2) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
    }
    return 0x1b;
}

// ---- rendering -------------------------------------------------------------
static void ed_scroll(editor_t *E)
{
    if (E->cy < E->rowoff) E->rowoff = E->cy;
    if (E->cy >= E->rowoff + TEXT_ROWS) E->rowoff = E->cy - TEXT_ROWS + 1;
    if (E->cx < E->coloff) E->coloff = E->cx;
    if (E->cx >= E->coloff + SCR_COLS) E->coloff = E->cx - SCR_COLS + 1;
}

static void ed_refresh(editor_t *E)
{
    ed_scroll(E);
    abuf ab = { 0 };
    ab_str(&ab, "\033[?25l\033[H");        // hide cursor, home

    // Title bar (inverse).
    char title[SCR_COLS + 32];
    int tl = snprintf(title, sizeof(title),
                      "  ESP32 nano   %s%s", E->filename, E->dirty ? "  *modified*" : "");
    ab_str(&ab, "\033[7m");
    ab_append(&ab, title, tl);
    for (int i = tl; i < SCR_COLS; i++) ab_str(&ab, " ");
    ab_str(&ab, "\033[m\r\n");

    // Text rows.
    for (int y = 0; y < TEXT_ROWS; y++) {
        int fr = E->rowoff + y;
        ab_str(&ab, "\033[K");
        if (fr < E->numrows) {
            int len = E->rows[fr].len - E->coloff;
            if (len < 0) len = 0;
            if (len > SCR_COLS) len = SCR_COLS;
            if (len > 0) ab_append(&ab, &E->rows[fr].chars[E->coloff], len);
        } else {
            ab_str(&ab, "~");
        }
        ab_str(&ab, "\r\n");
    }

    // Status message line.
    ab_str(&ab, "\033[K\033[7m");
    char st[SCR_COLS + 32];
    int sl = snprintf(st, sizeof(st), " %.*s", SCR_COLS - 2,
                      E->status[0] ? E->status : "");
    ab_append(&ab, st, sl);
    for (int i = sl; i < SCR_COLS; i++) ab_str(&ab, " ");
    ab_str(&ab, "\033[m\r\n");

    // Help line (nano-style shortcuts).
    ab_str(&ab, "\033[K^O");
    ab_str(&ab, " Save   ^X Exit   ^K Cut line   ^G Help   Arrows move");

    // Place the cursor.
    char cur[32];
    snprintf(cur, sizeof(cur), "\033[%d;%dH",
             (E->cy - E->rowoff) + 2, (E->cx - E->coloff) + 1);
    ab_str(&ab, cur);
    ab_str(&ab, "\033[?25h");               // show cursor

    shell_send_all(E->sock, ab.b, ab.len);
    ab_free(&ab);
}

// ---- key handling ----------------------------------------------------------
static void ed_move(editor_t *E, int key)
{
    erow *row = (E->cy < E->numrows) ? &E->rows[E->cy] : NULL;
    switch (key) {
        case K_LEFT:
            if (E->cx > 0) E->cx--;
            else if (E->cy > 0) { E->cy--; E->cx = E->rows[E->cy].len; }
            break;
        case K_RIGHT:
            if (row && E->cx < row->len) E->cx++;
            else if (row && E->cx == row->len && E->cy < E->numrows - 1) { E->cy++; E->cx = 0; }
            break;
        case K_UP:    if (E->cy > 0) E->cy--; break;
        case K_DOWN:  if (E->cy < E->numrows - 1) E->cy++; break;
        case K_HOME:  E->cx = 0; break;
        case K_END:   if (row) E->cx = row->len; break;
        case K_PGUP:  E->cy -= TEXT_ROWS; if (E->cy < 0) E->cy = 0; break;
        case K_PGDN:  E->cy += TEXT_ROWS; if (E->cy >= E->numrows) E->cy = E->numrows ? E->numrows - 1 : 0; break;
    }
    // Snap cx to end of the (possibly shorter) new line.
    row = (E->cy < E->numrows) ? &E->rows[E->cy] : NULL;
    int rl = row ? row->len : 0;
    if (E->cx > rl) E->cx = rl;
}

void editor_run(shell_ctx_t *ctx, const char *fs_path, const char *display_name)
{
    editor_t E = { 0 };
    E.sock = ctx->sock;
    strncpy(E.fs_path, fs_path, sizeof(E.fs_path) - 1);
    strncpy(E.filename, display_name, sizeof(E.filename) - 1);
    snprintf(E.status, sizeof(E.status), "New/opened buffer — ^O to save, ^X to exit");

    ed_load(&E);

    // Ask the client for character-at-a-time mode (telnet/PuTTY honour this;
    // raw nc ignores it, so those users must `stty raw -echo` first).
    const unsigned char tn_char[] = {
        0xFF, 0xFB, 0x01,   // IAC WILL ECHO
        0xFF, 0xFB, 0x03,   // IAC WILL SUPPRESS-GO-AHEAD
        0xFF, 0xFD, 0x03,   // IAC DO   SUPPRESS-GO-AHEAD
    };
    shell_send_all(E.sock, (const char *)tn_char, sizeof(tn_char));
    shell_send_all(E.sock, "\033[2J", 4);

    int running = 1;
    while (running) {
        ed_refresh(&E);
        int c = ed_read_key(&E);
        if (c == -1) break;                 // disconnected

        switch (c) {
            case 0x0F:                       // ^O save
                {
                    int n = ed_save(&E);
                    if (n >= 0) snprintf(E.status, sizeof(E.status), "Wrote %d lines to %s", E.numrows, E.filename);
                    else        snprintf(E.status, sizeof(E.status), "ERROR: could not write %s", E.filename);
                }
                break;
            case 0x18:                       // ^X exit
            case 0x03:                       // ^C exit
                if (E.dirty) {
                    snprintf(E.status, sizeof(E.status), "Unsaved changes! ^O to save, or ^X again to discard");
                    ed_refresh(&E);
                    int k = ed_read_key(&E);
                    if (k == 0x18 || k == 0x03) running = 0;   // discard
                    else if (k == 0x0F) { ed_save(&E); running = 0; }
                } else {
                    running = 0;
                }
                break;
            case 0x0B:                       // ^K cut current line
                if (E.numrows > 0 && E.cy < E.numrows) {
                    ed_del_row(&E, E.cy);
                    if (E.cy >= E.numrows && E.cy > 0) E.cy--;
                    E.cx = 0;
                    E.dirty = 1;
                }
                break;
            case 0x07:                       // ^G help
                snprintf(E.status, sizeof(E.status), "nano: arrows move, type to insert, ^O save, ^X quit, ^K cut");
                break;
            case '\r':
            case '\n':
                ed_insert_newline(&E);
                break;
            case 127:                        // backspace
            case 8:
                ed_del_char(&E);
                break;
            case K_DEL:
                ed_move(&E, K_RIGHT);
                ed_del_char(&E);
                break;
            case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT:
            case K_HOME: case K_END: case K_PGUP: case K_PGDN:
                ed_move(&E, c);
                break;
            case 0x1b:                       // stray ESC — ignore
                break;
            default:
                if (c >= 32 && c < 127) ed_insert_char(&E, c);
                break;
        }
    }

    // Return the client to line mode and clear the screen for the shell.
    const unsigned char tn_line[] = { 0xFF, 0xFC, 0x01 };   // IAC WONT ECHO
    shell_send_all(E.sock, (const char *)tn_line, sizeof(tn_line));
    shell_send_all(E.sock, "\033[2J\033[H", 7);

    for (int i = 0; i < E.numrows; i++) ed_free_row(&E.rows[i]);
    free(E.rows);
}
