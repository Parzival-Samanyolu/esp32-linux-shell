#include "fileserver.h"
#include "config.h"
#include "dmesg.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"

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

// Reject path traversal; only allow a bare filename under SD_MOUNT.
static bool safe_name(const char *name)
{
    if (!name || !*name) return false;
    if (strstr(name, "..")) return false;
    if (strchr(name, '/'))  return false;
    return true;
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
        "</style></head><body><h1>ESP32 SD Card — " SD_MOUNT "</h1>"
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

esp_err_t fileserver_start(void)
{
    if (s_httpd) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = FS_PORT;
    cfg.ctrl_port        = FS_PORT;
    cfg.max_uri_handlers = 8;
    cfg.stack_size       = 8192;
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err)); return err; }

    httpd_uri_t u_index  = { .uri = "/",       .method = HTTP_GET,  .handler = index_handler };
    httpd_uri_t u_dl     = { .uri = "/dl",     .method = HTTP_GET,  .handler = dl_handler };
    httpd_uri_t u_del    = { .uri = "/del",    .method = HTTP_GET,  .handler = del_handler };
    httpd_uri_t u_upload = { .uri = "/upload", .method = HTTP_POST, .handler = upload_handler };
    httpd_register_uri_handler(s_httpd, &u_index);
    httpd_register_uri_handler(s_httpd, &u_dl);
    httpd_register_uri_handler(s_httpd, &u_del);
    httpd_register_uri_handler(s_httpd, &u_upload);

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
