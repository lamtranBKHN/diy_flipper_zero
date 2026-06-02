# Bugfix Requirements Document

## Introduction

Four bugs degrade NFC emulation and scanning reliability on the DIY Flipper Zero (STM32WB55 + PN532 over I2C). Bug 1 causes 4-byte and 7-byte UID emulation to fail because `TgInitAsTarget` only receives the first 3 UID bytes. Bug 2 causes ISO-DEP multi-block (chained) card responses to be silently truncated, returning only the first fragment to the caller. Bug 3 prevents ISO15693 cards from being detected because the protocol is excluded from the PN532 scanner probe list due to a now-resolved I2C deadlock concern. Bug 4 causes FeliCa emulation to always advertise a hardcoded NFCID2t regardless of the card being emulated, making readers reject the emulated FeliCa card. Together these bugs make card emulation unreliable for the majority of real-world ISO14443-3A cards and make FeliCa emulation non-functional.

## Glossary

- **PN532**: NXP PN532 NFC controller connected to STM32WB55 via I2C at address 0x48.
- **TgInitAsTarget**: PN532 command that configures the chip as an NFC target (emulation mode). Accepts a 36-byte parameter block.
- **NFCID1**: 3-byte field in the `TgInitAsTarget` parameter block (bytes 3–5) used for ISO14443-3A anticollision at 106 kbps.
- **NFCID2t**: 8-byte field in the `TgInitAsTarget` parameter block (bytes 7–14 of the FeliCa params section) used for FeliCa target identification.
- **NFCID3t**: 10-byte field in the `TgInitAsTarget` parameter block (bytes 25–34) used for ISO-DEP target identification.
- **Cascade Tag (CT)**: Byte value `0x88` used as `NFCID1[0]` to signal to a reader that the UID continues in the next anticollision level.
- **SAK**: Select Acknowledge byte returned during ISO14443-3A SELECT. Bit 2 (`0x04`) signals that the UID is not complete (cascade continues).
- **PCB**: Protocol Control Byte — the first byte of an ISO14443-4 I-block, R-block, or S-block.
- **Chaining bit**: Bit 4 of the PCB byte in an ISO14443-4 I-block. When set (`PCB & 0x10 != 0`), the sender has more data to follow.
- **R(ACK)**: ISO14443-4 Receive-Ready Acknowledgement block sent by the receiver to request the next chained fragment.
- **InDataExchange**: PN532 command used to exchange data with an activated target.
- **InListPassiveTarget**: PN532 command used to detect and activate a passive NFC target.
- **ISO15693**: ISO/IEC 15693 vicinity card standard. PN532 supports it via `InListPassiveTarget` with baud rate code `0x05`.
- **INVENTORY response**: ISO15693 response to a INVENTORY command containing DSFID (1 byte) and UID (8 bytes, LSB first).
- **HAL**: Hardware Abstraction Layer — `furi_hal_nfc_pn532.c` and `furi_hal_pn532.c`.
- **NfcScanner**: `lib/nfc/nfc_scanner.c` — the protocol detection state machine that drives the PN532 probe loop.
- **listener_wait_event**: `furi_hal_nfc_pn532_listener_wait_event()` in `furi_hal_nfc_pn532.c` — the function that initialises `TgInitAsTarget` and waits for reader commands.
- **exchange_internal**: `furi_hal_nfc_pn532_exchange_internal()` in `furi_hal_nfc_pn532.c` — the central ISO-DEP/raw exchange dispatcher.
- **set_col_res_data**: `furi_hal_nfc_pn532_listener_set_col_res_data()` in `furi_hal_nfc_pn532.c` — stores UID/ATQA/SAK for use by `listener_wait_event`.

---

## Bug Analysis

### Bug 1: 4/7-byte UID Emulation Broken (TgInitAsTarget NFCID1 Truncation)

#### Current Behavior (Defect)

1.1 WHEN `furi_hal_nfc_pn532_listener_wait_event()` initialises `TgInitAsTarget` for ISO14443-3A target mode AND `listener_uid_len` is 4 or 7, THEN the HAL copies only `listener_uid[0..2]` into `params[3..5]` (NFCID1), truncating the UID to 3 bytes, causing the PN532 to respond to anticollision with an incorrect NFCID1 that does not match the card being emulated.

