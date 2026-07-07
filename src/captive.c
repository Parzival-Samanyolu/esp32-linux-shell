#include "captive.h"
#include "config.h"
#include "dmesg.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_netif.h"
#include "esp_log.h"

static const char *TAG = "captive";

// ---------------------------------------------------------------------------
//  Captive-portal DNS: answer every A query with the AP IP so the phone/laptop
//  captive check hits our web server and pops the desktop open.
// ---------------------------------------------------------------------------
static void dns_hijack_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { vTaskDelete(NULL); return; }
    struct sockaddr_in sa = {
        .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ESP_LOGW(TAG, "dns bind :53 failed");
        close(sock); vTaskDelete(NULL); return;
    }
    dmesg_add("captive: DNS hijack running (all names -> " AP_IP ")");

    uint32_t apip = esp_ip4addr_aton(AP_IP);   // network byte order
    uint8_t buf[600];
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || n > (int)sizeof(buf) - 20) continue;

        // Turn the query into an answer: set response + recursion-available bits,
        // keep 1 question, add 1 answer that points the name back to AP_IP.
        buf[2] |= 0x80;          // QR = response
        buf[3] = (buf[3] & 0x0f) | 0x80;   // RA
        buf[6] = 0; buf[7] = 1;  // ANCOUNT = 1
        buf[8] = 0; buf[9] = 0;  // NSCOUNT = 0
        buf[10] = 0; buf[11] = 0;// ARCOUNT = 0

        int len = n;
        static const uint8_t ans[] = {
            0xc0, 0x0c,          // name: pointer to the question
            0x00, 0x01,          // type A
            0x00, 0x01,          // class IN
            0x00, 0x00, 0x00, 0x3c,   // TTL 60s
            0x00, 0x04,          // RDLENGTH 4
        };
        memcpy(buf + len, ans, sizeof(ans)); len += sizeof(ans);
        memcpy(buf + len, &apip, 4);         len += 4;

        sendto(sock, buf, len, 0, (struct sockaddr *)&from, fl);
    }
}

void captive_portal_start(void)
{
    xTaskCreate(dns_hijack_task, "dns_hijack", 4096, NULL, 5, NULL);
}
