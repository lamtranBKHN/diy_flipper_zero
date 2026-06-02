# NFC MIFARE Freeze Fix — Bugfix Design

## Overview

Two related bugs cause device freezes and incorrect UI routing on the DIY Flipper Zero board (PN532 over I2C) when certain NFC cards are presented:

1. **Bug 1 — MIFARE Classic freeze**: After detecting a MIFARE Classic card without a key cache, the system proceeds to allocate a dictionary attack poller. The PN532 I2C bus is held too long during this allocation/auth sequence, causing the device to hang.

2. **Bug 2 — ATM/bank card (SAK=0x20) crash**: The `sak_is_iso_dep_only` guard in `nfc_scanner_state_handler_find_children_protocols()` correctly identifies SAK=0x20 as ISO-DEP-only and skips non-4A children. However, the MfClassic `detect_type` handler's SAK fallback path can still be reached if the protocol was already queued before the guard fires, or if the guard's filtering is incomplete for edge cases. The MfClassic poller then attempts crypto1 auth on an ISO-DEP-mode card, causing the PN532 to hang waiting for a response that will never come.

The fix strategy is two-fold: (a) ensure the scanner guard completely prevents MfClassic detection attempts on ISO-DEP-only cards, and (b) add a PN532-specific timeout/bail-out in the MfClassic poller's auth path so that even if an incompatible card slips through, the device recovers gracefully instead of freezing.

## Glossary

- **Bug_Condition (C)**: The condition that triggers the bug — PN532 I2C bus held indefinitely during MfClassic auth on an incompatible card, or during dictionary attack allocation on a genuine MfClassic card
- **Property (P)**: The desired behavior — device remains responsive, correct protocol is selected, no freeze occurs
- **Preservation**: Existing behavior that must remain unchanged — genuine MfClassic cards still detected and read, ST25R3916 builds unaffected, multi-protocol menu still shown on non-PN532 builds
- **`nfc_scanner_state_handler_find_children_protocols()`**: Function in `lib/nfc/nfc_scanner.c` that determines which child protocols to probe after base protocol detection
- **`mf_classic_poller_handler_detect_type()`**: Function in `lib/nfc/protocols/mf_classic/mf_classic_poller.c` that classifies a card as MfClassic 1K/4K/Mini based on ATQA/SAK
- **`sak_is_iso_dep_only`**: Boolean flag in the scanner that identifies SAK=0x20 cards with no MIFARE bits (0x08/0x10) as pure ISO-DEP cards
- **SAK**: Select Acknowledge byte from ISO14443-3A anticollision, encodes card capabilities
- **ATQA**: Answer To Request Type A, 2-byte response indicating card type/size
- **ISO-DEP**: ISO14443-4 transport layer; once activated, raw 14443-3A MIFARE frames are rejected

## Bug Details

### Bug Condition

The bugs manifest when a card is placed on the PN532 reader and either:
1. The card is a genuine MIFARE Classic but the dictionary attack allocation/auth sequence holds the I2C bus beyond the PN532's response window, OR
2. The card has SAK=0x20 (ISO-DEP only, e.g. ATM/bank card) and the MfClassic poller attempts crypto1 authentication on a card that has already entered ISO-DEP mode

In both cases, the PN532 chip stops responding on I2C, and the firmware enters an infinite wait loop.

**Formal Specification:**
```
FUNCTION isBugCondition(input)
  INPUT: input of type NfcCardPresentation
  OUTPUT: boolean

  // Bug 1: MfClassic freeze during dictionary attack on PN532
  condition1 := input.hardware = PN532_I2C
                AND input.detected_protocol = MfClassic
                AND input.key_cache_found = false
                AND i2c_bus_held_duration > PN532_RESPONSE_TIMEOUT

  // Bug 2: MfClassic auth attempted on ISO-DEP-only card
  condition2 := input.hardware = PN532_I2C
                AND input.sak = 0x20
                AND NOT (input.sak & 0x18)  // no MF Classic bits
                AND mf_classic_detection_attempted = true

  RETURN condition1 OR condition2
END FUNCTION
```

### Examples

