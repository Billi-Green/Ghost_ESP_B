# Troubleshooting

A guide to common issues and their solutions for GhostESP firmware.

## Table of Contents

- [Flashing Problems](#flashing-problems)
- [Board-Specific Issues](#board-specific-issues)
- [WiFi & Connection Issues](#wifi-and-connection-issues)
- [Display & UI Problems](#display-and-ui-problems)
- [Evil Portal Issues](#evil-portal)
- [Getting Help](#getting-help)

## Flashing Problems

### Connection Issues

- **Problem**: Can't connect or flash fails
- **Solutions**:
  1. USB Preparation:
     - Use data-capable USB cable (not charge-only)
     - Try different USB ports
     - Clean USB connections
  2. Driver Issues:
     - Install correct USB drivers
     - Clear browser cache for web flasher
     - Try different browser if issues persist

### Bootloader Mode

- **Basic Method**:
  1. Hold BOOT button
  2. Connect USB
  3. Release BOOT after connection
- **Alternative Method** (if basic fails):
  1. Hold BOOT
  2. Press RESET while holding BOOT
  3. Release RESET
  4. Release BOOT

### Common Flash Errors

- **Wrong Board Selected**
  - Verify physical USB port type
  - Match board selection to actual hardware
  - Check if board has multiple ports
- **Connection Timeout**
  - Retry bootloader sequence
  - Try different USB cable
  - Verify power LED is on

## Board-Specific Issues

### AWOK Dual Mini

- **Port Configuration**:
  - White port (WROOM): GUI/display firmware only
  - Orange port (S2): No BLE support
- **Common Problems**:
  - Cross-flashing between ports causes issues
  - Communication errors between Flipper and board
- **Solutions**:
  1. Flash correct firmware to each port
  2. Don't mix firmware between ports
  3. If port stops responding:
     - Enter bootloader mode
     - Reflash correct firmware
     - Verify connections

### CYD Boards

- **All Models Now Supported**:
  - Single USB-C
  - Dual USB ports
  - MicroUSB variants
- **Flash Requirements**:
  - Use bootloader mode sequence
  - Select correct board in flasher
  - Clear cache if connection fails

## WiFi and Connection Issues

### Web UI Connection

- **Problem**: Can't connect to the GhostNet ap
- **Solution**:
  
  1. Try the default password:  `GhostNet`
  2. Try the credentials you use to login to the web interface.

      - The GhostEsp used the same credentials for the web UI as it does for the wifi access point

- **Problem**: Can't access web interface
- **Solution**:
  1. Connect to ESP's WiFi network
  2. Browse to 192.168.4.1
  3. Enter WiFi credentials in settings

- **Problem**: I get booted from the GhostNet AP every time I issue a wifi command
- **Solution**:
  - This is a limitation of the ESP32. The chip can only serve an AP or connect/monitor other networks
  - Wait until the command finished and reconnect to the AP, or use a different control interface such as serial, the flipper app, or a touch screen.

## Display and UI Problems

### Touch Navigation

- **Design Layout**:
  - Upper screen half: Move UP
  - Lower screen half: Move DOWN
  - Middle: SELECT
  - Main menu: Direct touch selection

### Screen Issues

- **Black Screen**:
  - Verify correct firmware for display
  - Check power connections
  - Try reflashing firmware
- **Garbled Display**:
  - Update to latest firmware
  - Check display cable connection
  - Verify board selection matches hardware

## Evil Portal

### Evil Portal Setup Steps

1. Configure portal settings in web UI
2. Verify ESP has IP address before starting
3. Use appropriate URLs:
   - Desktop: domain.local/login
   - Mobile: include "https://" in portal URL
4. Note: If redirecting to MSN, you're likely connected to actual internet instead of the ESP portal

### Connecting to the Evil Portal

If you're using the Evil Portal feature, note the following:

1. **Connect to the Evil Portal AP**: Instead of connecting to "GhostNet," connect to the Evil Portal's Access Point (AP). The password for the Evil Portal AP is the same as the SSID.
2. **Web UI Replacement**: When the Evil Portal is active, the normal web UI is replaced. You won’t have access to the web UI until the board is restarted.
3. **Refreshing Access Points**: If "GhostNet" still appears after activating the Evil Portal, turn your device’s Wi-Fi off and on again to clear cached networks.

> **Tip**: Windows and other devices sometimes cache network access points, so refreshing your Wi-Fi can help display the correct AP.

### Common Portal Issues

- **Desktop Issues**:
  - Auto-redirect to MSN: ESP using actual internet DNS instead of portal
  - Failed to fetch: Check portal configuration
- **Mobile Issues**:
  - "Header fields too long": Add "https://" to URL
  - CSS display issues: Known limitation on mobile

## Getting Help

### Before Asking

1. Check latest firmware version
2. Review troubleshooting steps
3. Gather information:
   - Board type and ports
   - Firmware version
   - Error messages
   - Steps to reproduce

### Support Resources

- Use support form template
- Check apps_data/ghostesp for logs
- Review serial terminal output
- Command help: `help`
