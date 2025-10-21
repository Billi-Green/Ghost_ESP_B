---
title: "Scanning networks"
description: "Scan and review Wi-Fi access points from GhostESP."
weight: 10
---

Map the nearby Wi-Fi landscape so you can pick interesting targets or confirm coverage.

## Prerequisites

- GhostESP flashed device, powered on with a wireless antenna.

## Steps

### On-device UI
1. Open **Menu → WiFi → Scanning**.
   You should see options including **Scan Access Points**, **Scan APs Live**, **List Access Points**, and **Select AP**.
2. Choose **Scan Access Points**.
   You should see the terminal view open and report that a scan has started. Wait until the log prints that the scan finished.
3. Back out and select **List Access Points**.
   You should see each discovered network listed with SSID, channel, signal strength, and vendor information.
4. (Optional) Pick **Scan APs Live** to monitor in real time.
   You should see new entries stream into the terminal until you return to the submenu.

### CLI
1. Open the GhostESP terminal (serial, telnet, or on-device terminal view).
   You should see the command prompt ready for input.
2. Run `scanap`.
   You should see the scanner start, print progress, and finish with a summary once results are cached.
3. Run `list -a`.
   You should see the cached access points with SSID, RSSI, and vendor information.
4. (Optional) Run `scanap -live` to watch new results continuously.
   You should see networks appear line by line until you press the back button or cancel the command.

## Verify
- Confirm that **List Access Points** or `list -a` shows the networks captured during the latest scan.
- Confirm that **Scan APs Live** or `scanap -live` continues printing new rows without freezing.

## Troubleshooting
- **No networks listed**: The terminal prints `AP information not available` if nothing was captured. Move closer to wireless routers and repeat Step 2.
- **Selection warning**: The UI shows “You Need to Scan APs First...” when you try to choose **Select AP** before scanning. Run **Scan Access Points** and try again.
- **Live scan stops immediately**: Make sure no other Wi-Fi attack or portal is running. Stop those tasks from the Wi-Fi menu, then retry Step 4.

## FAQ
- **Can I scan while connected to Wi-Fi?** Yes. The device temporarily switches into scan mode and then resumes normal operation once the list is ready.
- **Where do the vendor names come from?** GhostESP matches each network’s hardware address against its built-in manufacturer database during the listing phase.
