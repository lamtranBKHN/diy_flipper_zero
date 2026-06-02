# Requirements Document

## Introduction

This document specifies the requirements for debugging and fixing the NFC subsystem on a DIY Flipper Zero board using a PN532 chip over I2C1 (address 0x48, STM32WB55). The primary issue is that MIFARE Classic authentication fails for all keys — both via PN532 native `InDataExchange` auth (returns 0x14 for all keys) and via the `InCommunicateThru` Crypto1 fallback path (times out with 0x01). Secondary issues include unreliable MfUltralight/NTAG detection and silent failure of MfClassic emulation mode.

## Glossary

- **PN532_HAL**: The hardware abstraction layer in `furi_hal_nfc_pn532.c` that translates Flipper NFC API calls into PN532 I2C commands
- **PN532_Driver**: The low-level I2C transport layer in `furi_hal_pn532.c` that handles frame encoding, ACK, and response parsing
- **MfClassic_Poller**: The MIFARE Classic protocol state machine in `mf_classic_poller.c` and `mf_classic_poller_i.c`
- **InDataExchange**: PN532 command (0x40) that sends application-level data to an in-listed target; interprets 0x60/0x61 as MIFARE auth internally
- **InCommunicateThru**: PN532 command (0x42) that sends raw ISO14443 frames without PN532 interpretation; used as Crypto1 fallback
- **Native_Auth**: The PN532's internal MIFARE Classic authentication via `InDataExchange` with command bytes 0x60/0x61
- **Crypto1_Fallback**: Host-side Crypto1 authentication using `InCommunicateThru` to send raw auth frames when Native_Auth fails
- **NT**: The 4-byte nonce returned by a MIFARE Classic card in response to an authentication command
- **Relist**: Re-polling the card via `InListPassiveTarget` after the card enters HALT state (e.g., after failed auth)
- **NFC_Scanner**: The protocol detection state machine in `nfc_scanner.c` that probes for card types
- **Supported_Card_Plugin**: Application-level plugins that attempt protocol-specific verification after initial card detection

## Requirements

### Requirement 1: Diagnose PN532 Native Auth Failure

**User Story:** As a firmware developer, I want to determine why the PN532 native `InDataExchange` auth returns error 0x14 for all keys including factory defaults, so that I can decide whether to fix or permanently bypass native auth.

#### Acceptance Criteria

1. WHEN the PN532_HAL sends a native auth command via `InDataExchange` with key FFFFFFFFFFFF to a known-good MIFARE Classic 1K card, THE PN532_Driver SHALL log the complete 12-byte auth command frame (cmd, block, key[6], uid[4]) sent to the PN532
2. WHEN the PN532 returns error status 0x14 from native auth, THE PN532_HAL SHALL log the raw response frame bytes for post-mortem analysis
3. WHEN native auth fails with error 0x14, THE PN532_HAL SHALL record whether the UID bytes passed to the auth command match the UID obtained during `InListPassiveTarget` detection
4. WHEN native auth consistently fails for factory-default keys on multiple cards, THE PN532_HAL SHALL expose a diagnostic flag (`pn532_native_auth_broken`) that disables native auth attempts entirely

### Requirement 2: Fix InCommunicateThru Auth Framing

**User Story:** As a firmware developer, I want the `InCommunicateThru` Crypto1 fallback path to correctly frame MIFARE Classic auth commands, so that the card responds with NT instead of timing out.

#### Acceptance Criteria

1. WHEN the Crypto1_Fallback sends an auth command via `InCommunicateThru`, THE PN532_HAL SHALL transmit exactly the bytes [auth_cmd, block_num] with short-frame (7-bit) framing as required by ISO14443-3A for the initial auth request
2. WHEN the PN532_HAL prepares an auth command for `InCommunicateThru`, THE PN532_HAL SHALL NOT append CRC-A bytes to the auth frame (the PN532 `InCommunicateThru` appends CRC automatically when configured with CRC generation enabled)
3. WHEN the PN532_HAL prepares an auth command for `InCommunicateThru`, THE PN532_HAL SHALL verify that the PN532 CIU_ManualRCV register has parity checking disabled for the encrypted NT response
4. WHEN a card responds to the auth command with a 4-byte NT via `InCommunicateThru`, THE PN532_HAL SHALL pass the NT bytes to the Crypto1 engine without stripping or adding CRC bytes
5. IF `InCommunicateThru` returns timeout error (0x01) for the auth command, THEN THE PN532_HAL SHALL verify the card is still in READY state by checking that a prior successful `InListPassiveTarget` occurred within the last 50ms

### Requirement 3: Fix Card State Management Between Auth Attempts

**User Story:** As a firmware developer, I want the card to be in the correct ISO14443-3A state (ACTIVE or IDLE) before each authentication attempt, so that auth commands are not sent to a HALTed card.

#### Acceptance Criteria

1. WHEN a MIFARE Classic auth attempt fails (native or Crypto1), THE PN532_HAL SHALL transition the card back to ACTIVE state via a fresh `InListPassiveTarget` poll before the next auth attempt
2. WHEN `mf_deauth()` is called after a failed auth, THE PN532_HAL SHALL set both `needs_relist=true` and `target_tick=0` to force a physical re-poll on the next exchange
3. WHEN `exchange_internal()` detects a stale target (target_tick older than 5 seconds or target_tick==0), THE PN532_HAL SHALL perform `InListPassiveTarget` with a timeout of at least 50ms before forwarding any command
4. WHEN the MfClassic_Poller calls `mf_classic_poller_halt()` between sector auth attempts, THE MfClassic_Poller SHALL ensure the subsequent auth call triggers a WUPA sequence to wake the card from HALT state

