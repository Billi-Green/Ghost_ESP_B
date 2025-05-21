# Ghost ESP Changelog

## Revival v1.5

### Added

- Support for ESP32C5

- Attacks
  - Deauthentication & DoS

    - Added support for direct station deauthentication
    - Added DHCP-Starve attack

  - Spoofing & Tracking

    - Added support for AirTag selection and spoofing
    - Added support for selecting and tracking Flipper Zero rssi

  - Beacon Management

    - Custom beacon SSID list management and spam

- Commands
  - Added station selection capability to existing select command
  - Added a timezone command to set the timezone with a POSIX TZ string
  - Enable passing custom DIAL device name via CLI argument

- Display
  - Add back button to options screen bottom center to return to main menu
  - Added swipe handling for the main menu and app gallery views
  - Add vertical swipe navigation for scrolling of menu items (requires a capacitive touch screen)
  - Added station scanning and the new station options to the wifi options screen
  - Added simple digital clock view
  - Settings menu (with old screen controls as an option)
  - Configurable main menu themes (15 different ones to choose from)
  - Added "Connect to saved WiFi" command
  - Configurable terminal text color
  - Added "List APs" command
  - Added "Invert Colors" option to settings menu

### Changed

- Attacks
  - If station data is available, directly deauth known stations of the AP selected for deauth
  - Deauth task now deauths on each AP's primary channel
  - Station scan now uses discovered AP channels for scanning
  - ESP32C5 shows band in AP scan results

- Display

  - Performance Optimizations

    - Refactored options screen to use lv_list instead of a custom flex container to improve performance
    - Replaced single lv_textarea in terminal view with scrollable lv_page and per-line lv_label children to improve performance
    - Optimize terminal screen by batching text additions

  - UI & UX Adjustments

    - Offset terminal page vertically by status bar height and adjust its height accordingly.
    - Remove index reset in main_menu_create to maintain selection across view switches
    - Default display timeout is now 30 seconds instead of 10
    - Status bar now updates every second instead of when views change
    - Removed rounding on the status bar
    - Changed bootup icon
    - Removed default shadow/border from back buttons
    - Changed option menu item color to be black and white
    - Added text to the splash screen and removed animation

- Commands
  - List stations with sanitized ascii and numeric index
  - Label APs with blank SSID fields as "Hidden"
  - Make congestion command ASCII-only for compatibility
  - Change display EP option to start default EP with a default SSID "FreeWiFi"
  - Update congestion to work with dualband channels

- Power
  - Suspend LVGL, status bar update timer, and misc tasks when backlight is off
  - Use wifi power saving mode if no client is connected
  - Poll touch 5x slower when backlight off
  - Enabled light-sleep idle and frequency scaling

- RGB
  - Refactored rgb_manager_set_color to use is_separate_pins flag instead of compile-time directives

- WebUI
  - Changed color theme to black and white

### Bug Fixes

- General
  - Fixed NVS persistence issues for AP credentials by ensuring a single shared NVS handle and settings instance.
  - Addressed unaligned memory access warning in ICMP ping logic by using an aligned buffer for checksum calculation.

- Display
  - Fixed an issue where an option would be duplicated and freeze the device.
  - Skip first touch event while backlight is dimmed so tap only wakes the screen without registering input
  - Fixed an issue where the numpad would register 2 inputs for a single tap.
  - Fixed screen timeout only resetting on the first wake-up tap
  - Add tap to wake functionality to non battery config models
  - Keep app gallery back button on top of icons

- Power
  - Fixed an issue where the device was reporting that it was not charging when it was.

