#include "camera.h"
#include "dmesg.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "sensor.h"

static const char *TAG = "camera";

// ---- Deneyap Kart camera (OV2640) DVP pin map ------------------------------
// From the official Deneyap board definition (CAM* -> GPIO). PWDN/RESET are
// not broken out on the FPC connector, so they are -1.
#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK   32   // CAMXC
#define CAM_PIN_SIOD   33   // CAMSD (SCCB SDA)
#define CAM_PIN_SIOC   25   // CAMSC (SCCB SCL)
#define CAM_PIN_D7     34   // CAMD9 (Y9)
#define CAM_PIN_D6     35   // CAMD8 (Y8)
#define CAM_PIN_D5     26   // CAMD7 (Y7)
#define CAM_PIN_D4     18   // CAMD6 (Y6)
#define CAM_PIN_D3     21   // CAMD5 (Y5)
#define CAM_PIN_D2     23   // CAMD4 (Y4)
#define CAM_PIN_D1     22   // CAMD3 (Y3)
#define CAM_PIN_D0     19   // CAMD2 (Y2)
#define CAM_PIN_VSYNC  36   // CAMV
#define CAM_PIN_HREF   39   // CAMH
#define CAM_PIN_PCLK    5   // CAMPC

// Frame size used while streaming (smooth); photos use the sensor's max.
#define STREAM_FRAMESIZE  FRAMESIZE_SVGA
#define STREAM_PORT       81

static bool             s_ready;
static framesize_t      s_max_framesize = FRAMESIZE_UXGA;
static const char      *s_sensor_name = "unknown";
static httpd_handle_t   s_stream_httpd;
static volatile bool    s_streaming;

esp_err_t camera_init(void)
{
    camera_config_t config = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7, .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5, .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3, .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1, .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href  = CAM_PIN_HREF,
        .pin_pclk  = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_UXGA,   // allocate buffers for the max size
        .jpeg_quality = 10,               // 0-63, lower = better quality
        .fb_count     = 2,                // needs PSRAM
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x (%s)", err, esp_err_to_name(err));
        dmesg_add("camera: init failed (0x%x) — is the ribbon seated?", err);
        s_ready = false;
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
        if (info) {
            s_sensor_name   = info->name;
            s_max_framesize = info->max_size;
        }
        // Start at native max for high-quality stills.
        s->set_framesize(s, s_max_framesize);
        // Push image quality: better JPEG + a touch more brightness/contrast/
        // saturation (the OV2640 defaults render a bit flat and dark).
        if (s->set_quality)    s->set_quality(s, 6);     // 0-63, lower = better
        if (s->set_brightness) s->set_brightness(s, 1);  // -2..2
        if (s->set_contrast)   s->set_contrast(s, 1);
        if (s->set_saturation) s->set_saturation(s, 1);
        if (s->set_gainceiling) s->set_gainceiling(s, GAINCEILING_4X);
    }

    int w = 0, h = 0;
    camera_max_resolution(&w, &h);
    s_ready = true;
    ESP_LOGI(TAG, "camera ready: %s max %dx%d", s_sensor_name, w, h);
    dmesg_add("camera: %s detected, max %dx%d", s_sensor_name, w, h);
    return ESP_OK;
}

bool camera_ready(void) { return s_ready; }
const char *camera_sensor_name(void) { return s_sensor_name; }

void camera_max_resolution(int *w, int *h)
{
    framesize_t fs = s_max_framesize;
    // resolution[] is exported by the esp32-camera driver.
    if (w) *w = resolution[fs].width;
    if (h) *h = resolution[fs].height;
}

bool camera_test_capture(size_t *jpg_len)
{
    if (!s_ready) return false;
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return false;
    bool ok = (fb->len > 100);            // a real JPEG, not a stub
    if (jpg_len) *jpg_len = fb->len;
    esp_camera_fb_return(fb);
    return ok;
}

esp_err_t camera_capture_to_file(const char *filepath, size_t *out_len)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    sensor_t *s = esp_camera_sensor_get();
    framesize_t restore = s ? (framesize_t)s->status.framesize : s_max_framesize;

    // Take stills at native max resolution even if the stream lowered it.
    bool bumped = false;
    if (s && s->status.framesize != s_max_framesize) {
        s->set_framesize(s, s_max_framesize);
        bumped = true;
        // Drop one stale frame captured at the old size.
        camera_fb_t *stale = esp_camera_fb_get();
        if (stale) esp_camera_fb_return(stale);
    }

    esp_err_t err = ESP_OK;
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "frame capture failed");
        err = ESP_FAIL;
        goto restore;
    }

    FILE *f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", filepath);
        esp_camera_fb_return(fb);
        err = ESP_FAIL;
        goto restore;
    }
    size_t wrote = fwrite(fb->buf, 1, fb->len, f);
    fclose(f);
    if (out_len) *out_len = wrote;
    if (wrote != fb->len) err = ESP_FAIL;
    esp_camera_fb_return(fb);