### Requirement 4: Fix MfUltralight/NTAG Detection Reliability

**User Story:** As a user, I want NTAG/MfUltralight cards to be reliably detected and correctly identified, so that I can read their contents without misidentification.

#### Acceptance Criteria

1. WHEN the NFC_Scanner detects an ISO14443-3A card with SAK=0x00 and ATQA matching NTAG/Ultralight patterns, THE NFC_Scanner SHALL attempt the MfUltralight child protocol detection with a timeout of at least 100ms per command
2. WHEN the MfUltralight poller sends a READ command (0x30) via `InCommunicateThru`, THE PN532_HAL SHALL NOT double-append CRC-A to the response (the PN532 already includes CRC in `InCommunicateThru` responses)
3. WHEN `InCommunicateThru` returns a response for MfUltralight READ, THE PN532_HAL SHALL strip the trailing 2-byte CRC from the response before passing it to the protocol layer, yielding exactly 16 bytes for a standard READ response
4. IF the MfUltralight detection times out on the first attempt, THEN THE NFC_Scanner SHALL retry detection once with a fresh `InListPassiveTarget` before declaring the protocol unsupported

### Requirement 5: MfClassic Emulate Mode Error Reporting

**User Story:** As a user, I want clear feedback when attempting to emulate a MIFARE Classic card, so that I understand the hardware limitation instead of seeing a silent failure.

#### Acceptance Criteria

1. WHEN a user selects "Emulate" for a saved MIFARE Classic card, THE NFC application SHALL display a message indicating that MIFARE Classic emulation is not supported on PN532 hardware due to Crypto1 listener limitations
2. WHEN the PN532_HAL receives a request to enter listener mode for MIFARE Classic, THE PN532_HAL SHALL return `FuriHalNfcErrorNotImplemented` within 10ms without attempting any I2C communication
3. THE NFC application SHALL disable or grey out the "Emulate" menu option for MIFARE Classic cards when `furi_hal_nfc_pn532_is_active()` returns true

### Requirement 6: Supported Card Plugin Timeout Optimization

**User Story:** As a user, I want the "Supported Card" verification phase to complete quickly even when authentication fails, so that I am not stuck on a "Reading..." screen for extended periods.

#### Acceptance Criteria

1. WHEN a Supported_Card_Plugin attempts MIFARE Classic authentication and the first auth attempt times out, THE MfClassic_Poller SHALL fail the plugin verification within 2 seconds total (not per-key)
2. WHEN multiple Supported_Card_Plugins are queued for verification, THE NFC application SHALL enforce a cumulative timeout of 5 seconds for all plugin checks combined
3. WHEN a plugin auth attempt fails with timeout, THE MfClassic_Poller SHALL NOT retry the same key on the same sector within the same plugin verification session
4. WHEN all plugin verifications complete (pass or timeout), THE NFC application SHALL proceed to the dict attack scene within 500ms

### Requirement 7: InCommunicateThru CRC and Parity Configuration

**User Story:** As a firmware developer, I want the PN532 CIU registers to be correctly configured for raw MIFARE Classic communication, so that `InCommunicateThru` frames are transmitted and received with correct framing.

#### Acceptance Criteria

1. WHEN the PN532_HAL initiates a MIFARE Classic auth sequence via `InCommunicateThru`, THE PN532_Driver SHALL configure CIU_ManualRCV to disable automatic parity checking (bit 4 = 1) for the encrypted card response
2. WHEN the PN532_HAL sends the initial auth command (0x60/0x61 + block) via `InCommunicateThru`, THE PN532_Driver SHALL ensure CIU_BitFraming is set to 0x00 (full 8-bit frames, no partial bits)
3. WHEN the PN532 receives the 4-byte encrypted NT response from the card, THE PN532_HAL SHALL interpret the response as raw bytes without CRC validation (MIFARE auth responses do not include CRC-A)
4. WHEN transitioning from MIFARE Classic auth to normal ISO14443-3A communication, THE PN532_Driver SHALL restore CIU_ManualRCV to its default value (automatic parity enabled)

### Requirement 8: Auth Flow Integration Testing

**User Story:** As a firmware developer, I want a diagnostic mode that exercises the complete auth flow step-by-step, so that I can isolate which stage of the auth handshake fails.

#### Acceptance Criteria

1. WHEN diagnostic mode is enabled via a compile-time flag (`NFC_AUTH_DIAG`), THE PN532_HAL SHALL log each stage of the auth flow: (a) InListPassiveTarget result, (b) native auth attempt and response, (c) deauth/relist, (d) InCommunicateThru auth frame sent, (e) InCommunicateThru response or timeout
2. WHEN diagnostic mode captures a successful NT response from `InCommunicateThru`, THE PN532_HAL SHALL log the 4 NT bytes and the subsequent Crypto1 handshake frames (NR+AR sent, card response)
3. WHEN diagnostic mode is disabled (default), THE PN532_HAL SHALL NOT include any diagnostic logging code in the compiled binary (zero flash cost when disabled)
