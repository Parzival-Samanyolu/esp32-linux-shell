#pragma once
#include "esp_err.h"
#include <stddef.h>

// Download a URL straight to a file on the SD card. Returns bytes written
// in *out_len (may be NULL). Large transfer buffer is allocated in PSRAM.
esp_err_t http_download(const char *url, const char *filepath, size_t *out_len);

// Fetch a URL and stream the response body to an open socket (used by curl).
esp_err_t http_get_to_socket(const char *url, int sock);