restore:
    if (bumped && s) s->set_framesize(s, restore);
    return err;
}

// ---------------------------------------------------------------------------
//  Video recording — Motion-JPEG in an AVI container
// ---------------------------------------------------------------------------
static int hput4(uint8_t *b, int p, const char *s) { memcpy(b + p, s, 4); return p + 4; }
static int hput32(uint8_t *b, int p, uint32_t v)
{ b[p]=v; b[p+1]=v>>8; b[p+2]=v>>16; b[p+3]=v>>24; return p + 4; }
static int hput16(uint8_t *b, int p, uint16_t v) { b[p]=v; b[p+1]=v>>8; return p + 2; }

static void patch_u32(FILE *f, long off, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24) };
    fseek(f, off, SEEK_SET);
    fwrite(b, 1, 4, f);
}
static void fwrite_u32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24) };
    fwrite(b, 1, 4, f);
}

esp_err_t camera_record_avi(const char *filepath, int seconds, int *frames_out, int *fps_out)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (seconds < 1) seconds = 1;
    if (seconds > 60) seconds = 60;

    sensor_t *s = esp_camera_sensor_get();
    framesize_t restore = s ? (framesize_t)s->status.framesize : s_max_framesize;
    const uint32_t W = 640, H = 480;                 // VGA for a usable framerate
    if (s) s->set_framesize(s, FRAMESIZE_VGA);
    for (int i = 0; i < 3; i++) {                    // let AE/framesize settle
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) esp_camera_fb_return(fb);
    }

    FILE *f = fopen(filepath, "wb");
    if (!f) { if (s) s->set_framesize(s, restore); return ESP_FAIL; }

    // --- 224-byte AVI header (patched at the end) --------------------------
    uint8_t h[224]; int p = 0;
    p = hput4(h,p,"RIFF"); p = hput32(h,p,0); p = hput4(h,p,"AVI ");
    p = hput4(h,p,"LIST"); p = hput32(h,p,192); p = hput4(h,p,"hdrl");
    p = hput4(h,p,"avih"); p = hput32(h,p,56);
    p = hput32(h,p,0);      // dwMicroSecPerFrame  @32
    p = hput32(h,p,0);      // dwMaxBytesPerSec
    p = hput32(h,p,0);      // dwPaddingGranularity
    p = hput32(h,p,0x10);   // dwFlags = AVIF_HASINDEX
    p = hput32(h,p,0);      // dwTotalFrames       @48
    p = hput32(h,p,0);      // dwInitialFrames
    p = hput32(h,p,1);      // dwStreams
    p = hput32(h,p,0);      // dwSuggestedBufferSize
    p = hput32(h,p,W);      // dwWidth             @64
    p = hput32(h,p,H);      // dwHeight            @68
    p = hput32(h,p,0); p = hput32(h,p,0); p = hput32(h,p,0); p = hput32(h,p,0);  // reserved[4]
    p = hput4(h,p,"LIST"); p = hput32(h,p,116); p = hput4(h,p,"strl");
    p = hput4(h,p,"strh"); p = hput32(h,p,56);
    p = hput4(h,p,"vids"); p = hput4(h,p,"MJPG"); p = hput32(h,p,0);   // flags
    p = hput16(h,p,0); p = hput16(h,p,0);          // priority, language
    p = hput32(h,p,0);      // dwInitialFrames
    p = hput32(h,p,1);      // dwScale             @128
    p = hput32(h,p,0);      // dwRate (fps)        @132
    p = hput32(h,p,0);      // dwStart
    p = hput32(h,p,0);      // dwLength            @140
    p = hput32(h,p,0);      // dwSuggestedBufferSize
    p = hput32(h,p,0xFFFFFFFF); // dwQuality = -1
    p = hput32(h,p,0);      // dwSampleSize
    p = hput16(h,p,0); p = hput16(h,p,0); p = hput16(h,p,W); p = hput16(h,p,H);  // rcFrame
    p = hput4(h,p,"strf"); p = hput32(h,p,40);
    p = hput32(h,p,40);     // biSize
    p = hput32(h,p,W);      // biWidth             @176
    p = hput32(h,p,H);      // biHeight            @180
    p = hput16(h,p,1); p = hput16(h,p,24);         // planes, bitcount
    p = hput4(h,p,"MJPG");  // biCompression       @188
    p = hput32(h,p,W*H*3);  // biSizeImage
    p = hput32(h,p,0); p = hput32(h,p,0); p = hput32(h,p,0); p = hput32(h,p,0);
    p = hput4(h,p,"LIST"); p = hput32(h,p,0); p = hput4(h,p,"movi");  // moviSize @216
    fwrite(h, 1, 224, f);   // p == 224

    // --- record frames -----------------------------------------------------
    int max_frames = seconds * 30 + 30;
    uint32_t *idx_off  = malloc(sizeof(uint32_t) * max_frames);
    uint32_t *idx_size = malloc(sizeof(uint32_t) * max_frames);
    uint32_t nframes = 0, movi_bytes = 0;
    esp_err_t err = ESP_OK;

    if (!idx_off || !idx_size) { err = ESP_ERR_NO_MEM; goto done; }

    int64_t t0 = esp_timer_get_time();
    int64_t t_end = t0 + (int64_t)seconds * 1000000;
    while (esp_timer_get_time() < t_end && nframes < (uint32_t)max_frames) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) break;
        uint32_t len = fb->len;
        fwrite("00dc", 1, 4, f);
        fwrite_u32(f, len);
        fwrite(fb->buf, 1, len, f);
        uint32_t pad = len & 1;
        if (pad) { uint8_t z = 0; fwrite(&z, 1, 1, f); }
        idx_off[nframes]  = 4 + movi_bytes;    // offset of "00dc" within movi
        idx_size[nframes] = len;
        movi_bytes += 8 + len + pad;
        esp_camera_fb_return(fb);
        nframes++;
    }
    int64_t elapsed = esp_timer_get_time() - t0;

    if (nframes == 0) { err = ESP_FAIL; goto done; }

    // --- idx1 index --------------------------------------------------------
    fwrite("idx1", 1, 4, f);
    fwrite_u32(f, nframes * 16);
    for (uint32_t i = 0; i < nframes; i++) {
        fwrite("00dc", 1, 4, f);
        fwrite_u32(f, 0x10);            // AVIIF_KEYFRAME
        fwrite_u32(f, idx_off[i]);
        fwrite_u32(f, idx_size[i]);
    }

    // --- patch header sizes ------------------------------------------------
    uint32_t fps = (elapsed > 0) ? (uint32_t)((uint64_t)nframes * 1000000 / elapsed) : 1;
    if (fps < 1) fps = 1;
    uint32_t us_per_frame = (uint32_t)(elapsed / nframes);
    uint32_t filesize = 224 + movi_bytes + 8 + nframes * 16;
    patch_u32(f, 4,   filesize - 8);       // RIFF size
    patch_u32(f, 32,  us_per_frame);
    patch_u32(f, 48,  nframes);            // avih dwTotalFrames
    patch_u32(f, 132, fps);                // strh dwRate
    patch_u32(f, 140, nframes);            // strh dwLength
    patch_u32(f, 216, 4 + movi_bytes);     // movi LIST size

    if (frames_out) *frames_out = nframes;
    if (fps_out)    *fps_out = fps;

