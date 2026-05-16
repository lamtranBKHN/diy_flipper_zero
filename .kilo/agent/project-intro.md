# Project Intro (DIY Flipper Zero)

## Build System
- `./fbt` — SCons wrapper from repo root. Load toolchain: `scripts/toolchain/fbtenv.cmd` (Win) or `source scripts/toolchain/fbtenv.sh` (Linux/macOS).
- Auto-updates git submodules (set `FBT_NO_SYNC=1` to skip; `FBT_GIT_SUBMODULE_SHALLOW=1` for CI).

## Target HW
- **STM32WB55CGU6** (DIY board), TARGET_HW=7 (default in `fbt_options.py:10`).
- HAL: `targets/f7/furi_hal/`. CI only builds `f7` matrix target.

## Key Hardware Quirks
- **PN532 NFC** on I2C1 at **0x24 (7-bit)** — NOT 0x48. Flag: `FURI_HAL_NFC_PN532_ONLY` (≠ `PN532_ENABLED`).
  - No listener mode, no RF field control, 270-byte I2C reads.
  - I2C bus reset permanently desyncs PN532 — use retry only, never `furi_hal_i2c_bus_reset()`.
- **PCF8574** I/O expander on I2C1 at 0x20: buttons (P0-P5), vibro (P6), buzzer (P7), INT=PB0.
- **CC1101** sub-GHz on SPI1: CS=PA15, G0=PA1.
- **SD card** on SPI1: CS=PA10.
- **OLED** (SH1106/SSD1306) on I2C1.
- **SPI1 shared**: display + CC1101 + SD → ViewPort lockup warnings (`view_port.c:208`).
- **USART1_RX** remapped to PB3 (PA10 used for SD_CS).
- **I2C3** (PA7/PB4) disabled — pins conflict with other functions.

## Build Commands
| Command | Flags |
|---------|-------|
| Debug | `./fbt` |
| Release | `./fbt TARGET_HW=7 DEBUG=0 COMPACT=1 copro_dist updater_package fap_dist` |
| Flash | `./fbt flash` (SWD) or `./fbt flash_usb_full` (DFU) |
| Unit tests | `./fbt FIRMWARE_APP_SET=unit_tests` |
| Lint | `./fbt lint_all` |
| Format | `./fbt format_all` |

## Key Options
`COMPACT=1`, `DEBUG=0`, `VERBOSE=1`, `FIRMWARE_APP_SET=unit_tests`

## Gotchas
- `./fbt lint_all` checks Python (black), C/C++ lint, image lint.
- API compatibility: `targets/f7/api_symbols.csv` must match OFW release channel.
- PN532 I2C has no HW RST pin → bus desync = fatal. Retry, never bus reset.
- Excessive debug logs (`log trace`) spam console. Use `log level info`.

## Storage Optimization TODOs
- `.mainmenu_apps.txt` re-read every menu open (`loader_menu.c:308`).
- FAP metadata re-read on every browser listing (`archive_browser.c:448-470`).
- SPI contention: shorter transactions, release bus fast after CC1101/SD.
