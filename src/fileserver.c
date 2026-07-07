#include "fileserver.h"
#include "config.h"
#include "dmesg.h"
#include "shell.h"
#include "commands.h"
#include "wifi.h"
#include "sdcard.h"
#include "camera.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "qrcodegen.h"

// The web-desktop page. gui_html.h is auto-generated from src/gui.html (byte
// array) — regenerate it after editing gui.html.
#include "gui_html.h"

static const char *TAG = "files";
#define FS_PORT 80

static httpd_handle_t s_httpd;

// ---- helpers ---------------------------------------------------------------
static const char *mime_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, ".png"))  return "image/png";
    if (!strcasecmp(dot, ".gif"))  return "image/gif";
    if (!strcasecmp(dot, ".txt") || !strcasecmp(dot, ".log") ||
        !strcasecmp(dot, ".md")  || !strcasecmp(dot, ".csv")) return "text/plain";
    if (!strcasecmp(dot, ".htm") || !strcasecmp(dot, ".html")) return "text/html";
    if (!strcasecmp(dot, ".avi"))  return "video/x-msvideo";
    return "application/octet-stream";
}

static bool is_image(const char *name)
{
    const char *m = mime_for(name);
    return strncmp(m, "image/", 6) == 0;
}

// Reject path traversal; only allow a bare filename under SD_MOUNT. Rejects
// path separators, "..", a leading dot, and control characters.
static bool safe_name(const char *name)
{
    if (!name || !*name) return false;
    if (name[0] == '.')     return false;   // no dotfiles / "." / ".."
    if (strstr(name, "..")) return false;
    if (strchr(name, '/'))  return false;
    if (strchr(name, '\\')) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p < 0x20 || *p == 0x7f) return false;
    return true;
}

// Escape a string for embedding inside a JSON double-quoted value.
static void json_escape(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 7 < n; p++) {
        if (*p == '"' || *p == '\\') { out[o++] = '\\'; out[o++] = *p; }
        else if (*p < 0x20) { o += snprintf(out + o, n - o, "\\u%04x", *p); }
        else out[o++] = *p;
    }
    out[o] = '\0';
}

// ---- GET / : directory listing --------------------------------------------
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>ESP32 Files</title><style>"
        "body{font-family:system-ui,sans-serif;margin:1rem;background:#f4f4f5;color:#18181b}"
        "h1{font-size:1.3rem}.grid{display:flex;flex-wrap:wrap;gap:12px}"
        ".card{background:#fff;border:1px solid #e4e4e7;border-radius:8px;padding:8px;width:180px}"
        ".card img{width:100%;height:120px;object-fit:cover;border-radius:4px;background:#eee}"
        ".name{font-size:.8rem;word-break:break-all;margin:6px 0}"
        ".sz{color:#71717a;font-size:.72rem}"
        "a{color:#2563eb;text-decoration:none;font-size:.8rem;margin-right:8px}"
        "form{background:#fff;border:1px solid #e4e4e7;border-radius:8px;padding:12px;margin:12px 0}"
        "</style></head><body><h1>ESP32 SD Card — " SD_MOUNT
        " &nbsp;<a href=/dash style='font-size:.9rem'>&#128421; dashboard</a></h1>"
        "<form method=post action=/upload enctype=multipart/form-data>"
        "<b>Upload a file:</b> <input type=file name=f required> "
        "<button>Upload</button></form><div class=grid>");

    DIR *d = opendir(SD_MOUNT);
    if (d) {
        struct dirent *e;
        char line[512];
        while ((e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR) continue;
            char full[300];
            snprintf(full, sizeof(full), "%s/%s", SD_MOUNT, e->d_name);
            struct stat st; long sz = 0;
            if (stat(full, &st) == 0) sz = (long)st.st_size;

            snprintf(line, sizeof(line), "<div class=card>");
            httpd_resp_sendstr_chunk(req, line);
            if (is_image(e->d_name)) {
                snprintf(line, sizeof(line), "<a href='/dl?f=%s'><img src='/dl?f=%s'></a>",
                         e->d_name, e->d_name);
                httpd_resp_sendstr_chunk(req, line);
            }
            snprintf(line, sizeof(line),
                     "<div class=name>%s</div><div class=sz>%ld bytes</div>"
                     "<a href='/dl?f=%s'>view</a>"
                     "<a href='/dl?f=%s&d=1'>download</a>"
                     "<a href='/del?f=%s' onclick=\"return confirm('Delete %s?')\">delete</a></div>",
                     e->d_name, sz, e->d_name, e->d_name, e->d_name, e->d_name);
            httpd_resp_sendstr_chunk(req, line);
        }
        closedir(d);
    }
    httpd_resp_sendstr_chunk(req, "</div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);   // end
    return ESP_OK;
}

