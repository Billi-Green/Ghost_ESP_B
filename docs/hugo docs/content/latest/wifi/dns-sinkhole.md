---
title: "DNS Sinkhole"
description: "Block ads and unwanted domains by running a DNS sinkhole on your GhostESP device."
weight: 55
---

Run a portable DNS sinkhole that blocks ad domains and other unwanted hosts. Any device on the same network that uses the GhostESP as its DNS server will receive `NXDOMAIN` for blocked domains.

> **Note**: This feature requires a device connected to a WiFi network (STA mode). It is mutually exclusive with Evil Portal — starting one will stop the other.

## Prerequisites

- GhostESP device connected to a WiFi network (use `connect` first).
- SD card inserted (required for devices without PSRAM; optional for PSRAM boards like ESP32-S3).
- A blocklist file at `/mnt/ghostesp/dns_sinkhole/blocklist.txt` on the SD card.
  - The file must be a plain text file with one domain per line, sorted alphabetically.
  - A default blocklist is included with the firmware. You can add your own domains.

## How it works

The DNS sinkhole listens on UDP port 53 and inspects every DNS query:

1. **Check the blocklist** — If the domain or one of its parent domains matches, the client receives a clean `NXDOMAIN` response.
2. **Forward to upstream** — If the domain is not blocked, the query is forwarded to the upstream DNS server (auto-detected from your WiFi network, or falls back to `8.8.8.8`).
3. **Return the response** — The upstream response is relayed back to the client.

Known Apple Private Relay, Firefox DoH canary, and DDR resolver-discovery domains are blocked automatically because they can bypass manual DNS settings.

### Lookup paths by hardware

| Configuration | Method | Speed |
|---|---|---|
| PSRAM enabled | Bloom filter + hash table in PSRAM | ~2-5 μs |
| No PSRAM | 8 KB Bloom filter + SD binary-search verification | Avoids SD reads for most misses |

Both paths use a small heap-backed lookup cache to speed up repeated queries while keeping task stack usage low on no-PSRAM devices. No-PSRAM devices also build a fixed 8 KB Bloom filter from the SD blocklist; most non-blocked domains are rejected from RAM, while Bloom hits are verified with the SD binary search.

> **Note**: Blocklist downloading is only available on PSRAM-enabled devices. On devices without PSRAM, copy a pre-sorted blocklist file to the SD card manually.

## Starting the sinkhole

### On-device UI

1. Connect to a WiFi network first.
2. Open **WiFi → DNS Sinkhole → Start**.
3. The display will show "Sinkhole On" and the ESP's IP address.

### Command line

1. Connect to WiFi: `connect`
2. Start the sinkhole: `sinkhole start`
3. The ESP will log: `DNS sinkhole started on <IP>:53`

For diagnostics, start with query logging enabled:

```
sinkhole start log
```

## Configuring client devices

The sinkhole operates in **listen-only mode** — it does not perform DHCP spoofing or ARP poisoning. You must manually configure each client device to use the GhostESP's IP address as its DNS server.

### Android
1. Open **Settings → Network & Internet → WiFi**.
2. Long-press your connected network → **Modify network**.
3. Set **IP settings** to **Static**.
4. Set **DNS 1** to the GhostESP IP address (e.g., `192.168.6.135`).

### iOS
1. Open **Settings → WiFi**.
2. Tap the **i** button next to your network.
3. Scroll to **Configure DNS** → **Manual**.
4. Remove existing DNS servers and add the GhostESP IP address.

### Windows
1. Open **Settings → Network & Internet → Change adapter options**.
2. Right-click your adapter → **Properties** → **IPv4** → **Properties**.
3. Set **Preferred DNS server** to the GhostESP IP address.

### macOS
1. Open **System Preferences → Network → Advanced → DNS**.
2. Add the GhostESP IP address as a DNS server.

