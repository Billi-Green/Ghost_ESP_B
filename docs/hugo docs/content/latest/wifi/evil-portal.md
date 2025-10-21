---
title: "Evil Portal"
description: "Host a captive portal on GhostESP to collect test credentials."
weight: 40
---

Spin up a fake access point that shows a captive portal page, records form submissions, and logs keystrokes for lab testing.

## Prerequisites
- SD card inserted and mounted so GhostESP can read portal files and store logs under `/mnt/ghostesp/evil_portal/`.
- Portal HTML saved in `/mnt/ghostesp/evil_portal/portals/` or ready to paste via the Flipper app.
- Optional: GhostNet WebUI for easier file transfers.

## Pick a portal
1. From the launcher open **WiFi → Evil Portal → Start Evil Portal**.
   You should see GhostESP launch the default portal (`default FreeWiFi`).
2. To use a custom page, choose **Start Custom Evil Portal** instead.
   You should see a list of HTML files populated from `/mnt/ghostesp/evil_portal/portals/`.
3. Select your page, then enter the SSID and (optional) password when prompted.
   You should see the command log confirm the chosen portal, SSID, and channel.

## CLI
1. Run `listportals` to inspect available HTML files.
   You should see each filename discovered in `/mnt/ghostesp/evil_portal/portals/`.
2. Start the portal with `startportal default FreeWiFi` or `startportal myportal.html MySSID`.
   You should see log lines showing the portal path, SSID, security mode, and DHCP captive portal URI.
3. (Optional) Supply a PSK: `startportal myportal.html MySSID MyPassword`.
   You should see the AP start in WPA2-PSK mode with your passphrase.
4. Stop the portal with `stopportal` or `stop`.
   You should see logging that the DNS server and web server shut down.

### Upload HTML over UART
1. In a serial session run `evilportal -c sethtmlstr`.
   You should see confirmation that HTML buffer mode is enabled.
2. Send `[HTML/BEGIN]` on its own line, stream your HTML, then finish with `[HTML/CLOSE]`.
   You should see the serial log accept each chunk; keep the session open until the closing marker is sent.
3. Launch the portal with `startportal default MySSID` (or any SSID you prefer).
   You should see the buffered HTML previewed in the log and served to clients. Remember the buffer is limited to roughly 2 KB, so keep the page compact.

## Capture results
- Credential posts write to `/mnt/ghostesp/evil_portal/portal_creds_<n>.txt` when an SD card is mounted.
- Keystroke logging writes to `/mnt/ghostesp/evil_portal/portal_keystrokes_<n>.txt`.
- Without an SD card, captured HTML can be buffered over UART/Flipper (limited to 2048 bytes) and the portal falls back to the default template.

## Verify
- Connect from a phone or laptop and open any URL. You should see the captive portal page.
- Submit fake credentials and confirm the entry appears in the newest `portal_creds_<n>.txt` file.
- If you enabled logging via the Flipper app, confirm your HTML upload is reflected in the log preview.

## Tips
- Use the GhostNet WebUI or a card reader to upload large portals quickly.
- Keep portals lightweight—mobile browsers may struggle with heavy assets.
- When the portal is active, the Wi-Fi status badge turns blue in the launcher (`display_manager.c`).

## Troubleshooting
- **No portal files listed**: Ensure `/mnt/ghostesp/evil_portal/portals/` exists and the SD card mounted correctly at boot.
- **Credential file missing**: The capture directory is created at startup. Reboot with the SD card inserted, then start the portal again.
- **Clients see no login page**: Make sure DNS is redirecting to `192.168.4.1` and that they disconnect from other Wi-Fi networks first as well as have any 'Private DNS' or similar setting disabled on their device
