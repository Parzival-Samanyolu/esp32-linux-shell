#include "fun.h"
#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_random.h"
#include "lwip/sockets.h"

// ---------------------------------------------------------------------------
//  Input helper: wait up to `ms` for one byte. Returns the byte (0..255),
//  -1 on timeout, or -2 on disconnect. Used by the interactive toys.
// ---------------------------------------------------------------------------
static int wait_key(int sock, int ms)
{
    fd_set r; FD_ZERO(&r); FD_SET(sock, &r);
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    int s = select(sock + 1, &r, NULL, NULL, &tv);
    if (s > 0 && FD_ISSET(sock, &r)) {
        unsigned char c;
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) return -2;
        return c;
    }
    return -1;
}

// Read a direction key: returns 'U','D','L','R' (WASD or arrows), 'q' to quit,
// or 0 for "nothing / ignore". Waits up to `ms` ms.
static int game_key(int sock, int ms)
{
    int c = wait_key(sock, ms);
    if (c == -2) return 'q';
    if (c < 0)  return 0;
    if (c == 0x1b) {                       // escape sequence (arrow keys)
        int a = wait_key(sock, 30);
        if (a == '[' || a == 'O') {
            int b = wait_key(sock, 30);
            switch (b) {
                case 'A': return 'U';
                case 'B': return 'D';
                case 'C': return 'R';
                case 'D': return 'L';
            }
            return 0;
        }
        return 'q';                        // bare ESC quits
    }
    switch (c) {
        case 'w': case 'W': return 'U';
        case 's': case 'S': return 'D';
        case 'a': case 'A': return 'L';
        case 'd': case 'D': return 'R';
        case 'q': case 'Q': case 0x03: return 'q';
    }
    return 0;
}

// ---------------------------------------------------------------------------
//  cowsay
// ---------------------------------------------------------------------------
void fun_cowsay(shell_ctx_t *ctx, int argc, char **argv)
{
    char msg[256];
    if (argc < 2) {
        strcpy(msg, "Mooo! (try: cowsay hello world)");
    } else {
        msg[0] = '\0';
        for (int i = 1; i < argc; i++) {
            if (i > 1) strlcat(msg, " ", sizeof(msg));
            strlcat(msg, argv[i], sizeof(msg));
        }
    }
    int len = strlen(msg);
    if (len > 60) { msg[60] = '\0'; len = 60; }   // keep the bubble tidy

    // Top border.
    shell_printf(ctx->sock, " ");
    for (int i = 0; i < len + 2; i++) shell_printf(ctx->sock, "_");
    shell_printf(ctx->sock, "\r\n< %s >\r\n ", msg);
    for (int i = 0; i < len + 2; i++) shell_printf(ctx->sock, "-");
    shell_printf(ctx->sock,
        "\r\n"
        "        \\   ^__^\r\n"
        "         \\  (oo)\\_______\r\n"
        "            (__)\\       )\\/\\\r\n"
        "                ||----w |\r\n"
        "                ||     ||\r\n");
}

// ---------------------------------------------------------------------------
//  fortune
// ---------------------------------------------------------------------------
void fun_fortune(shell_ctx_t *ctx)
{
    static const char *quotes[] = {
        "There are only two hard things in computer science: cache invalidation, naming things, and off-by-one errors.",
        "It works on my machine.",
        "Weeks of coding can save you hours of planning.",
        "A user interface is like a joke. If you have to explain it, it's not that good.",
        "There is no place like 127.0.0.1.",
        "Real programmers count from 0.",
        "Hardware: the parts of a computer that can be kicked.",
        "To err is human, but to really foul things up you need a computer.",
        "The best thing about a boolean is even if you are wrong, you are only off by a bit.",
        "Debugging is like being the detective in a crime movie where you are also the murderer.",
        "It's not a bug, it's an undocumented feature.",
        "99 little bugs in the code, take one down, patch it around, 127 little bugs in the code.",
        "Programming is 10% writing code and 90% understanding why it doesn't work.",
        "Keep calm and blame the compiler.",
        "An ESP32 has two cores so it can ignore you twice as fast.",
    };
    int n = sizeof(quotes) / sizeof(quotes[0]);
    int i = esp_random() % n;
    shell_printf(ctx->sock, "%s\r\n", quotes[i]);
}

