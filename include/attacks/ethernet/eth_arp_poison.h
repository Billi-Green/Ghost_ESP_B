#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef CONFIG_WITH_ETHERNET

// Start ARP poisoning + DNS proxy attack.
// Discovers hosts, sends spoofed ARP replies, and transparently proxies DNS.
esp_err_t eth_arp_poison_start(void);

// Stop the attack and restore ARP tables.
esp_err_t eth_arp_poison_stop(void);

bool eth_arp_poison_is_running(void);

// Print all captured domains to the log.
void eth_arp_poison_print_domains(void);

// Print current status (host count, domain count, running state).
void eth_arp_poison_print_status(void);

#endif // CONFIG_WITH_ETHERNET
