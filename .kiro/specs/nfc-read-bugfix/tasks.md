# Tasks

## Task 1: Fix PN532 MIFARE Classic auth to use InDataExchange native path [bug1-auth-fix]
- [x] 1.1 Verify `furi_hal_pn532_mf_auth()` in `targets/f7/furi_hal/furi_hal_pn532.c` correctly sends InDataExchange (0x40) with payload [target_num, 0x60/0x61, block_num, key(6), uid(4)] and confirm it exists and is wired correctly
- [x] 1.2 Verify `furi_hal_nfc_pn532_mf_auth_native()` in `targets/f7/furi_hal/furi_hal_nfc_pn532.c` calls `furi_hal_pn532_mf_auth()` and sets `mf_authed = true` on success
- [x] 1.3 Verify `mf_classic_poller_auth()` in `lib/nfc/protocols/mf_classic/mf_classic_poller_i.c` correctly routes to the PN532 native auth path when PN532 is active, setting `instance->pn532_mf_authed = true`
- [x] 1.4 Verify `mf_classic_poller_read_block()` in `lib/nfc/protocols/mf_classic/mf_classic_poller_i.c` checks `pn532_mf_authed` and calls `furi_hal_nfc_pn532_mf_read_block()` which uses InDataExchange for the read
- [x] 1.5 If any of the above verifications fail, fix the wiring so that auth uses InDataExchange (0x40) and reads also use InDataExchange (0x40), ensuring PN532 manages Crypto1 state internally throughout the session

## Task 2: Fix nested attack infinite loop on PN532 [bug2-nested-loop-fix]
- [x] 2.1 Add PN532 guard at the top of `mf_classic_poller_handler_nested_controller()` in `lib/nfc/protocols/mf_classic/mf_classic_poller.c`: if `furi_hal_nfc_pn532_is_active()` returns true, log "PN532: skipping nested attack (unsupported)", set `instance->state = MfClassicPollerStateSuccess`, and return `NfcCommandContinue`
- [x] 2.2 Ensure the guard is placed BEFORE any existing logic in the function (first executable statement after variable declarations)
- [x] 2.3 Verify the `#include "furi_hal_nfc_pn532.h"` is already present at the top of `mf_classic_poller.c` (it should be from prior work)

## Task 3: Fix SAK=0x20 false-positive MF Classic detection [bug3-sak-guard]
- [x] 3.1 In `mf_classic_poller_handler_detect_type()` in `lib/nfc/protocols/mf_classic/mf_classic_poller.c`, add a guard BEFORE the SAK fallback block: if `(sak & 0x20) && !(sak & 0x18)`, log "SAK 0x%02X: ISO-DEP only, not MF Classic", set `instance->state = MfClassicPollerStateFail`, and return `command`
- [x] 3.2 Verify dual-interface cards (SAK=0x28 with bits 0x20|0x08, SAK=0x38 with bits 0x20|0x18) still pass the guard and are correctly detected as MF Classic

## Task 4: Build verification [build-verify]
- [x] 4.1 Run `./fbt TARGET_HW=7 DEBUG=0 COMPACT=1` and verify the build succeeds
- [x] 4.2 Verify firmware.bin size is under 860KB (860,160 bytes)

## Task 5: Fix infinite loop in fail handler after SAK=0x20 rejection [bug4-fail-handler-fix]
- [x] 5.1 In `mf_classic_poller_handler_fail()` in `lib/nfc/protocols/mf_classic/mf_classic_poller.c`, wrap the `instance->state = MfClassicPollerStateDetectType` assignment in `if(command == NfcCommandContinue) { ... }` so the reset only occurs when the callback returns `NfcCommandContinue`
- [x] 5.2 Verify that when the callback returns `NfcCommandStop`, the function returns `NfcCommandStop` without modifying `instance->state`
- [x] 5.3 Verify that when the callback returns `NfcCommandContinue`, the function still resets `instance->state = MfClassicPollerStateDetectType` (regression: existing retry behavior preserved)
- [x] 5.4 Run `./fbt TARGET_HW=7 DEBUG=0 COMPACT=1` and verify the build succeeds with no new warnings
