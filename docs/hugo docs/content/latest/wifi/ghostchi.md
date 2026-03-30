---
title: "Ghostchi"
description: "Run Ghostchi sessions and find the files it saves on the SD card."
weight: 35
---

Ghostchi is an automated Wi-Fi hunting mode that sweeps for viable WPA targets, listens for EAPOL activity, and stores its own session artifacts on the SD card.

> **Legal note**: Only capture traffic from networks you own or have explicit permission to test. Unauthorized network testing is illegal in most jurisdictions.

## What Ghostchi saves

- One PCAP capture per Ghostchi session under `/mnt/ghostesp/ghostchi/pcaps/`.
- One text session log per run under `/mnt/ghostesp/ghostchi/sessions/`.
- Persistent learning data in `/mnt/ghostesp/ghostchi/learn.bin`.
- Persistent session state in `/mnt/ghostesp/ghostchi/state.bin`.

Ghostchi does not save captured EAPOL data to a CSV. Packet data is written to `.pcap` files for review in Wireshark or other packet tools.

## File naming

- PCAP files use names like `ghostchi_1.pcap`, `ghostchi_2.pcap`, and so on.
- Session logs use names like `ghostchi_20260330_142355.log` when the RTC time is available.

Each Ghostchi run keeps appending to its own PCAP file for the duration of that session, then flushes and closes the file when the session stops.

## How it differs from `capture -eapol`

- `capture -eapol` saves captures in the shared `/mnt/ghostesp/pcaps/` directory.
- Ghostchi saves its captures in `/mnt/ghostesp/ghostchi/pcaps/` so automated runs stay separate from manual captures.
- Both capture flows record Wi-Fi packet data as PCAP rather than CSV.

## Using Ghostchi

### On-device UI
1. Open **Menu -> WiFi -> Ghostchi**.
2. Insert and mount an SD card if Ghostchi reports that storage is required.
3. Start a session and leave the device running while it sweeps, listens, and reacts to nearby targets.
4. Stop Ghostchi from the same screen when you are done.
5. Copy the saved `.pcap` and `.log` files from the SD card.

## Pages

Use **Left/Right** buttons to cycle through three information pages:

### Page 1: Current State
Shows real-time hunting status:
- **MODE**: Active, Standby, or Blocked
- **CH**: Current channel being scanned
- **APS**: Number of access points visible
- **TARGET**: Current target SSID (if locked)
- **PWND**: Total handshakes captured
- **CONF**: Confidence percentage for current target

### Page 2: System Stats
Shows device resource information:
- **HEAP**: Free heap memory (KB)
- **IRAM**: Free internal RAM (KB)
- **TRIES**: Total capture attempts
- **MISSES**: Failed attempts
- **IDLE**: Time since last session
- **SESS**: Total sessions run

### Page 3: Character Passport
A character stats screen showing your unique Ghostchi:
- **Name**: Unique name generated from device MAC address (e.g., "kivexa", "mepule")
- **Mood**: Current emotional state based on activity
- **Level**: Progress level (1-3, increases with captures)
- **XP Bar**: Visual progress toward next level
- **Portrait**: Animated ghost sprite that reflects current state

## Unique Names

Each device generates a unique name for its Ghostchi character based on the WiFi MAC address. Names follow a pronounceable CVCVC+ending pattern (consonant-vowel-consonant-vowel-consonant-ending), producing names like:

- "bemira", "kivexa", "lofenu", "tafuri", "zasin", "wepulo"

The name is deterministic - the same MAC address will always produce the same name.

## Moods

The Ghostchi displays different moods based on its state:

| Mood | Condition |
|------|-----------|
| eager | No sessions run yet |
| hopeful | Has run sessions but no captures |
| proud | Has captured handshakes |
| drowsy | Idle for8+ hours |
| restless | Idle for 24+hours |
| thriving | Recently successful |
| hunting | Currently running |
| blocked | SD card not available |

## Dialogue System

Ghostchi displays speech bubbles with contextual messages that vary based on:

- Current state (sweeping, locked onto target, capturing, cooldown)
- Battery status (warnings when low)
- Nearby aerial devices (drones detected)
- GPS fix status
- Time since last session
- Success/failure of recent attempts

Messages cycle every few seconds with multiple variations per state for variety. Running states show progress messages like "scanning.", "waiting on a packet.", or "logged it." Idle states reflect the character's personality with messages like "probably clear.", "up to you.", or "run me already."

## Verify

- Confirm `/mnt/ghostesp/ghostchi/pcaps/` contains a new `.pcap` after a session.
- Confirm `/mnt/ghostesp/ghostchi/sessions/` contains a matching `.log` file.
- Open the `.pcap` in Wireshark to inspect EAPOL, association, and related Wi-Fi frames.

## Troubleshooting

- **Ghostchi says SD required**: Mount the SD card first and make sure `/mnt/ghostesp/` is available.
- **No PCAP created**: Stop Ghostchi cleanly so it can flush and close the capture file.
- **No useful packets inside**: Ghostchi still needs nearby WPA traffic. Move closer to the target or wait for a client to reconnect.