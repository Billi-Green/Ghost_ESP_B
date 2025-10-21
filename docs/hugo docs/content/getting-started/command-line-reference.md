---
title: "Command Line Reference"
description: "Common GhostESP CLI commands grouped by task."
weight: 20
toc: true
---

## Connecting to the CLI

- Use a [serial console](https://ghostesp.net/serial) (115200 baud is recommended) with a USB data cable or the built-in Terminal app on touch-enabled boards.
- From the web UI, open the Terminal panel for remote access (running Wi-Fi/BLE operations will drop the GhostNet AP while the single radio switches modes).
- Send `help` to confirm connectivity; output appears prefixed with `>` in the console.

## General Utilities

- **`help [category|all]`** — List commands; categories include `wifi`, `ble`, `portal`, `comm`, `sd`, `led`, `gps`, `misc`, `printer`, `cast`, `capture`, `beacon`, `attack`.
- **`mem [dump|trace <start|stop|dump>]`** — Print heap stats, dump allocation state, or control heap tracing (for developers).
- **`reboot`** — Soft restart the device.
- **`chipinfo`** — Print SoC model, cores, features, and IDF version.
- **`timezone <TZ>`** — Set timezone, e.g., `timezone EST5EDT,M3.2.0,M11.1.0`.

## Wi-Fi Operations

- **`scanap [seconds|-live|-stop]`** — Run an AP scan, optionally for a set duration, live channel hop, or stop (`-stop`).
- **`scansta`** — Hop channels and log associated stations.
- **`scanall [seconds]`** — Combined AP and STA scan with summary.
- **`list [-a|-s|-airtags]`** — Show AP scan results, associated stations, or AirTags.
- **`select [-a|-s|-airtag] <idx[,idx]>`** — Queue APs, a station, or an AirTag by index for later actions.
- **`attack -d|-e|-s <password>`** — Trigger deauth, EAPOL logoff, or SAE flood (`-s` needs ESP32-C5/C6 and the target PSK).
- **`stopdeauth` / `stopspam`** — Halt active attacks or beacon floods.
- **`beaconspam [mode]`** — Broadcast spoof SSIDs (`-r`, `-rr`, `-l`, or custom text).
- **`listenprobes [channel|stop]`** — Monitor probe requests and log to PCAP if SD is present.
- **`karma start [ssid...]`** / **`karma stop`** — Respond to client probes with saved or provided SSIDs.
- **`connect <ssid> [pass]`** — Join an infrastructure network (saves credentials); wrap SSID/password in quotes when they contain spaces, e.g., `connect "My SSID" "My Password"`.
- **`disconnect`** — Leave the current STA connection.
- **`apcred <ssid> <pass>`** or **`apcred -r`** — Change or reset GhostNet AP credentials.
- **`apenable on|off`** — Toggle AP persistence across reboots.
- **`scanports <local|ip> [all|start-end]`**, **`scanarp`**, **`scanlocal`**, **`scanssh <ip>`** — Scan the subnet, a target host, or run mDNS/SSH discovery utilities.
- **`dhcpstarve <start [threads]|stop|display>`** — Flood a DHCP server or show collected leases.
- **`pineap [-s]`** — Monitor Pineapple-style beacons; `-s` stops detection.
- **`saeflood <password>`** / **`stopsaeflood`** / **`saefloodhelp`** — Launch, stop, or review SAE flood attack guidance.
- **`capture <-probe|-deauth|-beacon>`** — Start packet captures for the specified frame type to SD.

## BLE Features *(ESP32-S2 excluded)*

- **`blescan [-f|-ds|-a|-r|-s]`** — Scan for BLE devices, Flippers, spam detectors, or raw advertising; `-s` stops.
- **`blewardriving [-s]`** — Log BLE beacons with GPS metadata.
- **`blespam [mode|-s]`** — Emit spoofed BLE advertisements (Apple, Microsoft, Samsung, Google, random).
- **`listflippers`** — Scan for nearby Flipper Zero devices.
- **`selectflipper <idx>`** — Choose a Flipper from the discovered list for interactions.
- **`listairtags`**, **`selectairtag <idx>`**, **`spoofairtag`**, **`stopspoof`** — AirTag tools.

## Portal and Web Tools

- **`startportal <path|default> <AP_SSID> [PSK]`** — Serve an Evil Portal bundle from SD or flash (`default` uses the built-in portal).
- **`stopportal`** — Shut down the active portal.
- **`listportals`** — List bundles on SD card or flash.
- **`evilportal -c <sethtmlstr|clear>`** — Manage the Evil Portal HTML buffer (`-c sethtmlstr` to capture inbound HTML, `-c clear` to revert to defaults).
- **`webauth on|off`** — Require or disable web UI login.

## Communication Bridge

- **`commdiscovery`** — Enter UART discovery mode, broadcasting handshake frames until peers reply (run before `commconnect`).
- **`commconnect <peer_name>`** — Connect to a discovered peer (after `commdiscovery`).
- **`commsend <command> [data...]`**, **`commstatus`**, **`commdisconnect`** — Issue commands, inspect state, or close the peer link.
- **`commsetpins <tx> <rx>`** — Save preferred pins.

## SD Card & Storage

- **`sd_config`** — Display SD mode, pins, and status.
- **`sd_pins_spi <cs> <clk> <miso> <mosi>`** — Configure SPI wiring.
- **`sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>`** — Configure SDIO wiring.
- **`sd_save_config`** — Persist SD settings to storage.

## RGB Lighting

- **`rgbmode <rainbow|police|strobe|off|color>`** — Run an LED effect immediately.
- **`setrgbmode <normal|rainbow|stealth>`** — Persist the LED mode across reboots.
- **`setrgbpins <r> <g> <b>`** — Override discrete RGB GPIOs; pass the same pin for all three values to switch into single-wire NeoPixel mode on that data pin.
- **`setneopixelbrightness <0-100>`** / **`getneopixelbrightness`** — Control NeoPixel intensity.

## GPS & Telemetry

- **`gpsinfo [-s]`** — Stream current fix, satellites, and speed; pass `-s` to stop the display task.
- **`blewardriving`** — Uses GPS data for logs; ensure GPS is enabled in settings.
- **`startwd [-s]`** — Begin Wi-Fi wardriving with GPS logging, CSV output, and monitor mode; pass `-s` to stop and flush logs.

## Printing & Casting

- **`powerprinter [ip text font alignment]`** — Send formatted PCL text jobs to LAN printers; pull saved defaults when arguments are omitted.
- **`dialconnect`** — Pair with a DIAL-capable device (e.g., Chromecast/YouTube).

## Peripheral Integrations

- **`stop`** — Global kill switch: halt Wi-Fi attacks, BLE/BLE spam, GPS logging, wardriving, PCAP/CSV captures, RGB effects, and other background timers, returning the device to idle.

## Settings Management

- **`settings list`** — Dump available configuration keys.
- **`settings help`** — Show supported subcommands.
- **`settings get <key>`** / **`settings set <key> <value>`** — Inspect or change individual options.
- **`settings reset [key]`** — Restore all settings or a specific key to defaults.

## Safety & Diagnostics Tips

- **`stop`** — Global kill switch: halt Wi-Fi attacks, BLE/BLE spam, GPS logging, wardriving, PCAP/CSV captures, RGB effects, and other background timers, returning the device to idle.
