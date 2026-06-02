---
title: "Asset Packs"
description: "Customize GhostESP icons, colors, and backgrounds with asset packs"
weight: 10
---

Asset packs let you customize the GhostESP UI: colors, icons, and background images. Pack everything into a single `.gtheme` archive or an extracted folder on your SD card.

## What You Can Customize

| Feature | PSRAM Devices | Internal-Only Devices |
|---------|--------------|----------------------|
| Colors (accent, background, surface, text) | Yes | Yes |
| Icons (up to 32 cached) | Yes | 2 cached |
| Background images (tiled or full-screen) | Yes | No |
| Custom icon sizes per resolution | Yes | Yes |

## Creating a Pack

### 1. Source Folder Structure

Create a folder with a `manifest.json` and your PNG artwork:

```
my_pack/
  manifest.json
  art/
    icons/
      wifi.png
      bluetooth.png
    bg_tile.png
```

### 2. Manifest Format

```json
{
  "id": "my_pack",
  "name": "My Pack",
  "version": 1,
  "app_icon": "GESPAppGallery",
  "colors": {
    "accent": "0x39FF14",
    "background": "0x050008",
    "surface": "0x120018",
    "surface_alt": "0x24102A",
    "text": "0xFFFFFF",
    "text_muted": "0x9A80AA"
  },
  "icon_variants": [32, 64],
  "icon_sources": {
    "wifi": "art/icons/wifi.png",
    "bluetooth": "art/icons/bluetooth.png",
    "Map": "art/icons/map.png",
    "settings_icon": "art/icons/settings.png",
    "clock_icon": "art/icons/clock.png",
    "ghost": "art/icons/ghost.png",
    "terminal_icon": "art/icons/terminal.png",
    "rave": "art/icons/rave.png",
    "GESPAppGallery": "art/icons/gallery.png"
  },
  "background_sources": {
    "bg_tile": {
      "source": "art/bg_tile.png",
      "width": 64,
      "height": 64,
      "format": "rgb565"
    }
  }
}
```

**Color values** are hex RGB without alpha. Supported keys: `accent`, `background`, `surface`, `surface_alt`, `text`, `text_muted`.

**Icon variants** define pixel sizes. The firmware picks the best fit for the screen. Small screens use the smaller variant, large screens use the larger one.

**`app_icon`** is optional. When present, App Gallery entries without a matching app-specific icon use that asset-pack icon. Leave it out if you only want exact icon-key replacements.

**Supported icon keys** match the main menu and built-in app gallery:

| Key | Location |
|-----|----------|
| `wifi` | Main menu |
| `bluetooth` | Main menu |
| `Map` | Main menu |
| `clock_icon` | Main menu |
| `settings_icon` | Main menu |
| `GESPAppGallery` | Main menu / App gallery |
| `lock` | Main menu |
| `dualcomm` | Main menu |
| `lan_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48` | Main menu |
| `infrared`, `nfc_icon`, `nrf24`, `subghz`, `usb` | Optional hardware menus |
| `compass`, `enviii`, `accelerometer_icon` | Optional sensor menus |
| `ghost` | App gallery |
| `terminal_icon` | App gallery |
| `rave` | App gallery |
| `speaker_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48` | Audio app |

**Background images** use `rgb565` format. Images smaller than the screen tile automatically. Images equal to or larger than the screen scale to fill using LVGL zoom. A 128x128 tile will scale up to fill any screen size. Background images require PSRAM and are used by the main menu, app gallery, settings-style screens, and other shared-layout views.

### 3. Building

```bash
# Build an extracted folder
gbt asset pack ./my_pack --out ./dist

# Build a .gtheme archive
gbt asset pack ./my_pack --out ./dist --archive
```

Source PNGs must be 8-bit RGBA. The converter handles resizing to the target dimensions.

## Installing

### Option A: Extracted Folder

Copy the generated folder to your SD card:

```
/mnt/ghostesp/themes/my_pack/
  manifest.json
  icons/
    wifi_l.gimg
    wifi_s.gimg
    ...
  bg_tile.gimg
```

### Option B: .gtheme Archive

Copy the archive to:

```
/mnt/ghostesp/themes/my_pack.gtheme
```

The firmware extracts it on first load. Subsequent boots reuse the extracted files if the archive hasn't changed.

## Selecting a Pack

1. Insert SD card with your pack
2. Go to **Settings > Appearance > Asset Pack**
3. Press **left/right** to cycle through installed packs
4. The pack loads immediately and the selection is saved

Missing icons fall back to the compiled-in defaults. You don't need to include every icon.

## Image Formats

| Format | Code | Description |
|--------|------|-------------|
| `rgb565` | 1 | 16-bit color, no alpha. Used for backgrounds. |
| `rgb565a8` | 2 | 16-bit color + 8-bit alpha plane. Used for icons. |

The `rgb565a8` format stores RGB565 and alpha as separate planes (not interleaved). The `gbt asset pack` tool handles this automatically. GIMG files store standard RGB565; firmware adapts the in-memory byte order for boards built with `LV_COLOR_16_SWAP`.

## PSRAM vs Internal RAM

**PSRAM devices** (most ESP32-S3 boards):
- Up to 32 icons cached in PSRAM
- Background images supported (tiled or full-screen)
- Each 64x64 icon uses ~12 KB PSRAM
- Full-screen 240x320 background uses ~150 KB PSRAM

**Internal-only devices** (ESP32-C5, some S2):
- 2 icons cached in internal RAM
- No background images
- Colors and icon mappings still work
- Icons fall back to compiled-in artwork when cache is full

## Converting a Single Image

```bash
gbt asset image ./icon.png --out ./icon.gimg --width 64 --height 64 --format rgb565a8
```

## Example Pack

See `examples/asset_packs/neon_ghost/` in the repository for a working sample pack.
