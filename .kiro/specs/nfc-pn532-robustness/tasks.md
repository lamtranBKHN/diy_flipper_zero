# Implementation Plan: NFC PN532 Robustness Fixes

## Overview

Four bugs degrade NFC emulation and scanning reliability on the DIY Flipper Zero (STM32WB55 + PN532 over I2C). This plan addresses them in dependency order: Bug 1 and Bug 4 are isolated changes to `furi_hal_nfc_pn532.c`; Bug 2 requires a new `furi_hal_pn532.c` function first; Bug 3 requires a new poll function plus a scanner probe-order change. Task 5 verifies the full build compiles cleanly.

**Language:** C (embedded firmware, STM32WB55 target)

---

## Tasks

- [x] 1. Bug 1 — Fix 4/7-byte UID cascade encoding in `listener_wait_event`
  - [x] 1.1 Replace the `listener_configured` UID-copy block in `listener_wait_event()`
    - File: `targets/f7/furi_hal/furi_hal_nfc_pn532.c`
    - Locate the `if(furi_hal_nfc_pn532.listener_configured)` block that copies `listener_uid[0..2]` into `params[3..5]`
    - Replace with the cascade-aware logic from the design:
      - `uid_len <= 3`: copy `uid[0..2]` directly into `params[3..5]`, set `params[6] = sak` unmodified
      - `uid_len == 4 or 7`: set `params[3] = 0x88` (CT), `params[4] = uid[0]`, `params[5] = uid[1]`, `params[6] = (uid_len == 4) ? (sak | 0x04) : 0x04`
    - After the if/else, add the NFCID3t loop: `for(uint8_t i = 0; i < uid_len && i < 10; i++) params[25 + i] = uid[i];`
    - _Requirements: 2.1, 2.2, 2.3, 2.4_

  - [ ]* 1.2 Write property test for cascade tag (Property 1)
    - **Property 1: Cascade Tag for Multi-Byte UIDs**
    - For uid_len in {4, 7}: call the params-building logic with a synthetic uid, assert `params[3] == 0x88`
    - **Validates: Requirements 2.1, 2.2**

  - [ ]* 1.3 Write property test for SAK cascade bit (Property 2)
    - **Property 2: SAK Cascade Bit for Multi-Byte UIDs**
    - For uid_len in {4, 7} and any sak value: assert `params[6] & 0x04 != 0`
    - **Validates: Requirements 2.1, 2.2**

  - [ ]* 1.4 Write property test for NFCID3t population (Property 3)
    - **Property 3: NFCID3t Populated with Full UID**
    - For uid_len in {4, 7}: assert `params[25 + i] == uid[i]` for all `i` in `[0, uid_len)`
    - **Validates: Requirements 2.3**

  - [ ]* 1.5 Write property test for 3-byte UID regression (Property 4)
    - **Property 4: 3-Byte UID Regression**
    - For uid_len == 3 and any sak: assert `params[3..5] == uid[0..2]` and `params[6] == sak` without modification
    - **Validates: Requirements 2.4, 3.1**

- [x] 2. Bug 2 — Add `in_data_exchange_ex` and fix ISO-DEP chaining in `exchange_internal`
  - [x] 2.1 Add `furi_hal_pn532_in_data_exchange_ex()` to `furi_hal_pn532.c`
    - File: `targets/f7/furi_hal/furi_hal_pn532.c`
    - Add the new function after the existing `furi_hal_pn532_in_data_exchange()`:
      - Same logic as `in_data_exchange` but accepts an extra `uint8_t* pn532_status` output parameter
      - Extracts `response[1]` (PN532 status byte) before stripping it; writes it to `*pn532_status` if non-NULL
      - Returns `FuriHalPn532ErrorComm` if the lower 6 bits of the status byte are non-zero
    - Refactor the existing `furi_hal_pn532_in_data_exchange()` to delegate to `_ex` with `pn532_status = NULL`
    - File: `targets/f7/furi_hal/furi_hal_pn532.h`
    - Add the declaration for `furi_hal_pn532_in_data_exchange_ex()` with a doc comment explaining bit 6 (`0x40`) = card chaining active
    - _Requirements: 5.1_

  - [x] 2.2 Replace the I-block exchange path in `exchange_internal()` with a chaining assembly loop
    - File: `targets/f7/furi_hal/furi_hal_nfc_pn532.c`
    - Locate the I-block path inside `furi_hal_nfc_pn532_exchange_internal()` that calls `furi_hal_pn532_in_data_exchange()` once
    - Replace with the chaining loop from the design:
      - Call `furi_hal_pn532_in_data_exchange_ex()` for the first fragment, capturing `pn532_status`
      - Define `#define PN532_STATUS_CHAINING 0x40` (or use the constant if already defined)
      - While `pn532_status & PN532_STATUS_CHAINING`: build R(ACK) byte `0xA2 | (iso_dep_block_num & 1)`, call `in_data_exchange_ex` again, append fragment to `assembled[]`
      - Overflow guard: if `assembled_len + frag_len > PN532_MAX_FRAME_SIZE`, set `last_error = FuriHalPn532ErrorBufferOverflow` and return `FuriHalNfcErrorBufferOverflow`
      - After loop: reconstruct I-block with `resp_pcb = 0x02 | (iso_dep_block_num & 1)`, toggle `iso_dep_block_num`, copy assembled payload into scratch, append CRC, call `prepare_rx` + `finalize_exchange`
    - Non-ISO-DEP paths (MIFARE Classic auth via `InCommunicateThru`, R-blocks, S-blocks) must remain unchanged
    - _Requirements: 5.1, 5.2, 5.3, 5.4, 6.1, 6.2, 6.3, 6.4_

  - [ ]* 2.3 Write property test for chained response assembly (Property 5)
    - **Property 5: Chained Response Assembly**
    - For N ≥ 2 fragments with chaining bit set on all but the last: assert returned payload equals byte-for-byte concatenation of all fragment payloads
    - **Validates: Requirements 5.1, 5.2**

  - [ ]* 2.4 Write property test for single-fragment no extra calls (Property 6)
    - **Property 6: Single-Fragment No Extra Calls**
    - For a single-fragment response (chaining bit clear): assert exactly 1 call to `in_data_exchange_ex`
    - **Validates: Requirements 6.1**

