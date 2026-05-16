## Build System

- `./fbt` must run from the repo root; it forwards to SCons.
- Load toolchain: `scripts\toolchain\fbtenv.cmd` (Windows) or `source scripts/toolchain/fbtenv.sh` (Linux/macOS).
- Shortcut: `./fbt -s env` loads the toolchain environment and dumps variables.

## Hardware Target

- **This is a DIY board** with STM32WB55CGU6, not the original Flipper Zero (STM32F7).
- Default target is `f7` (`TARGET_HW=7`), set in `fbt_options.py:10`.
- CI only builds `f7` matrix target (`.github/workflows/build.yml:29`).
- Target-specific HAL lives in `targets/f7/furi_hal/`.

## Build Commands

- `./fbt` – debug firmware, no external apps
- `./fbt TARGET_HW=7 DEBUG=0 COMPACT=1 copro_dist updater_package fap_dist` – release build (matches CI)
- `./fbt flash` – flash via ST-Link/Black Magic probe
- `./fbt flash_usb_full` – build + flash over USB (device must be in DFU mode)
- `./fbt updater_package` – OTA update bundle
- `./fbt copro_dist` – package Core2 BLE radio stack (`stm32wb5x_BLE_Stack_light_fw.bin`)

## App Development

- Build external app: `./fbt fap_{APPID}` (e.g. `./fbt fap_qrcode`)
- Build all apps: `./fbt faps` or `./fbt fap_dist`
- Build + launch on device: `./fbt launch APPSRC=applications_user/<path>`
- `APPSRC` path is relative to `applications_user/`

## Lint / Format

- `./fbt lint_all` – Python (`black --check`), C/C++ lint, image lint
- `./fbt format_all` – `black`, `clang-format`, image formatting
- Individual: `lint_py`, `format_py`, `lint_img`, `format_img`

## Debugging

- `./fbt debug` – GDB via OpenOCD (default probe)
- `./fbt blackmagic` – GDB via Black Magic probe
- `./fbt debug_other` – attach GDB to arbitrary ELF
- OpenOCD config in `fbt_options.py:74-83` (CMSIS-DAP, SWD, stm32wbx)

## Key Options

- `FIRMWARE_APP_SET=unit_tests` – build with unit tests
- `COMPACT=1` – optimize for size
- `DEBUG=0` – strip debug symbols
- `FBT_NO_SYNC=1` – skip submodule updates
- `VERBOSE=1` – verbose SCons output

## Hardware Quirks (DIY Board)

- **PN532 NFC** via I2C1 at `0x48` (7-bit) or `0x90/0x91` (8-bit write/read), enabled by `PN532_ENABLED` in `targets/f7/boards/custom_pn532_board.mk`
- **PCF8574** I/O expander at `0x20` on I2C1: buttons (P0-P5), vibro (P6), buzzer (P7), INT on PB0
- **CC1101** sub-GHz on SPI1: CS=PA15, G0=PA1
- **SD card** on SPI1: CS=PA10
- **OLED** (SH1106/SSD1306) on I2C1
- I2C3 (PA7/PB4) is **disabled** – pins used for other functions
- Pin macros: `targets/f7/furi_hal/furi_hal_resources.h`

### PN532 NFC Limitations

`FURI_HAL_NFC_PN532_ONLY` flag in `furi_hal_nfc.c` gates PN532-only paths:
- **No listener mode**: `set_mode(FuriHalNfcModeListener)` returns error. All `furi_hal_nfc_listener_*()` return error or timeout.
- **No RF field control**: `poller_field_on/off()` are no-ops (PN532 manages field internally). `field_detect_start/stop` are no-ops, `field_is_present` always returns `true`.
- **Protocol limited**: Scanner offers only ISO14443-3A, ISO14443-3B, FeliCa (PN532 native). ISO15693, ST25TB, etc. poller_init returns error.
- **I2C has no HW RST pin**: bus desync fatal → use retry logic, not full bus reset.
- **270-byte read on every I2C response**: performance issue, unused trailing bytes.

### NFC Bugs Fixed (2026-05-11)

6 bugs identified and fixed; see individual files for details:

1. **Mutex race** (4 wrapper funcs in `furi_hal_nfc.c`): `poller_tx/rx` + `listener_tx/rx` acquired/released mutex, then tech func re-acquired → race window. Fixed: removed acquire/release from wrappers (tech funcs own their locking).
2. **PN532 missing mutex** (`furi_hal_nfc_pn532.c`): `exchange_internal()` and `trx_short_frame()` lacked acquire/release. Fixed: added `furi_hal_nfc_acquire()`/`release()` with `goto release` cleanup pattern.
3. **Scanner filtered to ISO14443A only** (`nfc_scanner.c`): PN532 branch offered only protocol A. Fixed: now offers A, B, FeliCa (B+FeliCa will silently fail with "not present" via `poller_tx_common` returning `Communication`).
4. **Listener set_col_res_data ST25R3916 access** (`furi_hal_nfc_iso14443a.c`): no PN532 guard. Fixed: wrapped in `#ifndef FURI_HAL_NFC_PN532_ONLY`.
5. **low_power_mode_stop missing furi_check** (`furi_hal_nfc.c`): called `acquire()` without `furi_check(instance)`. Fixed: added `furi_check(instance)`.
6. **270-byte read inefficiency** (`furi_hal_nfc_pn532.c`): I2C read always requests full buffer. Diagnosed, minor perf impact only.

## Gotchas

- `./fbt` auto-updates git submodules on first run (set `FBT_NO_SYNC=1` to skip)
- `flash_usb_*` requires DFU mode + correct USB driver (Zadig on Windows)
- `fbt` aliases defined in `SConstruct`, not separate scripts
- API consistency checked in CI: `targets/f7/api_symbols.csv` must match OFW release channel
- **Unit tests build** (`FIRMWARE_APP_SET=unit_tests`) had pre-existing link errors for JS symbols (`js_thread_run`, `js_thread_stop`, `js_value_buffer_size`, `js_value_parse`). Fixed by removing JS entries from `unit_test_api_table_i.h` — JS app is compiled as FAP plugin, not linked into firmware ELF, so API table references were never resolvable. Build now passes cleanly.

## Performance Notes

- Animation cache (LRU, 2 slots) in `animation_storage.c` avoids re-reading SD frames on each animation switch
- ViewPort lockup warnings at `view_port.c:208` are expected on DIY board due to SPI1 bus sharing (display + CC1101 + SD)
- Excessive debug logs (`log trace`) will spam console - use `log level info` to reduce output

## Storage Optimization TODOs

- **`.mainmenu_apps.txt` cache** - Currently re-read from SD every menu open (`loader_menu.c:308`). Add timestamp-based cache to avoid re-parsing.
- **FAP metadata cache** - Each `.fap` file is opened during browser listing (`archive_browser.c:448-470`). Cache name/icon lookups.
- **SPI contention** - SPI1 shared by display (PB6), CC1101 (PA15), SD (PA10). ViewPort lockups occur when SPI mutex held too long. Consider:
  - Shorter SPI transactions
  - Release SPI bus quickly after CC1101/SD operations  
  - Avoid SPI I/O in draw callbacks
