# GhostESP Native SD Apps

Native apps are trusted ESP-IDF shared objects loaded from SD with Espressif `elf_loader`.

App layout on SD:

```text
/mnt/ghostesp/apps/<app_id>/
  manifest.json
  <entry-from-manifest>.so
```

Build the example app:

```powershell
cd plugins/examples/device_inspector
idf.py set-target esp32s3
idf.py build
```

Then copy `manifest.json` and `build/ghostesp_device_inspector.so` to `/mnt/ghostesp/apps/device_inspector/` on the SD card.

Build one binary per ESP target. Xtensa and RISC-V app binaries are not interchangeable.
The manifest `target` must match the firmware target, for example `esp32s3` for LilyGo TEmbedC1101.

Supported by the upstream loader today: `esp32`, `esp32s2`, `esp32s3`, `esp32c6`, `esp32c61`, and `esp32p4`.

Unsafe apps must set `"unsafe": true` in the manifest and require firmware built with `CONFIG_NATIVE_SD_APPS_UNSAFE_MODE`.
