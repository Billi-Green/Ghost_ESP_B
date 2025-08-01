# Ghost ESP Control Panel - README

## Overview

The **Ghost ESP Control Panel** is a GUI application for controlling and communicating with an ESP32 microcontroller over a serial connection. Built with Python and PyQt6, it provides WiFi/BLE scanning, packet capture, custom commands, and more.

## Features

- **Serial Connection Management**: Connect/disconnect to ESP32 devices via serial port.
- **WiFi Operations**: Scan networks, list APs/stations, de-auth, beacon spam, and more.
- **BLE Operations**: Scan for BLE devices, find Flippers/AirTags, stop scans.
- **Packet Capture**: Capture various WiFi packet types.
- **Custom Command Support**: Send any command directly.
- **Logging and Display**: Real-time logs and structured scan/status display.
- **Auto-Reconnect**: Optionally reconnect if the serial connection drops.
- **UI Lock/Overlay**: The UI disables and shows a visual overlay when not connected.
- **Resizable Panes**: Command and display areas can be resized.
- **Portal File Upload**: Upload custom HTML portals with progress indication.

## Table of Contents

- [Installation](#installation)
- [Usage](#usage)
  - [Starting the Application](#starting-the-application)
  - [Connecting to ESP32](#connecting-to-esp32)
  - [Available Operations](#available-operations)
- [Code Structure](#code-structure)
- [UI](#ui)
- [Troubleshooting](#troubleshooting)

## Installation

### Prerequisites

1. **Python 3.8+**: Install Python 3.8 or later.
2. **Dependencies**:
   ```bash
   sudo apt update
   sudo apt install libxcb-cursor0
   python -m venv .venv
   source .venv/bin/activate
   pip install -r requirements.txt
   ```
3. **Ghost ESP Firmware**: Flash your ESP32 with compatible firmware.

## Usage

### Starting the Application

```bash
python esp_ghost_control.py
```

### Connecting to ESP32

1. Select a serial port in the **Serial Connection** section.
2. Click **Refresh Ports** if needed.
3. Click **Connect**.
   - The UI will unlock and overlay will disappear when connected.
   - Status and errors are shown in the log area.

### Available Operations

#### WiFi Operations

- **Scan Access Points**: Find nearby WiFi APs.
- **Start/Stop Deauth**: Deauthenticate selected APs.
- **Beacon Spam**: Send random, Rickroll, or AP list beacons.

#### BLE Operations

- **Find Flippers**: Scan for Flipper BLE devices.
- **AirTag Scanner**: Detect AirTags.
- **Raw BLE Scan**: Low-level BLE scan.

#### Packet Capture

- **Capture Probes**: Detect WiFi probe requests.
- **Capture Deauth**: Track deauth packets.
- **Capture WPS**: Log WPS packets.

#### Portal File Upload

- **Send Local HTML as Portal**: Upload a custom HTML file as an evil portal.
- Progress is shown with an indicator/spinner.
- After upload, the portal dropdown updates to "uploaded html".

#### Custom Commands

- Type a command in the **Custom Command** field.
- Press **Enter** or click **Send**.

### Logging and Display

- **Log Area**: Shows timestamps and command feedback.
- **Display Area**: Shows scan results, status, and structured responses.

## Code Structure

- **`SerialMonitorThread`**: Reads serial data in a thread, emits via `data_received`.
- **`PortalFileSenderThread`**: Uploads portal files in a thread, emits progress and completion.
- **`ESP32ControlGUI`**: Main GUI class, sets up UI, handles events, manages commands.
  - **UI Components**: Tabs for WiFi, BLE, capture, portal, and settings.
  - **Overlay**: Visual indicator when not connected.
  - **Resizable Panes**: Uses splitters for flexible layout.

## UI

![ui](01.png)

## Troubleshooting

- **Cannot Connect to ESP32**: Check port, firmware, and power.
- **Unexpected Disconnects**: Check cable, try lower baud rate, enable auto-reconnect.
- **Command Errors**: Ensure commands match firmware.
- **UI Overlay Covers Controls**: Overlay only covers main UI; serial controls always accessible.
- **Portal Upload Hangs**: Make sure you are connected and the ESP32 is ready.

---

**Note**: This application is for development and diagnostics. Use responsibly and comply with local regulations when using network diagnostic tools.