- [x] 3. Checkpoint — Ensure all tests pass, ask the user if questions arise.

- [x] 4. Bug 3 — Implement `poll_iso15693` and add ISO15693 to the scanner probe order
  - [x] 4.1 Implement `furi_hal_pn532_poll_iso15693()` in `furi_hal_pn532.c`
    - File: `targets/f7/furi_hal/furi_hal_pn532.c`
    - Add the function after `furi_hal_pn532_poll_iso14443b` (or the last existing poll function)
    - Build `InListPassiveTarget` command: `{PN532_CMD_IN_LIST_PASSIVE, 0x01, 0x05, 0x26, 0x01}` (MaxTg=1, BrTy=0x05, INVENTORY flags+mask)
    - Call `pn532_exchange()` with `PN532_TIMEOUT_POLL_MS`; return `false` on error or `response_len < 12`
    - Check `response[1]` (NbTg); return `false` if 0
    - Populate `target`: `target_number = response[2]`, `uid_len = 8`, reverse UID bytes (`target->uid[i] = response[4 + (7 - i)]`), store DSFID in `target->atqa[0]`
    - Add `FURI_LOG_D` trace for the found UID
    - File: `targets/f7/furi_hal/furi_hal_pn532.h`
    - Add declaration: `bool furi_hal_pn532_poll_iso15693(FuriHalPn532Target* target);`
    - _Requirements: 8.1, 8.2, 8.3, 8.5_

  - [x] 4.2 Add `NfcProtocolIso15693_3` to `pn532_probe_order[]` in `nfc_scanner.c`
    - File: `lib/nfc/nfc_scanner.c`
    - Locate `pn532_probe_order[]` (currently contains ISO14443-3A, ISO14443-3B, FeliCa with ISO15693 commented out)
    - Add `NfcProtocolIso15693_3` as the fourth entry after `NfcProtocolFelica`
    - Update the comment from "causes I2C deadlock" to "I2C deadlock resolved; poll_iso15693 implemented"
    - _Requirements: 8.4_

  - [ ]* 4.3 Write property test for ISO15693 UID byte reversal (Property 7)
    - **Property 7: ISO15693 UID Byte Reversal**
    - For any 8-byte wire UID (LSB-first): assert `target->uid[i] == wire_uid[7 - i]` for all `i` in `[0, 8)`
    - Also test: response shorter than 12 bytes returns `false` without crash; NbTg=0 returns `false`
    - **Validates: Requirements 8.2, 9.4**

