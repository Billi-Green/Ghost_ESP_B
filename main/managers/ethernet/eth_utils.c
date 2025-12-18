#include "sdkconfig.h"

#ifdef CONFIG_WITH_ETHERNET

#include "managers/ethernet/eth_utils.h"
#include "managers/ethernet_manager.h"

#include "core/glog.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"

bool eth_ensure_interface_up(void) {
    esp_netif_t *eth_netif = ethernet_manager_get_netif();
    if (eth_netif == NULL) {
        glog("Ethernet interface not initialized. Bringing it up...\n");
        esp_err_t ret = ethernet_manager_init();
        if (ret != ESP_OK) {
            glog("Failed to initialize Ethernet: %s\n", esp_err_to_name(ret));
            return false;
        }
        glog("Ethernet interface initialized. Waiting for link and DHCP...\n");

        int link_wait_count = 0;
        while (!ethernet_manager_is_connected() && link_wait_count < 50) {
            vTaskDelay(pdMS_TO_TICKS(200));
            link_wait_count++;
        }

        if (!ethernet_manager_is_connected()) {
            glog("Ethernet link not established after 10 seconds\n");
            return false;
        }

        esp_netif_ip_info_t ip_info;
        int dhcp_wait_count = 0;
        const int MAX_DHCP_WAIT = 75;

        while (dhcp_wait_count < MAX_DHCP_WAIT) {
            if (ethernet_manager_get_ip_info(&ip_info) == ESP_OK && ip_info.ip.addr != 0) {
                char ip_str[16];
                ip4addr_ntoa_r(&ip_info.ip, ip_str, sizeof(ip_str));
                glog("Ethernet ready with IP address: %s\n", ip_str);
                return true;
            }
            if (dhcp_wait_count > 0 && dhcp_wait_count % 12 == 0) {
                glog("Waiting for DHCP... (%d seconds)\n", dhcp_wait_count / 5);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            dhcp_wait_count++;
        }

        glog("Warning: DHCP has not assigned an IP address yet (waited 15 seconds)\n");
        glog("The interface is up but may not be fully ready. Continuing anyway...\n");
    }
    return true;
}

#else

#include "managers/ethernet/eth_utils.h"

bool eth_ensure_interface_up(void) {
    return false;
}

#endif
