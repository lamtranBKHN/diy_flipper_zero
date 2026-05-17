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
| 2026-05-17 | 8 bugs (sizeof pointer, PCF8574 cooldown, PN532 ACK, InCommunicateThru timeout, I-block PCB, I2C3 crash, SSD1306 guard, menu cache) | FIXED, **UNSTAGED** |

---

## Current Unstaged Fixes (8 bugs, 13 files, +217/-59 lines)

These are already implemented but not yet committed:

1. `sizeof(pointer)` truncation — `furi_hal_nfc_pn532.c:783/796`
2. PCF8574 800ms I2C scan on glitch — `furi_hal_pcf8574.c`
3. ACK failure forces cold PN532 reinit — `furi_hal_pn532.c`
4. InCommunicateThru 250ms timeout too short — `furi_hal_pn532.c`
5. Listener I-block PCB byte never toggled — `furi_hal_nfc_pn532.c`
6. I2C3 dead code corrupts SPI — `furi_hal_i2c_config.c`
7. SSD1306/SSD1309 not mutually exclusive — `furi_hal_resources.h`
8. Menu cache scaffolded but never used — `loader_menu.c`

---

## New Bugs Discovered (2026-05-17 Audit)

### CRITICAL (3)

#### C1: SPI timeout ignored — deadlock risk
- **File:** `targets/f7/furi_hal/furi_hal_spi.c:93-102`
- **Line:** `UNUSED(timeout); // FIXME`
- **Impact:** SPI bus hang → permanent system deadlock
- **Reason:** Three infinite while-loops busy-wait with no timeout. SPI1 is shared by display (PB6), CC1101 (PA15), and SD (PA10). If any device stops responding, the firmware freezes permanently. All ViewPort lockup warnings trace back to this.
- **Fix:** Replace busy-wait loops with tick-based timeout. Return error on timeout.

#### C2: iso15693_3 boomerang memcpy — wasted alloc + logic bug
- **File:** `lib/nfc/protocols/iso15693_3/iso15693_3.c:97-114`
- **Impact:** malloc'd buffer populated then freed unused; memcpy direction copies FROM existing data INTO temp buffer that is immediately discarded
- **Reason:** `legacy_data = malloc(value_count)` at line 97. Lines 106-109 memcpy FROM `data->block_security` INTO `&legacy_data[1]`. Then line 114 frees `legacy_data` without ever using the copied data. The intent was likely to populate `data->block_security` FROM the legacy file format, not the reverse.
- **Fix:** Either remove dead code (if `data->block_security` is already populated by `flipper_format_read_hex`), or reverse memcpy direction to populate the simple_array from legacy_data.

#### C3: Missing NULL checks after malloc (17 instances)
- **Files:**
  - `applications/services/loader/loader_menu.c:79, 269, 452` (newly modified)
  - `lib/nfc/protocols/mf_classic/mf_classic_poller.c:34, 1115, 1636`
  - `targets/f7/src/update.c:58, 80, 153`
  - `targets/f7/furi_hal/furi_hal_nfc_event.c:11`
  - `targets/f7/furi_hal/furi_hal_memory.c:33`
  - `targets/f7/furi_hal/furi_hal_ibutton.c:38`
  - `targets/f7/furi_hal/furi_hal_adc.c:92`
  - `targets/f7/furi_hal/furi_hal_serial_control.c:266`
  - `targets/f7/ble_glue/ble_glue.c:73`
  - `targets/f7/ble_glue/ble_app.c:82`
  - `targets/f7/ble_glue/gap.c:544`
- **Impact:** NULL dereference → hard fault on out-of-memory
- **Reason:** STM32WB55 has 256KB RAM. Under memory pressure, malloc returns NULL. All 17 sites dereference without checking.
- **Fix:** Add `furi_check(ptr)` after each malloc, or return error code for non-init paths.

### HIGH (8)

#### H1: malloc(100) magic numbers — no bounds checking
- **File:** `applications/external/nfc_apdu_runner/nfc_worker.c:291, 346, 398`
- **Impact:** Maintainability, potential buffer overflow if format string grows
- **Fix:** `#define APDU_ERROR_MSG_MAX 100`, use consistently, add bounds check.

#### H2: Duplicate linker dependency
- **File:** `targets/f7/target.json:22, 52`
- **Impact:** Harmless (linker deduplicates), indicates copy-paste oversight
- **Fix:** Remove duplicate `"flipper7"` entry at line 52.

#### H3: Thread stack sizes as magic numbers
- **Files:** `loader_menu.c:66` (2048), `nfc_worker.c:582` (8192)
- **Impact:** Stack overflow risk, no compile-time validation
- **Fix:** Define named constants: `LOADER_MENU_STACK_SIZE`, `NFC_WORKER_STACK_SIZE`.

