#include "unixcmds.h"
#include "commands.h"
#include "shell.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"

// Read a whole file into a heap buffer (PSRAM-preferred), capped. Caller frees.
// Returns the buffer (NUL-terminated) and sets *len, or NULL on error.
static char *slurp(const char *path, size_t *len, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(cap + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    buf[n] = '\0';
    if (len) *len = n;
    return buf;
}

#define SLURP_CAP (512 * 1024)

// ---- grep [-i] [-n] <pattern> <file...> -----------------------------------
void ux_grep(shell_ctx_t *ctx, int argc, char **argv)
{
    bool icase = false, numbers = false;
    int a = 1;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-i")) icase = true;
        else if (!strcmp(argv[a], "-n")) numbers = true;
        else if (!strcmp(argv[a], "-in") || !strcmp(argv[a], "-ni")) { icase = numbers = true; }
    }
    if (argc - a < 2) { shell_printf(ctx->sock, "usage: grep [-i] [-n] <pattern> <file...>\r\n"); return; }
    const char *pat = argv[a++];

    for (; a < argc; a++) {
        char path[256];
        cmd_resolve_path(ctx, argv[a], path, sizeof(path));
        size_t len = 0;
        char *buf = slurp(path, &len, SLURP_CAP);
        if (!buf) { shell_printf(ctx->sock, "grep: %s: No such file\r\n", argv[a]); continue; }

        int lineno = 0;
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            lineno++;
            // strip trailing CR
            size_t l = strlen(line);
            if (l && line[l - 1] == '\r') line[l - 1] = '\0';
            bool hit = icase ? (strcasestr(line, pat) != NULL) : (strstr(line, pat) != NULL);
            if (hit) {
                if (numbers) shell_printf(ctx->sock, "%d:%s\r\n", lineno, line);
                else         shell_printf(ctx->sock, "%s\r\n", line);
            }
        }
        free(buf);
    }
}

// ---- wc <file> ------------------------------------------------------------
void ux_wc(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 2) { shell_printf(ctx->sock, "usage: wc <file>\r\n"); return; }
    char path[256];
    cmd_resolve_path(ctx, argv[1], path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) { shell_printf(ctx->sock, "wc: %s: No such file\r\n", argv[1]); return; }
    unsigned long lines = 0, words = 0, bytes = 0;
    int c, inword = 0;
    while ((c = fgetc(f)) != EOF) {
        bytes++;
        if (c == '\n') lines++;
        if (isspace(c)) inword = 0;
        else if (!inword) { inword = 1; words++; }
    }
    fclose(f);
    shell_printf(ctx->sock, "%7lu %7lu %7lu %s\r\n", lines, words, bytes, argv[1]);
}

// ---- head / tail ----------------------------------------------------------
static int parse_nflag(int argc, char **argv, int *file_idx, int def)
{
    int n = def; *file_idx = 1;
    if (argc >= 3 && !strcmp(argv[1], "-n")) { n = atoi(argv[2]); *file_idx = 3; }
    else if (argc >= 2 && argv[1][0] == '-' && isdigit((unsigned char)argv[1][1])) {
        n = atoi(argv[1] + 1); *file_idx = 2;   // e.g. head -20 file
    }
    if (n < 0) n = 0;
    return n;
}

void ux_head(shell_ctx_t *ctx, int argc, char **argv)
{
    int fi; int n = parse_nflag(argc, argv, &fi, 10);
    if (fi >= argc) { shell_printf(ctx->sock, "usage: head [-n N] <file>\r\n"); return; }
    char path[256];
    cmd_resolve_path(ctx, argv[fi], path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) { shell_printf(ctx->sock, "head: %s: No such file\r\n", argv[fi]); return; }
    char line[512]; int printed = 0;
    while (printed < n && fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
        shell_printf(ctx->sock, "%s\r\n", line);
        printed++;
    }
    fclose(f);
}

void ux_tail(shell_ctx_t *ctx, int argc, char **argv)
{
    int fi; int n = parse_nflag(argc, argv, &fi, 10);
    if (fi >= argc) { shell_printf(ctx->sock, "usage: tail [-n N] <file>\r\n"); return; }
    char path[256];
    cmd_resolve_path(ctx, argv[fi], path, sizeof(path));
    size_t len = 0;
    char *buf = slurp(path, &len, SLURP_CAP);
    if (!buf) { shell_printf(ctx->sock, "tail: %s: No such file\r\n", argv[fi]); return; }
    // Walk backwards counting newlines to find the start of the last n lines.
    int nl = 0; size_t start = len;
    while (start > 0) {
        if (buf[start - 1] == '\n') {
            if (++nl > n) break;
        }
        start--;
    }
    char *save = NULL;
    for (char *line = strtok_r(buf + start, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        size_t l = strlen(line);
        if (l && line[l-1] == '\r') line[l-1] = '\0';
        shell_printf(ctx->sock, "%s\r\n", line);
    }
    free(buf);
}

// ---- cp / mv --------------------------------------------------------------
void ux_cp(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 3) { shell_printf(ctx->sock, "usage: cp <src> <dst>\r\n"); return; }
    char src[256], dst[256];
    cmd_resolve_path(ctx, argv[1], src, sizeof(src));
    cmd_resolve_path(ctx, argv[2], dst, sizeof(dst));
    FILE *in = fopen(src, "rb");
    if (!in) { shell_printf(ctx->sock, "cp: %s: No such file\r\n", argv[1]); return; }
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); shell_printf(ctx->sock, "cp: cannot create %s\r\n", argv[2]); return; }
    char *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(8192);
    size_t r; unsigned long total = 0;
    while (buf && (r = fread(buf, 1, 8192, in)) > 0) { fwrite(buf, 1, r, out); total += r; }
    free(buf); fclose(in); fclose(out);
    shell_printf(ctx->sock, "copied %lu bytes -> %s\r\n", total, argv[2]);
}

