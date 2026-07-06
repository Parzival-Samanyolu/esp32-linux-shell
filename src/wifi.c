#include "wifi.h"
#include "config.h"
#include "dmesg.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Known-network list from config.h. The board connects to whichever of these
// is in range with the strongest signal.
typedef struct { const char *ssid; const char *pass; } known_net_t;
static const known_net_t s_known[] = WIFI_NETWORKS;
#define N_KNOWN (sizeof(s_known) / sizeof(s_known[0]))

static EventGroupHandle_t s_wifi_events;
static esp_netif_t       *s_netif;      // STA (client) interface
static esp_netif_t       *s_ap_netif;   // SoftAP (hotspot) interface
static volatile bool      s_connected;
// Auto-connect is gated until a scan has chosen a network, so scans don't
// fight with reconnect attempts.
static volatile bool      s_allow_connect;

static bool scan_and_select(void);   // defined below, used by sta_connect_task

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
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        dmesg_add("wifi: hotspot client joined " MACSTR, MAC2STR(e->mac));
        ESP_LOGI(TAG, "AP: station " MACSTR " joined", MAC2STR(e->mac));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        dmesg_add("wifi: hotspot client left " MACSTR, MAC2STR(e->mac));
    }
}

#if USE_SOFTAP
// Bring up the always-on SoftAP hotspot (SSID/pass from config.h). This runs
// alongside the STA client so the board is reachable at AP_IP even with no
// router in range.
static void softap_start(void)
{
    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen(AP_SSID);
    strncpy((char *)ap.ap.password, AP_PASS, sizeof(ap.ap.password) - 1);
    ap.ap.channel        = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode       = (strlen(AP_PASS) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    dmesg_add("wifi: hotspot \"%s\" up at " AP_IP, AP_SSID);
    printf(">>> Hotspot \"%s\" is up — connect to it and use %s\n", AP_SSID, AP_IP);
}
#endif

// Background task: scan for a known network and connect, retrying forever.
// Runs off the main boot path so the shell/camera come up immediately whether
// or not any router is in range (the hotspot is always available regardless).
static void sta_connect_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (scan_and_select()) {
            EventBits_t b = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                                pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
            if (b & WIFI_CONNECTED_BIT) {
                // Connected — the disconnect handler keeps us reconnected to
                // this network from here on, so this task is done.
                vTaskDelete(NULL);
                return;
            }
            // Connect stalled (bad password? AP dropped). Pause and rescan.
            s_allow_connect = false;
            esp_wifi_disconnect();
            ESP_LOGW(TAG, "connect attempt timed out, rescanning");
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
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
#if USE_SOFTAP
    s_ap_netif = esp_netif_create_default_wifi_ap();   // 192.168.4.1 + DHCP server
#endif

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

#if USE_SOFTAP
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));   // hotspot + client
#else
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#endif
    ESP_ERROR_CHECK(esp_wifi_start());

#if USE_SOFTAP
    softap_start();   // bring the always-on hotspot up immediately
#endif

    // Connect to a known network in the BACKGROUND so boot never blocks — the
    // shell/camera come up right away, reachable over the hotspot regardless.
    // The task scans, connects, and (once up) hands reconnection to the event
    // handler. If no known network is ever in range, only the hotspot serves.
    xTaskCreate(sta_connect_task, "sta_connect", 4096, NULL, 5, NULL);
}

bool wifi_is_connected(void) { return s_connected; }

void wifi_get_ip(esp_netif_ip_info_t *ip)
{
    memset(ip, 0, sizeof(*ip));
    // Prefer the STA (router) IP; fall back to the hotspot IP when not joined
    // to any router, so callers always show a reachable address.
    if (s_connected && s_netif) {
        esp_netif_get_ip_info(s_netif, ip);
    } else if (s_ap_netif) {
        esp_netif_get_ip_info(s_ap_netif, ip);
    } else if (s_netif) {
        esp_netif_get_ip_info(s_netif, ip);
    }
}

esp_netif_t *wifi_netif(void) { return s_netif; }