- [x] 5. Bug 4 — Add FeliCa NFCID2t state and `set_felica_params`, fix `listener_wait_event` FeliCa branch
  - [x] 5.1 Add `felica_nfcid2t` and `felica_nfcid2t_configured` fields to `FuriHalNfcPn532State`
    - File: `targets/f7/furi_hal/furi_hal_nfc_pn532.c`
    - Locate the `FuriHalNfcPn532State` struct definition (or the static instance `furi_hal_nfc_pn532`)
    - Add two fields after the existing listener fields:
      ```c
      uint8_t felica_nfcid2t[8];
      bool    felica_nfcid2t_configured;
      ```
    - In `furi_hal_nfc_pn532_reset()` (or the equivalent init path), add `furi_hal_nfc_pn532.felica_nfcid2t_configured = false;`
    - _Requirements: 11.1_

  - [x] 5.2 Implement `furi_hal_nfc_pn532_listener_set_felica_params()` in `furi_hal_nfc_pn532.c`
    - File: `targets/f7/furi_hal/furi_hal_nfc_pn532.c`
    - Add the function (near the other `listener_set_*` functions):
      ```c
      void furi_hal_nfc_pn532_listener_set_felica_params(const uint8_t* nfcid2t) {
          furi_check(nfcid2t);
          memcpy(furi_hal_nfc_pn532.felica_nfcid2t, nfcid2t, 8);
          furi_hal_nfc_pn532.felica_nfcid2t_configured = true;
          furi_hal_nfc_pn532.listener_active = false;
      }
      ```
    - File: `targets/f7/furi_hal/furi_hal_nfc_pn532.h`
    - Add declaration: `void furi_hal_nfc_pn532_listener_set_felica_params(const uint8_t* nfcid2t);`
    - _Requirements: 11.1, 11.4_

  - [x] 5.3 Replace the hardcoded FeliCa `TgInitAsTarget` params in `listener_wait_event()`
    - File: `targets/f7/furi_hal/furi_hal_nfc_pn532.c`
    - Locate the `if(furi_hal_nfc_pn532.tech == FuriHalNfcTechFelica)` branch in `listener_wait_event()`
    - Replace the hardcoded `params[]` array with the dynamic version from the design:
      - `memset(params, 0, 21)`; `params[0] = 0x02`
      - Define `static const uint8_t felica_nfcid2t_default[8] = {0x01, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};`
      - Select `nfcid2t = felica_nfcid2t_configured ? felica_nfcid2t : felica_nfcid2t_default`
      - `memcpy(&params[1], nfcid2t, 8)`; `params[17] = 0x12`; `params[18] = 0xFC`
    - ISO14443-3A listener branch must remain unaffected
    - _Requirements: 11.2, 11.3, 12.1, 12.2, 12.3, 12.4_

  - [ ]* 5.4 Write property test for FeliCa NFCID2t round-trip (Property 8)
    - **Property 8: FeliCa NFCID2t Round-Trip**
    - For any 8-byte NFCID2t `N`: call `set_felica_params(N)`, build FeliCa params, assert `params[1 + i] == N[i]` for all `i` in `[0, 8)`
    - **Validates: Requirements 11.2**

  - [ ]* 5.5 Write property test for `set_felica_params` resetting `listener_active` (Property 9)
    - **Property 9: FeliCa set_felica_params Resets listener_active**
    - For any 8-byte NFCID2t: call `set_felica_params`, assert `furi_hal_nfc_pn532.listener_active == false` immediately after
    - **Validates: Requirements 11.4**

  - [ ]* 5.6 Write property test for FeliCa last-write-wins (Property 10)
    - **Property 10: FeliCa Last-Write-Wins**
    - For any two distinct NFCID2t values `N1 != N2`: call `set_felica_params(N1)` then `set_felica_params(N2)`, build params, assert `params[1..8] == N2`
    - **Validates: Requirements 11.2, 11.4**

- [ ] 6. Build verification
  - [-] 6.1 Verify firmware compiles cleanly with `./fbt`
    - Run `./fbt` from the repo root (after loading toolchain via `scripts/toolchain/fbtenv.cmd` on Windows or `source scripts/toolchain/fbtenv.sh` on Linux/macOS)
    - Confirm zero errors and zero new warnings in `targets/f7/furi_hal/furi_hal_nfc_pn532.c`, `furi_hal_pn532.c`, and `lib/nfc/nfc_scanner.c`
    - _Requirements: all_

  - [~] 6.2 Verify unit test build compiles cleanly
    - Run `./fbt FIRMWARE_APP_SET=unit_tests` and confirm no link errors in the NFC-related translation units
    - _Requirements: all_

- [~] 7. Final checkpoint — Ensure all tests pass, ask the user if questions arise.

---

## Notes

- Tasks marked with `*` are optional and can be skipped for faster MVP
- Each task references specific requirements from `bugfix.md` for traceability
- Bug 2 (Task 2) depends on Bug 2 subtask 2.1 completing before 2.2 — `in_data_exchange_ex` must exist before `exchange_internal` can call it
- Bug 3 (Task 4) subtask 4.2 depends on 4.1 — the probe order change is safe only after `poll_iso15693` is implemented
- The `sizeof(pointer)` anti-pattern (see AGENTS.md) must not be introduced: always use `PN532_MAX_FRAME_SIZE` for buffer sizes, never `sizeof(ptr)`
- Property tests (Properties 1–10) are defined in `design.md` §Correctness Properties and map directly to the `*` sub-tasks above
- Build uses `./fbt` (SCons wrapper); toolchain must be loaded first via `fbtenv.cmd` / `fbtenv.sh`

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1", "2.1", "5.1"] },
    { "id": 1, "tasks": ["2.2", "4.1", "5.2"] },
    { "id": 2, "tasks": ["1.2", "1.3", "1.4", "1.5", "2.3", "2.4", "4.2", "5.3"] },
    { "id": 3, "tasks": ["4.3", "5.4", "5.5", "5.6"] },
    { "id": 4, "tasks": ["6.1", "6.2"] }
  ]
}
```
