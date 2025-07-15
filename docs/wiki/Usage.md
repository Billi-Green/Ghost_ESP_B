# Touch Screen Usage

GhostESP firmware includes touch support, enabling users to interact directly with on-screen elements. Here’s how to navigate the different menus and screens effectively.

## Touch Input Zones

Touch functionality in GhostESP is divided into specific zones for different menu types. This layout optimizes navigation, especially on screens without a dedicated touchscreen controller or with simpler touch capabilities.

### Main Menu Navigation

In the main menu, simply tap on an item directly to select it.

### Scroll Menus (Wi-Fi, Bluetooth)

For scrollable menus like the Wi-Fi and Bluetooth menus, the screen is divided into three sections to navigate more easily:

- **Top Half of Screen**: Tap here to move up one item in the menu.
- **Bottom Half of Screen**: Tap here to move down one item in the menu.
- **Middle of Screen**: Tap here to select the currently highlighted item.

You can also use the on-screen scroll buttons (if available) to move quickly through long lists.

### Submenus and Actions

- When you select a menu item that opens a submenu (such as "Attacks", "Capture", or "Start Custom Evil Portal"), the new menu will appear. Tap as above to navigate.
- For actions that require text input (such as entering an SSID for Evil Portal), a keyboard screen will appear. Tap the keys to enter text, then tap "Done" to submit.

### Full-Screen Menus Without Navigation

For full-screen menus without scrollable or selectable items, tapping anywhere on the screen will return to the previous menu.

### Status Bar

A status bar at the top of the screen shows the current menu, SD card status, Wi-Fi, Bluetooth, and battery status (if supported).

### Terminal and Output

When you launch commands (such as attacks, scans, or Evil Portal), the terminal view will show output and logs. Tap the screen to return to the previous menu when finished.

> **Note**: If you’re experiencing issues with touch responsiveness, make sure you have the latest firmware, as updates may improve touch functionality.

---

## Quick Tips

- **Start Evil Portal**:  
  Tap "Wi-Fi" → "Evil Portal" → "Start Evil Portal" to launch the default captive portal.
- **Start Custom Evil Portal**:  
  Tap "Wi-Fi" → "Evil Portal" → "Start Custom Evil Portal", select your HTML file, then enter the SSID when prompted.
- **Stop Evil Portal**:  
  Tap "Wi-Fi" → "Evil Portal" → "Stop Evil Portal".
- **Scrolling**:  
  Use the top/bottom screen zones or scroll buttons for long lists.
- **Back Navigation**:  
  Tap the on-screen back button (if present) or use the hardware back button to return to the previous menu.

---

For a full list of features and commands, see the [Features](Features.md) and [Commands](Commands.md) guides.