// ---------------------------------------------------------------------------
//  cmatrix — ANSI digital rain until a key is pressed
// ---------------------------------------------------------------------------
void fun_cmatrix(shell_ctx_t *ctx)
{
    if (!ctx->char_mode) {
        shell_printf(ctx->sock,
            "cmatrix needs a char-mode terminal (telnet/PuTTY). On nc, type 'arrows' first.\r\n");
        return;
    }
    int sock = ctx->sock;
    const int COLS = 70, ROWS = 22, TRAIL = 8;
    static const char cs[] = "abcdefghijklmnopqrstuvwxyz0123456789@#$%&*+=<>";
    int cslen = sizeof(cs) - 1;

    int head[70];
    for (int c = 0; c < COLS; c++) head[c] = -(int)(esp_random() % ROWS);

    shell_send_all(sock, "\033[2J\033[?25l", 9);   // clear + hide cursor

    char frame[4096];
    bool stop = false;
    while (!stop) {
        int len = 0;
        for (int c = 0; c < COLS; c++) {
            if ((esp_random() & 3) == 0) continue;         // vary column speed
            int h = head[c];
            if (h >= 0 && h < ROWS)                          // bright white head
                len += snprintf(frame + len, sizeof(frame) - len,
                                "\033[%d;%dH\033[97m%c", h + 1, c + 1, cs[esp_random() % cslen]);
            if (h - 1 >= 0 && h - 1 < ROWS)                  // green body behind it
                len += snprintf(frame + len, sizeof(frame) - len,
                                "\033[%d;%dH\033[32m%c", h, c + 1, cs[esp_random() % cslen]);
            int t = h - TRAIL;                               // erase the tail
            if (t >= 0 && t < ROWS)
                len += snprintf(frame + len, sizeof(frame) - len, "\033[%d;%dH ", t + 1, c + 1);
            head[c] = h + 1;
            if (head[c] > ROWS + TRAIL) head[c] = -(int)(esp_random() % 10);
            if (len > (int)sizeof(frame) - 64) { shell_send_all(sock, frame, len); len = 0; }
        }
        if (len) shell_send_all(sock, frame, len);
        int k = wait_key(sock, 55);                          // frame delay + input poll
        if (k == -2 || k >= 0) stop = true;                  // any key (or disconnect) stops
    }
    shell_send_all(sock, "\033[0m\033[2J\033[H\033[?25h", 15);   // reset colors, clear, show cursor
}

// ---------------------------------------------------------------------------
//  snake — classic ANSI game
// ---------------------------------------------------------------------------
#define SNK_W 40
#define SNK_H 18
#define SNK_MAX (SNK_W * SNK_H)

// Playfield cells: x in [1..SNK_W], y in [1..SNK_H]. Drawn from screen row 2
// (row 1 is the score line); border of '#' surrounds it.
static inline int scr_row(int y) { return y + 2; }   // 1 title, 2 top border...
static inline int scr_col(int x) { return x + 1; }

