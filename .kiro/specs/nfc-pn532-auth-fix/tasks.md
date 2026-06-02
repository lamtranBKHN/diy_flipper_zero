# Implementation Plan: NFC PN532 MIFARE Classic Auth Fix

## Overview

This plan implements the MIFARE Classic authentication fix for the PN532 module. The core issue is double-CRC framing and incorrect RxCRC/parity settings during `InCommunicateThru` auth. Tasks are ordered by dependency: CIU helper first, then auth interception fix, then secondary fixes (native auth bypass, MfUltralight retry, emulate guard, plugin timeout). Each task is independently buildable.

## Tasks

- [x] 1. Implement CIU register helper function
  - [x] 1.1 Add CIU register defines and `furi_hal_pn532_mf_auth_configure_ciu()` to `furi_hal_pn532.c`
    - Add `#define PN532_REG_CIU_RxMode 0x6303`, `PN532_REG_CIU_ManualRCV 0x630D`
    - Add default/auth-mode constants (`PN532_CIU_RXMODE_DEFAULT 0x08`, `PN532_CIU_RXMODE_NO_CRC 0x00`, `PN532_CIU_MANUAL_RCV_DEFAULT 0x00`, `PN532_CIU_MANUAL_RCV_NO_PAR 0x10`)
    - Implement `furi_hal_pn532_mf_auth_configure_ciu(bool auth_mode)` using existing `pn532_write_register()` helper
    - When `auth_mode=true`: write RxCRCEn=0 and ParityDisable=1
    - When `auth_mode=false`: restore both registers to defaults
    - Return `FuriHalPn532ErrorComm` on write failure, attempt best-effort restore
    - _Requirements: 7.1, 7.2, 7.4_

  - [x] 1.2 Declare `furi_hal_pn532_mf_auth_configure_ciu()` in `furi_hal_pn532.h`
    - Add function prototype with doxygen comment explaining purpose
    - _Requirements: 7.1_

- [x] 2. Fix `exchange_internal()` auth interception path
  - [x] 2.1 Add auth command detection and CRC stripping in `furi_hal_nfc_pn532.c`
    - In `exchange_internal()`, detect auth commands: `tx_bytes[0] == 0x60 || tx_bytes[0] == 0x61`
    - When auth detected and `send_len >= 4` (cmd + block + 2 CRC bytes from ISO layer): set `auth_len = 2` (strip trailing CRC)
    - When auth detected and `send_len == 2` (no CRC appended): use as-is
    - _Requirements: 2.1, 2.2_

  - [x] 2.2 Integrate CIU configuration into auth exchange flow
    - Before `InCommunicateThru` for auth: call `furi_hal_pn532_mf_auth_configure_ciu(true)`
    - If CIU config fails: call `furi_hal_pn532_mf_auth_configure_ciu(false)` for restore, return error
    - After `InCommunicateThru` completes (success or failure): call `furi_hal_pn532_mf_auth_configure_ciu(false)`
    - Use `PN532_TIMEOUT_MF_AUTH_MS` (define as 200ms if not already present) for auth timeout
    - _Requirements: 2.3, 7.1, 7.3, 7.4_

  - [x] 2.3 Handle NT response passthrough
    - On successful auth response (`rx_len == 4`): pass raw 4 bytes to `furi_hal_nfc_pn532_prepare_rx()` with `add_parity=false`, `use_comm_thru=true`
    - Set `furi_hal_nfc_pn532.mf_authed = true`
    - On timeout or unexpected response length: set `needs_relist = true`, return appropriate error
    - _Requirements: 2.4, 7.3_

  - [ ]* 2.4 Write property test for auth frame CRC stripping (Property 1)
    - **Property 1: Auth Frame CRC Stripping**
    - For any auth command (0x60/0x61) and any block number (0x00–0xFF), verify that a 4-byte input [cmd, block, CRC_L, CRC_H] results in exactly 2 bytes [cmd, block] passed to InCommunicateThru
    - Mock I2C layer to capture transmitted frame
    - Minimum 100 iterations with random block numbers
    - **Validates: Requirements 2.1, 2.2**

  - [ ]* 2.5 Write property test for CIU register round-trip (Property 2)
    - **Property 2: CIU Register Round-Trip**
    - For any auth sequence (random cmd, block, outcome), verify CIU_RxMode and CIU_ManualRCV are restored to defaults after auth completes
    - Mock `pn532_write_register()` to track register state
    - Test success path, timeout path, and CIU config failure path
    - Minimum 100 iterations
    - **Validates: Requirements 2.3, 7.1, 7.2, 7.4**

  - [ ]* 2.6 Write property test for NT passthrough integrity (Property 3)
    - **Property 3: NT Passthrough Integrity**
    - For any 4-byte NT value, verify bytes delivered to prepare_rx are identical to raw bytes received from InCommunicateThru
    - Generate random 4-byte NT values, mock InCommunicateThru response
    - Minimum 100 iterations
    - **Validates: Requirements 2.4, 7.3**

- [x] 3. Checkpoint - Verify core auth fix builds
  - Ensure all tests pass, ask the user if questions arise.
  - Run `./fbt` from repo root to verify clean build
  - Verify no new warnings in `furi_hal_pn532.c` or `furi_hal_nfc_pn532.c`

- [x] 4. Disable PN532 native auth via compile-time flag
  - [x] 4.1 Add `PN532_NATIVE_AUTH_DISABLED` flag to `mf_classic_poller_i.c`
    - Add `#ifndef PN532_NATIVE_AUTH_DISABLED` / `#define PN532_NATIVE_AUTH_DISABLED 1` / `#endif` near top of file
    - Wrap the native auth path in `mf_classic_poller_auth_common()` with `#if !PN532_NATIVE_AUTH_DISABLED`
    - The wrapped code is the block that calls `furi_hal_nfc_pn532_is_active()` and attempts `InDataExchange` auth
    - When disabled, execution falls through directly to Crypto1 fallback path
    - _Requirements: 1.4_