- **ATM card (SAK=0x20, ATQA=0044)**: Scanner finds ISO14443-4A + MfUltralight + MfClassic as children. Guard skips MfUltralight (non-4A) but MfClassic detection is still attempted → PN532 hangs during auth. **Expected**: MfClassic should be skipped entirely; card reported as ISO14443-4A only.
- **MIFARE Classic 1K (SAK=0x08, ATQA=0004)**: No key cache → dictionary attack starts → poller allocation + first auth holds I2C bus for >235ms → PN532 times out internally → device freezes. **Expected**: Auth attempts should have bounded timeouts; I2C bus released between attempts.
- **MIFARE Classic 4K (SAK=0x18, ATQA=0002)**: Same freeze pattern as 1K but with 4K sector count. **Expected**: Same bounded timeout behavior.
- **Genuine dual-interface card (SAK=0x28)**: Has both ISO-DEP bit (0x20) AND MfClassic bit (0x08). **Expected**: MfClassic detection IS attempted (correctly), no regression.

## Expected Behavior

### Preservation Requirements

**Unchanged Behaviors:**
- Genuine MIFARE Classic cards (SAK=0x08, 0x18, 0x09, 0x88, 0x89) must continue to be detected and read correctly
- MIFARE Plus SL2 cards (SAK=0x10, 0x11) must continue to be treated as MfClassic-compatible
- Dual-interface cards (SAK=0x28, 0x38) with both ISO-DEP and MfClassic bits must continue to have MfClassic detection attempted
- ST25R3916 (non-PN532) builds must be completely unaffected — no behavioral changes
- Multi-protocol selection menu must still appear on non-PN532 builds when multiple protocols are genuinely detected
- Key cache reads for known cards must continue to work without entering dictionary attack
- FeliCa, ISO14443-3B, and ISO15693 detection must remain unchanged
- Dictionary attack Skip button behavior must remain unchanged

**Scope:**
All inputs that do NOT involve PN532 + ISO-DEP-only cards or PN532 + MfClassic-without-cache should be completely unaffected by this fix. This includes:
- All card types on ST25R3916 builds
- Cards with valid key caches on PN532 builds
- Non-MfClassic cards (NTAG, Ultralight, FeliCa, etc.) on PN532 builds
- Cards with SAK bits indicating genuine MfClassic capability (0x08, 0x10, 0x18)

## Hypothesized Root Cause

Based on the bug description and code analysis, the most likely issues are:

1. **Incomplete scanner guard for MfClassic on ISO-DEP-only cards (Bug 2)**: The `sak_is_iso_dep_only` guard in `nfc_scanner_state_handler_find_children_protocols()` skips children that do NOT have `NfcProtocolIso14443_4a` as a parent. However, MfClassic's parent chain is `Iso14443_3a → MfClassic` — it does NOT have `Iso14443_4a` as a parent. The guard condition `!nfc_protocol_has_parent(i, NfcProtocolIso14443_4a)` should correctly skip MfClassic. **But**: looking at the crash log showing 3 children (ISO14443-4A, MfUltralight, MfClassic), the guard IS firing but MfClassic is still being probed. This suggests the `nfc_protocol_has_parent()` check for MfClassic may be returning an unexpected value, OR the protocol enumeration order means MfClassic is evaluated before the guard takes effect.

2. **MfClassic `detect_type` SAK fallback accepts SAK=0x20 (Bug 2)**: In `mf_classic_poller_handler_detect_type()`, there IS a guard that rejects `(sak & 0x20) && !(sak & 0x18)`. However, if the poller's `nfc_poller_detect()` call reaches the auth stage before `detect_type` can reject it, the PN532 will already be stuck. The detection flow is: `nfc_poller_detect()` → poller state machine → `detect_type` → auth. If the poller does any I2C communication before reaching the SAK check, the damage is already done.