// ---- GET /dl?f=NAME[&d=1] : view/download ----------------------------------
static esp_err_t dl_handler(httpd_req_t *req)
{
    char q[160], name[128] = {0}, dl[4] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "f", name, sizeof(name));
        httpd_query_key_value(q, "d", dl, sizeof(dl));
    }
    if (!safe_name(name)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name"); return ESP_FAIL; }

    char path[300];
    snprintf(path, sizeof(path), "%s/%s", SD_MOUNT, name);
    FILE *f = fopen(path, "rb");
    if (!f) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found"); return ESP_FAIL; }

    httpd_resp_set_type(req, mime_for(name));
    if (dl[0] == '1') {
        char cd[192];
        snprintf(cd, sizeof(cd), "attachment; filename=\"%s\"", name);
        httpd_resp_set_hdr(req, "Content-Disposition", cd);
    }

    char *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(8192);
    size_t r;
    while (buf && (r = fread(buf, 1, 8192, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) break;
    }
    free(buf);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ---- GET /del?f=NAME -------------------------------------------------------
static esp_err_t del_handler(httpd_req_t *req)
{
    char q[160], name[128] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "f", name, sizeof(name));
    if (safe_name(name)) {
        char path[300];
        snprintf(path, sizeof(path), "%s/%s", SD_MOUNT, name);
        unlink(path);
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- POST /upload : multipart/form-data ------------------------------------
// Streams the request body to a PSRAM buffer, extracts the single file part,
// and writes it to the SD card. Bounded to keep memory in check.
#define UPLOAD_MAX (3 * 1024 * 1024)

static esp_err_t upload_handler(httpd_req_t *req)
{
    // Find the multipart boundary from the Content-Type header.
    char ctype[128] = {0};
    httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype));
    char *bpos = strstr(ctype, "boundary=");
    if (!bpos) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no boundary"); return ESP_FAIL; }
    char boundary[80];
    snprintf(boundary, sizeof(boundary), "--%s", bpos + 9);

    int total = req->content_len;
    if (total <= 0 || total > UPLOAD_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too large / empty");
        return ESP_FAIL;
    }

    char *body = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem"); return ESP_FAIL; }

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv fail"); return ESP_FAIL; }
        got += r;
    }

    // Parse: find filename in the part header, then the data range.
    esp_err_t result = ESP_FAIL;
    char fname[128] = {0};
    char *fn = strstr(body, "filename=\"");
    if (fn) {
        fn += 10;
        char *end = strchr(fn, '"');
        if (end && (end - fn) < (int)sizeof(fname)) {
            memcpy(fname, fn, end - fn);
            fname[end - fn] = '\0';
        }
    }
    // Strip any path the browser included.
    char *slash = strrchr(fname, '/');
    if (slash) memmove(fname, slash + 1, strlen(slash + 1) + 1);
    slash = strrchr(fname, '\\');
    if (slash) memmove(fname, slash + 1, strlen(slash + 1) + 1);

    char *data = strstr(body, "\r\n\r\n");
    if (fname[0] && safe_name(fname) && data) {
        data += 4;                                   // start of file bytes
        // The data ends just before "\r\n--boundary".
        char endmark[84];
        snprintf(endmark, sizeof(endmark), "\r\n%s", boundary);
        char *dend = NULL;
        // memmem-style search over binary data.
        int dlen_max = total - (int)(data - body);
        int mlen = strlen(endmark);
        for (int i = 0; i <= dlen_max - mlen; i++) {
            if (memcmp(data + i, endmark, mlen) == 0) { dend = data + i; break; }
        }
        int flen = dend ? (int)(dend - data) : 0;
        if (flen > 0) {
            char path[300];
            snprintf(path, sizeof(path), "%s/%s", SD_MOUNT, fname);
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(data, 1, flen, f);
                fclose(f);
                result = ESP_OK;
                dmesg_add("files: uploaded %s (%d bytes)", fname, flen);
            }
        }
    }
    free(body);

    if (result == ESP_OK) {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "upload failed (no SD? bad form?)");
    }
    return result;
}