- RGB
  - Persist RGB pin settings to NVS and auto-init from saved config, closes [jaylikesbunda/Ghost_ESP#5](https://github.com/jaylikesbunda/Ghost_ESP/issues/5)

- GPS
  - Initialize GPS quality data and zero-init wardriving entries to prevent crash in wardriving mode
  - Don't check for csv file before flushing buffer over UART
  - Actually open a CSV file for wardriving when an SD card is present
  - Fix CSV file timestamp to reflect GPS date/time on SD card close
  - Reset GPS timeout flag on initialization

## Revival v1.4.9

### ❤️ New Stuff

- Basic changeable SD Card pin out through webUI and serial command line (requires existing sd support to be enabled in your board's build)
- Added default evil portal html directly in the firmware (credit to @breaching and @bigbrodude6119 for the tiny but great html file)
- Basic congestion command to quickly see channel usage
- Added scanall command to scan aps and stations together

### 🤏 Tweaks and Improvements

- Simplified the evil-portal command line arguments.
  - eg. ```startportal <google.html> (or <default>) <EVILAP> <PSK>```
- Save credentials in flash when using connect command
- Captive portal now supports Android devices
- Simplified the evil-portal command line arguments.
- set LWIP_MAX_SOCKETS to 16 instead of 10
- Save captured evil-portal credentials to SD card if available
- Added support for scanning aps for a specific amount of time eg. ```scanap 10```
- Connect command now uses saved credentials from flash when no arguments are provided
- Added channel hopping to station scan
- Include BSSID in scanap output

### 🐛 Bug Fixes

- Use "GhostNet" as fallback default webUI credentials if G_Settings fields are not set or invalid
- Fix webUI not using evilportal command line arguments
- Fix evil‑portal local file serving 
- Correctly parse station/AP MACs and ignore broadcast/multicast in Station Scan
- Fix station scanning using wrong frame bit fields and offsetting the mac addresses

----------------------

OK, we back. - 22 April 2025

-----------------------

Rest in Peace, GhostESP - 22 April 2025

______________________

## 1.4.7

### ❤️ New Stuff

General:

- Added WebUI "Terminal" for sending commands and receiving logs - @jaylikesbunda

Attacks:

- Added packet rate logging to deauth attacks with 5s intervals - @jaylikesbunda

Lighting:

- Added 'rgbmode' command to control the RGB LEDs directly with support for color and mode args- @jaylikesbunda
- Added new 'strobe' effect for RGB LEDs - @jaylikesbunda
- Added 'setrgbpins' command accessible through serial and webUI to set the RGB LED pins - @jaylikesbunda


### 🐛 Bug Fixes

- Immediate reconfiguration in apcred to bypass NVS dependency issues - @jaylikesbunda
- Disabled wifi_iram_opt for wroom models - @jaylikesbunda
- Fix station scanning not listing anything - @jaylikesbunda
- Connect command now supports SSID and PSK with spaces and special characters - @jaylikesbunda

### 🤏 Tweaks and Improvements

- General:
  - Added extra NVS recovery attempts - @jaylikesbunda
  - Cleaned up callbacks.c to reduce DIRAM usage - @jaylikesbunda
  - Removed some redundant checks to cleanup compiler warnings - @jaylikesbunda
  - Removed a bunch of dupe logs and reworded some - @jaylikesbunda
  - Updated police siren effect to use sine-based easing. - @jaylikesbunda
  - Improved WiFi connection output and connection state management - @jaylikesbunda
  - Optimised the WebUI to be smaller and faster to load - @jaylikesbunda

- Display Specific:
  - Update sdkconfig.CYD2USB2.4Inch_C_Varient config - @Spooks4576
  - Removed main menu icon shadow - @jaylikesbunda
  - Removed both options screen borders - @jaylikesbunda
  - Improved status bar containers - @jaylikesbunda
  - Tweaked terminal scrolling logic to be slightly more efficient - @jaylikesbunda
  - Added Reset AP Credentials as a display option - @jaylikesbunda


## 1.4.6

### ❤️ New Features

- Added Local Network Port Scanning - @Spooks4576
- Added support for New CYD Model (2432S024C) - @Spooks4576
- Added WiFi Pineapple/Evil Twin detection - @jaylikesbunda
- Added 'apcred' command to change or reset GhostNet AP credentials - @jaylikesbunda

### 🐛 Bug Fixes

- Fixed BLE Crash on some devices! - @Spooks4576
- Remove Incorrect PCAP log spam message - @jaylikesbunda
- retry deauth channel switch + vtaskdelays - @jaylikesbunda
- Resolve issues with JC3248W535EN devices #116 - @i-am-shodan, @jaylikesbunda

### 🤏 Tweaks and Improvements

- Overall Log Cleanup - @jaylikesbunda
- Added a IFDEF for Larger Display Buffers On Non ESP32 Devices - @Spooks4576
- Revised 'gpsinfo' logs to be more helpful and consistent - @jaylikesbunda
- Added logs to tell if GPS module is connected correctly- @jaylikesbunda
- Added RGB Pulse for AirTag and Card Skimmer detection - @jaylikesbunda
- Miscellaneous fixes and improvements - @Spooks4576, @jaylikesbunda
- Clang-Format main and include folders for better code readability - @jaylikesbunda

## 1.4.5

### 🛠️ Core Improvements

- Added starting logs to capture commands - @jaylikesbunda
- Improved WiFi connection logic - @jaylikesbunda
- Added support for variable display timeout on TWatch S3 - @jaylikesbunda
- Revise stop command callbacks to be more consistent - @jaylikesbunda, @Spooks4576

### 🌐 Network Features

- Enhanced Deauth Attack with bidirectional frames, proper 802.11 sequencing, and rate limiting (thank you @SpacehuhnTech for amazing reference code) - @jaylikesbunda  
- Added BLE Packet Capture support - @jaylikesbunda  
- Added BLE Wardriving - @jaylikesbunda  
- Added support for detecting and capturing packets from card skimmers - @jaylikesbunda  
- Added "gpsinfo" command to retrieve and display GPS information - @jaylikesbunda

### 🖥️ Interface & UI

- Added more terminal view logs - @jaylikesbunda, @Spooks4576  
- Better access for shared lvgl thread for panels where other work needs to be performed - @i-am-shodan
- Revised the WebUI styling to be more consistent with GhostESP.net - @jaylikesbunda
- Terminal View scrolling improvements - @jaylikesbunda
- Terminal_View_Add_Text queue system for adding text to the terminal view - @jaylikesbunda
- Revise options screen styling - @jaylikesbunda

### 🐛 Bug Fixes

- Fix GhostNet not coming back after stopping beacon - @Spooks4576
- Fixed GPS buffer overflow issue that could cause logging to stop - @jaylikesbunda
- Improved UART buffer handling to prevent task crashes in terminal view - @jaylikesbunda
- Terminal View trunication and cleanup to prevent overflow - @jaylikesbunda
- Fix and revise station scan command - @Spooks4576

### 🔧 Other Improvements

- Pulse LEDs Orange when Flipper is detected - @jaylikesbunda
- Refine DNS handling to more consistently handle redirects - @jaylikesbunda
- Removed Wi-Fi warnings and color codes for cleaner logs - @jaylikesbunda
- Miscellaneous fixes and improvements - @jaylikesbunda, @Spooks4576  
- WebUI fixes for better functionality - @Spooks4576

### 📦 External Updates

- New <https://ghostesp.net> website! - @jaylikesbunda
- Ghost ESP Flipper App v1.1.8 - @jaylikesbunda
- Cleanup README.md - @jaylikesbunda