void fun_snake(shell_ctx_t *ctx)
{
    if (!ctx->char_mode) {
        shell_printf(ctx->sock,
            "snake needs a char-mode terminal (telnet/PuTTY). On nc, type 'arrows' first.\r\n");
        return;
    }
    int sock = ctx->sock;

    // Body as a ring buffer of coordinates; occupancy grid for collisions.
    static uint8_t occ[SNK_W + 2][SNK_H + 2];
    static uint16_t bx[SNK_MAX], by[SNK_MAX];
    memset(occ, 0, sizeof(occ));

    int head_i = 0, tail_i = 0, length = 4;
    int hx = SNK_W / 2, hy = SNK_H / 2;
    int dx = 1, dy = 0;
    int score = 0;

    shell_send_all(sock, "\033[2J\033[?25l", 9);

    // Draw the border box once.
    char buf[2048]; int len = 0;
    len += snprintf(buf + len, sizeof(buf) - len,
        "\033[1;1H\033[92m*** SNAKE ***  WASD/arrows to move, q to quit\033[0m");
    for (int x = 0; x <= SNK_W + 1; x++) {
        len += snprintf(buf + len, sizeof(buf) - len, "\033[%d;%dH#", scr_row(0), scr_col(x));
        len += snprintf(buf + len, sizeof(buf) - len, "\033[%d;%dH#", scr_row(SNK_H + 1), scr_col(x));
    }
    for (int y = 1; y <= SNK_H; y++) {
        len += snprintf(buf + len, sizeof(buf) - len, "\033[%d;%dH#", scr_row(y), scr_col(0));
        len += snprintf(buf + len, sizeof(buf) - len, "\033[%d;%dH#", scr_row(y), scr_col(SNK_W + 1));
    }
    shell_send_all(sock, buf, len);

    // Seed the initial snake (horizontal, head at hx,hy).
    for (int i = 0; i < length; i++) {
        int x = hx - (length - 1 - i), y = hy;
        bx[i] = x; by[i] = y; occ[x][y] = 1;
        shell_printf(sock, "\033[%d;%dH\033[32mo\033[0m", scr_row(y), scr_col(x));
    }
    head_i = length - 1;
    tail_i = 0;

    // Place first food.
    int fx, fy;
    do { fx = 1 + esp_random() % SNK_W; fy = 1 + esp_random() % SNK_H; } while (occ[fx][fy]);
    shell_printf(sock, "\033[%d;%dH\033[91m*\033[0m", scr_row(fy), scr_col(fx));

    bool over = false, quit = false;
    int delay = 130;

    while (!over && !quit) {
        int k = game_key(sock, delay);
        if (k == 'q') { quit = true; break; }
        // Update direction, disallowing a 180-degree reversal.
        if (k == 'U' && dy != 1)  { dx = 0; dy = -1; }
        else if (k == 'D' && dy != -1) { dx = 0; dy = 1; }
        else if (k == 'L' && dx != 1)  { dx = -1; dy = 0; }
        else if (k == 'R' && dx != -1) { dx = 1; dy = 0; }

        int nx = hx + dx, ny = hy + dy;
        bool grow = (nx == fx && ny == fy);

        // Remove the tail first (unless growing) so chasing your own tail is legal.
        if (!grow) {
            int tx = bx[tail_i], ty = by[tail_i];
            occ[tx][ty] = 0;
            shell_printf(sock, "\033[%d;%dH ", scr_row(ty), scr_col(tx));
            tail_i = (tail_i + 1) % SNK_MAX;
            length--;
        }

        // Collision: wall or body.
        if (nx < 1 || nx > SNK_W || ny < 1 || ny > SNK_H || occ[nx][ny]) {
            over = true;
            break;
        }

        // Advance head.
        head_i = (head_i + 1) % SNK_MAX;
        bx[head_i] = nx; by[head_i] = ny; occ[nx][ny] = 1;
        hx = nx; hy = ny; length++;
        shell_printf(sock, "\033[%d;%dH\033[92mO\033[0m", scr_row(ny), scr_col(nx));

        if (grow) {
            score++;
            if (delay > 60) delay -= 4;                    // speed up a little
            do { fx = 1 + esp_random() % SNK_W; fy = 1 + esp_random() % SNK_H; } while (occ[fx][fy]);
            shell_printf(sock, "\033[%d;%dH\033[91m*\033[0m", scr_row(fy), scr_col(fx));
            shell_printf(sock, "\033[1;60H\033[93mScore: %d\033[0m", score);
        }
    }

    // Teardown.
    shell_send_all(sock, "\033[?25h", 6);                  // show cursor
    int r = scr_row(SNK_H + 3);
    if (quit)
        shell_printf(sock, "\033[%d;1H\033[0mSnake quit. Final score: %d\r\n", r, score);
    else
        shell_printf(sock, "\033[%d;1H\033[0m\033[91mGAME OVER\033[0m — score: %d\r\n", r, score);
}

// ---------------------------------------------------------------------------
//  tic-tac-toe — you (X) vs the ESP32 (O), which plays perfectly (minimax)
// ---------------------------------------------------------------------------
static const int TTT_LINES[8][3] = {
    {0,1,2},{3,4,5},{6,7,8}, {0,3,6},{1,4,7},{2,5,8}, {0,4,8},{2,4,6}
};