// ===========================================================================
//  Web dashboard: live camera + stats + browser command runner
// ===========================================================================

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// In-place URL decode (%xx and '+').
static void url_decode(char *s)
{
    char *d = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            int hi = hexval(s[1]), lo = hexval(s[2]);
            if (hi >= 0 && lo >= 0) { *d++ = (char)(hi * 16 + lo); s += 3; continue; }
        }
        if (*s == '+') { *d++ = ' '; s++; continue; }
        *d++ = *s++;
    }
    *d = '\0';
}

static const char *k_interactive[] = {
    "htop","nano","snake","cmatrix","matrix","tictactoe","ttt","python","py","js","node",
    "cc","gcc","run","put","get","arrows", NULL
};
static const char *k_destructive[] = { "rm","rmdir","reboot","format","mkfs","mv", NULL };

static bool in_list(const char *tok, const char **list)
{
    for (int i = 0; list[i]; i++) if (!strcmp(tok, list[i])) return true;
    return false;
}

// ---- GET /dash : the dashboard page ---------------------------------------
static esp_err_t dash_handler(httpd_req_t *req)
{
    // Make sure the MJPEG stream is up so the <img> has something to show.
    if (camera_ready() && !camera_stream_running()) camera_stream_start();

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>ESP32 Dashboard</title><style>"
        "body{font-family:ui-monospace,Menlo,Consolas,monospace;margin:0;background:#0b0f0b;color:#8f8}"
        "header{padding:12px 16px;background:#050805;border-bottom:1px solid #1e2a1e;color:#6f6}"
        "header b{color:#9f9}"
        ".wrap{display:flex;flex-wrap:wrap;gap:16px;padding:16px}"
        ".panel{background:#0f150f;border:1px solid #1e2a1e;border-radius:8px;padding:12px;flex:1;min-width:280px}"
        ".panel h2{margin:0 0 8px;font-size:.9rem;color:#6f6;font-weight:600}"
        "img{width:100%;border-radius:6px;background:#000;display:block}"
        ".stat{display:flex;justify-content:space-between;border-bottom:1px dotted #1e2a1e;padding:3px 0;font-size:.82rem}"
        ".stat span:last-child{color:#cfc}"
        "input,button{font-family:inherit;font-size:.85rem}"
        "#cmd{width:100%;box-sizing:border-box;background:#000;color:#9f9;border:1px solid #2a3a2a;border-radius:5px;padding:8px}"
        "button{background:#173017;color:#9f9;border:1px solid #2a4a2a;border-radius:5px;padding:7px 12px;cursor:pointer;margin-top:6px}"
        "button:hover{background:#1f411f}"
        "pre{background:#000;border:1px solid #1e2a1e;border-radius:5px;padding:10px;white-space:pre-wrap;"
        "word-break:break-word;max-height:340px;overflow:auto;font-size:.8rem;color:#adfaad;margin:8px 0 0}"
        ".row{display:flex;gap:6px;flex-wrap:wrap}.row button{margin-top:6px}"
        "small{color:#5a6a5a}"
        "</style></head><body>"
        "<header>&#128421; <b>ESP32 Dashboard</b> &mdash; Deneyap WROVER-E "
        "&nbsp;<small>| <a href=/ style='color:#6a8'>file manager</a></small></header>"
        "<div class=wrap>"
        "<div class=panel><h2>&#128247; Live camera</h2>"
        "<img id=cam alt='camera stream'>"
        "<div class=row><button onclick=\"q('stream start')\">Start</button>"
        "<button onclick=\"q('stream stop');document.getElementById('cam').src=''\">Stop</button>"
        "<button onclick=\"q('photo')\">Snap photo</button></div></div>"
        "<div class=panel><h2>&#128202; System</h2><div id=stats></div></div>"
        "</div>"
        "<div class=wrap><div class=panel style='min-width:100%'>"
        "<h2>&#128187; Command runner</h2>"
        "<input id=cmd placeholder='type a command, e.g. ls -la  /  free  /  neofetch' "
        "onkeydown='if(event.key==\"Enter\")run()'>"
        "<div class=row><button onclick=run()>Run</button>"
        "<button onclick=\"document.getElementById('cmd').value='neofetch';run()\">neofetch</button>"
        "<button onclick=\"document.getElementById('cmd').value='selftest';run()\">selftest</button>"
        "<button onclick=\"document.getElementById('cmd').value='ls -la';run()\">ls -la</button></div>"
        "<pre id=out>Ready. Interactive commands (htop, nano, snake...) need telnet.</pre>"
        "</div></div>"
        "<script>"
        "document.getElementById('cam').src='http://'+location.hostname+':81/stream';"
        "function fmt(b){if(b>1048576)return (b/1048576).toFixed(1)+' MB';if(b>1024)return (b/1024).toFixed(0)+' KB';return b+' B'}"
        "async function stats(){try{let r=await fetch('/api/stats');let s=await r.json();"
        "let u=s.uptime,h=Math.floor(u/3600),m=Math.floor(u%3600/60),sec=u%60;"
        "document.getElementById('stats').innerHTML="
        "row('Heap free',fmt(s.heap_free)+' / '+fmt(s.heap_total))+"
        "row('PSRAM free',fmt(s.psram_free)+' / '+fmt(s.psram_total))+"
        "row('SD free',fmt(s.sd_free)+' / '+fmt(s.sd_total))+"
        "row('Camera',s.cam)+row('IP',s.ip)+row('Stream',s.stream?'on':'off')+"
        "row('Uptime',h+'h '+m+'m '+sec+'s')}catch(e){}}"
        "function row(k,v){return '<div class=stat><span>'+k+'</span><span>'+v+'</span></div>'}"
        "async function q(c){let f=new FormData();f.append('cmd',c);f.append('confirm','1');"
        "await fetch('/api/cmd',{method:'POST',body:new URLSearchParams(f)});}"
        "async function run(){let c=document.getElementById('cmd').value;if(!c)return;"
        "let o=document.getElementById('out');o.textContent='running: '+c+' ...';"
        "let b=new URLSearchParams();b.append('cmd',c);"
        "let r=await fetch('/api/cmd',{method:'POST',body:b});let t=await r.text();"
        "if(t.startsWith('__CONFIRM__')){if(confirm(t.slice(11))){b.append('confirm','1');"
        "let r2=await fetch('/api/cmd',{method:'POST',body:b});t=await r2.text();}else{o.textContent='cancelled.';return;}}"
        "o.textContent=t||'(no output)';}"
        "stats();setInterval(stats,1500);"
        "</script></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// ---- GET /api/stats : live JSON -------------------------------------------
static esp_err_t stats_handler(httpd_req_t *req)
{
    size_t it = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t ifr = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t pt = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t pf = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint64_t sdt = 0, sdf = 0; sdcard_usage(&sdt, &sdf);
    esp_netif_ip_info_t ip; wifi_get_ip(&ip);

    char j[512];
    int n = snprintf(j, sizeof(j),
        "{\"heap_free\":%u,\"heap_total\":%u,\"psram_free\":%u,\"psram_total\":%u,"
        "\"uptime\":%u,\"sd_free\":%llu,\"sd_total\":%llu,\"cam\":\"%s\",\"ip\":\"" IPSTR "\","
        "\"stream\":%s}",
        (unsigned)ifr, (unsigned)it, (unsigned)pf, (unsigned)pt, (unsigned)up,
        (unsigned long long)sdf, (unsigned long long)sdt,
        camera_ready() ? camera_sensor_name() : "none", IP2STR(&ip.ip),
        camera_stream_running() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, j, n);
    return ESP_OK;
}

// ---- POST /api/cmd : run a command, return its captured output ------------
static esp_err_t cmd_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");

    int total = req->content_len;
    if (total <= 0 || total > 1024) { httpd_resp_sendstr(req, "bad request\n"); return ESP_OK; }
    char body[1088] = {0};
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) { httpd_resp_sendstr(req, "recv error\n"); return ESP_OK; }
        got += r;
    }
    body[got] = '\0';

    char line[512] = {0}, conf[8] = {0};
    httpd_query_key_value(body, "cmd", line, sizeof(line));
    httpd_query_key_value(body, "confirm", conf, sizeof(conf));
    url_decode(line);

    // First token → guard.
    char tok[40] = {0};
    { const char *s = line; while (*s == ' ') s++;
      int i = 0; while (*s && *s != ' ' && i < (int)sizeof(tok) - 1) tok[i++] = *s++;
      tok[i] = '\0'; }
    if (!tok[0]) { httpd_resp_sendstr(req, ""); return ESP_OK; }

    if (in_list(tok, k_interactive)) {
        httpd_resp_sendstr(req, "This command is interactive — open a telnet/PuTTY session for it.\n");
        return ESP_OK;
    }
    if (in_list(tok, k_destructive) && conf[0] != '1') {
        char msg[160];
        snprintf(msg, sizeof(msg), "__CONFIRM__'%s' is destructive. Run it anyway?", line);
        httpd_resp_sendstr(req, msg);
        return ESP_OK;
    }

    int vfd = shell_capture_begin();
    if (vfd == -1) { httpd_resp_sendstr(req, "server busy (no capture slot), try again\n"); return ESP_OK; }

    shell_ctx_t cctx;
    memset(&cctx, 0, sizeof(cctx));
    cctx.sock = vfd;
    cctx.id = 99;
    cctx.char_mode = 0;
    strlcpy(cctx.cwd, SD_MOUNT, sizeof(cctx.cwd));

    char linecopy[512];
    strlcpy(linecopy, line, sizeof(linecopy));
    cmd_execute(&cctx, linecopy);

    size_t olen = 0;
    char *out = shell_capture_end(vfd, &olen);
    if (out && olen) httpd_resp_send(req, out, olen);
    else             httpd_resp_sendstr(req, "(no output)\n");
    free(out);
    return ESP_OK;
}

