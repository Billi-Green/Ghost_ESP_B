# GhostESP Native SD Apps v1

Native SD apps are trusted ESP-IDF shared objects loaded from SD with Espressif `elf_loader`.

This v1 ABI is the baseline for the unmerged native-app PR. Rebuild apps against `plugins/sdk/ghostesp_plugin_api.h`; binaries built against earlier draft headers are not supported.

App layout on SD:

```text
/mnt/ghostesp/apps/<app_id>/
  manifest.json
  <entry-from-manifest>.so
```

Manifest v1 example:

```json
{
  "id": "device_inspector",
  "name": "Device Inspector",
  "version": "1.0.0",
  "author": "GhostESP",
  "description": "Inspect firmware status from a native SD app.",
  "category": "System",
  "entry": "ghostesp_device_inspector.so",
  "target": "esp32s3",
  "api_version": 1,
  "manifest_version": 1,
  "package_version": 1,
  "data_version": 1,
  "storage_scope": "app",
  "icon": "assets/icon.rgb565",
  "icon_width": 50,
  "icon_height": 50,
  "icon_format": "rgb565",
  "accent_color": "#56B6F7",
  "permissions": ["ui", "storage", "wifi", "rgb"],
  "memory_limit": 32768,
  "stack_size": 4096,
  "requires_psram": false
}
```

Required fields are `id`, `name`, `entry`, and `api_version`. `target` is strongly recommended and is required when firmware is built with target matching enabled.

Permissions are enforced by the host API. Apps must request the APIs they use: `ui`, `storage`, `commands`, `tasks`, `wifi`, `ble`, `nfc`, `ir`, `subghz`, `badusb`, `raw_gpio`, `lvgl`, and `rgb`.

For native-looking app screens, prefer the stable UI helper API over direct LVGL or internal GhostESP view calls. Apps with `ui` permission can create GhostESP-styled screens, cards, labels, buttons, status text, and popups using opaque `ghostesp_ui_obj_t` handles:

```c
ghostesp_ui_obj_t root = api->ui_screen_create("My App");
api->ui_label_create(root, "Ready");
api->ui_button_create(root, "Run", on_run_clicked, NULL);
api->ui_show_popup("My App", "Done");
```

The stable UI layer intentionally does not expose existing firmware view internals. That keeps apps source-compatible while allowing GhostESP's internal views to change.

Storage defaults to `storage_scope: "app"`, which maps app-relative storage calls to `/mnt/ghostesp/appdata/<app_id>/`. Use `api->app_storage_read/write/list/...` for migratable app data. `storage_scope: "ghostesp"` allows legacy absolute `/mnt/ghostesp/...` storage calls.

Memory limits are advisory for app allocations made through `api->app_malloc`, `api->app_calloc`, and `api->app_free`. Apps can inspect `api->app_memory_used()` and `api->app_memory_limit()`. Raw `malloc/free` remain available for C compatibility, but they are not tracked.

App failure state is stored in `/mnt/ghostesp/appdata/<app_id>/.state.json`. Apps that repeatedly fail to load or do not exit cleanly can be quarantined by firmware and refused by the loader.

Apps export one init function:

```c
const ghostesp_app_t *ghostesp_app_init(const ghostesp_api_t *api);
```

Every app descriptor must set `api_version = GHOSTESP_APP_API_VERSION` and `struct_size = GHOSTESP_APP_STRUCT_SIZE_V1`. Host API structs also include `struct_size`; future v1-compatible additions must be append-only.

Build the example app:

```powershell
cd plugins/examples/device_inspector
idf.py set-target esp32s3
idf.py build
```

Create a new app from the template:

```powershell
python plugins/tools/new_app.py my_tool --name "My Tool"
python plugins/tools/build_app.py plugins/examples/my_tool --target esp32s3
python plugins/tools/package_app.py plugins/examples/my_tool --gapp
```

For raw-folder development, copy `manifest.json` and `build/ghostesp_device_inspector.so` to `/mnt/ghostesp/apps/device_inspector/` on the SD card.

`package_app.py` creates a package folder under `dist/<app_id>/` and can also create a compressed native `.gapp` archive with `--gapp`. Raw folders are still supported for development.

The native `.gapp` format is a simple GhostESP streaming archive, not ZIP: each entry has a safe relative path, compression method, uncompressed checksum, and stored or raw-deflate data. Firmware extracts `.gapp` files into `/mnt/ghostesp/app_cache/<package>/` because Espressif `elf_loader` needs a real `.so` file path for `dlopen()`. The original `.gapp` file stays where the user placed it.

For automatic discovery, copy a `.gapp` file to either `/mnt/ghostesp/packages/` or `/mnt/ghostesp/apps/` on the SD card. The app gallery reload path scans those folders, refreshes the cache when the package size or timestamp changes, creates `/mnt/ghostesp/appdata/<app_id>/`, and loads the app from cache. Removing the source `.gapp` prevents its generated cache from being registered on the next reload.

Dynamic icons use raw RGB565 files for predictable memory use. Set `icon`, `icon_width`, `icon_height`, and `icon_format: "rgb565"`; invalid icons fall back to the default SD app icon.

Build one binary per ESP target. Xtensa and RISC-V app binaries are not interchangeable. Supported by the upstream loader today: `esp32`, `esp32s2`, `esp32s3`, `esp32c6`, `esp32c61`, and `esp32p4`.
