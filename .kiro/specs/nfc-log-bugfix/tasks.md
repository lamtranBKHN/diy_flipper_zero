# Implementation Plan

## Overview

Three bugs identified from debug log analysis. All changes are in two files: `targets/f7/furi_hal/furi_hal_pn532.c` and `lib/nfc/nfc_scanner.c`. No new APIs, no new files.

**Language:** C (embedded firmware, STM32WB55 target)

---

## Tasks

- [x] 1. Fix quick-poll timeout: increase from 50ms to PN532_TIMEOUT_POLL_MS
  - File: `targets/f7/furi_hal/furi_hal_nfc_pn532.c`
  - Locate `furi_hal_nfc_quick_poll()` (around line 1988)
  - Change `furi_hal_pn532_poll_iso14443a_timeout(&target, 50)` to `furi_hal_pn532_poll_iso14443a_timeout(&target, PN532_TIMEOUT_POLL_MS)`
  - `PN532_TIMEOUT_POLL_MS` is defined in `furi_hal_pn532.c` as 350ms — this matches the actual `InListPassiveTarget` command duration
  - This eliminates the spurious `PN532 wait ready: timeout` and `pn532_exchange: response timeout` warnings during idle scanning
  - _Requirements: 2.1, 2.2, 2.3_

- [x] 2. Downgrade poll_iso14443a_timeout log levels from [I] to [D]
  - File: `targets/f7/furi_hal/furi_hal_pn532.c`
  - Locate `furi_hal_pn532_poll_iso14443a_timeout()` (around line 1052)
  - Change `FURI_LOG_I(TAG, "poll_iso14443a: ENTER timeout=%lu", timeout_ms)` → `FURI_LOG_D`
  - Change `FURI_LOG_I(TAG, "InListPassiveTarget nb_targets=%u", response[1])` → `FURI_LOG_D`
  - Keep the existing `FURI_LOG_W` for `nb_targets > 1` (multiple cards) unchanged
  - Keep the existing `FURI_LOG_W` for `response_len < 7` (malformed response) unchanged
  - This eliminates the log spam during idle scanning (100+ [I] lines/minute)
  - _Requirements: 5.1, 5.2, 5.3_

- [x] 3. Increase quick-poll confirm threshold from 3 to 5 consecutive hits
  - File: `lib/nfc/nfc_scanner.c`
  - Locate `#define NFC_SCANNER_QP_CONFIRM_THRESHOLD 3` (near top of file)
  - Change to `#define NFC_SCANNER_QP_CONFIRM_THRESHOLD 5`
  - This requires 5 consecutive `InListPassiveTarget` hits before treating a target as genuine, reducing the false-positive confirmation rate from the observed 84% on this noisy hardware
  - Update the comment above the define to reflect the new value and rationale
  - _Requirements: 8.1, 9.1_

- [x] 4. Change QP_MAX_EMPTY_CONFIRMS behavior: reset and continue instead of terminate
  - File: `lib/nfc/nfc_scanner.c`
  - Locate the `qp_empty_confirms >= NFC_SCANNER_QP_MAX_EMPTY_CONFIRMS` check in `nfc_scanner_state_handler_try_base_pollers()`
  - Instead of `instance->state = NfcScannerStateComplete`, reset the counters and continue:
    ```c
    if(instance->qp_empty_confirms >= NFC_SCANNER_QP_MAX_EMPTY_CONFIRMS) {
        FURI_LOG_I(
            TAG,
            "QP max empty confirms (%u) reached, resetting debounce counters "
            "(hits=%lu FP=%lu empty_confirms=%lu)",
            (unsigned)NFC_SCANNER_QP_MAX_EMPTY_CONFIRMS,
            (unsigned long)instance->qp_hits,
            (unsigned long)instance->qp_false_positives,
            (unsigned long)instance->qp_empty_confirms);
        /* Reset debounce state — do NOT terminate. The user may still be
         * trying to present a card. The MAX_EMPTY_ROUNDS cap will terminate
         * the scan if no card is ever detected. */
        instance->qp_empty_confirms = 0;
        instance->qp_consecutive_hits = 0;
        instance->qp_recent_confirm = false;
    }
    ```
  - The `NFC_SCANNER_MAX_EMPTY_ROUNDS = 12` cap remains the authoritative termination condition
  - _Requirements: 8.2, 8.3, 9.2_

- [x] 5. Build verification
  - Run `./fbt TARGET_HW=7 DEBUG=0 COMPACT=1` from the repo root
  - Confirm zero errors and zero new warnings in `targets/f7/furi_hal/furi_hal_nfc_pn532.c` and `lib/nfc/nfc_scanner.c`
  - Confirm firmware.bin size is within expected range (no significant size change expected — these are constant and log-level changes only)
  - _Requirements: all_

---

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1", "2", "3", "4"] },
    { "id": 1, "tasks": ["5"] }
  ]
}
```

## Notes

- Tasks 1–4 are all independent and can be done in parallel (wave 0).
- Task 1 is the most impactful: it eliminates the I2C timeout cascade that was generating 6+ warning lines per quick-poll cycle.
- Task 2 is a pure log-level change with zero runtime behavior impact.
- Task 3 increases the debounce threshold from 3 to 5. On genuine card presentation, 5 consecutive hits at 50ms intervals = 250ms additional latency before the scanner starts a full probe round. This is acceptable given the 84% false-positive rate observed.
- Task 4 changes the `QP_MAX_EMPTY_CONFIRMS` behavior from "terminate scan" to "reset and continue". The `MAX_EMPTY_ROUNDS` cap (12 rounds × ~1s/round = ~12s) remains the authoritative termination condition. This prevents premature scan termination in noisy RF environments.
- The ViewPort lockup warning at t=24998 and the SPI warnings at t=57366/57528 are NOT bugs — they are expected behavior on this hardware (I2C1 bus contention during FAP loading, and SD card write latency during Dolphin state save respectively). No fix needed.
- The `iso15693_3_get_block_size` ELF relocation error at t=27039 is a FAP compiled against a different firmware version (the function IS in `api_symbols.csv` with the correct signature). This is a stale FAP issue, not a firmware bug. The FAP needs to be recompiled against the current firmware.