// ---- GET /gui : the web desktop (embedded page) ---------------------------
static esp_err_t gui_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");   // ~14 KB vs 42 KB raw
    return httpd_resp_send(req, (const char *)gui_html_gz, gui_html_gz_len);
}

// ---- GET /api/ls?path=DIR : JSON directory listing ------------------------
static esp_err_t ls_handler(httpd_req_t *req)
{
    char q[160], rel[128] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "path", rel, sizeof(rel));
    if (strstr(rel, "..")) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path"); return ESP_FAIL; }

    char dir[256];
    if (rel[0] == '\0' || !strcmp(rel, "/")) snprintf(dir, sizeof(dir), "%s", SD_MOUNT);
    else if (rel[0] == '/')                  snprintf(dir, sizeof(dir), "%s%s", SD_MOUNT, rel);
    else                                     snprintf(dir, sizeof(dir), "%s/%s", SD_MOUNT, rel);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e; bool first = true; char item[400];
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char full[320]; snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
            struct stat st; long sz = 0; bool isdir = (e->d_type == DT_DIR);
            if (stat(full, &st) == 0) { sz = (long)st.st_size; isdir = S_ISDIR(st.st_mode); }
            char ename[280]; json_escape(e->d_name, ename, sizeof(ename));
            snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"size\":%ld,\"dir\":%s}",
                     first ? "" : ",", ename, sz, isdir ? "true" : "false");
            httpd_resp_sendstr_chunk(req, item);
            first = false;
        }
        closedir(d);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// ---- POST /api/save : write a text file (path=..&body=..) ------------------
