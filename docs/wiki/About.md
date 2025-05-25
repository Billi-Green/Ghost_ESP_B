# About GhostESP

GhostESP is a robust and feature-rich firmware designed for ESP32 microcontrollers, enabling advanced wireless network exploration and security testing.

## Supported Hardware

The following ESP32 models are officially supported:

### Display-Enabled Boards

- **CYD (Cheap Yellow Display) variants**
  - **CYD2USB** (USB-C)
  - **CYDMicroUSB**
  - **CYDDualUSB** (Both ports)
  - **Compatibility Note**: Stable only with **ESP32-2432S028 (2.8 inches)**. Incompatible with **ESP32-2432S024 (2.4 inches)**.
- **Waveshare LCD (7-inch)**: 800x480 resolution, using ESP32-S3
- **Crowtech LCD (7-inch)**: 800x480 resolution, using ESP32-S3
- **ESP32-S3-Cardputer**: Compact design with a built-in display and keyboard
- **MarauderV6 & AwokDual**: 240x320 touchscreen models
- **AwokMini**: 128x128 display with joystick for navigation

### Generic Boards

- **ESP32 Wroom**: Standard model
- **ESP32-S2**
- **ESP32-C3**
- **ESP32-S3**
- **ESP32-C6**: Partial support with limited feature availability

## Important Considerations

- **Performance** varies across ESP32 models and may impact certain features.
- **Hardware-specific limitations**: Certain features, like SD card support and touchscreen responsiveness, depend on board compatibility.
- **Web flasher**: Cache clearing may be necessary for proper function.
- **Firmware updates**: Beta development may include experimental features.

## Version Status

- **Current version**: VA1.3.7
- **Framework**: ESP-IDF

### Recent Feature Additions

- **Qt6-based control application** for enhanced desktop control
- **CYD RGB LED control support** for customizable status indicators
- **SD card support** for compatible devices
- **Evil Portal**: Available in both online and offline modes
- **Screen support** with the Flappy Ghost game
- **Web UI with auto-updating settings** for streamlined configuration
- **Dial Connect** for Smart TV Casting

### Features Under Development

- **Additional screen support optimizations** to improve compatibility
- **Enhanced SD card compatibility** for more board models
- **BLE feature improvements** for device discovery and interaction
- **Touch screen calibration refinements** to enhance touch accuracy

## Acknowledgments

GhostESP builds upon the work of several open-source projects and developers:

- **JustCallMeKoKo**: ESP32 development foundations
- **thibauts**: CastV2 protocol insights
- **MarcoLucidi01**: DIAL protocol integration
- Community contributors for board support

## Project Goals

GhostESP aims to provide:

- Comprehensive wireless testing capabilities
- A user-friendly interface
- Stable and reliable performance
- Educational value for security researchers
- Regular feature updates and improvements
