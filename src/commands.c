#include "commands.h"
#include "shell.h"
#include "htop.h"
#include "config.h"
#include "dmesg.h"
#include "wifi.h"
#include "sdcard.h"
#include "http_client.h"
#include "camera.h"
#include "editor.h"
#include "fileserver.h"
#include "python.h"
#include "js.h"
#include "cc.h"
#include "fun.h"
#include "gpio_cmd.h"
#include "unixcmds.h"
#include "qrcmd.h"
#include "temp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "mbedtls/base64.h"

#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

// Internal die temperature (calibrated) — see temp.c.
static inline float esp32_temp_c(void) { return temp_read_c(); }

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "ping/ping_sock.h"

// ---------------------------------------------------------------------------
//  Banner + prompt
// ---------------------------------------------------------------------------
void cmd_banner(shell_ctx_t *ctx)
{
    static const char *banner =
        " ______  _____ _____ ____ ___  \r\n"
        "|  ____|/ ____|  __ \\___ \\__ \\ \r\n"
        "| |__  | (___ | |__) |__) | ) |\r\n"
        "|  __|  \\___ \\|  ___/|__ < / / \r\n"
        "| |____ ____) | |    ___) / /_ \r\n"
        "|______|_____/|_|   |____/____|\r\n"
        "\r\n"
        " ESP32 Linux Shell v1.0\r\n"
        " Kernel: ESP-IDF FreeRTOS\r\n"
        " Arch: Xtensa LX6 dual-core @ 240MHz\r\n"
        " RAM: 520KB SRAM + 8MB PSRAM\r\n"
        " Storage: SD FAT32\r\n"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\r\n"
        "Last login: just now from tcp\r\n";
    shell_send_all(ctx->sock, banner, strlen(banner));
}

void cmd_display_path(const char *fs_path, char *out, size_t n)
{
    size_t mlen = strlen(SD_MOUNT);
    if (strncmp(fs_path, SD_MOUNT, mlen) == 0) {
        const char *rest = fs_path + mlen;
        if (*rest == '\0') snprintf(out, n, "/");
        else               snprintf(out, n, "%s", rest);
    } else {
        snprintf(out, n, "%s", fs_path);
    }
}

void cmd_prompt_build(shell_ctx_t *ctx, char *out, size_t n)
{
    char disp[256];
    if (strcmp(ctx->cwd, SD_MOUNT) == 0) snprintf(disp, sizeof(disp), "~");
    else cmd_display_path(ctx->cwd, disp, sizeof(disp));
    snprintf(out, n, "root@esp32:%s# ", disp);
}

void cmd_prompt(shell_ctx_t *ctx)
{
    char p[300];
    cmd_prompt_build(ctx, p, sizeof(p));
    shell_send_all(ctx->sock, p, strlen(p));
}

// ---------------------------------------------------------------------------
//  Path helpers
// ---------------------------------------------------------------------------
static void normalize_virtual(const char *vpath, char *out, size_t n)
{
    char tmp[512];
    strncpy(tmp, vpath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *comps[64];
    int   nc = 0;
    char *save = NULL;
    for (char *t = strtok_r(tmp, "/", &save); t && nc < 64;
         t = strtok_r(NULL, "/", &save)) {
        if (strcmp(t, ".") == 0) continue;
        if (strcmp(t, "..") == 0) { if (nc > 0) nc--; continue; }
        comps[nc++] = t;
    }

    if (nc == 0) { snprintf(out, n, "/"); return; }
    size_t p = 0;
    out[0] = '\0';
    for (int i = 0; i < nc && p < n; i++)
        p += snprintf(out + p, n - p, "/%s", comps[i]);
}

void cmd_resolve_path(shell_ctx_t *ctx, const char *arg, char *out, size_t n)
{
    if (!arg || !*arg) { snprintf(out, n, "%s", ctx->cwd); return; }

    char vin[512];
    if (arg[0] == '/') {
        snprintf(vin, sizeof(vin), "%s", arg);
    } else if (arg[0] == '~') {
        snprintf(vin, sizeof(vin), "%s", (arg[1] ? arg + 1 : "/"));
    } else {
        char cwd_v[256];
        cmd_display_path(ctx->cwd, cwd_v, sizeof(cwd_v));
        snprintf(vin, sizeof(vin), "%s/%s",
                 (strcmp(cwd_v, "/") == 0 ? "" : cwd_v), arg);
    }

    char vnorm[512];
    normalize_virtual(vin, vnorm, sizeof(vnorm));

    if (strcmp(vnorm, "/") == 0) snprintf(out, n, "%s", SD_MOUNT);
    else                         snprintf(out, n, "%s%s", SD_MOUNT, vnorm);
}

// ---------------------------------------------------------------------------
//  Tokenizer
// ---------------------------------------------------------------------------
#define MAX_ARGS 32
static int tokenize(char *line, char **argv)
{
    int argc = 0;
    char *save = NULL;
    for (char *t = strtok_r(line, " \t", &save); t && argc < MAX_ARGS;
         t = strtok_r(NULL, " \t", &save))
        argv[argc++] = t;
    return argc;
}

static void require_sd(shell_ctx_t *ctx)
{
    if (!sdcard_mounted())
        shell_printf(ctx->sock, "warning: SD card not mounted\r\n");
}

// ---------------------------------------------------------------------------
//  Individual commands
// ---------------------------------------------------------------------------
static void do_ls(shell_ctx_t *ctx, int argc, char **argv)
{
    bool longfmt = false;
    const char *patharg = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') { if (strchr(argv[i], 'l')) longfmt = true; }
        else patharg = argv[i];
    }

    char path[256];
    cmd_resolve_path(ctx, patharg, path, sizeof(path));

    DIR *d = opendir(path);
    if (!d) {
        shell_printf(ctx->sock, "ls: cannot access '%s': No such file or directory\r\n",
                     patharg ? patharg : ".");
        return;
    }

    struct dirent *e;
    int col = 0;
    while ((e = readdir(d)) != NULL) {
        if (longfmt) {
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
            struct stat st;
            char datebuf[24] = "??? ?? ??:??";
            long size = 0;
            char type = '-';
            if (stat(full, &st) == 0) {
                size = (long)st.st_size;
                type = S_ISDIR(st.st_mode) ? 'd' : '-';
                struct tm tm;
                localtime_r(&st.st_mtime, &tm);
                strftime(datebuf, sizeof(datebuf), "%b %e %H:%M", &tm);
            } else if (e->d_type == DT_DIR) {
                type = 'd';
            }
            shell_printf(ctx->sock, "%crw-r--r-- 1 root root %8ld %s %s\r\n",
                         type, size, datebuf, e->d_name);
        } else {
            const char *slash = (e->d_type == DT_DIR) ? "/" : "";
            shell_printf(ctx->sock, "%s%s  ", e->d_name, slash);
            col++;
        }
    }
    closedir(d);
    if (!longfmt && col > 0) shell_printf(ctx->sock, "\r\n");
    else if (!longfmt) shell_printf(ctx->sock, "\r\n");
}

static void do_cat(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 2) { shell_printf(ctx->sock, "usage: cat <file>\r\n"); return; }

    for (int a = 1; a < argc; a++) {
        char path[256];
        cmd_resolve_path(ctx, argv[a], path, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (!f) {
            shell_printf(ctx->sock, "cat: %s: No such file or directory\r\n", argv[a]);
            continue;
        }
        // Stream through a PSRAM buffer.
        size_t bufsz = 8 * 1024;
        char *buf = heap_caps_malloc(bufsz, MALLOC_CAP_SPIRAM);
        if (!buf) { buf = malloc(bufsz); }
        if (!buf) { fclose(f); shell_printf(ctx->sock, "cat: out of memory\r\n"); continue; }
        size_t r;
        while ((r = fread(buf, 1, bufsz, f)) > 0)
            shell_send_all(ctx->sock, buf, r);
        free(buf);
        fclose(f);
    }
}