static esp_err_t save_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    int total = req->content_len;
    if (total <= 0 || total > (256 * 1024)) { httpd_resp_sendstr(req, "too big\n"); return ESP_OK; }
    char *body = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM);
    if (!body) { httpd_resp_sendstr(req, "no mem\n"); return ESP_OK; }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) { free(body); httpd_resp_sendstr(req, "recv error\n"); return ESP_OK; }
        got += r;
    }
    body[got] = '\0';

    // Split "name=<file>&body=<urlencoded>". We keep the name simple (no slashes).
    char name[128] = {0};
    httpd_query_key_value(body, "name", name, sizeof(name));
    if (!safe_name(name)) { free(body); httpd_resp_sendstr(req, "bad name\n"); return ESP_OK; }

    char *bpos = strstr(body, "body=");
    const char *content = bpos ? bpos + 5 : "";
    // URL-decode the content in place.
    char *dec = malloc(strlen(content) + 1);
    if (dec) {
        char *o = dec; const char *s = content;
        while (*s) {
            if (*s == '%' && s[1] && s[2]) { int hi=hexval(s[1]),lo=hexval(s[2]);
                if(hi>=0&&lo>=0){*o++=(char)(hi*16+lo);s+=3;continue;} }
            if (*s == '+') { *o++ = ' '; s++; continue; }
            *o++ = *s++;
        }
        *o = '\0';
    }

    char path[300]; snprintf(path, sizeof(path), "%s/%s", SD_MOUNT, name);
    FILE *f = fopen(path, "wb");
    if (f && dec) { fwrite(dec, 1, strlen(dec), f); fclose(f);
        char msg[160]; snprintf(msg, sizeof(msg), "saved %s (%u bytes)\n", name, (unsigned)strlen(dec));
        httpd_resp_sendstr(req, msg);
    } else {
        if (f) fclose(f);
        httpd_resp_sendstr(req, "save failed (no SD?)\n");
    }
    free(dec); free(body);
    return ESP_OK;
}