### Linux
```bash
# Temporary test
dig @<ESP_IP> google.com

# Set as system DNS (example using systemd-resolved)
resolvectl dns <interface> <ESP_IP>
```

## Stopping the sinkhole

### On-device UI
Open **WiFi → DNS Sinkhole → Stop**.

### Command line
```
sinkhole stop
```

The internal access point (GhostNet) is automatically stopped while the sinkhole is running and restarted when it stops.

## Commands

| Command | Description |
|---|---|
| `sinkhole start [dns] [log]` | Start the DNS sinkhole |
| `sinkhole stop` | Stop the DNS sinkhole |
| `sinkhole status` | Show query count and blocked count |
| `sinkhole download [n]` | Download a blocklist (PSRAM only; list sources with no argument) |
| `sinkhole add <domain>` | Add a domain to the blocklist |
| `sinkhole remove <domain>` | Remove a domain (restart to apply) |
| `sinkhole reload` | Reload blocklist (restart to apply) |
| `sinkhole log` | Toggle query logging to SD card |

## Query logging

To log all DNS queries (blocked and forwarded) to the SD card:

```
sinkhole log on
```

You can also enable logging at startup with `sinkhole start log`. Query logging writes to the SD card on every DNS query, so leave it off for normal long-running use on SPI SD-card devices.

Logs are written to `/mnt/ghostesp/dns_sinkhole/queries.log` in CSV format:

```
<timestamp_ms>,<client_ip>,<domain>,<qtype>,BLOCKED|FORWARDED|BLOCKED_CNAME,<matched_domain>
```

For forwarded queries, `<matched_domain>` is `-`. For blocked wildcard matches, it shows the parent blocklist entry that caused the block. `BLOCKED_CNAME` means the original query was forwarded, but an upstream CNAME target matched the blocklist. Built-in privacy-relay blocks are logged as `builtin:<domain>`.

## Custom blocklists

The blocklist file is located at `/mnt/ghostesp/dns_sinkhole/blocklist.txt` on the SD card. Requirements:

- One domain per line.
- Must be sorted alphabetically (required for binary search on non-PSRAM devices).
- Parent-domain matches are blocked too, so `example.com` also blocks `www.example.com` and deeper subdomains.
- Lines starting with `#` are treated as comments.
- Case-insensitive matching.

To sort a blocklist on your PC:
```bash
sort -f -u blocklist.txt > blocklist_sorted.txt
```

### Adding domains

```
sinkhole add ad.example.com
```

This inserts the domain into the sorted blocklist file. Restart the sinkhole for it to take effect.

## Troubleshooting

- **"STA not available"**: Connect to a WiFi network first using `connect`.
- **"SD card required for DNS sinkhole (no PSRAM)"**: Your device has no PSRAM. Insert and mount an SD card before starting.
- **"Blocklist download requires a PSRAM-enabled device"**: Blocklist downloading is only supported on PSRAM-enabled devices. On devices without PSRAM, copy a pre-sorted blocklist file to `/mnt/ghostesp/dns_sinkhole/blocklist.txt` on the SD card manually.
- **Queries not being blocked**: Verify the blocklist file exists at `/mnt/ghostesp/dns_sinkhole/blocklist.txt` and is sorted alphabetically. Check `sinkhole status` to see if blocked count is increasing.
- **Pages not loading (forwarding broken)**: Ensure the GhostESP can reach the internet. The upstream DNS is auto-detected from your WiFi network. If it falls back to `8.8.8.8`, verify that UDP port 53 is not blocked on your network.
- **"DNS sinkhole: no blocklist, running in proxy mode"**: No blocklist file was found. The sinkhole will forward all queries without blocking. Add a blocklist and restart.
- **Sinkhole stops when WiFi disconnects**: The sinkhole automatically stops if the STA connection is lost. Reconnect to WiFi and restart.
- **Cannot start with Evil Portal running**: The sinkhole and Evil Portal are mutually exclusive. Stop one before starting the other.
