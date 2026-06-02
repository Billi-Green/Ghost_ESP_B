# Neon Ghost Asset Pack

Sample source pack for the GhostESP asset pack pipeline. It reuses existing repo PNG artwork and generates SD-ready `.gimg` files.

Build:

```bash
gbt asset pack examples/asset_packs/neon_ghost --out dist --archive
```

Install on SD by copying the generated folder to:

```text
/mnt/ghostesp/themes/neon_ghost/
```

Or copy the generated archive to:

```text
/mnt/ghostesp/themes/neon_ghost.gtheme
```

Then use `Settings > Appearance > Asset Pack` and press left/right to select it, or reboot.
