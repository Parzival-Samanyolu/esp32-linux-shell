#include "http_client.h"
#include "shell.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "http";

#define HTTP_BUFSZ (16 * 1024)   // lives in PSRAM

// Shared transfer core. Exactly one of (fp, sock>=0) is the sink.
static esp_err_t http_transfer(const char *url, FILE *fp, int sock, size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url                = url,
        .timeout_ms         = 15000,
        .crt_bundle_attach  = NULL,        // plain HTTP; HTTPS needs cert bundle
        .buffer_size        = 2048,
        .disable_auto_redirect = false,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        return err;
    }

    int64_t content_len = esp_http_client_fetch_headers(cli);
    (void)content_len;
    int status = esp_http_client_get_status_code(cli);
    ESP_LOGI(TAG, "GET %s -> HTTP %d", url, status);

    // Large read buffer in PSRAM per the memory strategy.
    char *buf = heap_caps_malloc(HTTP_BUFSZ, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(HTTP_BUFSZ);
    if (!buf) {
        esp_http_client_cleanup(cli);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (1) {
        int r = esp_http_client_read(cli, buf, HTTP_BUFSZ);
        if (r < 0) { err = ESP_FAIL; break; }
        if (r == 0) {
            if (esp_http_client_is_complete_data_received(cli)) break;
            if (errno == EAGAIN) continue;
            break;
        }
        total += r;
        if (fp) {
            if (fwrite(buf, 1, r, fp) != (size_t)r) { err = ESP_FAIL; break; }
        } else if (sock >= 0) {
            if (shell_send_all(sock, buf, r) != 0) { err = ESP_FAIL; break; }
        }
    }

    free(buf);
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (out_len) *out_len = total;
    if (err == ESP_OK && status >= 400) return ESP_FAIL;
    return err;
}

esp_err_t http_download(const char *url, const char *filepath, size_t *out_len)
{
    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "cannot open %s for writing", filepath);
        return ESP_FAIL;
    }
    esp_err_t err = http_transfer(url, fp, -1, out_len);
    fclose(fp);
    return err;
}

esp_err_t http_get_to_socket(const char *url, int sock)
{
    return http_transfer(url, NULL, sock, NULL);
}