static void do_echo(shell_ctx_t *ctx, int argc, char **argv)
{
    // Find a redirection token.
    int redir = -1;      // index of > or >>
    bool append = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], ">") == 0)  { redir = i; append = false; break; }
        if (strcmp(argv[i], ">>") == 0) { redir = i; append = true;  break; }
    }

    // Build the text (argv[1 .. text_end]).
    int text_end = (redir >= 0) ? redir : argc;
    char text[512];
    size_t p = 0;
    text[0] = '\0';
    for (int i = 1; i < text_end; i++)
        p += snprintf(text + p, sizeof(text) - p, "%s%s",
                      (i > 1 ? " " : ""), argv[i]);

    if (redir < 0) {
        shell_printf(ctx->sock, "%s\r\n", text);
        return;
    }

    if (redir + 1 >= argc) {
        shell_printf(ctx->sock, "echo: syntax error near '>'\r\n");
        return;
    }
    char path[256];
    cmd_resolve_path(ctx, argv[redir + 1], path, sizeof(path));
    FILE *f = fopen(path, append ? "ab" : "wb");
    if (!f) { shell_printf(ctx->sock, "echo: %s: cannot open\r\n", argv[redir + 1]); return; }
    fprintf(f, "%s\n", text);
    fclose(f);
}

static void do_mkdir(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 2) { shell_printf(ctx->sock, "usage: mkdir <dir>\r\n"); return; }
    for (int a = 1; a < argc; a++) {
        char path[256];
        cmd_resolve_path(ctx, argv[a], path, sizeof(path));
        if (mkdir(path, 0777) != 0)
            shell_printf(ctx->sock, "mkdir: cannot create '%s': %s\r\n",
                         argv[a], strerror(errno));
    }
}

static void do_rm(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 2) { shell_printf(ctx->sock, "usage: rm <file>\r\n"); return; }
    for (int a = 1; a < argc; a++) {
        if (argv[a][0] == '-') continue;   // ignore flags like -f/-r
        char path[256];
        cmd_resolve_path(ctx, argv[a], path, sizeof(path));
        if (unlink(path) != 0) {
            // maybe it's a directory
            if (rmdir(path) != 0)
                shell_printf(ctx->sock, "rm: cannot remove '%s': %s\r\n",
                             argv[a], strerror(errno));
        }
    }
}

static void do_pwd(shell_ctx_t *ctx)
{
    char disp[256];
    cmd_display_path(ctx->cwd, disp, sizeof(disp));
    shell_printf(ctx->sock, "%s\r\n", disp);
}

static void do_cd(shell_ctx_t *ctx, int argc, char **argv)
{
    const char *arg = (argc >= 2) ? argv[1] : "~";
    char path[256];
    cmd_resolve_path(ctx, arg, path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        shell_printf(ctx->sock, "cd: %s: No such file or directory\r\n", arg);
        return;
    }
    strncpy(ctx->cwd, path, sizeof(ctx->cwd) - 1);
    ctx->cwd[sizeof(ctx->cwd) - 1] = '\0';
}

static void do_free(shell_ctx_t *ctx)
{
    size_t int_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t int_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ps_total  = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t ps_free   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    shell_printf(ctx->sock, "              total        used        free\r\n");
    shell_printf(ctx->sock, "Internal:  %8u B  %8u B  %8u B\r\n",
                 (unsigned)int_total, (unsigned)(int_total - int_free), (unsigned)int_free);
    if (ps_total)
        shell_printf(ctx->sock, "PSRAM:     %8u B  %8u B  %8u B\r\n",
                     (unsigned)ps_total, (unsigned)(ps_total - ps_free), (unsigned)ps_free);
    else
        shell_printf(ctx->sock, "PSRAM:        (not detected)\r\n");
    shell_printf(ctx->sock, "Heap free (all): %u B, min ever: %u B\r\n",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size());
}

static void do_uptime(shell_ctx_t *ctx)
{
    uint64_t us = (uint64_t)esp_timer_get_time();
    uint32_t secs = us / 1000000ULL;
    uint32_t days = secs / 86400;
    uint32_t hh = (secs % 86400) / 3600;
    uint32_t mm = (secs % 3600) / 60;
    uint32_t ss = secs % 60;
    if (days)
        shell_printf(ctx->sock, "up %u days, %02u:%02u:%02u\r\n",
                     (unsigned)days, (unsigned)hh, (unsigned)mm, (unsigned)ss);
    else
        shell_printf(ctx->sock, "up %02u:%02u:%02u\r\n",
                     (unsigned)hh, (unsigned)mm, (unsigned)ss);
}

static void do_ifconfig(shell_ctx_t *ctx)
{
    esp_netif_ip_info_t ip;
    wifi_get_ip(&ip);

    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    wifi_ap_record_t ap;
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    shell_printf(ctx->sock, "wlan0     Link encap:Ethernet  HWaddr %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    shell_printf(ctx->sock, "          inet addr:" IPSTR "  Mask:" IPSTR "\r\n",
                 IP2STR(&ip.ip), IP2STR(&ip.netmask));
    shell_printf(ctx->sock, "          gateway:" IPSTR "\r\n", IP2STR(&ip.gw));
    shell_printf(ctx->sock, "          %s  RSSI:%d dBm  SSID:%s\r\n",
                 wifi_is_connected() ? "UP RUNNING" : "DOWN",
                 rssi, (char *)ap.ssid);
}

// ---- ping ------------------------------------------------------------------
typedef struct {
    int sock;
    SemaphoreHandle_t done;
} ping_user_t;

static void ping_on_success(esp_ping_handle_t hdl, void *args)
{
    ping_user_t *pu = (ping_user_t *)args;
    uint8_t  ttl;
    uint16_t seqno;
    uint32_t elapsed, size;
    ip_addr_t target;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target, sizeof(target));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &size, sizeof(size));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    shell_printf(pu->sock, "%u bytes from %s: icmp_seq=%u ttl=%u time=%u ms\r\n",
                 (unsigned)size, ipaddr_ntoa(&target),
                 (unsigned)seqno, (unsigned)ttl, (unsigned)elapsed);
}

static void ping_on_timeout(esp_ping_handle_t hdl, void *args)
{
    ping_user_t *pu = (ping_user_t *)args;
    uint16_t seqno;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    shell_printf(pu->sock, "Request timeout for icmp_seq %u\r\n", (unsigned)seqno);
}

static void ping_on_end(esp_ping_handle_t hdl, void *args)
{
    ping_user_t *pu = (ping_user_t *)args;
    uint32_t transmitted, received, total_time;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &total_time, sizeof(total_time));
    uint32_t loss = transmitted ? (100 * (transmitted - received)) / transmitted : 0;
    shell_printf(pu->sock, "\r\n--- ping statistics ---\r\n");
    shell_printf(pu->sock, "%u packets transmitted, %u received, %u%% packet loss, time %ums\r\n",
                 (unsigned)transmitted, (unsigned)received, (unsigned)loss, (unsigned)total_time);
    xSemaphoreGive(pu->done);
}

static void do_ping(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 2) { shell_printf(ctx->sock, "usage: ping <host>\r\n"); return; }
    const char *host = argv[1];

    struct addrinfo hint = { .ai_family = AF_INET };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hint, &res) != 0 || !res) {
        shell_printf(ctx->sock, "ping: cannot resolve %s: Unknown host\r\n", host);
        return;
    }
    struct in_addr addr4 = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    ip_addr_t target = { 0 };
    inet_addr_to_ip4addr(ip_2_ip4(&target), &addr4);
    IP_SET_TYPE_VAL(target, IPADDR_TYPE_V4);
    freeaddrinfo(res);

    shell_printf(ctx->sock, "PING %s (%s): 56 data bytes\r\n", host, ipaddr_ntoa(&target));

    ping_user_t pu = { .sock = ctx->sock, .done = xSemaphoreCreateBinary() };

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count       = 4;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_on_success,
        .on_ping_timeout = ping_on_timeout,
        .on_ping_end     = ping_on_end,
        .cb_args         = &pu,
    };

    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
        shell_printf(ctx->sock, "ping: failed to create session\r\n");
        vSemaphoreDelete(pu.done);
        return;
    }
    esp_ping_start(ping);
    xSemaphoreTake(pu.done, portMAX_DELAY);
    esp_ping_stop(ping);
    esp_ping_delete_session(ping);
    vSemaphoreDelete(pu.done);
}

