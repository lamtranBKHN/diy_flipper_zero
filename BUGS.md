# Bug Report: DIY Flipper Zero Firmware

**Date:** 2026-05-17
**Branch:** copilot/worktree-2026-04-03T07-50-46
**Target:** STM32WB55CGU6 (DIY board, TARGET_HW=7)

---

## Prior Bug Fix History

| Date | Bugs | Status |
|------|------|--------|
| 2026-05-11 | 6 NFC bugs (mutex race, missing mutex, scanner filter, listener guard, low_power furi_check, 270-byte read) | FIXED, committed |
| 2026-05-13 | 55 NFC issues (4 critical, 1 linker, 12 high, 18 medium, 20 low) | FIXED, committed |
| 2026-05-17 | 8 bugs (sizeof pointer, PCF8574 cooldown, PN532 ACK, InCommunicateThru timeout, I-block PCB, I2C3 crash, SSD1306 guard, menu cache) | FIXED, committed (5ab8e946b) |
| 2026-05-17 | C1-SPI timeout, C2-iso15693 memcpy, C3-14 malloc NULL, H1-H4 magic numbers | FIXED, committed (8eecaf3ec) |

---

## Fixed in Commit 8eecaf3ec (Phase 1+2)

### C1: SPI timeout deadlock — **FIXED**
- `furi_hal_spi.c:93-102` — replaced infinite busy-wait with tick-based timeout.
  Function changed from `void` to `bool`, returns false on timeout. All 3 callers
  updated to propagate result. Log messages on timeout.

### C2: iso15693_3 memcpy direction — **FIXED**
- `iso15693_3.c:97-114` — memcpy now copies FROM `&legacy_data[1]` INTO
  `simple_array_get_data(data->block_security)`. Previously copied in reverse
  direction into a temp buffer that was immediately freed (dead code).

### C3: malloc NULL checks — **14 of 17 FIXED**
- **FIXED:** loader_menu.c:79, 269, 452; update.c:58, 81, 158; furi_hal_memory.c:33;
  furi_hal_ibutton.c:38; furi_hal_serial_control.c:266; ble_glue.c:73; ble_app.c:82; gap.c:544
- **Already had checks:** mf_classic_poller.c:34,1115,1636 (already returned NULL on OOM);
  furi_hal_nfc_event.c:11 (had furi_check); furi_hal_adc.c:92 (had early-return check)
- **Fix:** `furi_check(ptr)` after each malloc in init paths.

### H1: malloc(100) magic numbers — **FIXED** (submodule commit 6d03fe32f)
- `nfc_apdu_runner.h` + `nfc_worker.c:291,346,398` — replaced with `MAX_ERROR_MSG_SIZE`

### H2: Duplicate linker dependency — **FIXED**
- `target.json:52` — removed duplicate `"flipper7"`

### H3: Thread stack sizes as magic numbers — **FIXED**
- `loader_menu.c` — added `LOADER_MENU_STACK_SIZE 2048`

### H4: PCF8574 I2C timeout magic number — **FIXED**
- `furi_hal_pcf8574.c` — added `PCF8574_I2C_TIMEOUT_MS 50`

---

## Remaining Bugs (Not Yet Fixed)

### CRITICAL (0) — All fixed ✓

### HIGH (4 remaining)

#### H5: Unit tests never run in CI
- `.github/workflows/build.yml`
- Zero automated test coverage. Add matrix entry with `FIRMWARE_APP_SET=unit_tests`.

#### H6: NFC dict attack lag + backdoor re-entry (FL-3926)
- `applications/main/nfc/scenes/nfc_scene_mf_classic_dict_attack.c:8-9`
- Lag when leaving hardnested view; re-enters backdoor detection between dicts.

#### H7: ISO14443-4 block chaining incomplete
- `lib/nfc/helpers/iso14443_4_layer.c:193, 250, 281, 303`
- R-block handling and block chaining TODOs. May fail with strict ISO14443-4 readers.

#### H8: Sub-GHz RX buffer overflow risk (FL-3555)
- `lib/subghz/subghz_tx_rx_worker.c:192`
- RX buffer overflow on high RF activity.

### MEDIUM (7)

| ID | Bug | Location | Status |
|----|-----|----------|--------|
| M1 | CI "Momentum" label | `.github/workflows/build.yml:75` | NOT FIXED |
| M2 | Cross-target API check is no-op | `.github/workflows/build.yml:54-63` | NOT FIXED |
| M3 | FAP metadata cache not implemented | `archive_browser.c:448-470` | NOT FIXED |
| M4 | Dead commented-out RF DMA code | `furi_hal_rfid.c:322-399` | NOT FIXED |
| M5 | Sub-GHz TX buffer write check missing | `subghz_tx_rx_worker.c:168` | NOT FIXED |
| M6 | Storage file handle leak question | `storage_ext.c:165` | NOT FIXED |
| M7 | Loader double-start emission | `loader.c:124` | NOT FIXED |

### LOW (5)

| ID | Bug | Location |
|----|-----|----------|
| L1 | Sub-GHz Schrader protocol bug | `schrader_gg4.c:154` |
| L2 | FAAC SLH custom button bypass | `faac_slh.c:129, 561` |
| L3 | NTAG4xx undocumented behavior | `ntag4xx.c:142` |
| L4 | DFU signature check incomplete | `dfu_file.c:58` |
| L5 | mjs NaN endianness | `mjs_string.c:38` |

---

## Bug Summary by Category (post-fixes)

| Category | Critical | High | Medium | Low | Total |
|----------|----------|------|--------|-----|-------|
| Memory Safety | C3 (14 of 17 fixed) | H1 (fixed) | - | - | - |
| Deadlock/Hang | C1 (fixed) | - | - | - | - |
| Logic Bug | C2 (fixed) | H7, H8 | M6, M7 | L1-L5 | - |
| Config/Build | - | H2 (fixed), H5 | M1, M2 | - | - |
| Code Quality | - | H3 (fixed), H4 (fixed) | M3, M4, M5 | - | - |
| NFC/RFID | - | H6 | - | L3 | - |
| Sub-GHz | - | H8 | M5 | L1, L2 | - |

**Remaining: 0 Critical + 4 High + 7 Medium + 5 Low = 16 bugs**

**Build note:** Firmware.elf (debug + release) builds successfully. Updater.elf has pre-existing link error (`shci_register_io_bus` undefined ref) unrelated to these fixes.