---
title: "Infrared Files"
description: "Organize, rename, and edit GhostESP IR files"
weight: 30
---

## Directory layout

- `/ghostesp/infrared/remotes` holds individual remotes captured on-device.
- `/ghostesp/infrared/universals` stores library files with many commands.
- The Infrared UI reads from `/mnt/ghostesp/infrared/...` when the SD card is mounted.

### Flipper IR libraries

- GhostESP reads the standard Flipper `.ir` format, so you can copy files from community packs.
- A large collection is available at [Lucaslhm/Flipper-IRDB](https://github.com/Lucaslhm/Flipper-IRDB); place downloaded files under `infrared/universals` for universal files or `infrared/remotes`.

## Rename, add, or delete remotes

1. Open a remote in the Infrared view.
2. Use the *Rename Remote*, *Add Signal*, or *Delete Remote* actions at the bottom of the list.
3. Confirm prompts; GhostESP updates the `.ir` file on the SD card.

### Append new signals

- Choose *Add Signal* while a remote is open to append a newly learned button.
- Easy Learn suggests button names; otherwise you will be prompted via the on-screen keyboard.

## Web UI management

- Connect to the GhostNet AP and open the web UI.
- Browse to the file manager and navigate to `/ghostesp/infrared/`.
- Upload `.ir` files to the appropriate folder or download existing ones for backups.

### Tips

- Keep file names short; the UI truncates long names in lists.
- After mass uploads, reload the Infrared view to refresh the cache.
- Back up your IR folder before flashing new firmware or reformatting the SD card.