static void do_uname(shell_ctx_t *ctx, int argc, char **argv)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const char *model = "ESP32";
    switch (chip.model) {
        case CHIP_ESP32:   model = "ESP32";   break;
        case CHIP_ESP32S2: model = "ESP32-S2"; break;
        case CHIP_ESP32S3: model = "ESP32-S3"; break;
        case CHIP_ESP32C3: model = "ESP32-C3"; break;
        default: break;
    }

    bool all = (argc >= 2 && strcmp(argv[1], "-a") == 0);
    if (all) {
        shell_printf(ctx->sock,
            "ESP32 esp32 %s FreeRTOS SMP %s rev%d %d-core Xtensa-LX6 @240MHz\r\n",
            esp_get_idf_version(), model, chip.revision, chip.cores);
    } else {
        shell_printf(ctx->sock, "ESP32\r\n");
    }
}

static void do_wget(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 2) { shell_printf(ctx->sock, "usage: wget <url> [path]\r\n"); return; }
    require_sd(ctx);
    const char *url = argv[1];

    // Destination filename.
    char path[256];
    if (argc >= 3) {
        cmd_resolve_path(ctx, argv[2], path, sizeof(path));
    } else {
        const char *slash = strrchr(url, '/');
        const char *name = (slash && slash[1]) ? slash + 1 : "index.html";
        cmd_resolve_path(ctx, name, path, sizeof(path));
    }

    shell_printf(ctx->sock, "Connecting... downloading '%s'\r\n", url);
    size_t len = 0;
    esp_err_t err = http_download(url, path, &len);
    if (err == ESP_OK) {
        char disp[256];
        cmd_display_path(path, disp, sizeof(disp));
        shell_printf(ctx->sock, "Saved %u bytes to %s\r\n", (unsigned)len, disp);
    } else {
        shell_printf(ctx->sock, "wget: download failed (%s)\r\n", esp_err_to_name(err));
    }
}

static void do_curl(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 2) { shell_printf(ctx->sock, "usage: curl <url>\r\n"); return; }
    esp_err_t err = http_get_to_socket(argv[1], ctx->sock);
    shell_printf(ctx->sock, "\r\n");
    if (err != ESP_OK)
        shell_printf(ctx->sock, "curl: request failed (%s)\r\n", esp_err_to_name(err));
}

static void do_df(shell_ctx_t *ctx)
{
    uint64_t total = 0, freeb = 0;
    sdcard_usage(&total, &freeb);
    shell_printf(ctx->sock, "Filesystem     Size     Used    Avail  Mounted on\r\n");
    shell_printf(ctx->sock, "%-12s %6lluM %7lluM %7lluM  %s\r\n", "sdcard",
                 total / (1024 * 1024),
                 (total - freeb) / (1024 * 1024),
                 freeb / (1024 * 1024), SD_MOUNT);
}

// ---- camera: photo ---------------------------------------------------------
static void do_photo(shell_ctx_t *ctx, int argc, char **argv)
{
    if (!camera_ready()) {
        shell_printf(ctx->sock, "photo: camera not available (check the ribbon cable)\r\n");
        return;
    }
    if (!sdcard_mounted()) {
        shell_printf(ctx->sock, "photo: SD card not mounted — nowhere to save\r\n");
        return;
    }

    char path[256];
    if (argc >= 2) {
        // Use the given name; append .jpg if it has no extension.
        const char *name = argv[1];
        if (strchr(name, '.')) cmd_resolve_path(ctx, name, path, sizeof(path));
        else {
            char withext[128];
            snprintf(withext, sizeof(withext), "%s.jpg", name);
            cmd_resolve_path(ctx, withext, path, sizeof(path));
        }
    } else {
        // Auto-name: first free /sdcard/photo_NNN.jpg
        int idx = 1;
        for (; idx < 1000; idx++) {
            char cand[64];
            snprintf(cand, sizeof(cand), "photo_%03d.jpg", idx);
            char full[256];
            cmd_resolve_path(ctx, cand, full, sizeof(full));
            struct stat st;
            if (stat(full, &st) != 0) { strcpy(path, full); break; }
        }
        if (idx >= 1000) { shell_printf(ctx->sock, "photo: too many photos\r\n"); return; }
    }

    shell_printf(ctx->sock, "Capturing...\r\n");
    size_t len = 0;
    esp_err_t err = camera_capture_to_file(path, &len);
    if (err == ESP_OK) {
        char disp[256];
        cmd_display_path(path, disp, sizeof(disp));
        int w = 0, h = 0;
        camera_max_resolution(&w, &h);
        shell_printf(ctx->sock, "Saved %s  (%ux%u, %u bytes)\r\n",
                     disp, (unsigned)w, (unsigned)h, (unsigned)len);
    } else {
        shell_printf(ctx->sock, "photo: capture failed (%s)\r\n", esp_err_to_name(err));
    }
}

// ---- camera: video recording ----------------------------------------------
static void do_video(shell_ctx_t *ctx, int argc, char **argv)
{
    if (!camera_ready())   { shell_printf(ctx->sock, "video: camera not available\r\n"); return; }
    if (!sdcard_mounted()) { shell_printf(ctx->sock, "video: SD card not mounted\r\n"); return; }

    int secs = (argc >= 2) ? atoi(argv[1]) : 10;
    if (secs < 1) secs = 1;
    if (secs > 60) secs = 60;

    // Auto-name /video_NNN.avi
    char path[256];
    int idx = 1;
    for (; idx < 1000; idx++) {
        char cand[64];
        snprintf(cand, sizeof(cand), "video_%03d.avi", idx);
        char full[256];
        cmd_resolve_path(ctx, cand, full, sizeof(full));
        struct stat st;
        if (stat(full, &st) != 0) { strcpy(path, full); break; }
    }
    if (idx >= 1000) { shell_printf(ctx->sock, "video: too many files\r\n"); return; }

    shell_printf(ctx->sock, "Recording %ds of video (VGA)... hold still\r\n", secs);
    int frames = 0, fps = 0;
    esp_err_t err = camera_record_avi(path, secs, &frames, &fps);
    if (err == ESP_OK) {
        struct stat st;
        long sz = (stat(path, &st) == 0) ? (long)st.st_size : 0;
        char disp[256];
        cmd_display_path(path, disp, sizeof(disp));
        shell_printf(ctx->sock, "Saved %s  (%d frames, ~%d fps, %ld bytes)\r\n",
                     disp, frames, fps, sz);
    } else {
        shell_printf(ctx->sock, "video: recording failed (%s)\r\n", esp_err_to_name(err));
    }
}

// ---- camera: stream --------------------------------------------------------
static void do_stream(shell_ctx_t *ctx, int argc, char **argv)
{
    if (!camera_ready()) {
        shell_printf(ctx->sock, "stream: camera not available (check the ribbon cable)\r\n");
        return;
    }

    esp_netif_ip_info_t ip;
    wifi_get_ip(&ip);
    const char *action = (argc >= 2) ? argv[1] : "status";

    if (!strcmp(action, "start")) {
        if (camera_stream_running()) {
            shell_printf(ctx->sock, "stream: already running at http://" IPSTR ":81/\r\n",
                         IP2STR(&ip.ip));
            return;
        }
        if (camera_stream_start() == ESP_OK)
            shell_printf(ctx->sock, "Stream started. Open in a browser:  http://" IPSTR ":81/\r\n",
                         IP2STR(&ip.ip));
        else
            shell_printf(ctx->sock, "stream: failed to start server\r\n");
    } else if (!strcmp(action, "stop")) {
        if (!camera_stream_running()) {
            shell_printf(ctx->sock, "stream: not running\r\n");
            return;
        }
        camera_stream_stop();
        shell_printf(ctx->sock, "Stream stopped.\r\n");
    } else {  // status
        if (camera_stream_running())
            shell_printf(ctx->sock, "stream: ON  ->  http://" IPSTR ":81/\r\n", IP2STR(&ip.ip));
        else
            shell_printf(ctx->sock, "stream: OFF  (use 'stream start')\r\n");
    }
}

