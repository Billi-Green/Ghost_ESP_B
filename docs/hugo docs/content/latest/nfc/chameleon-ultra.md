---
title: "Chameleon Ultra"
description: "Connect GhostESP to a Chameleon Ultra reader"
weight: 5
---

## Overview

GhostESP can control a Chameleon Ultra over Bluetooth Low Energy. Once paired you can reuse the same NFC scan, save, and dump flows as if you had a PN532 module attached.

## Prerequisites

- **Firmware:** GhostESP build with BLE enabled and the Chameleon Ultra features compiled in.
- **Hardware:** A powered Chameleon Ultra advertising over BLE and charged enough for the session.

## Connecting to Chameleon Ultra

> **Note:** GhostESP automatically suspends the access point and Wi-Fi services during the connection to free memory for BLE operations. Services restore when you disconnect.

### On-Device UI

1. Open **Main Menu → NFC → Chameleon Ultra → Connect**.
   The device scans for nearby Chameleon Ultra devices.
2. Wait for the connection popup to show "Connected".
   Battery voltage and percentage appear below the status.
3. Use **Disconnect** from the same menu when finished.
   Wi-Fi services resume automatically after disconnecting.

### Command Line

1. Run `chameleon connect` to start pairing.
   Optional arguments:
   - `timeout` in seconds (default 10) if you need more time for discovery.
   - `pin` if your Chameleon Ultra requires a 4–6 digit security code.
   Example: `chameleon connect 15 1234`
2. Wait for pairing confirmation in the terminal.
   The CLI announces when the link succeeds.
3. Check status with `chameleon status` or battery with `chameleon battery`.
   Voltage and percentage display before starting long dumps.
4. Run `chameleon disconnect` when finished.
   BLE releases and the Wi-Fi services will restore automatically.

## Using the Chameleon Ultra

### Scanning Tags

- **Switch to reader mode:** Run `chameleon reader` in the CLI or select **Reader Mode** in the UI.
- **Scan HF tags:** Use `chameleon scanhf` in the CLI while holding the tag near the antenna. The terminal shows UID, ATQA, SAK, and brute-force progress for MIFARE Classic cards.
- **View details:** Results appear in both the CLI and the on-device terminal view with the same formatting as PN532 scans.

See [Scanning with Chameleon Ultra]({{< relref "scanning.md#chameleon-ultra-scanning" >}}) for complete workflow details.

### Saving Dumps

- **Save HF scans:** After `chameleon scanhf` completes, run `chameleon savehf <name>` to write the dump to `/mnt/ghostesp/chameleon/`.
- **Name files clearly:** Use short descriptive names without spaces, for example `office_door`.
- **Skip the UI:** Chameleon Ultra dumps cache in RAM immediately; no need to reopen the PN532 scan popup.

See [Saving from Chameleon Ultra]({{< relref "saving.md#chameleon-ultra-saves" >}}) for storage details.

## Troubleshooting

- **Connection timeouts:** Keep the Chameleon Ultra awake (press any button) and re-run `chameleon connect 20` for a longer window.
- **PIN failures:** Verify the configured code on the Chameleon Ultra; three failed attempts may force a power cycle.
- **No BLE advertisements:** Check the Chameleon Ultra’s BLE settings or reboot it; GhostESP only connects to active broadcasts.

## Next Steps

- **Scanning:** Continue with [PN532-based scanning guide]({{< relref "scanning.md" >}}).
- **Saving:** Store remote dumps using [Saving Tags]({{< relref "saving.md" >}}).
- **Compatibility:** Review what tag families work via [Supported Tags]({{< relref "supported.md" >}}).
