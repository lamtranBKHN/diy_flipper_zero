# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build System

This is a Flipper Zero firmware project using **fbt** (Flipper Build Tool), a wrapper around SCons.

### Common Commands

```bash
# Build firmware (default target)
./fbt

# Build firmware and create distribution package
./fbt fw_dist

# Build external apps (.fap files)
./fbt fap_dist

# Build single app by appid
./fbt fap_nfc

# Flash firmware over SWD (requires probe)
./fbt flash

# Flash firmware over USB (self-update)
./fbt flash_usb_full

# Run unit tests
./fbt FIRMWARE_APP_SET=unit_tests

# Generate compilation database for IDE
./fbt firmware_cdb

# Lint code
./fbt lint
./fbt format

# Open CLI session over USB
./fbt cli
```

### Build Configuration

- `fbt_options.py` - default configuration
- `fbt_options_local.py` - local overrides (not tracked)
- `FIRMWARE_APP_SET` - select app preset (e.g., `unit_tests`)
- `TARGET_HW` - build for specific hardware target (default: f7)
- `COMPACT=1` - optimized build
- `DEBUG=1` - debug build with symbols

Build outputs go to `build/` with `build/latest` symlink to most recent build.

## Architecture

### Layer Structure

1. **furi/** - Core OS primitives (threads, events, memory, logging)
2. **lib/** - Shared libraries (nfc, subghz, infrared, mbedtls, etc.)
3. **targets/f7/furi_hal/** - Hardware abstraction layer for STM32WB55
4. **applications/** - User-facing apps and system services

### Application Types (App Manifests)

Each component has `application.fam` defining its properties:

- `SERVICE` - System service, created at startup
- `APP` - Main menu application
- `PLUGIN` - Plugin for host app
- `EXTERNAL` - Built as .fap for SD card
- `SYSTEM` - Not shown in menus, started by other apps
- `DEBUG` - Debug menu only
- `SETTINGS` - System settings menu
- `STARTUP` - Callback at system startup
- `METAPACKAGE` - Dependency bundle, no code

Key manifest fields:
- `appid` - Build system identifier
- `apptype` - Application type
- `entry_point` - C function entry point
- `requires` - Dependency app IDs
- `conflicts` - Conflicting app IDs
- `stack_size` - Stack allocation in bytes
- `targets` - Compatible hardware targets

### Services (applications/services/)

Background services providing APIs:
- `cli` - Console service
- `desktop` - Desktop/menu service
- `gui` - GUI framework
- `input` - Input handling
- `loader` - Application loader
- `storage` - File system (internal + SD)
- `notification` - Sound/vibration/LED
- `power` - Power management
- `rpc` - Remote procedure call
- `bt` - Bluetooth/BLE

### Main Apps (applications/main/)

User-facing applications:
- `nfc` - NFC (HF RFID, EMV, etc.)
- `subghz` - SubGHz (433 MHz fobs)
- `infrared` - IR remote control
- `lfrfid` - LF RFID (125 kHz)
- `ibutton` - iButton/OneWire
- `gpio` - GPIO control & USART bridge
- `bad_usb` - USB HID attacks

### Hardware Abstraction (targets/f7/furi_hal/)

Hardware-specific implementations:
- `furi_hal_nfc*.c` - NFC PN532 driver and protocol support
- `furi_hal_subghz.c` - CC1101 SubGHz radio
- `furi_hal_infrared.c` - IR transmitter
- `furi_hal_i2c.c` - I2C bus (including PCF8574 GPIO expander)
- `furi_hal_spi.c` - SPI bus
- `furi_hal_serial.c` - UART/USART
- `furi_hal_sd.c` - SD card
- `furi_hal_usb*.c` - USB (CDC, HID, CCID, U2F)

### Libraries (lib/)

Reusable components:
- `nfc/` - NFC protocol implementations
- `subghz/` - SubGHz protocols
- `infrared/` - IR protocols
- `lfrfid/` - LF RFID protocols
- `mbedtls/` - Crypto library
- `u8g2/` - Graphics library (SSD1306 OLED)
- `nanopb/` - Protocol buffers

## NFC Subsystem

NFC is built on PN532 controller with layered architecture:

**Library Layer** (`lib/nfc/`):
- Protocol implementations (ISO14443A/B, ISO15693, FeliCa)
- Card type detection and parsing
- Emulation support

**HAL Layer** (`targets/f7/furi_hal/furi_hal_nfc*.c`):
- `furi_hal_nfc.c` - Main NFC HAL interface
- `furi_hal_nfc_pn532.c` - PN532 driver
- `furi_hal_nfc_iso14443a.c` - ISO14443A timing
- `furi_hal_nfc_iso14443b.c` - ISO14443B timing
- `furi_hal_nfc_iso15693.c` - ISO15693 timing
- `furi_hal_nfc_felica.c` - FeliCa timing
- `furi_hal_nfc_event.c` - Event handling
- `furi_hal_nfc_irq.c` - Interrupt handling

**Application Layer** (`applications/main/nfc/`):
- `nfc_app.c` - Main app entry point
- `api/` - NFC API for other apps
- `cli/` - CLI commands
- `helpers/protocol_support/` - Protocol plugins

## Unit Tests

Located in `applications/debug/unit_tests/tests/`:

```bash
# Build and run unit tests
./fbt FIRMWARE_APP_SET=unit_tests flash
```

Test structure:
- `tests/common/` - Shared test utilities
- `tests/nfc/` - NFC tests
- `tests/furi/` - Core OS tests
- `tests/storage/` - File system tests
- etc.

Each test is a PLUGIN that registers with `unit_tests` STARTUP app.

## Hardware Targets

Target definitions in `targets/*/target.json`:

- `f7` - STM32WB55 (default)
- Other targets inherit from base configuration

Specify target: `./fbt TARGET_HW=18`

## External Apps (.fap)

Apps built for SD card deployment:

```bash
# Build all external apps
./fbt fap_dist