// ---- nano: full-screen editor ---------------------------------------------
static void do_nano(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 2) { shell_printf(ctx->sock, "usage: nano <file>\r\n"); return; }
    if (!ctx->char_mode) {
        shell_printf(ctx->sock,
            "nano needs a character-mode terminal (your client sends whole lines).\r\n"
            "Connect with telnet or PuTTY:  telnet %s %d\r\n"
            "or wrap netcat in raw mode:    stty raw -echo; nc <ip> %d; stty sane\r\n"
            "(For quick edits over netcat, use:  echo text > file   or the 'files' web upload.)\r\n",
            "<ip>", SHELL_PORT, SHELL_PORT);
        return;
    }
    if (!sdcard_mounted()) {
        shell_printf(ctx->sock, "nano: SD card not mounted — can't save files\r\n");
        return;
    }
    char path[256];
    cmd_resolve_path(ctx, argv[1], path, sizeof(path));
    char disp[256];
    cmd_display_path(path, disp, sizeof(disp));
    editor_run(ctx, path, disp);
}

// ---- files: browser file manager ------------------------------------------
static void do_files(shell_ctx_t *ctx, int argc, char **argv)
{
    esp_netif_ip_info_t ip;
    wifi_get_ip(&ip);
    const char *action = (argc >= 2) ? argv[1] : "status";

    if (!strcmp(action, "start")) {
        if (fileserver_running()) {
            shell_printf(ctx->sock, "files: already running at http://" IPSTR "/\r\n", IP2STR(&ip.ip));
            return;
        }
        if (fileserver_start() == ESP_OK)
            shell_printf(ctx->sock, "File manager started:  http://" IPSTR "/\r\n", IP2STR(&ip.ip));
        else
            shell_printf(ctx->sock, "files: failed to start server\r\n");
    } else if (!strcmp(action, "stop")) {
        fileserver_stop();
        shell_printf(ctx->sock, "File manager stopped.\r\n");
    } else {
        if (fileserver_running())
            shell_printf(ctx->sock, "files: ON  ->  http://" IPSTR "/\r\n", IP2STR(&ip.ip));
        else
            shell_printf(ctx->sock, "files: OFF  (use 'files start')\r\n");
    }
}

// ---- get: base64-dump a file to the terminal ------------------------------
static void do_get(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 2) { shell_printf(ctx->sock, "usage: get <file>\r\n"); return; }
    char path[256];
    cmd_resolve_path(ctx, argv[1], path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) { shell_printf(ctx->sock, "get: %s: not found\r\n", argv[1]); return; }

    char disp[256];
    cmd_display_path(path, disp, sizeof(disp));
    shell_printf(ctx->sock, "----BEGIN base64 %s----\r\n", disp);

    unsigned char *in  = heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
    unsigned char *out = heap_caps_malloc(4200, MALLOC_CAP_SPIRAM);
    if (in && out) {
        size_t r;
        while ((r = fread(in, 1, 3072, f)) > 0) {   // multiple of 3: no padding mid-stream
            size_t olen = 0;
            mbedtls_base64_encode(out, 4200, &olen, in, r);
            shell_send_all(ctx->sock, (char *)out, olen);
            shell_send_all(ctx->sock, "\r\n", 2);
        }
    }
    free(in); free(out);
    fclose(f);
    shell_printf(ctx->sock, "----END base64----\r\n");
}

// ---- put: receive base64 from the terminal into a file --------------------
static void do_put(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 2) { shell_printf(ctx->sock, "usage: put <file>   (paste base64, then a line 'EOF')\r\n"); return; }
    if (!sdcard_mounted()) { shell_printf(ctx->sock, "put: SD card not mounted\r\n"); return; }

    char path[256];
    cmd_resolve_path(ctx, argv[1], path, sizeof(path));

    shell_printf(ctx->sock, "Paste base64 now; finish with a line containing only: EOF\r\n");

    size_t cap = 64 * 1024, len = 0;
    char *b64 = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!b64) { shell_printf(ctx->sock, "put: out of memory\r\n"); return; }

    char line[1024];
    while (1) {
        int n = shell_read_line(ctx->sock, line, sizeof(line));
        if (n < 0) { free(b64); return; }             // disconnected
        if (strcmp(line, "EOF") == 0) break;
        // Append non-whitespace chars.
        for (int i = 0; line[i]; i++) {
            char c = line[i];
            if (c == ' ' || c == '\t') continue;
            if (len + 1 >= cap) { cap *= 2; b64 = realloc(b64, cap); if (!b64) return; }
            b64[len++] = c;
        }
    }

    size_t out_cap = (len / 4) * 3 + 8;
    unsigned char *out = heap_caps_malloc(out_cap, MALLOC_CAP_SPIRAM);
    size_t olen = 0;
    int rc = out ? mbedtls_base64_decode(out, out_cap, &olen, (unsigned char *)b64, len) : -1;
    free(b64);

    if (rc != 0) {
        shell_printf(ctx->sock, "put: invalid base64 (%d)\r\n", rc);
        free(out);
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { shell_printf(ctx->sock, "put: cannot open file\r\n"); free(out); return; }
    fwrite(out, 1, olen, f);
    fclose(f);
    free(out);

    char disp[256];
    cmd_display_path(path, disp, sizeof(disp));
    shell_printf(ctx->sock, "Wrote %u bytes to %s\r\n", (unsigned)olen, disp);
}

