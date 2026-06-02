# Implementation Plan

## Overview

Fix two related NFC bugs on the DIY Flipper Zero (PN532 over I2C) that cause device freezes: (1) MfClassic detection attempted on ISO-DEP-only cards (SAK=0x20) causing PN532 hang, and (2) dictionary attack auth loop holding I2C bus indefinitely on genuine MfClassic cards without key cache. The fix adds explicit MfClassic exclusion in the scanner guard, early SAK rejection in detect_type, I2C bus yield in the auth loop, and PN532-aware protocol filtering in the detect scene.

## Tasks

- [ ] 1. Write bug condition exploration test
  - **Property 1: Bug Condition** - ISO-DEP-Only Card Triggers MfClassic Detection on PN532
  - **CRITICAL**: This test MUST FAIL on unfixed code - failure confirms the bug exists
  - **DO NOT attempt to fix the test or the code when it fails**
  - **NOTE**: This test encodes the expected behavior - it will validate the fix when it passes after implementation
  - **GOAL**: Surface counterexamples that demonstrate the bug exists
  - **Scoped PBT Approach**: Scope the property to concrete failing cases: SAK=0x20 with no MfClassic bits (SAK & 0x18 == 0) on PN532 hardware where `furi_hal_nfc_pn532_is_active()` returns true
  - Test that `nfc_scanner_state_handler_find_children_protocols()` with SAK=0x20 does NOT include `NfcProtocolMfClassic` in `children_protocols[]` (from Bug Condition in design: `isBugCondition(input)` where `input.sak = 0x20 AND NOT (input.sak & 0x18) AND mf_classic_detection_attempted = true`)
  - Test that `mf_classic_poller_handler_detect_type()` with SAK=0x20 returns `MfClassicPollerStateFail` immediately without any I2C communication (from Expected Behavior: device remains responsive, no auth attempted on ISO-DEP card)
  - Generate SAK values where `(sak & 0x20) && !(sak & 0x18)` (i.e. SAK=0x20, 0x60, 0xA0, 0xE0) and verify MfClassic is excluded from children list
  - Run test on UNFIXED code
  - **EXPECTED OUTCOME**: Test FAILS — MfClassic IS included in children_protocols despite sak_is_iso_dep_only=true (this proves the bug exists: the guard fires but MfClassic still gets through because `furi_hal_nfc_pn532_target_is_valid()` returns false after InRelease, so `sak_is_iso_dep_only` stays false)
  - Document counterexamples found: e.g. "SAK=0x20, ATQA=0044 → MfClassic (protocol 7) appears in children list with 3 children total (ISO14443-4A, MfUltralight, MfClassic)"
  - Mark task complete when test is written, run, and failure is documented
  - _Requirements: 1.3, 2.3, 2.4_

- [ ] 2. Write preservation property tests (BEFORE implementing fix)
  - **Property 2: Preservation** - Genuine MfClassic Cards and Non-PN532 Builds Unaffected
  - **IMPORTANT**: Follow observation-first methodology
  - Observe: `find_children_protocols()` with SAK=0x08 (genuine MfClassic 1K) includes MfClassic in children list on unfixed code
  - Observe: `find_children_protocols()` with SAK=0x18 (genuine MfClassic 4K) includes MfClassic in children list on unfixed code
  - Observe: `find_children_protocols()` with SAK=0x28 (dual-interface: ISO-DEP + MfClassic) includes MfClassic in children list on unfixed code
  - Observe: `find_children_protocols()` with SAK=0x10 (MIFARE Plus SL2 1K) includes MfClassic in children list on unfixed code
  - Observe: `find_children_protocols()` with SAK=0x11 (MIFARE Plus SL2 4K) includes MfClassic in children list on unfixed code
  - Observe: `mf_classic_poller_handler_detect_type()` with SAK=0x08, ATQA=0x0004 returns type=1K on unfixed code
  - Observe: On non-PN532 builds (`furi_hal_nfc_pn532_is_active()` returns false), all protocol detection unchanged
  - Write property-based test: for all SAK values where `(sak & 0x08) || (sak & 0x10)`, MfClassic IS included in children protocols list (from Preservation Requirements in design)
  - Write property-based test: for all SAK values where `(sak & 0x20) && (sak & 0x18)` (dual-interface), MfClassic IS included in children protocols list
  - Write property-based test: for non-PN532 builds, all protocol detection produces identical results regardless of SAK value
  - Verify tests pass on UNFIXED code
  - **EXPECTED OUTCOME**: Tests PASS (this confirms baseline behavior to preserve)
  - Mark task complete when tests are written, run, and passing on unfixed code
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6_