void ux_mv(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 3) { shell_printf(ctx->sock, "usage: mv <src> <dst>\r\n"); return; }
    char src[256], dst[256];
    cmd_resolve_path(ctx, argv[1], src, sizeof(src));
    cmd_resolve_path(ctx, argv[2], dst, sizeof(dst));
    if (rename(src, dst) == 0) shell_printf(ctx->sock, "%s -> %s\r\n", argv[1], argv[2]);
    else shell_printf(ctx->sock, "mv: cannot move '%s'\r\n", argv[1]);
}

// ---- find / du (recursive) ------------------------------------------------
static void walk(shell_ctx_t *ctx, const char *dir, const char *match,
                 uint64_t *total, int depth)
{
    if (depth > 8) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char full[300];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        bool isdir = (e->d_type == DT_DIR);
        long sz = 0;
        if (stat(full, &st) == 0) { sz = (long)st.st_size; isdir = S_ISDIR(st.st_mode); }
        if (total) { if (!isdir) *total += sz; }
        else {
            // find: print matches (or everything if no match given)
            if (!match || strstr(e->d_name, match))
                shell_printf(ctx->sock, "%s%s\r\n", full, isdir ? "/" : "");
        }
        if (isdir) walk(ctx, full, match, total, depth + 1);
    }
    closedir(d);
}

void ux_find(shell_ctx_t *ctx, int argc, char **argv)
{
    char base[256];
    const char *arg = (argc >= 2 && argv[1][0] != '-') ? argv[1] : ".";
    cmd_resolve_path(ctx, arg, base, sizeof(base));
    const char *match = (argc >= 3) ? argv[2] : NULL;
    // allow `find <name>` shorthand: if arg has no path chars, treat as match in cwd
    walk(ctx, base, match, NULL, 0);
}

void ux_du(shell_ctx_t *ctx, int argc, char **argv)
{
    char base[256];
    cmd_resolve_path(ctx, (argc >= 2) ? argv[1] : ".", base, sizeof(base));
    uint64_t total = 0;
    walk(ctx, base, NULL, &total, 0);
    shell_printf(ctx->sock, "%llu KB\t%s\r\n", (unsigned long long)(total / 1024),
                 (argc >= 2) ? argv[1] : ".");
}

// ---- hexdump <file> -------------------------------------------------------
void ux_hexdump(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 2) { shell_printf(ctx->sock, "usage: hexdump <file>\r\n"); return; }
    char path[256];
    cmd_resolve_path(ctx, argv[1], path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) { shell_printf(ctx->sock, "hexdump: %s: No such file\r\n", argv[1]); return; }
    unsigned char row[16];
    unsigned long off = 0;
    size_t r;
    while ((r = fread(row, 1, 16, f)) > 0) {
        char line[100]; int p = 0;
        p += snprintf(line + p, sizeof(line) - p, "%08lx  ", off);
        for (size_t i = 0; i < 16; i++) {
            if (i < r) p += snprintf(line + p, sizeof(line) - p, "%02x ", row[i]);
            else       p += snprintf(line + p, sizeof(line) - p, "   ");
            if (i == 7) p += snprintf(line + p, sizeof(line) - p, " ");
        }
        p += snprintf(line + p, sizeof(line) - p, " |");
        for (size_t i = 0; i < r; i++)
            p += snprintf(line + p, sizeof(line) - p, "%c", isprint(row[i]) ? row[i] : '.');
        shell_printf(ctx->sock, "%s|\r\n", line);
        off += r;
        if (off > 64 * 1024) { shell_printf(ctx->sock, "... (truncated at 64 KB)\r\n"); break; }
    }
    fclose(f);
}

// ---- history --------------------------------------------------------------
#define UX_HIST 20
static char  s_hist[UX_HIST][96];
static int   s_hist_count;

void ux_history_add(const char *line)
{
    if (!line || !*line) return;
    // skip duplicate of the most recent
    int last = (s_hist_count - 1) % UX_HIST;
    if (s_hist_count > 0 && strncmp(s_hist[last], line, sizeof(s_hist[0]) - 1) == 0) return;
    strlcpy(s_hist[s_hist_count % UX_HIST], line, sizeof(s_hist[0]));
    s_hist_count++;
}

void ux_history(shell_ctx_t *ctx)
{
    int start = (s_hist_count > UX_HIST) ? s_hist_count - UX_HIST : 0;
    for (int i = start; i < s_hist_count; i++)
        shell_printf(ctx->sock, "%4d  %s\r\n", i + 1, s_hist[i % UX_HIST]);
}