1.2 WHEN a reader performs ISO14443-3A anticollision against the emulated target AND the emulated card has a 4-byte UID, THEN the reader receives a 3-byte NFCID1 without a Cascade Tag prefix and without SAK bit 2 set, causing the reader to treat the UID as complete and reject the card when the full UID does not match.

1.3 WHEN a reader performs ISO14443-3A anticollision against the emulated target AND the emulated card has a 7-byte UID, THEN the reader receives a 3-byte NFCID1 without double-cascade encoding, causing the reader to reject the emulated card after the first anticollision level.

### Expected Behavior (Correct)

2.1 WHEN `listener_uid_len` is 4, THEN `furi_hal_nfc_pn532_listener_wait_event()` SHALL set `params[3]` to `0x88` (CT), `params[4..6]` to `listener_uid[0..2]`, and `params[6]` (SEL_RES) to `listener_sak | 0x04` to signal cascade continuation to the reader.

2.2 WHEN `listener_uid_len` is 7, THEN `furi_hal_nfc_pn532_listener_wait_event()` SHALL set `params[3]` to `0x88` (CT), `params[4..6]` to `listener_uid[0..2]`, and `params[6]` (SEL_RES) to `0x04`; the PN532 handles the second cascade level internally using the NFCID3t field.

2.3 WHEN `listener_uid_len` is 4 or 7, THEN `furi_hal_nfc_pn532_listener_wait_event()` SHALL populate `params[25..34]` (NFCID3t) with the full `listener_uid` bytes so that ISO-DEP readers can match the complete UID during target activation.

2.4 WHEN `listener_uid_len` is 3, THEN `furi_hal_nfc_pn532_listener_wait_event()` SHALL copy `listener_uid[0..2]` directly into `params[3..5]` and use `listener_sak` unmodified, preserving the existing 3-byte UID emulation behaviour.

### Unchanged Behavior (Regression Prevention)

3.1 WHEN `listener_uid_len` is 3 AND the card being emulated is a standard ISO14443-3A card with a 3-byte UID, THEN the HAL SHALL CONTINUE TO emulate the card correctly with the same NFCID1 and SAK as before this fix.

3.2 WHEN `furi_hal_nfc_pn532_listener_set_col_res_data()` is called with a valid UID, ATQA, and SAK, THEN the HAL SHALL CONTINUE TO store those values and force re-initialisation of `TgInitAsTarget` on the next `listener_wait_event` call.

3.3 WHEN the PN532 is in FeliCa listener mode, THEN `listener_wait_event()` SHALL NOT apply the cascade UID encoding logic and SHALL CONTINUE TO use the FeliCa-specific `TgInitAsTarget` parameter block.

### Correctness Properties

P1.1 FOR ALL `uid_len` in {4, 7}: WHEN `listener_wait_event()` builds the `TgInitAsTarget` params with a UID of length `uid_len`, THEN `params[3]` SHALL equal `0x88` (CT).

P1.2 FOR ALL `uid_len` in {4, 7}: WHEN `listener_wait_event()` builds the `TgInitAsTarget` params, THEN `params[6]` (SEL_RES) SHALL have bit 2 set (`params[6] & 0x04 != 0`).

P1.3 FOR ALL `uid_len` in {4, 7}: WHEN `listener_wait_event()` builds the `TgInitAsTarget` params, THEN `params[25..25+uid_len-1]` SHALL equal `listener_uid[0..uid_len-1]`.

P1.4 FOR `uid_len` == 3: WHEN `listener_wait_event()` builds the `TgInitAsTarget` params, THEN `params[3..5]` SHALL equal `listener_uid[0..2]` AND `params[6]` SHALL equal `listener_sak` without modification.

---

## Bug 2: ISO-DEP Chaining (Multi-Block Responses) Silently Truncated

### Current Behavior (Defect)

