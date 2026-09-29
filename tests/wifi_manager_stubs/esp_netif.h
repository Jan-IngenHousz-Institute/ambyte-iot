#ifndef WM_STUB_ESP_NETIF_H
#define WM_STUB_ESP_NETIF_H
#include <stdint.h>
#include "esp_err.h"
typedef struct wm_stub_netif esp_netif_t;
typedef enum { ESP_NETIF_DNS_MAIN = 0, ESP_NETIF_DNS_BACKUP = 1 } esp_netif_dns_type_t;
#define ESP_IPADDR_TYPE_V4 0
typedef struct {
    struct {
        union { struct { uint32_t addr; } ip4; } u_addr;
        uint8_t type;
    } ip;
} esp_netif_dns_info_t;
enum { IP_EVENT_STA_GOT_IP = 0 };
esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_err_t esp_netif_get_dns_info(esp_netif_t *netif, esp_netif_dns_type_t type,
                                 esp_netif_dns_info_t *info);
esp_err_t esp_netif_set_dns_info(esp_netif_t *netif, esp_netif_dns_type_t type,
                                 esp_netif_dns_info_t *info);
uint32_t esp_ip4addr_aton(const char *addr);
#endif
