#include "qrcmd.h"
#include "shell.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "esp_heap_caps.h"
#include "qrcodegen.h"

// Render the QR to the terminal using Unicode half-blocks (2 modules per text
// row) with an explicit black-on-white colour set, so it scans regardless of
// the terminal's colour theme. A 4-module quiet zone surrounds it.
static void qr_print(int sock, const uint8_t *qr)
{
    int size = qrcodegen_getSize(qr);
    const int border = 4;

    for (int y = -border; y < size + border; y += 2) {
        char line[420];
        int p = 0;
        p += snprintf(line + p, sizeof(line) - p, "\033[30;47m");   // black fg, white bg
        for (int x = -border; x < size + border; x++) {
            bool top = (x >= 0 && x < size && y >= 0 && y < size)
                       && qrcodegen_getModule(qr, x, y);
            bool bot = (x >= 0 && x < size && (y + 1) >= 0 && (y + 1) < size)
                       && qrcodegen_getModule(qr, x, y + 1);
            const char *ch = (top && bot) ? "\xe2\x96\x88"   // full block
                           : top          ? "\xe2\x96\x80"   // upper half
                           : bot          ? "\xe2\x96\x84"   // lower half
                                          : " ";
            p += snprintf(line + p, sizeof(line) - p, "%s", ch);
        }
        p += snprintf(line + p, sizeof(line) - p, "\033[0m");
        shell_printf(sock, "%s\r\n", line);
    }
}

void do_qr(shell_ctx_t *ctx, int argc, char **argv)
{
    // Build the payload: joined args, or the Wi-Fi hotspot login by default.
    char text[256];
    if (argc >= 2) {
        text[0] = '\0';
        for (int i = 1; i < argc; i++) {
            if (i > 1) strlcat(text, " ", sizeof(text));
            strlcat(text, argv[i], sizeof(text));
        }
    } else {
        // WIFI: standard join format — phones recognise it and offer to connect.
        snprintf(text, sizeof(text), "WIFI:S:%s;T:WPA;P:%s;;", AP_SSID, AP_PASS);
        shell_printf(ctx->sock, "Scan to join the ESP32 hotspot \"%s\":\r\n\r\n", AP_SSID);
    }

    uint8_t *qr   = heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX, MALLOC_CAP_SPIRAM);
    uint8_t *temp = heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX, MALLOC_CAP_SPIRAM);
    if (!qr || !temp) { free(qr); free(temp); shell_printf(ctx->sock, "qr: out of memory\r\n"); return; }

    // Cap the version so the code stays narrow enough for a terminal (<= ~65 cols).
    bool ok = qrcodegen_encodeText(text, temp, qr, qrcodegen_Ecc_MEDIUM,
                                   qrcodegen_VERSION_MIN, 11,
                                   qrcodegen_Mask_AUTO, true);
    if (!ok) {
        shell_printf(ctx->sock, "qr: text too long to encode (try something shorter)\r\n");
    } else {
        qr_print(ctx->sock, qr);
        shell_printf(ctx->sock, "\r\n%s\r\n", text);
    }
    free(qr);
    free(temp);
}