# Build single app
./fbt fap_snake_game

# Deploy to connected device
./fbt fap_deploy
```

External apps use `apptype=FlipperAppType.EXTERNAL` in manifest.

## I2C and GPIO Expansion

This firmware includes PCF8574 I2C GPIO expander support:

- HAL: `targets/f7/furi_hal/furi_hal_pcf8574.c`
- Config: `targets/f7/furi_hal/furi_hal_i2c_config.c`

PCF8574 provides 8 additional GPIO pins via I2C bus.

## Display

Uses U8G2 graphics library for SSD1306 OLED:
- Library: `lib/u8g2/`
- HAL integration in furi_hal

## Development Notes

- Use `furi_check()` for assertions (replaces assert)
- Event loop pattern for async operations
- Message queues for inter-thread communication
- All apps must declare stack size in manifest
- NULL checks required for HAL functions
- NFC timing is critical - avoid blocking in IRQ handlers

---

## Goal

Fix MIFARE 1K/4K cards showing as "unknown type" in NFC reader. Only UID extracted, data sectors not read. Device crashes while exiting NFC tool.

## Instructions

- Work exclusively in directory: `copilot-worktree-2026-04-03T07-50-46`
- Maintain caveman communication style (full level) unless user overrides
- All technical substance stays; fluff dies

## Discoveries

- **Detection pipeline:** NFC Scanner (`lib/nfc/nfc_scanner.c`) iterates base protocols, calls each poller's `detect()`, collects detected protocols. ISO14443-3A base poller activates tag and reads ATQA + SAK. Child protocols (like MIFARE Classic) must succeed at detection for card to be recognized.
- **MIFARE Classic child poller** (`lib/nfc/protocols/mf_classic/mf_classic_poller.c`, function `mf_classic_poller_handler_detect_type()`) maps ATQA/SAK to type. Old code only matched `ATQA0 ∈ {0x02, 0x04}` — missing vendor-prefixed variants `0x42` and `0x44`.
- **GET_TYPE fallback** tried AUTH on blocks 254 (4K) or 62 (1K) with NULL key. When AUTH fails (non-default keys — extremely common on real cards), it defaulted to `MfClassicTypeMini`, which is wrong for most real-world cards.
- **SAK `0x88`/`0x89`** (ISO14443-4A compatible Classic/Mini) was not handled at all in old code.
- **Legacy mapping** in `applications/external/esubghz_chat/lib/nfclegacy/protocols/mifare_common.c` shows full set of valid ATQA/SAK combos that in-app poller was missing.
- **Device name** comes from `mf_classic.c` (lines 10-41): maps `MfClassicType` to `"Mifare Classic Mini 0.3K"`, `"Mifare Classic 1K"`, or `"Mifare Classic 4K"`.
- **Supported cards plugins** (`applications/main/nfc/plugins/supported_cards/`) — 30+ `_parser.fal` plugins handle specific card applications. Without correct type detection, no plugin matches, so only raw UID is shown.
- **Protocol tree** (`lib/nfc/protocols/nfc_protocol.c`): MIFARE Classic is child of ISO14443-3A. Detection must succeed at child level for card to be recognized as Classic rather than generic "ISO14443-3A (Unknown)".
- **Edge case:** ATQA=`[0x01, 0x0F]` + SAK=`0x01` is valid NXP MIFARE Classic combo currently not in explicit table — falls through to GET_TYPE/SAK inference, which still produces correct result but could be made explicit.

---

## Session: NFC PN532 I2C Audit (2026-05-12)

## Goal

Verify NFC Tool exclusively uses PN532 module over I2C1 (PA9/PB9) w/ software interrupt polling. No HW INT/IRQ pins. Confirm all code paths consistent.

## Corrected Understanding

- **I2C1 pins:** PA9(SCL) + PB9(SDA) via AF4 — **permanent, already correct**
- PB9 is a valid I2C1 SDA pin on STM32WB55 (AF4) — no bug existed
- `I2C_1_SDA_Pin=LL_GPIO_PIN_9` on `GPIOB` is correct (bit-9 mask ≠ port encoding)
- Prior analysis claiming PA10 was needed was **wrong** — re-verified against STM32WB55 RM

## Verification Chain (all consistent)

| Layer | File | Check | Status |
|-------|------|-------|--------|
| Pin macros | `furi_hal_resources.h:284-287` | SCL=PA9, SDA=PB9 | ✓ |
| GpioPin structs | `furi_hal_resources.c:64-65` | `{GPIOA, PIN_9}` + `{GPIOB, PIN_9}` | ✓ |
| GPIO init | `furi_hal_i2c_config.c:75-86` | AF4 open-drain both | ✓ |
| Bus activation | `furi_hal_i2c_config.c:35-36` | I2C1 periph enable + PCLK1 src | ✓ |
| Bus reset | `furi_hal_i2c.c:17-36` | Toggles PA9+PB9 correct | ✓ |
| PN532 uses | `furi_hal_pn532.c` | `furi_hal_i2c_handle_power` (I2C1) | ✓ |
| IRQ stubs | `furi_hal_nfc_irq.c` | No-op log stubs (correct for polling) | ✓ |
| Listener gating | `furi_hal_nfc.c:246-308` | All listener funcs return error when PN532 active | ✓ |
| Bus reset on PN532 | `furi_hal_pn532.c:41-45` | Never called (no HW RST pin) | ✓ |
| I2C timing | `furi_hal_i2c_config.c:95` | 100kHz (conservative, change not needed) | ✓ |

## Accomplished

- **Completed:** Full NFC codebase map (60+ files: lib/nfc/, applications/main/nfc/, targets/f7/furi_hal/)
- **Completed:** Read & analyzed core engine (nfc.c TX/RX state machine, nfc_poller.c, nfc_listener.c, nfc_scanner.c)
- **Completed:** Read & analyzed HAL layer (furi_hal_nfc.c dispatcher, furi_hal_nfc_pn532.c I2C driver, furi_hal_nfc_iso14443a.c, furi_hal_nfc_irq.c stubs)
- **Completed:** Read & analyzed all protocol implementations (ISO14443-3A/4A/3B/4B, ISO15693-3, Felica, MIFARE Classic/Ultralight, NTAG4xx, SLIX, ST25TB, EMV, Type4Tag)
- **Completed:** Read & analyzed all NFC app scenes (detect, read, write, emulate, MF Classic detect, SLIX unlock, etc.)
- **Completed:** Read & analyzed protocol support dispatch, helpers, CRC modules
- **Completed:** I2C1 pin audit — PA9/PB9 confirmed correct permanent mapping
- **Completed:** Consolidated NFC pin/I2C audit report

## Key Decisions

- I2C1 permanently PA9/PB9 — no change needed
- PN532 uses `furi_hal_i2c_handle_power` (I2C1) — same bus as PCF8574 + INA219
- I2C3 (PA7/PB4) disabled — pins conflict w/ SPI_MOSI + other functions
- Software interrupt polling correct (IRQ stubs already no-op)
- 100kHz I2C adequate; 400kHz upgrade deferred as low priority

## Next Steps

(none — audit complete, no code changes required)

## Accomplished

- **Completed:** Full codebase analysis of NFC card type detection pipeline.
- **Completed:** Identified all root causes (narrow ATQA matching, broken fallback, missing SAK variants).
- **Completed:** Applied fix for `mf_classic_poller_handler_detect_type()` in `lib/nfc/protocols/mf_classic/mf_classic_poller.c` (lines 166-177):
  - Removed broken GET_TYPE fallback (NULL key auth fails on real cards).
  - Fixed SAK fallback: `0x18/0x88 → 4K`, `0x09/0x89 → Mini`, else `1K`.
- **Completed:** Fixed uninitialized `data->type` in `mf_classic_alloc()` by adding `memset(data, 0, sizeof(MfClassicData))`.
- **Completed:** Built firmware successfully (`dist/f7-C/firmware.bin`).

## Next Steps

1. **Flash firmware:** Run `./fbt flash` or `./fbt flash_usb_full` to flash to Flipper Zero.
2. **Test with real cards:** Use MIFARE Classic 1K and 4K cards that previously showed "unknown type" — verify they now show correct type name and that data sectors are read.

## Relevant files / directories

```
lib/nfc/protocols/mf_classic/mf_classic_poller.c        — DETECTION FUNCTION (fixed: removed GET_TYPE fallback, improved SAK fallback)
lib/nfc/protocols/mf_classic/mf_classic.c               — Type-to-name mapping, mf_classic_alloc (fixed: zero-init)
lib/nfc/protocols/mf_classic/mf_classic.h               — MfClassicType enum, MfClassicData struct
lib/nfc/protocols/iso14443_3a/iso14443_3a_poller.c      — Base poller (reads ATQA/SAK)
lib/nfc/protocols/iso14443_3a/iso14443_3a.h             — Iso14443_3aData struct (atqa, sak fields)
lib/nfc/nfc_scanner.c                                   — Scanner state machine
lib/nfc/nfc_poller.c                                    — Poller chaining architecture
lib/nfc/protocols/nfc_protocol.c                        — Protocol tree hierarchy
applications/external/esubghz_chat/lib/nfclegacy/protocols/mifare_common.c — Legacy ATQA/SAK reference mapping
applications/main/nfc/plugins/supported_cards/          — Card-specific parser plugins
```

## NFC Bug Fix Marathon — 2026-05-13

All 55 issues from the NFC code review have been fixed across 4 waves. The fixes span the PN532 driver, NFC HAL, lib/nfc stack, and NFC application layer.

### Summary by severity
- 🔴 Critical (4): uint8 overflow in frame length, scanner memcpy byte-count, scanner div-by-zero, listener double-free
- 🔴 Linker (1): unguarded declaration in PN532-only build
- 🟡 High (12): mutex-less I2C ops, init desync, off-by-one bounds, missing state handlers, NULL-dict deref, broken CRC append in tx path
- 🟡 Medium (18): missing frame validation, inconsistent error flags, spin-loops, abort delay, draw-callback mutations, leak paths, PRNG cancellation
- 🔵 Info/Low (20): code quality, strict aliasing UB, dead code, missing timeouts, CLI acquire/release

### Key fixes by component

**PN532 Driver** (`furi_hal_pn532.c`, `furi_hal_nfc_pn532.c`):
- Fixed uint8 overflow in frame LEN field (P1)
- Added postamble validation to response parser (P17)
- Added mutex acquire/release to set_mode, reset_mode, low_power_mode, pn532_rx (P6, P11, P24)
- Fixed ACK-failure drain in init sequence to prevent I2C desync (P8)
- Tightened InDataExchange/InCommunicateThru length checks (P9, P10)
- Fixed spurious CRC append in InCommunicateThru tx path that broke MIFARE Classic auth (P21)
- Fixed abort handling to exit polling loop immediately (P22)
- Checked write_register pn532_read_response return (P7)
- Added null-target guards in poll functions (P19)

**NFC HAL** (`furi_hal_nfc.c`, `furi_hal_nfc_iso14443a.c`):
- Guarded listener_set_col_res_data declaration/call for PN532-only builds (P5)
- Removed UNUSED macros from used parameters (P46)
- Early return from listener_init when PN532 active (P45)
- Fixed iso4443a function name typo (P47)

**NFC Stack** (`nfc.c`, `nfc_scanner.c`):
- Fixed memcpy byte-count bug in protocol detection (P2)
- Added zero-protocol guard in scanner (P3)
- Added NfcCommStateFailed state handler (P14)
- Added double-start guard in nfc_start (P12)
- Replaced spin-loops with timed loops (P13)
- Increased TX buffer to fit data+parity bits (P26)

**MIFARE Classic** (`mf_classic_poller.c`, `mf_classic.c`):
- Replaced VLA with fixed array (P28)
- Replaced furi_assert crash with soft error handling (P29)
- Added backdoor auth variable passthrough (P30)
- Added periodic abort check in PRNG brute-force loop (P51)
- Added calibration distance fallback (P52)
- Fixed strict aliasing UB with memcpy (P48)
- Moved value assignment after validation check (P49)
- Removed dead code after exhaustive switches (P50)

**NFC Application** (scenes, helpers, CLI):
- Fixed double-free in mfkey32_logger (P4)
- Added NULL-dict guard (P15)
- Added checked storage_common_copy calls (P16)
- Fixed path truncation on save failure (P31)
- Moved header mutation out of draw callback (P32)
- Added div-by-zero guard in progress bar (P33)
- Fixed 5 resource leaks in nfc_app_free (P34)
- Added STRING_FAILURE check in save path (P35)
- Fixed memmove on key deletion (P36)
- Added CLI acquire/release for SPI safety (P53)
- Replaced blocking notification with non-blocking (P54)
- Added emulate timeout (P55)
- Used strlen for shadow extension length (P56)

### Verification
- Debug build (`./fbt`): PASS
- Release build (`./fbt TARGET_HW=7 DEBUG=0 COMPACT=1 copro_dist updater_package fap_dist`): PASS
- Source audit: 54/55 patterns confirmed (P38 removed as dead code — can never trigger on uint8_t)
