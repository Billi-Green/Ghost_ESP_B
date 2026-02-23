---
title: "WiGLE Upload"
description: "Configure WiGLE API integration for uploading wardriving data"
weight: 10
---

Upload your wardriving CSV captures to [WiGLE.net](https://wigle.net) to contribute to the global wireless network mapping project.

## Prerequisites

- GhostESP device with GPS module connected
- SD card mounted for storing CSV captures
- Wi-Fi connection (for uploading)
- WiGLE account with API credentials

## Getting WiGLE API Credentials

1. Create a free account at [wigle.net](https://wigle.net)
2. Go to your [account page](https://wigle.net/account)
3. Find the "API" section and copy your **API Name** and **API Token**
4. Your credentials are in the format: `APIName:APIToken`

## Configuration

### On-device Display

1. Open **Menu → Settings → Wigle**
2. Toggle **Auto Upload** to enable automatic uploads when WiFi connects
3. Toggle **Donate Data** to share your scans with WiGLE (recommended)

### Command Line

Set your API key:
```
wigle API <APIName>:<APIToken>
```

Enable auto-upload at boot:
```
wigle auto on
```

Enable data donation:
```
wigle donate on
```

View current settings:
```
wigle show
```

## CLI Commands

| Command | Description |
|---------|-------------|
| `wigle API <name>:<token>` | Set your WiGLE API credentials |
| `wigle auto on/off` | Enable/disable auto-upload at boot |
| `wigle donate on/off` | Enable/disable data donation |
| `wigle show` | Display current settings |
| `wigle list` | List previously uploaded files |
| `wigle upload` | Manually trigger upload |

## Auto Upload

When **Auto Upload** is enabled, GhostESP will automatically upload any pending CSV files when:

1. The device connects to Wi-Fi (STA mode)
2. An API key is configured
3. There are CSV files that haven't been uploaded yet

Files are tracked in a queue to prevent duplicate uploads.

## Manual Upload

To manually trigger an upload:
```
wigle upload
```

This will process all pending CSV files and upload them to WiGLE.

## Data Donation

When **Donate Data** is enabled (default), your uploads contribute to WiGLE's public database. This helps:

- Map wireless networks globally
- Research wireless security trends
- Improve coverage in undermapped areas

You can disable this if you prefer private uploads (WiGLE Pro feature).

## CSV Format

GhostESP generates WiGLE-compatible CSV files in the standard format:

```
WigleWifi-1.6,appRelease=2.0.0,model=GhostESP,release=2.0.0,device=GhostESP,display=LCD,board=ESP32,brand=GhostESP
MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type
```

Files are saved to `/mnt/ghostesp/gps/` on your SD card.

## Troubleshooting

- **"no API key set"**: Run `wigle API <name>:<token>` to configure credentials
- **Upload fails**: Ensure device is connected to Wi-Fi and has internet access
- **Files not uploading**: Check that CSV files exist in `/mnt/ghostesp/gps/`
- **Duplicate uploads**: The upload queue tracks files by name and size to prevent duplicates

## Notes

- Uploads require an active Wi-Fi connection (not AP mode)
- Large uploads may take time depending on file size and connection speed
- Upload progress is shown in the terminal/logs
- Files remain on SD card after successful upload