// ---- selftest: component test ---------------------------------------------
static void do_selftest(shell_ctx_t *ctx)
{
    int passed = 0, total = 0;
    #define CHECK(name, cond, detail_fmt, ...) do {                       \
        total++;                                                          \
        bool _ok = (cond);                                               \
        if (_ok) passed++;                                               \
        shell_printf(ctx->sock, "  [%s] %-10s " detail_fmt "\r\n",       \
                     _ok ? "PASS" : "FAIL", name, ##__VA_ARGS__);        \
    } while (0)

    shell_printf(ctx->sock, "Running component self-test...\r\n");

    // Internal heap.
    size_t ifree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    CHECK("heap", ifree > 20 * 1024, "%u KB internal free", (unsigned)(ifree / 1024));

    // PSRAM: allocate 1MB, write/verify a pattern.
    bool ps_ok = false;
    size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (ps_total) {
        size_t n = 1024 * 1024;
        uint8_t *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
        if (p) {
            for (size_t i = 0; i < n; i += 4096) p[i] = (uint8_t)(i >> 12);
            ps_ok = true;
            for (size_t i = 0; i < n; i += 4096) if (p[i] != (uint8_t)(i >> 12)) { ps_ok = false; break; }
            free(p);
        }
    }
    CHECK("psram", ps_ok, "%u KB total, 1MB rw ok", (unsigned)(ps_total / 1024));

    // SD card: write, read back, delete a temp file.
    bool sd_ok = false;
    if (sdcard_mounted()) {
        const char *tp = SD_MOUNT "/.selftest.tmp";
        FILE *f = fopen(tp, "wb");
        if (f) { fputs("esp32-selftest", f); fclose(f);
            char buf[32] = {0};
            f = fopen(tp, "rb");
            if (f) { fgets(buf, sizeof(buf), f); fclose(f); sd_ok = (strcmp(buf, "esp32-selftest") == 0); }
            unlink(tp);
        }
    }
    CHECK("sdcard", sd_ok, "%s", sdcard_mounted() ? "mount+rw ok" : "not mounted");

    // SD write/read speed: a ~256 KB temp file, timed.
    if (sdcard_mounted()) {
        const char *sp = SD_MOUNT "/.speedtest.tmp";
        const size_t N = 256 * 1024;
        char *chunk = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        if (chunk) {
            memset(chunk, 0x5A, 4096);
            int64_t t0 = esp_timer_get_time();
            FILE *f = fopen(sp, "wb");
            bool wok = false;
            if (f) {
                size_t w = 0;
                while (w < N && fwrite(chunk, 1, 4096, f) == 4096) w += 4096;
                fclose(f);
                wok = (w >= N);
            }
            int64_t t1 = esp_timer_get_time();
            double wmb = wok ? (N / 1048576.0) / ((t1 - t0) / 1e6) : 0;
            // read back
            int64_t t2 = esp_timer_get_time();
            size_t rd = 0; f = fopen(sp, "rb");
            if (f) { size_t r; while ((r = fread(chunk, 1, 4096, f)) > 0) rd += r; fclose(f); }
            int64_t t3 = esp_timer_get_time();
            double rmb = (rd >= N) ? (N / 1048576.0) / ((t3 - t2) / 1e6) : 0;
            unlink(sp);
            free(chunk);
            CHECK("sd-speed", (wok && rd >= N), "write %.2f MB/s, read %.2f MB/s", wmb, rmb);
        }
    } else {
        CHECK("sd-speed", false, "no SD card");
    }

    // PSRAM full-span walk: grab the largest free block and pattern-verify it.
    bool span_ok = false; size_t span_kb = 0;
    if (ps_total) {
        size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        if (big > 64 * 1024) {
            size_t n = big - 32 * 1024;                 // leave a margin
            uint32_t *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
            if (p) {
                size_t words = n / 4;
                for (size_t i = 0; i < words; i++) p[i] = (uint32_t)(i * 2654435761u);
                span_ok = true;
                for (size_t i = 0; i < words; i++)
                    if (p[i] != (uint32_t)(i * 2654435761u)) { span_ok = false; break; }
                span_kb = n / 1024;
                free(p);
            }
        }
    }
    CHECK("psram-span", span_ok, "%u KB walked + verified", (unsigned)span_kb);

    // Camera: grab one frame.
    size_t jlen = 0;
    bool cam_ok = camera_test_capture(&jlen);
    CHECK("camera", cam_ok, "%s (frame %u bytes)", camera_sensor_name(), (unsigned)jlen);

    // GPIO4 loopback: drive the onboard-LED pin high/low, read it back.
    CHECK("gpio", gpio_led_loopback(), "GPIO4 drive/read-back (onboard LED)");

    // Wi-Fi.
    esp_netif_ip_info_t ip; wifi_get_ip(&ip);
    CHECK("wifi", wifi_is_connected(), "ip " IPSTR, IP2STR(&ip.ip));

    // NTP clock sync (needs internet on the network).
    time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
    CHECK("ntp-clock", (tmv.tm_year + 1900) >= 2024, "%04d-%02d-%02d %02d:%02d:%02d",
          tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    // Flash / running partition (informational).
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run)
        shell_printf(ctx->sock, "  [INFO] %-10s app '%s' @ 0x%06lx, %lu KB\r\n",
                     "partition", run->label, (unsigned long)run->address,
                     (unsigned long)(run->size / 1024));

    // SD free space (informational).
    if (sdcard_mounted()) {
        uint64_t sdt = 0, sdf = 0; sdcard_usage(&sdt, &sdf);
        shell_printf(ctx->sock, "  [INFO] %-10s %llu MB free of %llu MB\r\n",
                     "sd-space", sdf / (1024 * 1024), sdt / (1024 * 1024));
    }

    // Internal temperature (informational, ESP32 sensor is rough).
    shell_printf(ctx->sock, "  [INFO] %-10s ~%.0f C (internal, approximate)\r\n",
                 "temp", esp32_temp_c());

    shell_printf(ctx->sock, "----\r\nSelf-test: %d/%d passed. %s\r\n",
                 passed, total, (passed == total) ? "All systems nominal." : "Check FAILs above.");
    #undef CHECK
}

// ---- stress test ----------------------------------------------------------
typedef struct { volatile uint64_t iters; volatile bool run; } stress_worker_t;

static void stress_task(void *arg)
{
    stress_worker_t *w = (stress_worker_t *)arg;
    volatile double x = 0;
    while (w->run) {
        for (int i = 0; i < 20000; i++) x += (i * 1.5) - 0.5;
        w->iters += 20000;
    }
    (void)x;
    vTaskDelete(NULL);
}

// ---- single-core throughput micro-benchmarks (return Mops/s) --------------
// Each runs a dependent op chain for ~ms milliseconds so the compiler can't
// fold it away, then reports millions of ops per second.
static double bench_int(int ms)
{
    volatile uint32_t sink = 0;
    uint32_t a = 12345;
    uint64_t ops = 0;
    int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < (int64_t)ms * 1000) {
        for (int i = 0; i < 20000; i++) a = a * 1664525u + 1013904223u;   // LCG
        ops += 20000;
    }
    int64_t dt = esp_timer_get_time() - t0;
    sink = a; (void)sink;
    return (double)ops / (double)dt;   // ops/us == Mops/s
}

static double bench_f32(int ms)
{
    volatile float sink = 0;
    float a = 1.0f;
    uint64_t ops = 0;
    int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < (int64_t)ms * 1000) {
        for (int i = 0; i < 20000; i++) a = a * 1.0000001f + 0.5f;        // hardware FPU
        ops += 20000;
        if (a > 1e30f) a = 1.0f;
    }
    int64_t dt = esp_timer_get_time() - t0;
    sink = a; (void)sink;
    return (double)ops / (double)dt;
}

static double bench_f64(int ms)
{
    volatile double sink = 0;
    double a = 1.0;
    uint64_t ops = 0;
    int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < (int64_t)ms * 1000) {
        for (int i = 0; i < 20000; i++) a = a * 1.0000001 + 0.5;          // soft-float (slow)
        ops += 20000;
        if (a > 1e300) a = 1.0;
    }
    int64_t dt = esp_timer_get_time() - t0;
    sink = a; (void)sink;
    return (double)ops / (double)dt;
}

// ---- memory read/write bandwidth for a heap-caps region (MB/s) ------------
static void mem_bench(shell_ctx_t *ctx, const char *label, uint32_t caps, size_t bytes)
{
    // Clamp to the largest contiguous free block (internal RAM is fragmented by
    // the interpreters + Wi-Fi, so only ~30 KB may be available in one piece).
    size_t big = heap_caps_get_largest_free_block(caps);
    if (bytes + 8 * 1024 > big) bytes = (big > 12 * 1024) ? (big - 8 * 1024) : 0;
    if (bytes < 4096) { shell_printf(ctx->sock, "  %-13s not enough free RAM to test\r\n", label); return; }
    bytes &= ~3u;   // word-align the size

    uint32_t *b = heap_caps_malloc(bytes, caps);
    if (!b) { shell_printf(ctx->sock, "  %-13s alloc %u KB failed\r\n", label, (unsigned)(bytes / 1024)); return; }
    size_t words = bytes / 4;

    // Write bandwidth: fill the block over and over for ~300 ms.
    int64_t t0 = esp_timer_get_time(); uint64_t wpasses = 0;
    while (esp_timer_get_time() - t0 < 300000) {
        for (size_t i = 0; i < words; i++) b[i] = (uint32_t)i;
        wpasses++;
    }
    double wsec = (esp_timer_get_time() - t0) / 1e6;
    double wmb = (wpasses * (double)bytes) / 1048576.0 / wsec;

    // Read bandwidth: sum the block over and over.
    volatile uint32_t vsink = 0; uint64_t rpasses = 0;
    int64_t t1 = esp_timer_get_time();
    while (esp_timer_get_time() - t1 < 300000) {
        uint32_t s = 0;
        for (size_t i = 0; i < words; i++) s += b[i];
        vsink += s; rpasses++;
    }
    double rsec = (esp_timer_get_time() - t1) / 1e6;
    double rmb = (rpasses * (double)bytes) / 1048576.0 / rsec;
    (void)vsink;
    free(b);

    shell_printf(ctx->sock, "  %-13s write %5.0f MB/s   read %5.0f MB/s   (%u KB block)\r\n",
                 label, wmb, rmb, (unsigned)(bytes / 1024));
}