done:
    free(idx_off);
    free(idx_size);
    fclose(f);
    if (s) s->set_framesize(s, restore);
    return err;
}

// ---------------------------------------------------------------------------
//  MJPEG streaming server (multipart/x-mixed-replace)
// ---------------------------------------------------------------------------
#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req)
{
    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char part[64];
    while (s_streaming) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { res = ESP_FAIL; break; }

        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK) {
            int n = snprintf(part, sizeof(part), STREAM_PART, (unsigned)fb->len);
            res = httpd_resp_send_chunk(req, part, n);
        }
        if (res == ESP_OK)
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);

        esp_camera_fb_return(fb);
        if (res != ESP_OK) break;   // client disconnected
    }
    return res;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    // Minimal page that just shows the stream fullscreen.
    static const char *page =
        "<!doctype html><html><head><title>ESP32 Cam</title>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<style>body{margin:0;background:#111}img{width:100%;height:auto;display:block}</style>"
        "</head><body><img src='/stream'></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, strlen(page));
}

esp_err_t camera_stream_start(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (s_stream_httpd) return ESP_OK;   // already running

    // Lower the frame size for a smooth stream.
    sensor_t *s = esp_camera_sensor_get();
    if (s) s->set_framesize(s, STREAM_FRAMESIZE);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = STREAM_PORT;
    cfg.ctrl_port        = STREAM_PORT;   // keep off the default 32768
    cfg.max_uri_handlers = 4;
    cfg.stack_size       = 8192;

    esp_err_t err = httpd_start(&s_stream_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        if (s) s->set_framesize(s, s_max_framesize);
        return err;
    }

    httpd_uri_t index_uri  = { .uri = "/",       .method = HTTP_GET, .handler = index_handler };
    httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler };
    httpd_register_uri_handler(s_stream_httpd, &index_uri);
    httpd_register_uri_handler(s_stream_httpd, &stream_uri);

    s_streaming = true;
    dmesg_add("camera: stream started on tcp/%d", STREAM_PORT);
    return ESP_OK;
}

esp_err_t camera_stream_stop(void)
{
    if (!s_stream_httpd) return ESP_OK;
    s_streaming = false;
    esp_err_t err = httpd_stop(s_stream_httpd);
    s_stream_httpd = NULL;

    // Restore native max resolution for stills.
    sensor_t *s = esp_camera_sensor_get();
    if (s) s->set_framesize(s, s_max_framesize);

    dmesg_add("camera: stream stopped");
    return err;
}

bool camera_stream_running(void) { return s_streaming; }
