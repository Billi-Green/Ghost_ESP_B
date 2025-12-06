---
title: "Screen Mirroring"
description: "Mirror your GhostESP display to your desktop for easier viewing and remote control."
weight: 35
---

Screen Mirroring lets you view your GhostESP device's display on your computer in real-time. You can also control the device using your keyboard or on-screen buttons, making it easier to navigate menus and operate GhostESP from your desk.

## Prerequisites

- A GhostESP device with a display (CYD, T-Deck, Cardputer, etc.)
- Python 3.8 or newer installed on your computer
- USB cable to connect the device

## Installation

The screen mirror script is located in `scripts/screen_mirror/`. Required packages (pygame, pyserial, numpy) are automatically installed on first run.

## Starting the Mirror

1. Connect your GhostESP device via USB
2. Open a terminal in the `scripts/screen_mirror` folder
3. Run the script:

```
python ghost_mirror.py
```

Or specify a port directly:

```
python ghost_mirror.py COM3
```

To list available serial ports:

```
python ghost_mirror.py --list
```

## Using the Mirror

### Window Controls

- **Title bar**: Drag to move the window
- **× button**: Close the application

### Port Selection

Use the **◄** and **►** buttons in the header to cycle through available COM ports. The current port is displayed between the buttons.

### Connection

- **Connect**: Opens the serial connection and enables mirroring
- **Disconnect**: Closes the connection cleanly

### Display Controls

The virtual D-pad on the right side mirrors the physical controls on your device:

| Button | Action |
|--------|--------|
| ▲ | Navigate up |
| ▼ | Navigate down |
| ◄ | Navigate left / Go back |
| ► | Navigate right |
| ● | Select / Confirm |

### Keyboard Shortcuts

| Key | Action |
|-----|--------|
| W / ↑ | Up |
| S / ↓ | Down |
| A / ← | Left |
| D / → | Right |
| Enter / Space | Select |
| Escape | Exit |

### Byte Swap Toggle

If colors appear wrong (inverted or incorrect), click the **Swap** button to toggle byte order. This forces a full screen refresh.

## Command Line Options

| Option | Description |
|--------|-------------|
| `--scale N` | Scale the display by factor N (default: 2) |
| `--baud N` | Set baud rate (default: 115200) |
| `--list` | List available serial ports |

### Examples

```
# Run with 3x scaling
python ghost_mirror.py COM3 --scale 3

# Use a different baud rate
python ghost_mirror.py COM3 --baud 921600
```

## Status Indicators

- **Green dot**: Connected and receiving data
- **Red dot**: Disconnected or connection lost
- **FPS counter**: Shows current frame rate
- **Resolution**: Displays current screen dimensions

## Troubleshooting

### No display or black screen

- Ensure the device is powered on and showing content on its physical display
- Try clicking **Connect** to re-establish the connection
- Check that the correct COM port is selected

### Wrong colors

- Click the **Swap** button to toggle byte order
- The display will refresh with corrected colors

### Connection lost

- The status indicator will turn red
- Click **Connect** to reconnect
- If the device was reset, wait for it to boot before reconnecting

### Device resets when connecting

- This is normal for some boards on first connection
- The script disables DTR/RTS to prevent resets after initial connection

### Slow or laggy display

- Screen mirroring uses USB serial which has bandwidth limitations
- Reduce scale factor if needed
- Close other applications using the serial port

## Notes

- Screen mirroring sends display data over USB serial at 115200 baud by default
- The mirror only shows content when the device's display is updating
- Input commands are sent as text commands over the same serial connection
- Works with any GhostESP device that has a display