static void do_stress(shell_ctx_t *ctx, int argc, char **argv)
{
    int secs = (argc >= 2) ? atoi(argv[1]) : 8;
    if (secs < 1) secs = 1;
    if (secs > 60) secs = 60;

    shell_printf(ctx->sock, "Stress test: CPU throughput + memory bandwidth + %ds 2-core burn...\r\n", secs);

    size_t heap_before = esp_get_free_heap_size();
    size_t frag_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    float  temp_before = esp32_temp_c();

    // ---- CPU throughput: one core, three number types --------------------
    shell_printf(ctx->sock, "  [1/4] CPU throughput (single core)...\r\n");
    double m_int = bench_int(250);
    double m_f32 = bench_f32(250);
    double m_f64 = bench_f64(250);

    // ---- Memory bandwidth: internal SRAM vs external PSRAM ---------------
    shell_printf(ctx->sock, "  [2/4] Memory bandwidth (internal RAM vs PSRAM)...\r\n");
    mem_bench(ctx, "Internal RAM", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, 48 * 1024);
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM))
        mem_bench(ctx, "PSRAM (ext)", MALLOC_CAP_SPIRAM, 256 * 1024);

    // ---- PSRAM sweep: grab as much as we can, pattern-write + verify -------
    shell_printf(ctx->sock, "  [3/4] PSRAM sweep...\r\n");
    size_t ps_tested = 0;
    #define PS_MAXBLK 8
    uint8_t *blocks[PS_MAXBLK] = {0};
    int nblk = 0;
    for (nblk = 0; nblk < PS_MAXBLK; nblk++) {
        size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        if (big < 96 * 1024) break;
        size_t bsz = big - 64 * 1024;               // leave margin for other allocs
        blocks[nblk] = heap_caps_malloc(bsz, MALLOC_CAP_SPIRAM);
        if (!blocks[nblk]) break;
        memset(blocks[nblk], 0xA5, bsz);
        bool ok = true;
        for (size_t i = 0; i < bsz; i += 4096) if (blocks[nblk][i] != 0xA5) { ok = false; break; }
        if (blocks[nblk][bsz - 1] != 0xA5) ok = false;
        if (ok) ps_tested += bsz;
    }

    // ---- SD I/O burn: write + verify a multi-MB file ----------------------
    double sd_wmb = 0, sd_rmb = 0; size_t sd_bytes = 0;
    if (sdcard_mounted()) {
        shell_printf(ctx->sock, "  [4/4] SD I/O burn + 2-core CPU burn...\r\n");
        const char *sp = SD_MOUNT "/.stress.tmp";
        const size_t TARGET = 2 * 1024 * 1024;      // 2 MB
        char *chunk = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
        if (chunk) {
            for (int i = 0; i < 8192; i++) chunk[i] = (char)(i & 0xFF);
            int64_t t0 = esp_timer_get_time();
            FILE *f = fopen(sp, "wb"); size_t w = 0;
            if (f) { while (w < TARGET && fwrite(chunk, 1, 8192, f) == 8192) w += 8192; fclose(f); }
            int64_t t1 = esp_timer_get_time();
            bool vok = true; size_t rd = 0;
            f = fopen(sp, "rb");
            if (f) {
                size_t r;
                while ((r = fread(chunk, 1, 8192, f)) > 0) {
                    for (size_t i = 0; i < r; i++) if (chunk[i] != (char)((rd + i) & 0xFF)) { vok = false; break; }
                    rd += r;
                    if (!vok) break;
                }
                fclose(f);
            }
            int64_t t2 = esp_timer_get_time();
            if (w >= TARGET) sd_wmb = (w / 1048576.0) / ((t1 - t0) / 1e6);
            if (vok && rd >= TARGET) sd_rmb = (rd / 1048576.0) / ((t2 - t1) / 1e6);
            sd_bytes = w;
            unlink(sp);
            free(chunk);
        }
    }

    // ---- CPU burn on both cores ------------------------------------------
    if (!sdcard_mounted())
        shell_printf(ctx->sock, "  [4/4] CPU burn on both cores (%ds)...\r\n", secs);
    stress_worker_t main_w = { .iters = 0, .run = true };
    stress_worker_t other_w = { .iters = 0, .run = true };
    int mycore = xPortGetCoreID();
    int othercore = mycore ? 0 : 1;
    xTaskCreatePinnedToCore(stress_task, "stress_wk", 4096, &other_w, 10, NULL, othercore);

    int64_t t_end = esp_timer_get_time() + (int64_t)secs * 1000000;
    volatile double x = 0;
    while (esp_timer_get_time() < t_end) {
        for (int i = 0; i < 20000; i++) x += (i * 1.5) - 0.5;
        main_w.iters += 20000;
    }
    (void)x;
    other_w.run = false;
    vTaskDelay(pdMS_TO_TICKS(50));   // let the worker exit

    double mops_a = (double)main_w.iters  / (secs * 1e6);
    double mops_b = (double)other_w.iters / (secs * 1e6);

    // Free PSRAM BEFORE measuring so the leak check compares like-for-like.
    for (int i = 0; i < nblk; i++) free(blocks[i]);
    size_t heap_after = esp_get_free_heap_size();
    size_t frag_after = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    float  temp_after = esp32_temp_c();

    shell_printf(ctx->sock, "----\r\n");
    shell_printf(ctx->sock, "CPU 1-core: int %.0f Mops/s  |  float32 %.0f Mops/s (HW FPU)  |  float64 %.1f Mops/s (soft)\r\n",
                 m_int, m_f32, m_f64);
    shell_printf(ctx->sock, "  (float64 is slow by design: ESP32 has no 64-bit FPU, so it's emulated in software)\r\n");
    shell_printf(ctx->sock, "CPU 2-core burn: %.1f Mops/s float64  (core%d %.1f + core%d %.1f)\r\n",
                 mops_a + mops_b, mycore, mops_a, othercore, mops_b);
    shell_printf(ctx->sock, "PSRAM: %u KB swept + verified across %d blocks\r\n",
                 (unsigned)(ps_tested / 1024), nblk);
    if (sd_bytes)
        shell_printf(ctx->sock, "SD:    %u KB, write %.2f MB/s, read+verify %.2f MB/s\r\n",
                     (unsigned)(sd_bytes / 1024), sd_wmb, sd_rmb);
    else
        shell_printf(ctx->sock, "SD:    (no card — skipped)\r\n");
    shell_printf(ctx->sock, "Heap:  %u B free before / %u B after (leak check)\r\n",
                 (unsigned)heap_before, (unsigned)heap_after);
    shell_printf(ctx->sock, "Frag:  largest internal block %u -> %u KB\r\n",
                 (unsigned)(frag_before / 1024), (unsigned)(frag_after / 1024));
    shell_printf(ctx->sock, "Temp:  ~%.0f C -> ~%.0f C (internal, approximate)\r\n",
                 temp_before, temp_after);
    shell_printf(ctx->sock, "Result: %s\r\n",
                 (ps_tested > 0 && heap_after + 8192 >= heap_before) ? "STABLE" : "CHECK");
    #undef PS_MAXBLK
}

// ---- scripting languages --------------------------------------------------
static void do_python(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc >= 2) {
        char path[256];
        cmd_resolve_path(ctx, argv[1], path, sizeof(path));
        python_run_file(ctx, path);
    } else {
        python_repl(ctx);
    }
}

static void do_js(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc >= 2) {
        char path[256];
        cmd_resolve_path(ctx, argv[1], path, sizeof(path));
        js_run_file(ctx, path);
    } else {
        js_repl(ctx);
    }
}

static void do_cc(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 2) { shell_printf(ctx->sock, "usage: cc <program.c>   (must define main())\r\n"); return; }
    char path[256];
    cmd_resolve_path(ctx, argv[1], path, sizeof(path));
    cc_run_file(ctx, path);
}

// `run <file>` — dispatch by extension to the right interpreter.
static void do_run(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 2) { shell_printf(ctx->sock, "usage: run <script.py|.js|.c>\r\n"); return; }
    char path[256];
    cmd_resolve_path(ctx, argv[1], path, sizeof(path));
    const char *dot = strrchr(argv[1], '.');
    if (dot && !strcasecmp(dot, ".py"))      python_run_file(ctx, path);
    else if (dot && !strcasecmp(dot, ".js")) js_run_file(ctx, path);
    else if (dot && !strcasecmp(dot, ".c"))  cc_run_file(ctx, path);
    else shell_printf(ctx->sock, "run: unsupported type (supported: .py, .js, .c)\r\n");
}

// ---- misc unix-ish commands -----------------------------------------------
static void do_whoami(shell_ctx_t *ctx)   { shell_printf(ctx->sock, "root\r\n"); }
static void do_hostname(shell_ctx_t *ctx) { shell_printf(ctx->sock, "esp32\r\n"); }
static void do_id(shell_ctx_t *ctx)
{
    shell_printf(ctx->sock, "uid=0(root) gid=0(root) groups=0(root)\r\n");
}

