# SD Card Contents

## Where are the SD card files?

The SD card contents for the Flipper Zero are stored in the **`resources`** folder at the root of this repository.

```
diy_flipper_zero/
├── resources/              ← SD card files are here!
│   ├── apps/              # External applications
│   ├── apps_data/         # Application data files
│   ├── asset_packs/       # Asset packs
│   ├── badusb/            # Bad USB scripts
│   ├── dolphin/           # Dolphin animations
│   ├── ibutton_fuzzer/    # iButton fuzzer files
│   ├── infrared/          # Infrared remote files
│   ├── lfrfid_fuzzer/     # LFRFID fuzzer files
│   ├── mifare_fuzzer/     # Mifare fuzzer files
│   ├── nfc/               # NFC files
│   ├── subghz/            # Sub-GHz files
│   ├── u2f/               # U2F files
│   └── wav_player/        # Audio files
```

## How to build/deploy resources

The resources are automatically packaged when you build the firmware update package:

- **Full update package** (includes firmware + resources):
  ```bash
  ./fbt updater_package
  ```

- **Build only resources**:
  ```bash
  ./fbt resources
  ```

- **Build dolphin animations for SD card**:
  ```bash
  ./fbt dolphin_ext
  ```

## Where do resources go on the device?

When flashed to the Flipper Zero device, these resources are stored on the microSD card at the root level (accessed as `/ext/` in the firmware).

For example:
- `resources/infrared/` → `/ext/infrared/` on the SD card
- `resources/subghz/` → `/ext/subghz/` on the SD card
- `resources/apps/` → `/ext/apps/` on the SD card

## Related documentation

- [Apps On SD Card](AppsOnSDCard.md) - Information about FAP (Flipper App Package) files
- [FBT Build Tool](fbt.md) - Build system documentation including resource targets
- [OTA Updates](OTA.md) - Over-the-air update information including resource packages
