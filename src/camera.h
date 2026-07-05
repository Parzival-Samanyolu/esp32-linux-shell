#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

// Initialise the OV2640 on the Deneyap Kart's DVP camera connector.
// Frame buffers are allocated in PSRAM. Safe to call once at boot; returns
// ESP_OK on success. If it fails (no camera attached), the rest of the shell
// still runs and camera commands report the error.
esp_err_t camera_init(void);

// True once the camera initialised successfully.
bool camera_ready(void);

// Human-readable sensor name (e.g. "OV2640") and its max WxH, for dmesg/status.
const char *camera_sensor_name(void);
void camera_max_resolution(int *w, int *h);

// Grab one frame and immediately return it — used by `selftest` to prove the
// sensor produces data without needing the SD card. *jpg_len gets the size.
bool camera_test_capture(size_t *jpg_len);

// Capture one JPEG at the sensor's native max resolution and write it to
// `filepath`. Returns bytes written in *out_len (may be NULL). Temporarily
// raises the frame size if a stream is running, then restores it.
esp_err_t camera_capture_to_file(const char *filepath, size_t *out_len);

// Record `seconds` of Motion-JPEG video to an .avi file on the SD card (VGA
// for a smooth framerate). Returns frames captured and measured fps via the
// out params (may be NULL).
esp_err_t camera_record_avi(const char *filepath, int seconds,
                            int *frames_out, int *fps_out);

// ---- MJPEG stream server (port 81) ----------------------------------------
esp_err_t camera_stream_start(void);   // start HTTP server, SVGA frames
esp_err_t camera_stream_stop(void);    // stop it, free the socket
bool      camera_stream_running(void);