// ---- GET /api/qr?text=T : QR as a JSON bit-matrix -------------------------
static esp_err_t qr_handler(httpd_req_t *req)
{
    char q[300], text[220] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "text", text, sizeof(text));
    url_decode(text);
    if (!text[0]) snprintf(text, sizeof(text), "WIFI:S:%s;T:WPA;P:%s;;", AP_SSID, AP_PASS);

    uint8_t *qr = heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX, MALLOC_CAP_SPIRAM);
    uint8_t *tmp = heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX, MALLOC_CAP_SPIRAM);
    httpd_resp_set_type(req, "application/json");
    if (!qr || !tmp ||
        !qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, 11, qrcodegen_Mask_AUTO, true)) {
        httpd_resp_sendstr(req, "{\"size\":0,\"rows\":[]}");
        free(qr); free(tmp); return ESP_OK;
    }
    int size = qrcodegen_getSize(qr);
    char head[32]; snprintf(head, sizeof(head), "{\"size\":%d,\"rows\":[", size);
    httpd_resp_sendstr_chunk(req, head);
    char row[200];
    for (int y = 0; y < size; y++) {
        int p = 0;
        row[p++] = (y ? ',' : ' '); row[p++] = '"';
        for (int x = 0; x < size; x++) row[p++] = qrcodegen_getModule(qr, x, y) ? '1' : '0';
        row[p++] = '"'; row[p] = '\0';
        httpd_resp_sendstr_chunk(req, row);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    free(qr); free(tmp);
    return ESP_OK;
}

