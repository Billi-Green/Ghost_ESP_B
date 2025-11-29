// ethernet_manager.h

#ifndef ETHERNET_MANAGER_H
#define ETHERNET_MANAGER_H

#include "esp_err.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"

// Initialize Ethernet Manager with W5500
esp_err_t ethernet_manager_init(void);

// Deinitialize Ethernet Manager
esp_err_t ethernet_manager_deinit(void);

// Get Ethernet connection status
bool ethernet_manager_is_connected(void);

// Get Ethernet IP address
esp_err_t ethernet_manager_get_ip_info(esp_netif_ip_info_t *ip_info);

// Get Ethernet netif handle (for accessing DNS info, etc.)
esp_netif_t *ethernet_manager_get_netif(void);

// Get DHCP server IP address (returns ESP_OK if found, ESP_ERR_NOT_FOUND if not available)
esp_err_t ethernet_manager_get_dhcp_server_ip(ip4_addr_t *server_ip);

#endif // ETHERNET_MANAGER_H