3. **No I2C bus timeout in MfClassic auth path (Bug 1)**: The `mf_classic_poller_auth()` function calls down to `InCommunicateThru` on the PN532. While the PN532 command timeout was fixed to 1000ms (Bug fix #4 from 2026-05-17), the overall auth retry loop in the dictionary attack has no upper bound on total I2C bus hold time. Multiple consecutive failed auths can accumulate bus hold time beyond what the PN532 can tolerate.

4. **Stack overflow during dictionary attack allocation (Bug 1)**: The scan worker thread was bumped to 12KB, but the dictionary attack loads 2311 keys into RAM. If `keys_dict_alloc()` or the subsequent poller allocation triggers deep recursion or large stack frames, the 12KB may still be insufficient for the PN532 path where I2C framing adds overhead.

## Correctness Properties

Property 1: Bug Condition - ISO-DEP-Only Cards Never Trigger MfClassic Detection

_For any_ card presentation where the hardware is PN532 and SAK=0x20 with no MfClassic bits (SAK & 0x18 == 0), the fixed scanner SHALL NOT include MfClassic in the children protocols list, and SHALL NOT attempt MfClassic authentication, ensuring the device remains responsive.

**Validates: Requirements 2.3, 2.4**

Property 2: Bug Condition - MfClassic Dictionary Attack Does Not Freeze Device

_For any_ MIFARE Classic card presentation on PN532 where no key cache exists, the fixed system SHALL proceed to the dictionary attack scene without freezing, with bounded I2C bus hold times between auth attempts, ensuring the device remains responsive throughout.

**Validates: Requirements 2.1, 2.2**

Property 3: Preservation - Genuine MfClassic Cards Still Detected

_For any_ card presentation where SAK indicates genuine MfClassic capability (SAK & 0x08 or SAK & 0x10, e.g. SAK=0x08, 0x18, 0x28, 0x10, 0x11), the fixed scanner SHALL continue to include MfClassic in the children protocols list and attempt detection, producing the same result as the original code.

**Validates: Requirements 3.1, 3.3**

Property 4: Preservation - Non-PN532 Builds Unaffected

_For any_ card presentation on ST25R3916 (non-PN532) hardware, the fixed code SHALL produce exactly the same behavior as the original code, preserving all existing protocol detection and UI routing logic.

**Validates: Requirements 3.2, 3.4**

## Fix Implementation

### Changes Required

Assuming our root cause analysis is correct:

**File**: `lib/nfc/nfc_scanner.c`

**Function**: `nfc_scanner_state_handler_find_children_protocols()`

**Specific Changes**:
1. **Verify guard logic**: Confirm that the `sak_is_iso_dep_only` guard's skip condition `!nfc_protocol_has_parent(i, NfcProtocolIso14443_4a)` correctly identifies MfClassic as a non-4A child. Add explicit logging to trace which protocols are being skipped vs. allowed through.
2. **Add explicit MfClassic exclusion**: As a belt-and-suspenders measure, add an explicit check: if `sak_is_iso_dep_only` and the protocol is `NfcProtocolMfClassic`, skip it regardless of parent chain analysis. This guards against `nfc_protocol_has_parent()` returning unexpected results for MfClassic.

**File**: `lib/nfc/protocols/mf_classic/mf_classic_poller.c`

**Function**: `mf_classic_poller_handler_detect_type()`

**Specific Changes**:
3. **Early SAK rejection on PN532**: Move the ISO-DEP-only SAK guard to the very top of `detect_type`, before any I2C communication occurs. Ensure that if `(sak & 0x20) && !(sak & 0x18)`, the function returns `MfClassicPollerStateFail` immediately without touching the PN532.

**File**: `lib/nfc/protocols/mf_classic/mf_classic_poller.c`

**Function**: `mf_classic_poller_auth()` / auth retry loop

**Specific Changes**:
4. **Add I2C bus yield between auth attempts**: After each failed auth attempt on PN532, insert a `furi_delay_ms(NFC_SCANNER_INTER_PROBE_YIELD_MS)` to release the I2C bus and allow other peripherals (display, PCF8574) to communicate.
5. **Add cumulative timeout for dictionary attack auth loop**: Track total elapsed time in the auth retry loop. If total time exceeds a threshold (e.g. 5 seconds per sector), bail out of the current sector and move to the next, logging a warning.

**File**: `applications/main/nfc/scenes/nfc_scene_detect.c`

**Function**: `nfc_scene_detect_on_event()`

**Specific Changes**:
6. **PN532-aware protocol count filtering**: On PN532 builds, if `protocol_num > 1` but one of the detected protocols is ISO14443-4A and the SAK indicates ISO-DEP-only, filter out non-4A protocols before deciding whether to show the selection menu. This is a defense-in-depth measure in case the scanner guard is bypassed.

## Testing Strategy

### Validation Approach

The testing strategy follows a two-phase approach: first, surface counterexamples that demonstrate the bug on unfixed code, then verify the fix works correctly and preserves existing behavior.

### Exploratory Bug Condition Checking

**Goal**: Surface counterexamples that demonstrate the bug BEFORE implementing the fix. Confirm or refute the root cause analysis. If we refute, we will need to re-hypothesize.

**Test Plan**: Write tests that simulate PN532 card presentations with SAK=0x20 and trace the scanner's children protocol list. Also simulate MfClassic dictionary attack initiation and measure I2C bus hold times. Run these tests on the UNFIXED code to observe failures.

**Test Cases**:
1. **ISO-DEP Card Scanner Test**: Present a card with SAK=0x20, ATQA=0044 on PN532 build. Verify that MfClassic appears in `children_protocols[]` (will demonstrate bug on unfixed code)
2. **MfClassic Auth on ISO-DEP Card**: Attempt `mf_classic_poller_auth()` on a SAK=0x20 card. Verify that the PN532 hangs or returns timeout (will demonstrate bug on unfixed code)
3. **Dictionary Attack Bus Hold**: Start dictionary attack on a genuine MfClassic card without key cache. Measure cumulative I2C bus hold time (will show >235ms holds on unfixed code)
4. **Protocol Count Routing**: Present SAK=0x20 card, verify detect scene routes to SelectProtocol instead of Read (will demonstrate Bug 2 UI issue on unfixed code)

**Expected Counterexamples**:
- MfClassic is included in children protocols list despite `sak_is_iso_dep_only = true`
- PN532 hangs during `InCommunicateThru` when MfClassic auth is attempted on ISO-DEP card
- Possible causes: `nfc_protocol_has_parent(MfClassic, Iso14443_4a)` returning unexpected value, or MfClassic parent chain including 4A

### Fix Checking

**Goal**: Verify that for all inputs where the bug condition holds, the fixed function produces the expected behavior.

**Pseudocode:**
```
FOR ALL input WHERE isBugCondition(input) DO
  result := handleCardDetection_fixed(input)
  ASSERT device_responsive(result)
  ASSERT NOT mf_classic_auth_attempted_on_iso_dep(result)
  ASSERT i2c_bus_hold_time(result) < MAX_HOLD_THRESHOLD
END FOR
```

### Preservation Checking

**Goal**: Verify that for all inputs where the bug condition does NOT hold, the fixed function produces the same result as the original function.

**Pseudocode:**
```
FOR ALL input WHERE NOT isBugCondition(input) DO
  ASSERT handleCardDetection_original(input) = handleCardDetection_fixed(input)
END FOR
```

**Testing Approach**: Property-based testing is recommended for preservation checking because:
- It generates many SAK/ATQA combinations automatically across the input domain
- It catches edge cases (e.g. SAK=0x28 dual-interface) that manual unit tests might miss
- It provides strong guarantees that genuine MfClassic detection is unchanged

**Test Plan**: Observe behavior on UNFIXED code first for genuine MfClassic cards and non-MfClassic cards, then write property-based tests capturing that behavior.

**Test Cases**:
1. **Genuine MfClassic Preservation**: Verify that cards with SAK=0x08, 0x18, 0x09 continue to have MfClassic in children list after fix
2. **Dual-Interface Preservation**: Verify that SAK=0x28 (ISO-DEP + MfClassic) cards still get MfClassic detection attempted
3. **MIFARE Plus SL2 Preservation**: Verify SAK=0x10, 0x11 cards still treated as MfClassic-compatible
4. **Non-PN532 Build Preservation**: Verify all protocol detection unchanged when `furi_hal_nfc_pn532_is_active()` returns false

### Unit Tests

- Test `nfc_scanner_state_handler_find_children_protocols()` with SAK=0x20 → MfClassic NOT in children list
- Test `nfc_scanner_state_handler_find_children_protocols()` with SAK=0x08 → MfClassic IS in children list
- Test `nfc_scanner_state_handler_find_children_protocols()` with SAK=0x28 → MfClassic IS in children list
- Test `mf_classic_poller_handler_detect_type()` with SAK=0x20 → immediate fail, no auth attempted
- Test `mf_classic_poller_handler_detect_type()` with SAK=0x08 → type detected as 1K
- Test detect scene routing with single protocol → NfcSceneRead
- Test detect scene routing with filtered single protocol on PN532 → NfcSceneRead

### Property-Based Tests

- Generate random SAK values (0x00–0xFF) and verify: if `(sak & 0x20) && !(sak & 0x18)` then MfClassic is NOT in children list on PN532 builds
- Generate random SAK values and verify: if `(sak & 0x08) || (sak & 0x10)` then MfClassic IS in children list (preservation)
- Generate random ATQA/SAK pairs and verify `detect_type` returns fail for ISO-DEP-only SAKs and success for genuine MfClassic SAKs

### Integration Tests

- Full scan flow with SAK=0x20 card on PN532: verify device remains responsive, no freeze
- Full scan flow with SAK=0x08 card on PN532 without key cache: verify dictionary attack starts without freeze
- Full scan flow with SAK=0x28 card on PN532: verify MfClassic detection attempted correctly
- Verify detect scene routes directly to Read (not SelectProtocol) for SAK=0x20 on PN532