4.1 WHEN `furi_hal_nfc_pn532_exchange_internal()` receives an ISO-DEP I-block response from `furi_hal_pn532_in_data_exchange()` AND the PCB byte of the reconstructed I-block has the chaining bit set (`PCB & 0x10 != 0`), THEN the HAL returns the first fragment to the caller without fetching the remaining fragments, causing the caller to receive an incomplete APDU response.

4.2 WHEN the caller receives a truncated chained response AND the upper-layer ISO-DEP state machine sends an R(ACK) to request the next fragment, THEN `exchange_internal()` forwards the R(ACK) to `InDataExchange` as a raw APDU payload, which the PN532 does not understand in this context, causing a communication error or an incorrect response.

4.3 WHEN a card returns a chained response of N fragments (N ≥ 2), THEN the HAL SHALL NOT return any data to the caller until all N fragments have been received and assembled.

### Expected Behavior (Correct)

5.1 WHEN `furi_hal_pn532_in_data_exchange()` returns a response AND the PN532 status byte indicates chaining (PN532 sets bit 4 of the first response byte when chaining is active), THEN `exchange_internal()` SHALL send an R(ACK) via a subsequent `InDataExchange` call and receive the next fragment.

5.2 WHEN `exchange_internal()` is assembling a chained response, THEN the HAL SHALL repeat the R(ACK) / receive loop until the PN532 returns a fragment without the chaining indicator, appending each fragment payload to a single contiguous buffer.

5.3 WHEN the assembled buffer would exceed `PN532_MAX_FRAME_SIZE`, THEN the HAL SHALL stop chaining, set `last_error` to `FuriHalPn532ErrorBufferOverflow`, and return `FuriHalNfcErrorBufferOverflow` to the caller.

5.4 WHEN all fragments have been received, THEN `exchange_internal()` SHALL reconstruct a single I-block with the assembled payload and the correct PCB byte (block number toggled once for the complete response) and return it to the caller as if the card had responded with a single block.

### Unchanged Behavior (Regression Prevention)

6.1 WHEN a card returns a single-fragment ISO-DEP response (chaining bit clear), THEN `exchange_internal()` SHALL CONTINUE TO return that response to the caller immediately without any additional `InDataExchange` calls.

6.2 WHEN `exchange_internal()` is in non-ISO-DEP mode (MIFARE Classic, raw), THEN the chaining assembly loop SHALL NOT be entered and the existing exchange path SHALL CONTINUE TO operate unchanged.

6.3 WHEN `exchange_internal()` handles R-blocks and S-blocks (WTX, DESELECT) in ISO-DEP mode, THEN those paths SHALL CONTINUE TO operate as before and SHALL NOT be affected by the chaining fix.

6.4 WHEN `exchange_internal()` handles a MIFARE Classic auth command (0x60/0x61), THEN the auth path via `InCommunicateThru` SHALL CONTINUE TO operate unchanged.

### Correctness Properties

P2.1 FOR ALL chained responses of N fragments (N ≥ 2): WHEN `exchange_internal()` completes, THEN the payload returned to the caller SHALL equal the concatenation of all N fragment payloads in order.

P2.2 FOR ALL single-fragment responses: WHEN `exchange_internal()` completes, THEN the number of `InDataExchange` calls issued SHALL equal 1 (no extra R(ACK) calls).

P2.3 FOR ALL chained responses of N fragments: WHEN `exchange_internal()` completes successfully, THEN the number of `InDataExchange` calls issued SHALL equal N (one per fragment, with R(ACK) embedded in each call after the first).

P2.4 WHEN the assembled chained payload length exceeds `PN532_MAX_FRAME_SIZE`, THEN `exchange_internal()` SHALL return `FuriHalNfcErrorBufferOverflow` and SHALL NOT write beyond the scratch buffer boundary.

---

## Bug 3: ISO15693 Polling Not Implemented (Excluded Due to Resolved I2C Deadlock)

### Current Behavior (Defect)

7.1 WHEN `nfc_scanner_state_handler_idle()` builds the PN532 probe order, THEN `NfcProtocolIso15693_3` is excluded from `pn532_probe_order[]` with the comment "causes I2C deadlock", preventing ISO15693 cards from ever being detected on the PN532 backend.

