# gbt — Ghost Build Tool

Build and package [GhostESP](https://github.com/GhostESP-Revival/GhostESP) native SD apps from the command line.

## Install

```bash
pip install ghostbt
```

Requires Python 3.8+ and [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) (install via `gbt setup`).

## Quick Start

```bash
# Set up ESP-IDF (one-time)
gbt setup

# Create a new app
gbt create my_app --name "My App"

# Build and package
gbt dist ./my_app --gapp

# Flash firmware to device
gbt firmware cardputer
gbt flash firmware --board cardputer --monitor
```

## Commands

| Command | Description |
|---------|-------------|
| `gbt create <id>` | Scaffold a new native SD app |
| `gbt build [dir]` | Build app with ESP-IDF |
| `gbt package [dir] --gapp` | Package as folder or `.gapp` archive |
| `gbt dist [dir] --gapp` | Build + package in one step |
| `gbt setup` | Install/configure ESP-IDF toolchain |
| `gbt boards` | List available firmware board configs |
| `gbt firmware <board>` | Build GhostESP firmware |
| `gbt flash firmware` | Flash firmware to device |
| `gbt flash app` | Instructions for loading app via SD card |
| `gbt monitor` | Serial monitor |
| `gbt ports` | List serial ports |

## Requirements

- Python 3.8 or later
- ESP-IDF (auto-installed by `gbt setup` if missing)
- Git (for `gbt setup`)

## Links

- [GhostESP GitHub](https://github.com/GhostESP-Revival/GhostESP)
- [Full documentation](https://github.com/GhostESP-Revival/GhostESP/tree/apps/docs/hugo%20docs/content/latest/development/gbt.md)