// Captive-portal catch-all: any unknown URL (the OS connectivity checks like
// /generate_204, /hotspot-detect.html, /ncsi.txt) 302-redirects to the desktop.
// Relative Location so it works whatever host the client's check requested.
static esp_err_t captive_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/gui");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ESP32-OS \xe2\x86\x92 /gui");
    return ESP_OK;
}

esp_err_t fileserver_start(void)
{
    if (s_httpd) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = FS_PORT;
    cfg.ctrl_port        = FS_PORT;
    cfg.max_uri_handlers = 12;
    cfg.stack_size       = 10240;   // command runner calls cmd_execute()
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err)); return err; }

    httpd_uri_t u_index  = { .uri = "/",       .method = HTTP_GET,  .handler = index_handler };
    httpd_uri_t u_dl     = { .uri = "/dl",     .method = HTTP_GET,  .handler = dl_handler };
    httpd_uri_t u_del    = { .uri = "/del",    .method = HTTP_GET,  .handler = del_handler };
    httpd_uri_t u_upload = { .uri = "/upload", .method = HTTP_POST, .handler = upload_handler };
    httpd_uri_t u_dash   = { .uri = "/dash",       .method = HTTP_GET,  .handler = dash_handler };
    httpd_uri_t u_stats  = { .uri = "/api/stats",  .method = HTTP_GET,  .handler = stats_handler };
    httpd_uri_t u_cmd    = { .uri = "/api/cmd",    .method = HTTP_POST, .handler = cmd_handler };
    httpd_uri_t u_gui    = { .uri = "/gui",        .method = HTTP_GET,  .handler = gui_handler };
    httpd_uri_t u_ls     = { .uri = "/api/ls",     .method = HTTP_GET,  .handler = ls_handler };
    httpd_uri_t u_save   = { .uri = "/api/save",   .method = HTTP_POST, .handler = save_handler };
    httpd_uri_t u_qr     = { .uri = "/api/qr",     .method = HTTP_GET,  .handler = qr_handler };
    httpd_register_uri_handler(s_httpd, &u_index);
    httpd_register_uri_handler(s_httpd, &u_dl);
    httpd_register_uri_handler(s_httpd, &u_del);
    httpd_register_uri_handler(s_httpd, &u_upload);
    httpd_register_uri_handler(s_httpd, &u_dash);
    httpd_register_uri_handler(s_httpd, &u_stats);
    httpd_register_uri_handler(s_httpd, &u_cmd);
    httpd_register_uri_handler(s_httpd, &u_gui);
    httpd_register_uri_handler(s_httpd, &u_ls);
    httpd_register_uri_handler(s_httpd, &u_save);
    httpd_register_uri_handler(s_httpd, &u_qr);

    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, captive_redirect);

    dmesg_add("files: server started on tcp/%d", FS_PORT);
    return ESP_OK;
}

esp_err_t fileserver_stop(void)
{
    if (!s_httpd) return ESP_OK;
    esp_err_t err = httpd_stop(s_httpd);
    s_httpd = NULL;
    dmesg_add("files: server stopped");
    return err;
}

bool fileserver_running(void) { return s_httpd != NULL; }