static void do_date(shell_ctx_t *ctx)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 < 2020) {
        shell_printf(ctx->sock, "Clock not synced yet (NTP in progress; needs internet)\r\n");
    } else {
        char buf[64];
        strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S %Z %Y", &tm);
        shell_printf(ctx->sock, "%s\r\n", buf);
    }
}

// temp — show the (calibrated) internal die temperature, or calibrate it.
static void do_temp(shell_ctx_t *ctx, int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "cal")) {
        float real = atof(argv[2]);
        if (real < -40 || real > 125) { shell_printf(ctx->sock, "temp: give a real temperature in C (e.g. temp cal 26)\r\n"); return; }
        temp_calibrate(real);
        shell_printf(ctx->sock, "Calibrated: raw %.1f C, now reads %.1f C (offset %+.1f C, saved)\r\n",
                     temp_raw_c(), temp_read_c(), temp_offset());
        return;
    }
    if (argc >= 2 && !strcmp(argv[1], "reset")) {
        temp_calibrate(temp_raw_c());   // offset 0
        shell_printf(ctx->sock, "Calibration cleared (offset 0).\r\n");
        return;
    }
    shell_printf(ctx->sock, "Chip temperature: %.1f C   (raw %.1f C, offset %+.1f C)\r\n",
                 temp_read_c(), temp_raw_c(), temp_offset());
    if (temp_offset() == 0.0f)
        shell_printf(ctx->sock,
            "Uncalibrated — the raw sensor is offset per-chip. Measure the real temp\r\n"
            "(ideally right after a cold power-on) and run:  temp cal <celsius>\r\n");
}

// Enable the interactive line editor (arrow-key history) for THIS session —
// for raw-mode netcat users who can't run a telnet client.
static void do_arrows(shell_ctx_t *ctx)
{
    ctx->char_mode = 1;
    shell_printf(ctx->sock,
        "Interactive mode ON: arrow-key history, inline editing, and nano are enabled.\r\n"
        "This only feels right if your terminal is in RAW mode. If keys look garbled,\r\n"
        "disconnect and reconnect like this, then type 'arrows' again:\r\n"
        "    stty raw -echo; nc <ip> %d; stty sane\r\n", SHELL_PORT);
}

static void do_ps(shell_ctx_t *ctx)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *t = malloc(n * sizeof(TaskStatus_t));
    if (!t) { shell_printf(ctx->sock, "ps: out of memory\r\n"); return; }
    uint32_t tr;
    n = uxTaskGetSystemState(t, n, &tr);
    shell_printf(ctx->sock, "  PID  PRI  STATE    STACKFREE  NAME\r\n");
    for (UBaseType_t i = 0; i < n; i++) {
        const char *st;
        switch (t[i].eCurrentState) {
            case eRunning:   st = "RUN";   break;
            case eReady:     st = "READY"; break;
            case eBlocked:   st = "BLOCK"; break;
            case eSuspended: st = "SUSP";  break;
            case eDeleted:   st = "DEAD";  break;
            default:         st = "?";     break;
        }
        shell_printf(ctx->sock, "%5u %4u  %-7s  %6uB    %s\r\n",
                     (unsigned)t[i].xTaskNumber, (unsigned)t[i].uxCurrentPriority,
                     st, (unsigned)t[i].usStackHighWaterMark, t[i].pcTaskName);
    }
    free(t);
}

static void do_neofetch(shell_ctx_t *ctx)
{
    uint64_t us = (uint64_t)esp_timer_get_time();
    uint32_t secs = us / 1000000ULL;
    esp_chip_info_t chip; esp_chip_info(&chip);
    size_t it = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t ifr = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t pt = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t pf = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint64_t sdt = 0, sdf = 0; sdcard_usage(&sdt, &sdf);
    esp_netif_ip_info_t ip; wifi_get_ip(&ip);

    shell_printf(ctx->sock, "\r\n");
    shell_printf(ctx->sock, "   _____ ____  ____ ___ ___     \x1b[1mroot@esp32\x1b[0m\r\n");
    shell_printf(ctx->sock, "  |  ___/ ___||  _ \\___ \\__ \\    ---------------\r\n");
    shell_printf(ctx->sock, "  | |_  \\___ \\| |_) |__) | ) |   OS: ESP-IDF FreeRTOS %s\r\n", esp_get_idf_version());
    shell_printf(ctx->sock, "  |  _|  ___) |  __/ / __/ / /    Host: Deneyap WROVER-E (ESP32-D0WD)\r\n");
    shell_printf(ctx->sock, "  |_|   |____/|_|   |_____/_/     Uptime: %02u:%02u:%02u\r\n",
                 (unsigned)(secs/3600), (unsigned)((secs%3600)/60), (unsigned)(secs%60));
    shell_printf(ctx->sock, "                                 CPU: Xtensa LX6 x%d @ 240MHz\r\n", chip.cores);
    shell_printf(ctx->sock, "                                 RAM: %uK / %uK internal\r\n",
                 (unsigned)((it-ifr)/1024), (unsigned)(it/1024));
    if (pt) shell_printf(ctx->sock, "                                 PSRAM: %uK / %uK @ 80MHz\r\n",
                 (unsigned)((pt-pf)/1024), (unsigned)(pt/1024));
    shell_printf(ctx->sock, "                                 Disk: %lluMB SD FAT32\r\n", sdt/(1024*1024));
    shell_printf(ctx->sock, "                                 Camera: %s\r\n",
                 camera_ready() ? camera_sensor_name() : "none");
    shell_printf(ctx->sock, "                                 IP: " IPSTR "\r\n\r\n", IP2STR(&ip.ip));
}

static void do_help(shell_ctx_t *ctx)
{
    static const char *h =
        "Available commands:\r\n"
        "  ls [-la] [path]   list directory contents\r\n"
        "  cat <file>        print file contents\r\n"
        "  echo <t> > <f>    write text to file (>> to append)\r\n"
        "  mkdir <dir>       create a directory\r\n"
        "  rm <file>         delete a file or empty directory\r\n"
        "  pwd               print working directory\r\n"
        "  cd <dir>          change directory\r\n"
        "  df                show SD card usage\r\n"
        "  wget <url> [path] download a file to the SD card\r\n"
        "  curl <url>        print a URL's response body\r\n"
        "  free              show internal + PSRAM memory\r\n"
        "  uptime            show system uptime\r\n"
        "  ifconfig          show IP, MAC and Wi-Fi RSSI\r\n"
        "  ping <host>       ICMP ping a host\r\n"
        "  python [file.py]  Python 3 interpreter (REPL, or run a .py file)\r\n"
        "  js [file.js]      JavaScript interpreter (REPL, or run a .js file)\r\n"
        "  cc <prog.c>       interpret a C program (PicoC; must have main())\r\n"
        "  run <file>        run a script by extension (.py, .js, .c)\r\n"
        "  photo [name]      capture a JPEG to the SD card\r\n"
        "  video [seconds]   record an MJPEG .avi clip to the SD card\r\n"
        "  stream start|stop live MJPEG stream in a browser (port 81)\r\n"
        "  files start|stop  browser file manager: view/download/upload (port 80)\r\n"
        "  nano <file>       full-screen text editor (telnet/PuTTY, or nc raw)\r\n"
        "  get <file>        print a file as base64 (copy it off the device)\r\n"
        "  put <file>        write a file from pasted base64 (end with 'EOF')\r\n"
        "  selftest          full hardware test: PSRAM, SD speed, camera, GPIO, NTP...\r\n"
        "  stress [secs]     stress CPU + PSRAM + SD I/O and report throughput\r\n"
        "  gpio <sub> ...    read/write/blink pins (gpio info for the map)\r\n"
        "  pwm <pin> <0-255> analog brightness on a pin (LEDC)\r\n"
        "  led on|off|blink|breathe|pulse|rainbow   onboard LED effects (GPIO4)\r\n"
        "  rgb <r> <g> <b>   drive the onboard RGB LED (0/1 each)\r\n"
        "  grep [-in] <p> <f> print lines matching a pattern\r\n"
        "  wc <file>         count lines, words, bytes\r\n"
        "  head/tail [-n N]  first/last N lines of a file\r\n"
        "  cp/mv <src> <dst> copy / move a file\r\n"
        "  find [path] [name] list files recursively\r\n"
        "  du [path]         disk usage of a tree\r\n"
        "  hexdump <file>    hex + ASCII dump\r\n"
        "  history           recent commands\r\n"
        "  cowsay <text>     an ASCII cow says your text\r\n"
        "  fortune           print a random programming quote\r\n"
        "  cmatrix           Matrix digital rain (telnet; any key stops)\r\n"
        "  snake             play snake (telnet/PuTTY; WASD/arrows, q quits)\r\n"
        "  tictactoe / ttt   play tic-tac-toe vs the ESP32 (telnet)\r\n"
        "  qr [text]         print a scannable QR (default: hotspot Wi-Fi login)\r\n"
        "  htop              live task/memory monitor (q to quit)\r\n"
        "  dmesg             show the boot / system log\r\n"
        "  uname -a          show system information\r\n"
        "  whoami / id       show the current user\r\n"
        "  hostname          show the hostname\r\n"
        "  date              show the current date/time (NTP)\r\n"
        "  temp [cal <C>]    show chip temperature; 'temp cal 26' calibrates it\r\n"
        "  ps                list running tasks\r\n"
        "  neofetch          system summary with logo\r\n"
        "  arrows            enable arrow-key history (raw-mode terminals)\r\n"
        "  reboot            restart the board\r\n"
        "  clear / cls       clear the screen\r\n"
        "  help              show this help\r\n"
        "  exit              close the connection\r\n";
    shell_send_all(ctx->sock, h, strlen(h));
}