- [x] 5. Add MfUltralight detection retry logic
  - [x] 5.1 Implement retry with fresh poll in `nfc_scanner.c`
    - In the MfUltralight/NTAG child protocol detection path (SAK=0x00, ATQA matching UL patterns)
    - If first detection attempt times out: perform fresh `InListPassiveTarget` poll
    - Retry MfUltralight detection once after successful re-poll
    - If retry also fails: declare protocol unsupported (existing behavior)
    - Ensure detection timeout is at least 100ms per command
    - _Requirements: 4.1, 4.4_

  - [ ]* 5.2 Write property test for MfUltralight READ response length (Property 6)
    - **Property 6: MfUltralight READ Response Length**
    - For any MfUltralight READ response of 18 bytes (16 data + 2 CRC), verify exactly 16 bytes delivered to protocol layer
    - Generate random 16-byte payloads, append valid CRC, mock InCommunicateThru response
    - Minimum 100 iterations
    - **Validates: Requirements 4.2, 4.3**

- [x] 6. Add emulate mode guard for PN532
  - [x] 6.1 Block MfClassic emulation on PN532 in `mf_classic.c`
    - In the emulate scene entry (or protocol support handler), check `furi_hal_nfc_pn532_is_active()`
    - If PN532 active and protocol is MfClassic: return `FuriHalNfcErrorNotImplemented` immediately
    - Display user-facing message: "MF Classic emulation not supported on PN532"
    - Do not attempt any I2C communication for this path
    - _Requirements: 5.1, 5.2, 5.3_

- [x] 7. Add cumulative timeout enforcement for supported card plugins
  - [x] 7.1 Implement per-plugin and cumulative timeout in `nfc_supported_cards.c`
    - Define `SUPPORTED_CARD_PLUGIN_TIMEOUT_MS 2000` (2s per plugin)
    - Define `SUPPORTED_CARD_TOTAL_TIMEOUT_MS 5000` (5s cumulative)
    - Track `total_start = furi_get_tick()` before plugin loop
    - Break out of plugin loop if cumulative timeout exceeded
    - Within each plugin: fail auth verification if first auth times out (no per-key retry within same session)
    - After all plugins complete: proceed to dict attack scene within 500ms
    - _Requirements: 6.1, 6.2, 6.3, 6.4_

- [x] 8. Add diagnostic logging (compile-time gated)
  - [x] 8.1 Add `NFC_AUTH_DIAG` compile-time logging to `furi_hal_nfc_pn532.c`
    - Define `AUTH_DIAG_LOG` macro: expands to `FURI_LOG_I("AuthDiag", ...)` when `NFC_AUTH_DIAG` defined, otherwise `do {} while(0)`
    - Add log points at each auth stage: InListPassiveTarget result, CIU config, auth frame sent, response/timeout, NT bytes, Crypto1 handshake frames
    - Verify zero flash cost when `NFC_AUTH_DIAG` is not defined (macro expands to nothing)
    - _Requirements: 8.1, 8.2, 8.3_

- [x] 9. Implement deauth state reset fix
  - [x] 9.1 Fix `furi_hal_nfc_pn532_mf_deauth()` state management
    - Ensure `mf_deauth()` sets: `needs_relist = true`, `mf_authed = false`, `target_tick = 0`
    - Ensure `exchange_internal()` checks stale target: re-poll if `target_tick == 0` or elapsed > `PN532_TARGET_FRESHNESS_TIMEOUT_MS`
    - After successful re-poll: update `target_tick` to current tick, set `needs_relist = false`
    - _Requirements: 3.1, 3.2, 3.3, 3.4_

  - [ ]* 9.2 Write property test for deauth state reset (Property 4)
    - **Property 4: Deauth State Reset**
    - For any prior state of mf_authed, needs_relist, target_tick: after calling mf_deauth(), verify needs_relist=true, mf_authed=false, target_tick=0
    - Generate random initial states, call deauth, verify postconditions
    - Minimum 100 iterations
    - **Validates: Requirements 3.1, 3.2**

  - [ ]* 9.3 Write property test for stale target detection (Property 5)
    - **Property 5: Stale Target Detection**
    - For any target_tick value and current_tick, verify re-poll triggered iff target_tick==0 OR elapsed > timeout
    - Generate random tick values, mock InListPassiveTarget, verify re-poll decision
    - Minimum 100 iterations
    - **Validates: Requirements 3.3, 3.4**

- [x] 10. Final checkpoint - Full build verification
  - Ensure all tests pass, ask the user if questions arise.
  - Run `./fbt` from repo root — must compile cleanly with no new warnings
  - Verify firmware.bin size stays within 28KB free flash budget
  - Confirm `NFC_AUTH_DIAG` disabled by default (no flash cost)

## Notes

- Tasks marked with `*` are optional and can be skipped for faster MVP
- Each task references specific requirements for traceability
- Property tests validate universal correctness properties from the design document
- The implementation language is C (embedded firmware, matching existing codebase)
- Build verification: `./fbt` from repo root (target f7, default)
- No unit test framework available on-device; property tests use mock harness with parameterized loops
- Tasks 1–2 are the critical path (core auth fix); tasks 4–9 are secondary fixes that can proceed in parallel
- 28KB free flash budget — all changes must fit within this constraint