- [ ] 3. Fix for NFC MIFARE freeze on PN532 with ISO-DEP-only and MfClassic cards

  - [ ] 3.1 Add explicit MfClassic exclusion in scanner guard (`lib/nfc/nfc_scanner.c`)
    - In `nfc_scanner_state_handler_find_children_protocols()`, add belt-and-suspenders check: if `sak_is_iso_dep_only` AND protocol is `NfcProtocolMfClassic`, skip it regardless of `nfc_protocol_has_parent()` result
    - Cache the SAK from the detection phase so it's available even if `furi_hal_nfc_pn532_target_is_valid()` returns false after InRelease (root cause: SAK is only read when target_valid=true, but target may become invalid between detection and children enumeration)
    - Add explicit logging when MfClassic is skipped by the new guard
    - _Bug_Condition: isBugCondition(input) where input.hardware = PN532_I2C AND input.sak = 0x20 AND NOT (input.sak & 0x18) AND mf_classic_detection_attempted = true_
    - _Expected_Behavior: MfClassic NOT in children_protocols[], device remains responsive_
    - _Preservation: Genuine MfClassic cards (SAK & 0x08 or SAK & 0x10) still included; dual-interface (SAK & 0x20 && SAK & 0x18) still included; non-PN532 builds unaffected_
    - _Requirements: 1.3, 2.3, 2.4, 3.1, 3.2, 3.3_

  - [ ] 3.2 Move SAK=0x20 rejection to top of detect_type (`lib/nfc/protocols/mf_classic/mf_classic_poller.c`)
    - In `mf_classic_poller_handler_detect_type()`, move the ISO-DEP-only SAK guard `(sak & 0x20) && !(sak & 0x18)` to the very first check, BEFORE any ATQA/SAK pair matching
    - On PN532 builds, this ensures no I2C communication occurs before the rejection — defense-in-depth in case the scanner guard is bypassed
    - Return `MfClassicPollerStateFail` immediately with `NfcCommandReset`
    - _Bug_Condition: MfClassic detect_type reached with SAK=0x20 on PN532 → auth attempted → PN532 hangs_
    - _Expected_Behavior: Immediate fail return, no I2C communication, no auth attempt_
    - _Preservation: Cards with SAK & 0x18 (genuine MfClassic bits) still pass through to type detection_
    - _Requirements: 1.3, 2.3, 3.1, 3.3_

  - [ ] 3.3 Add I2C bus yield and cumulative timeout for dictionary attack auth (`lib/nfc/protocols/mf_classic/mf_classic_poller.c`)
    - In the dictionary attack auth retry loop (inside `mf_classic_poller_handler_request_read_sector_blocks()` dict key iteration), add `furi_delay_ms(NFC_SCANNER_INTER_PROBE_YIELD_MS)` between consecutive auth attempts to release I2C bus
    - Add cumulative timeout tracking: if total elapsed time in the dict key loop exceeds 5000ms per sector, bail out and move to next sector with a warning log
    - This prevents Bug 1 (MfClassic freeze during dictionary attack) where consecutive failed auths accumulate I2C bus hold time beyond PN532 tolerance
    - _Bug_Condition: isBugCondition(input) where input.protocol = MfClassic AND input.hardware = PN532_I2C AND input.key_cache_found = false AND i2c_bus_held_duration > PN532_RESPONSE_TIMEOUT_
    - _Expected_Behavior: Device remains responsive, dict attack proceeds with bounded I2C hold times_
    - _Preservation: Dictionary attack still attempts all keys within timeout; Skip button still works; key cache reads unaffected_
    - _Requirements: 1.1, 1.2, 2.1, 2.2, 3.1, 3.5, 3.6_

  - [ ] 3.4 Add PN532-aware protocol count filtering in detect scene (`applications/main/nfc/scenes/nfc_scene_detect.c`)
    - In `nfc_scene_detect_on_event()`, when `protocol_num > 1` on PN532 builds, check if the detected protocols include ISO14443-4A and the SAK indicates ISO-DEP-only
    - If so, filter out non-4A protocols (MfClassic, MfUltralight) before deciding whether to show SelectProtocol menu
    - If after filtering only 1 protocol remains, route directly to `NfcSceneRead` instead of `NfcSceneSelectProtocol`
    - This is defense-in-depth: if the scanner guard is somehow bypassed and multiple protocols are reported, the UI still does the right thing
    - _Bug_Condition: Scanner reports protocol_num > 1 with ISO-DEP-only card on PN532_
    - _Expected_Behavior: Single protocol selected, routes to NfcSceneRead, no selection menu shown_
    - _Preservation: Non-PN532 builds with genuine multi-protocol cards still show SelectProtocol menu; genuine dual-interface cards still show menu_
    - _Requirements: 1.4, 2.4, 3.2, 3.4_

  - [ ] 3.5 Verify bug condition exploration test now passes
    - **Property 1: Expected Behavior** - ISO-DEP-Only Card Never Triggers MfClassic Detection
    - **IMPORTANT**: Re-run the SAME test from task 1 - do NOT write a new test
    - The test from task 1 encodes the expected behavior: SAK=0x20 cards must NOT have MfClassic in children list, and detect_type must reject immediately
    - When this test passes, it confirms the expected behavior is satisfied
    - Run bug condition exploration test from step 1
    - **EXPECTED OUTCOME**: Test PASSES (confirms bug is fixed — MfClassic excluded from children, detect_type rejects SAK=0x20 immediately)
    - _Requirements: 2.3, 2.4_

  - [ ] 3.6 Verify preservation tests still pass
    - **Property 2: Preservation** - Genuine MfClassic Cards and Non-PN532 Builds Unaffected
    - **IMPORTANT**: Re-run the SAME tests from task 2 - do NOT write new tests
    - Run preservation property tests from step 2
    - **EXPECTED OUTCOME**: Tests PASS (confirms no regressions — genuine MfClassic cards still detected, dual-interface cards still work, non-PN532 builds unchanged)
    - Confirm all tests still pass after fix (no regressions)