7.2 WHEN a user presents an ISO15693 card to the DIY Flipper Zero, THEN the NFC scanner completes without detecting any protocol and the card is silently ignored.

7.3 WHEN `furi_hal_pn532_poll_iso15693()` is called, THEN no such function exists in `furi_hal_pn532.c`, so ISO15693 polling cannot be added to the scanner without first implementing the function.

### Expected Behavior (Correct)

8.1 THE HAL SHALL implement `furi_hal_pn532_poll_iso15693()` in `furi_hal_pn532.c` using `InListPassiveTarget` with baud rate code `0x05` (ISO15693 mode) to detect ISO15693 cards.

8.2 WHEN `furi_hal_pn532_poll_iso15693()` is called AND an ISO15693 card is present, THEN the function SHALL parse the INVENTORY response, extract the 8-byte UID (stored LSB-first by the PN532) and the 1-byte DSFID, populate a `FuriHalPn532Target` struct, and return `true`.

8.3 WHEN `furi_hal_pn532_poll_iso15693()` is called AND no ISO15693 card is present, THEN the function SHALL return `false` without blocking for longer than the `InListPassiveTarget` timeout.

8.4 WHEN `nfc_scanner_state_handler_idle()` builds the PN532 probe order AND `furi_hal_pn532_poll_iso15693()` is implemented, THEN `NfcProtocolIso15693_3` SHALL be added to `pn532_probe_order[]` after `NfcProtocolFelica`.

8.5 WHERE ISO15693 write support is not available via PN532 native commands, THE HAL SHALL expose `furi_hal_pn532_poll_iso15693()` as a read-only detection function and SHALL NOT implement write operations.

### Unchanged Behavior (Regression Prevention)

9.1 WHEN no ISO15693 card is present AND the scanner probes `NfcProtocolIso15693_3`, THEN the probe SHALL complete within the `InListPassiveTarget` timeout and SHALL NOT block the scanner for longer than the existing ISO14443-3A or FeliCa probe durations.

9.2 WHEN the PN532 is probing ISO14443-3A, ISO14443-3B, or FeliCa, THEN the addition of the ISO15693 probe SHALL NOT affect those probe paths or their timing.

9.3 WHEN an ISO15693 card is detected, THEN the NFC scanner SHALL CONTINUE TO follow the existing `NfcScannerStateFindChildrenProtocols` → `NfcScannerStateDetectChildrenProtocols` flow for any child protocols registered under `NfcProtocolIso15693_3`.

9.4 WHEN `furi_hal_pn532_poll_iso15693()` is called AND the I2C bus returns an error, THEN the function SHALL return `false` and SHALL NOT leave the PN532 in an undefined state that would cause subsequent ISO14443-3A polls to fail.

### Correctness Properties

P3.1 WHEN `furi_hal_pn532_poll_iso15693()` is called with a simulated INVENTORY response of `[DSFID(1)] [UID(8)]`, THEN the populated `FuriHalPn532Target.uid` SHALL equal the UID bytes in the order `UID[7]..UID[0]` (MSB-first, reversed from the PN532 LSB-first wire format).

P3.2 WHEN `furi_hal_pn532_poll_iso15693()` is called AND `InListPassiveTarget` returns a response shorter than 10 bytes (1 status + 1 DSFID + 8 UID), THEN the function SHALL return `false` and SHALL NOT read beyond the response buffer.

P3.3 WHEN `NfcProtocolIso15693_3` is present in `pn532_probe_order[]`, THEN `instance->base_protocols_num` after `nfc_scanner_state_handler_idle()` SHALL be 4 (ISO14443-3A, ISO14443-3B, FeliCa, ISO15693-3).

---

## Bug 4: FeliCa Emulation Uses Hardcoded NFCID2t

### Current Behavior (Defect)

10.1 WHEN `furi_hal_nfc_pn532_listener_wait_event()` initialises `TgInitAsTarget` for FeliCa target mode, THEN the function uses a hardcoded `params[]` array with NFCID2t bytes `{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}` regardless of the FeliCa card being emulated.

