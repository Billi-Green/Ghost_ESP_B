// ethernet_manager.h

#ifndef ETHERNET_MANAGER_H
#define ETHERNET_MANAGER_H

#include "esp_err.h"
#include "esp_netif.h"

// Initialize Ethernet Manager with W5500
esp_err_t ethernet_manager_init(void);

// Deinitialize Ethernet Manager
esp_err_t ethernet_manager_deinit(void);

// Get Ethernet connection status
bool ethernet_manager_is_connected(void);

// Get Ethernet IP address
esp_err_t ethernet_manager_get_ip_info(esp_netif_ip_info_t *ip_info);

#endif // ETHERNET_MANAGER_H

