#pragma once
#include "esp_netif.h"

// Bring up Wi-Fi in station mode using the credentials in config.h.
// Blocks (up to a timeout) until an IP is obtained, then returns.
// Automatic reconnect is handled internally on disconnect events.
void wifi_init_sta(void);

// True once an IP address has been acquired.
bool wifi_is_connected(void);

// Fill *ip with the current IPv4 address (0 if not connected).
void wifi_get_ip(esp_netif_ip_info_t *ip);

// Handle to the STA netif (for ifconfig, etc.). NULL until initialised.
esp_netif_t *wifi_netif(void);
