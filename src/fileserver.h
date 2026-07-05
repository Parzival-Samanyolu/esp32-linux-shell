#pragma once
#include "esp_err.h"
#include <stdbool.h>

// Browser-based SD file manager on port 80: list files, preview images,
// download any file, and upload files onto the card.
esp_err_t fileserver_start(void);
esp_err_t fileserver_stop(void);
bool      fileserver_running(void);
