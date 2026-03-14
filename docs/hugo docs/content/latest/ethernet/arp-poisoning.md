---
title: "ARP Poisoning"
description: "Man-in-the-middle attack via ARP spoofing with DNS interception."
weight: 25
---

Perform ARP poisoning attacks to intercept network traffic between hosts and the gateway.

## Overview

The ARP poisoning attack:

- Performs bidirectional ARP spoofing (victim ↔ gateway)
- Uses ICMP ping sweep + ARP scan for fast host discovery
- Passively discovers new hosts at runtime
- Intercepts and logs all DNS queries
- Proxies DNS using the network's actual DNS server
- Forwards IP packets for transparent MITM

## Starting the Attack

```
ethpoison start
```

This will:
1. Scan the /24 subnet using ICMP ping and ARP requests
2. Discover active hosts and their MAC addresses
3. Begin bidirectional ARP poisoning
4. Start DNS proxy on port 53
5. Enable passive host discovery

## Stopping the Attack

```
ethpoison stop
```

This will:
1. Stop all poisoning tasks
2. Restore ARP tables to correct values
3. Display captured domain count

## Viewing Status

Check current attack status:

```
ethpoison status
```

Shows:
- Running state
- Number of poisoned hosts
- Number of captured domains

## Viewing Captured Domains

List all DNS queries that were intercepted:

```
ethpoison list
```

## How It Works

1. **Host Discovery**: ICMP echo requests wake up hosts, followed by ARP scanning
2. **ARP Spoofing**: Sends forged ARP replies claiming to be the gateway (to victims) and each victim (to the gateway)
3. **DNS Interception**: Receives DNS queries on port 53, logs them, forwards to the real DNS server
4. **Packet Forwarding**: Relays non-local traffic to maintain connectivity
5. **Passive Discovery**: Monitors network traffic to discover new hosts without rescanning

## Requirements

- Ethernet connection must be active
- IP forwarding should be enabled in firmware (`CONFIG_LWIP_IP_FORWARD=y`)
- Target hosts must use the gateway as their DNS server (common default)

## Notes

- The attack automatically uses the network's DNS server from DHCP
- New hosts discovered passively are automatically added to the poison list
- Stopping the attack restores ARP tables to prevent network disruption
