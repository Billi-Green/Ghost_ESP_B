---
title: "Capturing packets"
description: "Record Wi-Fi traffic to a PCAP for later analysis."
weight: 20
---

Save live Wi-Fi traffic to the SD card so you can review it in desktop tools or on a Flipper Zero.

## Prerequisites

- GhostESP flashed device.
- Mounted SD Card or a Flipper Zero with a microSD card if you plan to save captures there.

## Steps

### On-device UI
1. Open **Menu → WiFi → Capture**.
   You should see options such as **Capture Probe**, **Capture Deauth**, **Capture Beacon**, and **Capture Raw**.
2. Choose the capture you need.
   You should see the terminal view open and report that capture has started. Leave the device running until you have enough traffic.
3. Back out of the terminal view to stop capture.
   You should see a serial log confirming the capture finished and the file path on the SD card.
4. Remove the SD card or open the GhostNet WebUI file browser to retrieve the PCAP.
   You should see the capture saved under `/mnt/ghostesp/pcaps/` with a name such as `probescan_3.pcap`.

### CLI
1. Open the GhostESP terminal (serial, telnet, or on-device terminal view).
   You should see the command prompt ready for input.
2. Run `capture -probe` (or `-deauth`, `-beacon`, `-raw`, etc.).
   You should see logging that the capture has started and which packets are being recorded.
3. Let the command run while traffic is collected.
   You should see packets listed until you are ready to stop.
4. Run `capture -stop` when you are finished.
   You should see confirmation that the capture ended and where the file was written.

### Capture modes
- **-probe**: Records probe requests so you can see devices searching for known SSIDs.
- **-deauth**: Records deauthentication frames to diagnose disconnect storms or targeted kicks.
- **-beacon**: Records beacons to review advertised SSIDs and channel metadata.
- **-raw**: Dumps every Wi-Fi frame seen on the tuned channel for full analysis.
- **-eapol**: Captures WPA/WPA2 4-way handshakes, PMKID messages, and rekeys so you can validate client authentications or export the flow for offline key cracking.
- **-pwn**: Records frames from `Pwnagotchi` devices.
- **-wps**: Captures Wi-Fi Protected Setup traffic to confirm whether a router exposes WPS enrolment.
- **-802154** (ESP32-C5/C6 only): Records IEEE 802.15.4 frames when you supply `capture -802154`.

## Verify
- Confirm the SD card contains a `.pcap` file.
- Open the file in Wireshark app to make sure packets are listed.

## Notes
- Captures save to `/mnt/ghostesp/pcaps/` only when an SD card is mounted. If the folder is missing the device streams packets over UART to the Flipper Zero when writing a file.
- The firmware logs `PCAP: saving to SD as ...` when file storage is active, or `PCAP: streaming over UART` when it falls back to the terminal.
- Flipper Zero GhostESP app expects captures under `/ext/apps_data/ghost_esp/pcaps/` on its microSD. Copy the `.pcap` using QFlipper or Straight from the Flipper's SD Card to review it in Wireshark or et cetera.
- Large captures can take time to copy. Use a card reader instead of QFlipper or the WebUI for faster transfers.

## Troubleshooting

- **File missing after capture**: Reboot the device with the SD Card inserted and make sure it logs that the SD Card is mounted at boot using a [Serial Console](https://ghostesp.net/serial) and that you exited through the back button or using the `stop` CLI command to trigger the save.

