# Board-Specific Guide for GhostESP Firmware

This guide helps you identify your board type and start using GhostESP firmware.

## 🔍 Identifying Your Board

### CYD (Cheap Yellow Display) Boards

CYD boards are easily recognizable with their yellow color and built-in display.  
**Features:**

- Built-in display with touch functionality
- SD card slot
- RGB LED indicators
- Supported variants:
  - **CYD2USB**: USB-C only
  - **CYDMicroUSB**: Micro USB only
  - **CYDDualUSB**: USB-C and Micro USB (now supported)

> **Note**: CYD boards using the **ESP32-2432S028** (2.8 inches) are supported with GhostESP firmware. The **ESP32-2432S024** variant (2.4 inches) is not compatible.

If you need further help identifying your CYD please see our [CYD ID Guide](./CYD-ID-Guide.md)

### 7-Inch Display Boards

High-resolution display boards supported:

- **Waveshare LCD**: 800x480 resolution
- **Crowtech LCD**: 800x480 resolution  
Both variants use the **ESP32-S3**.

### Awok & Marauder Boards

Multiple variants are supported:

- **MarauderV6** and **AwokDual**: 240x320 touchscreen
- **AwokMini**: 128x128 display with joystick
- **Awok V5**

### Rabbit labs GhostEsp Board

- **Currently discontinued** (stay tuned :ghost:)
- ESP32-C6 based
- Rabbit labs GPS module port
- 3x RGB LEDs for triple the fun
- Builtin SD card slot

### ESP32 Cardputer

A compact, keyboard-integrated board designed for portability.

- Built-in display (240x135)
- Integrated keyboard
- SD card slot

### Generic ESP32 Boards

Base models with different levels of compatibility:

- **ESP32** (standard model)
- **ESP32-S2** (includes the Flipper WiFi Devboard)
- **ESP32-S3**
- **ESP32-C3**
- **ESP32-C6**

## 📱 Display Support and Resolutions

### Supported Resolutions

- **CYD Boards**: 240x320 with touchscreen (compatible with ESP32-2432S028 2.8-inch only)
- **7-inch Displays**: 800x480 with touchscreen
- **Cardputer**: 240x135
- **AwokMini**: 128x128 with joystick control
- **Marauder V6 / AwokDual**: 240x320 with touchscreen

### Feature Notes

- SD card compatibility varies across models:
  - Full support: **CYD boards** and **Cardputer**
  - Not supported: **Marauder V6**, **Awok Dual Touch**, **Generic ESP32-Wroom** chips, and **Awok Mini**
- Standby mode available for non-touch displays.
- Touchscreen support is actively developed, with fixes available for **Marauder V6**.

## 🔌 Getting Started with GhostESP

1. Go to [Spooky Tools Web Flasher](https://flasher.ghostesp.net/).
2. Select your board type and follow the flasher instructions.
3. For troubleshooting, join the community on [Discord](https://discord.gg/5cyNmUMgwh).

## SD Card Compatibility

- Fully supported on **CYD boards** and **Cardputer**.
- Not supported on **Marauder V6**,**Awok** variants, and **Generic ESP32** builds.
- For latest compatibility updates, check [Discord](https://discord.gg/5cyNmUMgwh) announcements.

## ⚠️ Known Limitations

- **Marauder V6**, **Awok variants**, and **Generic ESP32** builds: No SD card support.
- Some features, like BLE spam, may be missing in alpha releases.
- Cache-clearing may be required when using the web flasher.
- ESP32S2 Boards do not support bluetooth functionality due to lack of hardware.

## 👥 Support & Community

- Join the [Discord Community](https://discord.gg/5cyNmUMgwh) for live support and updates.
- Check the [GitHub Repository](https://github.com/jaylikesbunda/Ghost_ESP) for releases and issue reporting.
- Web flasher tool: [Spooky Tools Web Flasher](https://flasher.ghostesp.net/).