static char ttt_winner(const char *b)
{
    for (int i = 0; i < 8; i++) {
        char a = b[TTT_LINES[i][0]];
        if (a != ' ' && a == b[TTT_LINES[i][1]] && a == b[TTT_LINES[i][2]]) return a;
    }
    for (int i = 0; i < 9; i++) if (b[i] == ' ') return 0;   // game continues
    return 'D';                                              // draw
}

// Score from O's perspective; depth makes it prefer quicker wins / slower losses.
static int ttt_minimax(char *b, char player, int depth)
{
    char w = ttt_winner(b);
    if (w == 'O') return 10 - depth;
    if (w == 'X') return depth - 10;
    if (w == 'D') return 0;

    int best = (player == 'O') ? -1000 : 1000;
    for (int i = 0; i < 9; i++) {
        if (b[i] != ' ') continue;
        b[i] = player;
        int s = ttt_minimax(b, player == 'O' ? 'X' : 'O', depth + 1);
        b[i] = ' ';
        if (player == 'O') { if (s > best) best = s; }
        else               { if (s < best) best = s; }
    }
    return best;
}

static int ttt_best_move(char *b)
{
    int bi = -1, bs = -1000;
    for (int i = 0; i < 9; i++) {
        if (b[i] != ' ') continue;
        b[i] = 'O';
        int s = ttt_minimax(b, 'X', 0);
        b[i] = ' ';
        if (s > bs) { bs = s; bi = i; }
    }
    return bi;
}

static void ttt_render(int sock, const char *b)
{
    shell_send_all(sock, "\033[2J\033[H", 7);
    shell_printf(sock, "\033[92mTic-Tac-Toe\033[0m  —  you are \033[93mX\033[0m, ESP32 is \033[91mO\033[0m\r\n");
    shell_printf(sock, "Pick a cell 1-9 (q to quit)\r\n\r\n");
    for (int row = 0; row < 3; row++) {
        char cell[3][8];
        for (int c = 0; c < 3; c++) {
            int i = row * 3 + c;
            if (b[i] == 'X')      snprintf(cell[c], sizeof(cell[c]), "\033[93mX\033[0m");
            else if (b[i] == 'O') snprintf(cell[c], sizeof(cell[c]), "\033[91mO\033[0m");
            else                  snprintf(cell[c], sizeof(cell[c]), "%d", i + 1);
        }
        shell_printf(sock, "  %s | %s | %s\r\n", cell[0], cell[1], cell[2]);
        if (row < 2) shell_printf(sock, " ---+---+---\r\n");
    }
    shell_printf(sock, "\r\n");
}

void fun_tictactoe(shell_ctx_t *ctx)
{
    if (!ctx->char_mode) {
        shell_printf(ctx->sock,
            "tictactoe needs a char-mode terminal (telnet/PuTTY). On nc, type 'arrows' first.\r\n");
        return;
    }
    int sock = ctx->sock;
    char b[9]; memset(b, ' ', 9);

    for (;;) {
        ttt_render(sock, b);
        char w = ttt_winner(b);
        if (w) {
            if (w == 'X')      shell_printf(sock, "\033[93mYou win!\033[0m (the AI let you — impressive!)\r\n");
            else if (w == 'O') shell_printf(sock, "\033[91mESP32 wins.\033[0m Try again!\r\n");
            else               shell_printf(sock, "Draw — well played.\r\n");
            return;
        }

        // Player move.
        int move = -1;
        while (move < 0) {
            int k = wait_key(sock, 120000);
            if (k == -2 || k == 'q' || k == 'Q' || k == 0x03) {
                shell_printf(sock, "\r\nGame quit.\r\n"); return;
            }
            if (k >= '1' && k <= '9') {
                int idx = k - '1';
                if (b[idx] == ' ') move = idx;
            }
        }
        b[move] = 'X';

        if (ttt_winner(b)) continue;      // player may have just won/filled

        int cpu = ttt_best_move(b);
        if (cpu >= 0) b[cpu] = 'O';
    }
}