// ---------------------------------------------------------------------------
//  Dispatch
// ---------------------------------------------------------------------------
void cmd_execute(shell_ctx_t *ctx, char *line)
{
    // Skip leading whitespace / empty lines.
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') return;

    ux_history_add(line);   // record before tokenize() mutates the buffer

    char *argv[MAX_ARGS];
    int argc = tokenize(line, argv);
    if (argc == 0) return;

    const char *cmd = argv[0];

    if      (!strcmp(cmd, "ls"))       do_ls(ctx, argc, argv);
    else if (!strcmp(cmd, "cat"))      do_cat(ctx, argc, argv);
    else if (!strcmp(cmd, "echo"))     do_echo(ctx, argc, argv);
    else if (!strcmp(cmd, "mkdir"))    do_mkdir(ctx, argc, argv);
    else if (!strcmp(cmd, "rm"))       do_rm(ctx, argc, argv);
    else if (!strcmp(cmd, "pwd"))      do_pwd(ctx);
    else if (!strcmp(cmd, "cd"))       do_cd(ctx, argc, argv);
    else if (!strcmp(cmd, "df"))       do_df(ctx);
    else if (!strcmp(cmd, "wget"))     do_wget(ctx, argc, argv);
    else if (!strcmp(cmd, "curl"))     do_curl(ctx, argc, argv);
    else if (!strcmp(cmd, "free"))     do_free(ctx);
    else if (!strcmp(cmd, "uptime"))   do_uptime(ctx);
    else if (!strcmp(cmd, "ifconfig")) do_ifconfig(ctx);
    else if (!strcmp(cmd, "ping"))     do_ping(ctx, argc, argv);
    else if (!strcmp(cmd, "python") || !strcmp(cmd, "py")) do_python(ctx, argc, argv);
    else if (!strcmp(cmd, "js") || !strcmp(cmd, "node")) do_js(ctx, argc, argv);
    else if (!strcmp(cmd, "cc") || !strcmp(cmd, "gcc")) do_cc(ctx, argc, argv);
    else if (!strcmp(cmd, "run"))      do_run(ctx, argc, argv);
    else if (!strcmp(cmd, "photo"))    do_photo(ctx, argc, argv);
    else if (!strcmp(cmd, "video"))    do_video(ctx, argc, argv);
    else if (!strcmp(cmd, "stream"))   do_stream(ctx, argc, argv);
    else if (!strcmp(cmd, "nano"))     do_nano(ctx, argc, argv);
    else if (!strcmp(cmd, "files"))    do_files(ctx, argc, argv);
    else if (!strcmp(cmd, "get"))      do_get(ctx, argc, argv);
    else if (!strcmp(cmd, "put"))      do_put(ctx, argc, argv);
    else if (!strcmp(cmd, "selftest")) do_selftest(ctx);
    else if (!strcmp(cmd, "test"))     do_selftest(ctx);
    else if (!strcmp(cmd, "stress"))   do_stress(ctx, argc, argv);
    else if (!strcmp(cmd, "gpio"))     gpio_cmd(ctx, argc, argv);
    else if (!strcmp(cmd, "pwm"))      gpio_pwm(ctx, argc, argv);
    else if (!strcmp(cmd, "led"))      gpio_led(ctx, argc, argv);
    else if (!strcmp(cmd, "rgb"))      gpio_rgb(ctx, argc, argv);
    else if (!strcmp(cmd, "cowsay"))   fun_cowsay(ctx, argc, argv);
    else if (!strcmp(cmd, "fortune"))  fun_fortune(ctx);
    else if (!strcmp(cmd, "cmatrix") || !strcmp(cmd, "matrix")) fun_cmatrix(ctx);
    else if (!strcmp(cmd, "snake"))    fun_snake(ctx);
    else if (!strcmp(cmd, "tictactoe") || !strcmp(cmd, "ttt")) fun_tictactoe(ctx);
    else if (!strcmp(cmd, "grep"))     ux_grep(ctx, argc, argv);
    else if (!strcmp(cmd, "wc"))       ux_wc(ctx, argc, argv);
    else if (!strcmp(cmd, "head"))     ux_head(ctx, argc, argv);
    else if (!strcmp(cmd, "tail"))     ux_tail(ctx, argc, argv);
    else if (!strcmp(cmd, "cp"))       ux_cp(ctx, argc, argv);
    else if (!strcmp(cmd, "mv"))       ux_mv(ctx, argc, argv);
    else if (!strcmp(cmd, "find"))     ux_find(ctx, argc, argv);
    else if (!strcmp(cmd, "du"))       ux_du(ctx, argc, argv);
    else if (!strcmp(cmd, "hexdump") || !strcmp(cmd, "xxd")) ux_hexdump(ctx, argc, argv);
    else if (!strcmp(cmd, "history"))  ux_history(ctx);
    else if (!strcmp(cmd, "qr"))       do_qr(ctx, argc, argv);
    else if (!strcmp(cmd, "htop"))     htop_run(ctx);
    else if (!strcmp(cmd, "dmesg"))    dmesg_dump(ctx->sock);
    else if (!strcmp(cmd, "uname"))    do_uname(ctx, argc, argv);
    else if (!strcmp(cmd, "whoami"))   do_whoami(ctx);
    else if (!strcmp(cmd, "hostname")) do_hostname(ctx);
    else if (!strcmp(cmd, "id"))       do_id(ctx);
    else if (!strcmp(cmd, "date"))     do_date(ctx);
    else if (!strcmp(cmd, "temp"))     do_temp(ctx, argc, argv);
    else if (!strcmp(cmd, "ps"))       do_ps(ctx);
    else if (!strcmp(cmd, "neofetch")) do_neofetch(ctx);
    else if (!strcmp(cmd, "arrows"))   do_arrows(ctx);
    else if (!strcmp(cmd, "cls"))      shell_printf(ctx->sock, "\033[2J\033[H");
    else if (!strcmp(cmd, "help"))     do_help(ctx);
    else if (!strcmp(cmd, "clear"))    shell_printf(ctx->sock, "\033[2J\033[H");
    else if (!strcmp(cmd, "reboot")) {
        shell_printf(ctx->sock, "Rebooting...\r\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    else if (!strcmp(cmd, "exit") || !strcmp(cmd, "logout")) {
        shell_printf(ctx->sock, "logout\r\n");
        ctx->want_close = 1;
    }
    else {
        shell_printf(ctx->sock, "%s: command not found\r\n", cmd);
    }
}
