# Bugfix Requirements Document

## Introduction

Three related bugs cause NFC read failures and UI freezes when reading MIFARE Classic cards on the DIY Flipper Zero (STM32WB55 + PN532 over I2C). Bug 1 prevents block reads after successful authentication. Bug 2 causes an infinite loop in the nested attack phase, freezing the UI. Bug 3 causes false-positive MF Classic detection of ISO-DEP cards (e.g., bank cards), leading to failed reads. Together these bugs make MIFARE Classic reading unreliable or impossible on the PN532 backend.

## Bug Analysis

### Current Behavior (Defect)

1.1 WHEN a MIFARE Classic card is authenticated via PN532 native auth (`InDataExchange` with 0x60/0x61) AND a subsequent block read is attempted via `InDataExchange` with the READ command (0x30), THEN the PN532 returns status error 0x06 and the read fails, causing `mf_authed` to be cleared and all further reads to abort.

1.2 WHEN the dictionary attack phase completes with at least one key found AND the nested attack phase begins on a PN532 backend, THEN `mf_classic_poller_get_nt()` always fails because it uses `InCommunicateThru` for raw auth which the PN532 cannot complete, causing the state machine to loop indefinitely between `NestedCollectNt` and `NestedController`, freezing the "Reading MIFARE Classic" screen.

1.3 WHEN a card with SAK=0x20 (ISO-DEP / ISO14443-4 compliant, e.g., bank cards, transit cards) is presented to the NFC reader, THEN the `detect_type` fallback path incorrectly classifies it as MF Classic 1K because the SAK does not match any known ATQA/SAK pair, leading to authentication attempts against a non-MF-Classic card that ultimately fail.

### Expected Behavior (Correct)

2.1 WHEN a MIFARE Classic card is authenticated via PN532 native auth AND a subsequent block read is attempted, THEN the system SHALL successfully read the 16-byte block data without error, maintaining the authenticated session for further reads within the same sector.

2.2 WHEN the dictionary attack phase completes on a PN532 backend (which lacks raw Crypto1 timing support), THEN the system SHALL skip the nested attack phase entirely and proceed directly to the success state with whatever keys were found during the dictionary attack, without freezing or looping.

2.3 WHEN a card with SAK bit 6 set (SAK & 0x20 != 0) is presented AND the card does not also have MF Classic SAK bits (0x08 or 0x18), THEN the system SHALL NOT classify the card as MF Classic and SHALL reject it from the MF Classic read flow.

### Unchanged Behavior (Regression Prevention)

3.1 WHEN a MIFARE Classic card is authenticated via the manual Crypto1 path (non-PN532 or fallback) AND block reads use the encrypted `InCommunicateThru` path, THEN the system SHALL CONTINUE TO read blocks correctly via the existing Crypto1 encryption/decryption logic.

3.2 WHEN the nested attack phase is entered on a non-PN532 backend (e.g., ST25R3916) with raw Crypto1 support, THEN the system SHALL CONTINUE TO perform nested nonce collection and key recovery as before.

3.3 WHEN a genuine MIFARE Classic card with SAK=0x08 (1K), SAK=0x18 (4K), or SAK=0x09 (Mini) is presented, THEN the system SHALL CONTINUE TO correctly detect and classify the card type regardless of whether the ATQA matches known pairs.

3.4 WHEN a card has both ISO-DEP capability (SAK bit 6) AND MF Classic capability (SAK=0x28 or SAK=0x38, i.e., dual-interface cards), THEN the system SHALL CONTINUE TO detect it as MF Classic (these cards legitimately support both protocols).

3.5 WHEN mouse/button interactions occur during NFC reading (e.g., back button to cancel), THEN the system SHALL CONTINUE TO respond to user input without delay or freeze.

3.6 WHEN `InDataExchange` is used for non-MF-Classic operations (ISO-DEP APDUs, FeliCa commands), THEN the system SHALL CONTINUE TO function correctly without interference from the MF Classic auth fix.

### Bug 4: Infinite Loop in fail handler after SAK=0x20 rejection (Bug 3 interaction)

**Current Behavior (Defect)**

4.1 WHEN `mf_classic_poller_handler_detect_type()` sets `instance->state = MfClassicPollerStateFail` (e.g., because the SAK=0x20 guard from Bug 3 fires) AND the state machine subsequently calls `mf_classic_poller_handler_fail()`, THEN `handler_fail` fires the `MfClassicPollerEventTypeFail` callback, receives `NfcCommandContinue` from the caller, and unconditionally resets `instance->state = MfClassicPollerStateDetectType`, causing the state machine to re-enter `detect_type`, re-trigger the SAK=0x20 guard, and loop indefinitely.

**Expected Behavior (Correct)**

4.2 WHEN `mf_classic_poller_handler_fail()` is called AND the fail is terminal (i.e., the card is not MF Classic and there is no retry intent), THEN the MF Classic Poller SHALL return `NfcCommandStop` to the NFC framework, terminating the polling session without resetting state to `MfClassicPollerStateDetectType`.

4.3 WHEN `mf_classic_poller_handler_fail()` is called AND the callback returns `NfcCommandStop`, THEN the MF Classic Poller SHALL propagate `NfcCommandStop` as its own return value and SHALL NOT reset `instance->state` to `MfClassicPollerStateDetectType`.

**Unchanged Behavior (Regression Prevention)**

4.4 WHEN `mf_classic_poller_handler_fail()` is called AND the callback returns `NfcCommandContinue` (indicating a retry is desired by the upper layer), THEN the MF Classic Poller SHALL reset `instance->state = MfClassicPollerStateDetectType` and return `NfcCommandContinue`, preserving the existing retry behavior for callers that expect it.

4.5 WHEN a genuine MIFARE Classic card is presented and authentication fails transiently (e.g., wrong key, card removed mid-session), THEN the fail handler SHALL continue to behave as before: fire the fail callback and allow the upper layer to decide whether to retry or stop.
