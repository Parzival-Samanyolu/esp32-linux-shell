#include "wifi.h"
#include "config.h"
#include "dmesg.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Known-network list from config.h. The board connects to whichever of these
// is in range with the strongest signal.
typedef struct { const char *ssid; const char *pass; } known_net_t;
static const known_net_t s_known[] = WIFI_NETWORKS;
#define N_KNOWN (sizeof(s_known) / sizeof(s_known[0]))

static EventGroupHandle_t s_wifi_events;
static esp_netif_t       *s_netif;
static volatile bool      s_connected;
// Auto-connect is gated until a scan has chosen a network, so scans don't
// fight with reconnect attempts.
static volatile bool      s_allow_connect;

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_allow_connect) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (!s_allow_connect) return;      // don't fight the initial scan
        dmesg_add("wifi: disconnected, reconnecting...");
        ESP_LOGW(TAG, "disconnected, retrying");
        // Reconnect logic: keep trying forever.
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        s_connected = true;
        dmesg_add("wifi: got ip " IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "Connected. IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        printf("\n>>> Wi-Fi connected. IP address: " IPSTR "\n", IP2STR(&evt->ip_info.ip));
        printf(">>> Connect a terminal with:  nc " IPSTR " %d\n\n",
               IP2STR(&evt->ip_info.ip), SHELL_PORT);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

// Scan, print nearby networks, and if one matches the known list, configure
// and start connecting to the strongest match. Returns true if a match was
// found and a connect attempt was kicked off.
static bool scan_and_select(void)
{
    // Pause auto-connect so the scan runs cleanly.
    s_allow_connect = false;

    printf("\n>>> Scanning for nearby Wi-Fi networks...\n");
    fflush(stdout);
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {   // blocking scan
        printf(">>> scan failed\n");
        return false;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 30) n = 30;
    wifi_ap_record_t *aps = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (!aps) return false;
    esp_wifi_scan_get_ap_records(&n, aps);

    printf(">>> Found %u network(s):\n", (unsigned)n);
    printf("      %-32s  RSSI  CH\n", "SSID");
    for (int i = 0; i < n; i++)
        printf("      %-32s  %4d  %2d\n",
               (char *)aps[i].ssid, aps[i].rssi, aps[i].primary);
    printf("\n");

    // Scan results are sorted strongest-first: pick the first known match.
    int chosen = -1, kidx = -1;
    for (int i = 0; i < n && chosen < 0; i++)
        for (int k = 0; k < (int)N_KNOWN; k++)
            if (strcmp((char *)aps[i].ssid, s_known[k].ssid) == 0) {
                chosen = i; kidx = k; break;
            }

    bool ok = false;
    if (chosen >= 0) {
        wifi_config_t wc = { 0 };
        strncpy((char *)wc.sta.ssid, s_known[kidx].ssid, sizeof(wc.sta.ssid) - 1);
        strncpy((char *)wc.sta.password, s_known[kidx].pass, sizeof(wc.sta.password) - 1);
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        esp_wifi_set_config(WIFI_IF_STA, &wc);

        printf(">>> Matched known network \"%s\" (RSSI %d). Connecting...\n",
               s_known[kidx].ssid, aps[chosen].rssi);
        dmesg_add("wifi: connecting to \"%s\"", s_known[kidx].ssid);
        ESP_LOGI(TAG, "Connecting to \"%s\"...", s_known[kidx].ssid);

        s_allow_connect = true;
        esp_wifi_connect();
        ok = true;
    } else {
        printf(">>> No known network in range. Known list:\n");
        for (int k = 0; k < (int)N_KNOWN; k++)
            printf("      - %s\n", s_known[k].ssid);
        printf(">>> Add this location's SSID to WIFI_NETWORKS in include/config.h "
               "and reflash.\n");
    }
    fflush(stdout);
    free(aps);
    return ok;
}

void wifi_init_sta(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

#if USE_STATIC_IP
    // Assign a fixed IP so the board is always reachable at the same address.
    esp_netif_dhcpc_stop(s_netif);
    esp_netif_ip_info_t ipcfg = { 0 };
    ipcfg.ip.addr      = esp_ip4addr_aton(STATIC_IP);
    ipcfg.gw.addr      = esp_ip4addr_aton(STATIC_GATEWAY);
    ipcfg.netmask.addr = esp_ip4addr_aton(STATIC_NETMASK);
    esp_netif_set_ip_info(s_netif, &ipcfg);
    esp_netif_dns_info_t dns = { 0 };
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(STATIC_DNS);
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
    dmesg_add("wifi: using static IP " STATIC_IP);
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Scan, pick the strongest known network, and connect. If none are in
    // range yet, keep rescanning every few seconds (e.g. router still booting,
    // or we're mid-move). Once a match connects, the disconnect handler keeps
    // it reconnected to that same network.
    for (int attempt = 0; ; attempt++) {
        if (scan_and_select()) {
            EventBits_t b = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                                pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
            if (b & WIFI_CONNECTED_BIT) return;      // got an IP — done
            // Connect stalled (bad password? AP dropped). Pause and rescan.
            s_allow_connect = false;
            esp_wifi_disconnect();
            ESP_LOGW(TAG, "connect attempt timed out, rescanning");
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

bool wifi_is_connected(void) { return s_connected; }

void wifi_get_ip(esp_netif_ip_info_t *ip)
{
    memset(ip, 0, sizeof(*ip));
    if (s_netif) esp_netif_get_ip_info(s_netif, ip);
}

esp_netif_t *wifi_netif(void) { return s_netif; }
