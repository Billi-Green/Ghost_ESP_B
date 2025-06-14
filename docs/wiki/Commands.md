# Ghost ESP Commands Guide

## 🔍 Basic Network Scanning

- `scanap` - Start scanning for all WiFi networks in range
- `list -a` - Show complete list of found WiFi networks with technical details (signal strength, security type, channels)
- `scansta` - Find devices connected to WiFi networks around you
- `list -s` - Show all discovered connected devices
- `stopscan` - Stop any active scanning operation
- `select -a <number>` - Target a specific network from the scan list (use the number shown in list -a)

## ⚡ Attack Modes

- `attack -d` - Start deauthentication (temporarily disconnects devices from selected network)
- `stopdeauth` - Stop the deauthentication attack

## 📡 Network Generation

- `beaconspam -r` - Create multiple random fake networks
- `beaconspam -rr` - Create Never Gonna Give You Up themed networks
- `beaconspam -l` - Clone all visible networks in the area
- `beaconspam <name>` - Create a network with your chosen name
- `stopspam` - Stop creating fake networks

## 🕸️ Evil Portal Creation

- Online Mode (clones a real website):
 `startportal <website-url> <real-wifi-name> <wifi-password> <portal-name> <fake-domain>`

 For example:
 `startportal https://example.com MyWiFi password123 "Free WiFi" login.com`

- Offline Mode (uses local HTML file):
 `startportal <file-path> <portal-name> <fake-domain>`

 For example:
 `startportal index.html "Free WiFi" login.com`

- Stop Portal:
 `stopportal`

## 💾 Network Capture (Requires SD Card/Flipper)

- `capture -probe` - Save devices searching for WiFi
- `capture -beacon` - Save network broadcast information
- `capture -deauth` - Record deauthentication packets
- `capture -raw` - Save all wireless traffic
- `capture -wps` - Capture WPS setup packets
- `capture -pwn` - Record Pwnagotchi activity
- `capture -eapol` - Record EAPOL/handshake packets
- `capture -stop` - Stop recording and save data

## 🌐 Network Connection & Tools

- `connect <network> <password>` - Connect to a WiFi network
- `dialconnect` - Find and interact with smart TVs on network
- `powerprinter <ip> <text> <size> <position>` - Send text to network printers
 Positions: CM (center), TL (top-left), TR (top-right), BR (bottom-right), BL (bottom-left)

## 📱 Bluetooth Operations

Not available on ESP32-S2:

- `blescan -f` - Find Flipper Zero devices
- `blescan -ds` - Detect Bluetooth spam
- `blescan -a` - Scan for AirTags
- `blescan -r` - View all Bluetooth traffic
- `blescan -s` - Stop Bluetooth scanning

## 📍 GPS Features

- `startwd` - Begin recording networks with GPS location
- `startwd -s` - Stop GPS recording

## 🔧 System Commands

- `help` - Show complete command list
- `stop` - Stop all running operations
- `reboot` - Restart device
- `setcountry` - command for setting the country code
- `timezone` - for setting a timezone

> Remember to check your hardware compatibility before using commands