#### H4: PCF8574 I2C timeout magic number (50ms)
- **File:** `targets/f7/furi_hal/furi_hal_pcf8574.c:36, 59, 85, 105`
- **Impact:** Maintainability
- **Fix:** `#define PCF8574_I2C_TIMEOUT_MS 50`

#### H5: Unit tests never run in CI
- **File:** `.github/workflows/build.yml`
- **Impact:** Zero automated test coverage for core logic
- **Fix:** Add matrix entry with `FIRMWARE_APP_SET=unit_tests` or separate test job.

#### H6: NFC dict attack lag + backdoor re-entry (FL-3926)
- **File:** `applications/main/nfc/scenes/nfc_scene_mf_classic_dict_attack.c:8-9`
- **Impact:** Poor UX, potential crash from re-entering backdoor detection
- **Fix:** Requires investigation of state machine transitions between user/system dictionary phases.

#### H7: ISO14443-4 block chaining incomplete
- **File:** `lib/nfc/helpers/iso14443_4_layer.c:193, 250, 281, 303`
- **Impact:** May fail with strict ISO14443-4 readers
- **Fix:** Implement R-block handling and block chaining per ISO14443-4 spec.

#### H8: Sub-GHz RX buffer overflow risk (FL-3555)
- **File:** `lib/subghz/subghz_tx_rx_worker.c:192`
- **Impact:** Data corruption on high RF activity
- **Fix:** Add boundary check before writing to RX buffer.

### MEDIUM (7)

#### M1: CI "Momentum" label misleading
- **File:** `.github/workflows/build.yml:75`
- **Impact:** Cosmetic confusion in PR comments

#### M2: Cross-target API check is no-op
- **File:** `.github/workflows/build.yml:54-63`
- **Impact:** False sense of security — only targets/f7/ exists

#### M3: FAP metadata cache not implemented
- **File:** AGENTS.md Storage Optimization TODOs
- **Impact:** SD I/O overhead on every archive browser listing

#### M4: Dead commented-out RF DMA code
- **File:** `targets/f7/furi_hal/furi_hal_rfid.c:322-399`
- **Impact:** Confusion, bloat

#### M5: Sub-GHz TX buffer write check missing (FL-3554)
- **File:** `lib/subghz/subghz_tx_rx_worker.c:168`
- **Impact:** Silent data loss on TX

#### M6: Storage file handle leak question
- **File:** `applications/services/storage/storages/storage_ext.c:165`
- **Impact:** Potential file descriptor exhaustion

#### M7: Loader double-start emission
- **File:** `applications/services/loader/loader.c:124`
- **Impact:** Potential double app launch

### LOW (5)

#### L1: Sub-GHz Schrader protocol bug
- **File:** `lib/subghz/protocols/schrader_gg4.c:154`
- **Comment:** `// TODO locate and fix`

#### L2: FAAC SLH custom button bypass
- **File:** `lib/subghz/protocols/faac_slh.c:129, 561`
- **Comment:** `// TODO: Stupid bypass for custom button, remake later`

#### L3: NTAG4xx undocumented behavior
- **File:** `lib/nfc/protocols/ntag4xx/ntag4xx.c:142`
- **Comment:** `// TODO: there is no info online or in other implementations`

#### L4: DFU signature check incomplete
- **File:** `lib/update_util/dfu_file.c:58`
- **Comment:** `/* TODO FL-3561: check DfuSignature?.. */`

#### L5: mjs NaN endianness
- **File:** `lib/mjs/mjs_string.c:38`
- **Comment:** `/* TODO(lsm): NaN payload location depends on endianness */`

---

## Bug Summary by Category

| Category | Critical | High | Medium | Low | Total |
|----------|----------|------|--------|-----|-------|
| Memory Safety | C3 (17 malloc NULL) | H1 (malloc magic) | - | - | 18 |
| Deadlock/Hang | C1 (SPI timeout) | - | - | - | 1 |
| Logic Bug | C2 (boomerang memcpy) | H7 (ISO14443-4), H8 (RX overflow) | M6 (file leak), M7 (double-start) | L1-L5 (5) | 10 |
| Config/Build | - | H2 (dup dep), H5 (no CI tests) | M1 (CI label), M2 (no-op check) | - | 4 |
| Code Quality | - | H3 (stack magic), H4 (timeout magic) | M3 (FAP cache), M4 (dead code), M5 (TX check) | - | 5 |
| NFC/RFID | - | H6 (dict attack lag) | - | L3 (NTAG4xx) | 2 |
| Sub-GHz | - | H8 (RX overflow) | M5 (TX check) | L1, L2 (2) | 4 |

**Total: 3 Critical + 8 High + 7 Medium + 5 Low = 23 bugs**