10.2 WHEN a reader performs a FeliCa SENSF_REQ (Request Response) against the emulated target, THEN the PN532 responds with the hardcoded NFCID2t instead of the NFCID2t of the card being emulated, causing the reader to reject the emulated card.

10.3 WHEN `furi_hal_nfc_pn532_listener_set_col_res_data()` is called with a FeliCa UID (NFCID2t, 8 bytes), THEN the stored `listener_uid` is not used in the FeliCa `TgInitAsTarget` parameter block, making the stored value ineffective.

### Expected Behavior (Correct)

11.1 THE HAL SHALL extend `furi_hal_nfc_pn532_listener_set_col_res_data()` or add a dedicated `furi_hal_nfc_pn532_listener_set_felica_params()` function to accept the 8-byte NFCID2t for FeliCa emulation and store it in the `FuriHalNfcPn532State` struct.

11.2 WHEN `furi_hal_nfc_pn532_listener_wait_event()` initialises `TgInitAsTarget` for FeliCa target mode AND a FeliCa NFCID2t has been configured, THEN the HAL SHALL copy the configured NFCID2t into `params[1..8]` (bytes 1–8 of the FeliCa `TgInitAsTarget` parameter block).

11.3 WHEN `furi_hal_nfc_pn532_listener_wait_event()` initialises `TgInitAsTarget` for FeliCa target mode AND no FeliCa NFCID2t has been configured, THEN the HAL SHALL use a default NFCID2t of `{0x01, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}` (manufacturer code 0x01, random prefix 0xFE per NFC-F spec) instead of the previous hardcoded value.

11.4 WHEN the configured NFCID2t is set, THEN `listener_wait_event()` SHALL force re-initialisation of `TgInitAsTarget` on the next call (i.e., set `listener_active = false`) so the new NFCID2t takes effect immediately.

### Unchanged Behavior (Regression Prevention)

12.1 WHEN the HAL is in ISO14443-3A listener mode, THEN the FeliCa NFCID2t configuration SHALL NOT affect the ISO14443-3A `TgInitAsTarget` parameter block.

12.2 WHEN `furi_hal_nfc_pn532_listener_set_col_res_data()` is called for ISO14443-3A emulation (UID 3/4/7 bytes, ATQA, SAK), THEN the existing ISO14443-3A parameter storage behaviour SHALL CONTINUE TO operate unchanged.

12.3 WHEN the FeliCa listener receives a command from a reader after `TgInitAsTarget` completes, THEN the data path through `furi_hal_pn532_tg_get_data()` and the parity-encoding logic in `listener_wait_event()` SHALL CONTINUE TO operate unchanged.

12.4 WHEN no FeliCa NFCID2t has been explicitly configured, THEN the HAL SHALL CONTINUE TO initialise `TgInitAsTarget` with a valid default NFCID2t so that FeliCa listener mode is functional even without explicit configuration.

### Correctness Properties

P4.1 FOR ALL 8-byte NFCID2t values `N`: WHEN `listener_set_felica_params(N)` is called AND `listener_wait_event()` subsequently builds the FeliCa `TgInitAsTarget` params, THEN `params[1..8]` SHALL equal `N[0..7]`.

P4.2 WHEN no FeliCa NFCID2t has been configured AND `listener_wait_event()` builds the FeliCa `TgInitAsTarget` params, THEN `params[1]` SHALL equal `0x01` and `params[2]` SHALL equal `0xFE` (NFC-F manufacturer/random prefix convention).

P4.3 WHEN `listener_set_felica_params()` is called, THEN `furi_hal_nfc_pn532.listener_active` SHALL be set to `false` so that the next `listener_wait_event()` call re-issues `TgInitAsTarget` with the new NFCID2t.

P4.4 FOR ALL 8-byte NFCID2t values `N1` and `N2` where `N1 != N2`: WHEN `listener_set_felica_params(N1)` is called followed by `listener_set_felica_params(N2)`, THEN the `TgInitAsTarget` params built by the next `listener_wait_event()` call SHALL use `N2`, not `N1`.
