---
title: "Crackable Handshakes"
description: "Detect WPA handshakes with GhostESP and prepare them for offline cracking."
weight: 30
---

Use GhostESP to spot WPA/WPA2 handshakes, save them, and move the capture into your cracking toolchain.

## Prerequisites
- GhostESP with SD card mounted and enough space under `/mnt/ghostesp/pcaps/`.
- Target access point visible from your location.
- Optional: Laptop with Wireshark/hashcat or Flipper Zero with the GhostESP app installed.

## Steps

### On-device UI
1. Select the target AP from **Menu → WiFi → Scanning → Select AP**.
   You should see a confirmation showing the SSID and channel of the chosen network.
2. Open **Menu → WiFi → Capture → Capture Eapol**.
   You should see your device log the capture has started and on what channel the capture is locked to if you selected an AP.
3. Leave GhostESP listening while a client connects or reconnects to the target AP.
   You should see `Handshake found!` logged when the 4-way exchange completes.
4. Back out of the terminal view once you record the handshake.
5. Remove the SD card or browse to `/mnt/ghostesp/pcaps/` from the device to copy the capture.
   You should see the PCAP ready for transfer.

### CLI
1. Run `select -a <index>` using the access point number from the most recent scan (`list -a`).
   You should see "Selected Access Point" confirmation along with the channel that will be locked.
2. Run `capture -eapol` from the GhostESP terminal.
   You should see logging that EAPOL capture has started.
3. Wait while stations authenticate to the network.
   You should see per-handshake `Handshake found!` messages as they are detected.
4. Run `stop` when you collect a handshake.
   You should see confirmation that the capture ended and where the PCAP was stored.

## Export and crack
- Copy the `.pcap` to your machine and convert it to `hccapx` with [cap2hccapx](https://hashcat.net/cap2hccapx/) before running `hashcat`.
- Feed the resulting `hccapx` into `hashcat` or use the original `.pcap` with `aircrack-ng` for verification.
- For Flipper Zero, create `/ext/apps_data/ghost_esp/pcaps/` if needed, copy the file, then open **Apps → WiFi Sniffer → GhostESP** to view the handshake.

## Troubleshooting
- **No handshake found**: Force a client reconnect (toggle Wi-Fi on the target device or send a deauth) and make sure GhostESP is locked to the correct channel when starting the capture.
- **Capture isn't showing up!**: Verify an SD card is present; otherwise GhostESP streams packets over UART to the Flipper Zero whether attached or not. 