- [ ] 4. Checkpoint - Ensure all tests pass
  - Run full test suite to confirm no regressions
  - Verify bug condition test (Property 1) passes — ISO-DEP-only cards no longer trigger MfClassic detection
  - Verify preservation test (Property 2) passes — genuine MfClassic, dual-interface, and non-PN532 behavior unchanged
  - Build firmware with `./fbt` to confirm no compile errors
  - Ensure all tests pass, ask the user if questions arise.

## Task Dependency Graph

```json
{
  "waves": [
    ["1", "2"],
    ["3.1", "3.2", "3.3", "3.4"],
    ["3.5"],
    ["3.6"],
    ["4"]
  ]
}
```

## Notes

- Tasks 1 and 2 MUST be completed BEFORE any implementation (tasks 3.x). The exploration test is expected to FAIL on unfixed code (confirming the bug), while preservation tests are expected to PASS on unfixed code (confirming baseline behavior).
- The root cause is that `sak_is_iso_dep_only` is only set when `furi_hal_nfc_pn532_target_is_valid()` returns true, but the target may become invalid between detection and children enumeration (after InRelease). Task 3.1 addresses this by caching the SAK from the detection phase.
- Task 3.2 (detect_type early rejection) is defense-in-depth — even if MfClassic somehow gets into the children list, the poller will reject it before any I2C communication.
- Task 3.3 addresses Bug 1 (dictionary attack freeze) independently of Bug 2 (ISO-DEP card freeze).
- Task 3.4 is UI-level defense-in-depth for the detect scene routing.
- All changes are gated behind `furi_hal_nfc_pn532_is_active()` to ensure ST25R3916 builds are completely unaffected.
