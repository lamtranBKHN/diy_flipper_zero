# NFC Improvement Plan for PN532-Based Board

## Summary

Current PN532 NFC backend on this DIY board supports ISO14443-3A (poller+listener), ISO14443-3B (poller, ATQB synthesized), and FeliCa (poller) with basic key derivation and dictionary attacks for MIFARE Classic. However, a critical **ISO14443-4A blocking issue (0x0B error during RATS/ATS)** prevents 5+ protocol families from working (DESFire, Plus, NTAG4xx, Type 4 Tag, EMV). Card emulation is limited to ISO14443-4A via TGINITASTARGET. NDEF, SRIX4K, P2P (LLCP/SNEP), EMV, and value-block operations are absent or broken.

This plan covers 18 improvement items across 4 priority tiers, organized into 4 implementation phases. Reference implementations from Adafruit, Elechouse, BruceDevices, Momentum Firmware, and other open-source PN532 projects are identified for each item.

---

## Priority Matrix

### Tier 1 — High Impact, Feasible on PN532
| ID | Title | Impact |
|---|---|---|
| NFC-01 | Fix ISO14443-4A Child Protocol Detection (0x0B Error) | Unlocks 5+ protocol families |
| NFC-02 | Enable MIFARE Classic Backdoor Auth on PN532 | Enables magic-card operations |
| NFC-03 | Add SRIX4K/SRIX512 Read/Write via InCommunicateThru | New protocol family |
| NFC-04 | Add NDEF Extraction and Writing | Usability for all card types |

### Tier 2 — Medium Impact, Feasible on PN532
| ID | Title | Impact |
|---|---|---|
| NFC-05 | Enable FeliCa Listener on PN532 | Card emulation parity |
| NFC-06 | Add EMV Credit Card Reader | Payment card reading |
| NFC-07 | Add MIFARE Classic UID Write (CUID/CUID2) | Magic card personalization |
| NFC-08 | Add MIFARE Classic Value Block Operations | Banking/transit card support |

### Tier 3 — Lower Impact / Requires Investigation
| ID | Title | Impact |
|---|---|---|
| NFC-09 | FeliCa Authentication Key Recovery / Dictionary | Extended FeliCa access |
| NFC-10 | ISO14443-4A P2P (LLCP/SNEP) | Phone-to-board NDEF exchange |
| NFC-11 | Improve Dictionary Attack Performance | Faster card reading |
| NFC-12 | MIFARE Plus SL1 Read Support | Secure card reading |
| NFC-13 | NTAG/Ultralight Password Authentication | Password-protected tag access |
| NFC-14 | Investigate ISO15693-3 via InCommunicateThru | Vicinity card support (low confidence) |
| NFC-15 | Type 4 Tag Emulation Improvements | Better card emulation |
| NFC-16 | Enhanced Error Reporting and Diagnostics | Debuggability |
| NFC-17 | Mfkey32 Integration Polish | Better key recovery UX |
| NFC-18 | 270-Byte I2C Read Optimization | Performance |

### Tier 4 — Hardware-Limited (Cannot Fix on PN532)
- ISO15693-3 (native support absent in PN532; InCommunicateThru may not drive 13.56 MHz subcarrier for vicinity cards)
- ST25TB (requires ST25R3916-specific framing)
- MIFARE DESFire EV2/EV3 advanced features (requires ISO7816-4 chaining that PN532 cannot accelerate)
- EMV contactless kernel-level processing (PN532 cannot handle the full EMV state machine at hardware level)

---

## Detailed Improvement Items

---

### NFC-01: Fix ISO14443-4A Child Protocol Detection (0x0B Error)

- **Tier**: 1
- **Current State**: When the PN532 poller detects an ISO14443-4A-capable card and sends RATS, the InDataExchange command returns status 0x0B (invalid parameter / wrong mode). This blocks all child protocols that sit atop ISO14443-4A: MIFARE DESFire, MIFARE Plus, NTAG4xx (with 4A capability), Type 4 Tag, and EMV. The ATS (Answer To Select) response is never properly received, so the 4A poller layer cannot complete activation.
- **Target State**: RATS/ATS exchange completes successfully. The PN532 correctly enters ISO14443-4A state and forwards I-blocks so that child protocol pollers (DESFire, Plus, 4A Tag, EMV) can activate and exchange data.
- **Reference**: `Flipper-Zero-ESP32-Port` — This project intercepts the RATS response from PN532 and manually wraps the ATS into the Flipper's 4A layer. See their `nfc_pn532.c` / `iso14443_4a_poller.c` integration. Also `BruceDevices firmware` (`PN532.cpp` `mifare` class) handles RATS/ATS for DESFire.
- **Files to Modify**:
  - `targets/f7/furi_hal/furi_hal_nfc_pn532.c` — The `poller_trx()` wrapper that calls `InDataExchange` for non-3A frames. May need special handling for the RATS command (0xE0) and ATS response (which exceeds PN532's normal 4-byte short frame handling).
  - `lib/nfc/protocols/iso14443_4a/iso14443_4a_poller.c` — The `iso14443_4a_poller_activate()` function. May need an alternate path that bypasses ATS re-exchange when PN532 has already performed it internally.
- **Implementation Approach**:
  1. Investigate what `InDataExchange` does with the RATS command (CID + 0xE0 + params). PN532 datasheet suggests RATS is handled internally and ATS is returned as a side effect — but the Flipper's 4A layer expects to send RATS itself and receive ATS separately.
  2. Two potential solutions:
     - **Option A**: Bypass the Flipper's RATS send in `iso14443_4a_poller_activate()` when PN532 is active. Instead, after anti-collision/selection succeeds, call a PN532-specific `get_ats()` that retrieves the ATS from the PN532's internal state (using `InCommunicateThru` or `InDataExchange` with a dummy RATS to trigger the internal exchange).
     - **Option B**: In `poller_trx()`, detect when the command is RATS (first byte 0xE0) and handle the response specially — the PN532's `InDataExchange` may be wrapping the ATS in a proprietary response format that needs de-wrapping.
  3. Look at the ESP32 port's approach: they use `InCommunicateThru` instead of `InDataExchange` for 4A frames, which bypasses the PN532's internal 4A state machine and leaves activation to the host. This may be the cleanest fix.
  4. Implement `pn532_in_communicate_thru()` as a new HAL function that sends raw bytes and returns raw response, then use it for 4A activation frames.
- **Risk Assessment**: Medium complexity. Risk of regression to existing 3A/Crypto1 operations if InCommunicateThru path has different timing or buffer constraints. Must test MIFARE Classic + Ultralight still work after change.
- **Testing**: Enable `unit_tests` FIRMWARE_APP_SET and run `test_nfc` suite. Manual testing with a DESFire, NTAG 4xx, or Type 4 Tag card — card should appear in reader with correct protocol detection.

---

### NFC-02: Enable MIFARE Classic Backdoor Auth on PN532

- **Tier**: 1
- **Current State**: In `mf_classic_poller.c:235-247`, backdoor detection and use is explicitly gated behind `#ifndef FURI_HAL_NFC_PN532_ONLY`. The comment states "backdoor not supported on PN532". This means magic cards (MIFARE Classic clones with backdoor commands like Gen1A/Gen2) cannot be written to.
- **Target State**: Backdoor auth (0x64/0x65 commands with Crypto1 encrypted payload) works on PN532 via `InCommunicateThru`. Magic cards can be detected, UID-block written, and data blocks written without knowing the key.
- **Reference**: `Momentum-Firmware` `lib/nfc/protocols/mf_classic/mf_classic_poller.c` — Their backdoor auth implementation uses raw frame send/receive. `BruceDevices firmware` `PN532.cpp` `mifare_classic_authenticate_backdoor()`.
- **Files to Modify**:
  - `lib/nfc/protocols/mf_classic/mf_classic_poller.c` — Remove the `#ifndef` guard and add PN532-specific backdoor path.
  - `targets/f7/furi_hal/furi_hal_nfc_pn532.c` — Add `pn532_send_raw_frame()` for `InCommunicateThru` which sends bytes with 3A framing. This reuses the existing `InCommunicateThru` command but with proper timeout for backdoor operations.
  - `targets/f7/furi_hal/furi_hal_nfc.h` — Expose raw frame send if not already present.
- **Implementation Approach**:
  1. The backdoor auth flow: send `0x64` (Gen1A) or `0x65` (Gen2) with exact 3A framing via `InCommunicateThru`. The command format is: `[2 bytes CRC] [0x64] [block] [key_type] [0x00]*6` (Gen1A) or `[2 bytes CRC] [0x65] [block] [key_type] [key_byte0..5]` (Gen2). The PN532 will handle the 3A SOF/EOF framing but must not add auth protocol overhead.
  2. The `InCommunicateThru` command (0x42) sends raw 3A data. Use `mifare_classic_send_raw_frame()` (add as new function) that wraps `furi_hal_nfc_poller_trx()` with PN532-specific raw mode.
  3. Must calculate CRC-A manually since PN532 won't do it for raw frames. Use existing `iso13239_crc_append()`.
  4. After backdoor auth, standard `InCommunicateThru` can read/write blocks without key.
- **Risk Assessment**: Low-medium. Backdoor auth is well-understood. Risk of desyncing PN532's internal state machine if raw frames break 3A protocol state. Implement with retry logic.
- **Testing**: Use a Gen1A/Gen2 MIFARE Classic card. Run "Read MIFARE Classic" without knowing keys — should detect and use backdoor. Write a block < 0x04 and verify readback.

---

### NFC-03: Add SRIX4K/SRIX512 Read/Write via InCommunicateThru

- **Tier**: 1
- **Current State**: SRIX4K (STMicroelectronics SR tag family) is completely unimplemented. There is no poller, no listener, and no HAL support.
- **Target State**: A new SRIX protocol layer reads UID, reads/writes blocks on SRIX4K, SRIX512, and SRIX1K tags. Implemented using PN532's `InCommunicateThru` to send raw SRIX frames (with SRIX custom framing, not ISO14443-3A standard).
- **Reference**: `BruceDevices firmware` — `lib/PN532_SRIX/pn532_srix.cpp` provides complete SRIX implementation. `lib/PN532_SRIX/srix.h` defines commands. Also `PN532-on-STM32` project has SRIX example.
- **Files to Modify**:
  - `lib/nfc/protocols/` — New directory `srix/` with `srix_poller.c`, `srix_poller_i.c`, `srix_listener.c`, `srix_device.c`.
  - `targets/f7/furi_hal/furi_hal_nfc_pn532.c` — Add `pn532_srix_exchange()` function that sends custom-framed SRIX data via `InCommunicateThru`.
  - `lib/nfc/nfc_protocols.c` — Register SRIX protocol.
  - `applications/main/nfc/` — Add SRIX plugin app or extend existing reader for SRIX detection.
- **SRIX Command Reference**:
  - `INITIATE` (0x06) with 3-bit mask — detect tags
  - `SELECT` (0x0E) with 8-bit chip ID — select tag
  - `GETUID` (0x0B) — returns 8-byte UID
  - `READBLOCK` (0x08) with block number — returns 4 bytes data
  - `WRITEBLOCK` (0x09) with block number + 4 bytes data
  - `READCHIPID` (0x1B) / `WRITECHIPID` (0x1C) for chip ID read/write
  - SRIX uses 7-bit or 8-bit anti-collision, not 3A standard.
- **Implementation Approach**:
  1. InCommunicateThru sends data at the 3A transport layer with additional framing bits. The SRIX protocol uses a different frame format (short frames with custom CRC). Bruce firmware shows how to pre-pend SRIX frame type bits before the raw command bytes.
  2. The key insight: PN532's InCommunicateThru transmits raw bytes and receives raw bytes. The SRIX protocol is a 3A-compatible protocol (uses same 13.56 MHz carrier with 106 kbps), so raw frames will work if framing is correct.
  3. Implement `srix_inventory()` which sends INITIATE with progressive masks (ala ISO14443-3A anticollision but with SRIX-specific SELECT and CHIP_ID).
  4. For block I/O: READBLOCK returns 4 bytes on success. WRITEBLOCK needs a 4-byte data payload.
- **Risk Assessment**: Medium. SRIX may need non-standard frame timing. PN532 documentation does not explicitly support SRIX — this is reverse-engineered behavior. Need to verify that InCommunicateThru handles the SRIX's 3A-compatible framing without corruption.
- **Testing**: Acquire an SRIX4K or SRIX512 tag (common in SKI passes, some hotel key cards). Verify: UID read, block read, block write, inventory with multiple tags.

---

### NFC-04: Add NDEF Extraction and Writing

- **Tier**: 1
- **Current State**: MIFARE Classic, Ultralight, and NTAG data is dumped as raw hex bytes. The user sees `[data]` blocks without understanding that the data is an NDEF message. No structured NDEF parsing or writing is available.
- **Target State**: When reading a tag that contains NDEF data (recognizable by NDEF TLV `0x03` at block/offset 4 of capability container), the app auto-parses NDEF messages and displays decoded content (URL, text, vCard, etc.). Writing NDEF messages is possible through a simple "Write NDEF URL" or "Write NDEF Text" app dialog.
- **Reference**: `Elechouse PN532` `NDEF/` library — full NDEF parser/writer for Classic, Ultralight, NTAG. `BruceDevices firmware` `apdu.cpp` includes NDEF parsing. `Adafruit-PN532` `Adafruit_PN532.cpp` NDEF helper methods (`format_ndef()`, `write_ndef_uri()`, `read_ndef_uri()`).
- **Files to Modify**:
  - New: `lib/nfc/helpers/ndef.c` and `ndef.h` — NDEF message parser and builder.
  - New: `lib/nfc/helpers/nfc_tlv.c` and `nfc_tlv.h` — TLV block parser (NDEF TLV `0x03`, Proprietary TLV `0x05`, Terminator TLV `0xFE`, Lock Control TLV `0x01`, Memory Control TLV `0x02`).
  - Modify: `applications/main/nfc/plugins/supported_cards/` — Each card plugin's info display can optionally call NDEF parser.
  - Modify: `applications/main/nfc/scenes/nfc_scene_read.c` — After read, attempt NDEF parse and display NDEF content in addition to raw data.
- **Implementation Approach**:
  1. **TLV Parser**: Parse capability container (CC) at block 0x03 of Ultralight/NTAG or sector 0x00 block 0x03 of Classic to find NDEF TLV offset and length.
  2. **NDEF Parser**: Implement NDEF record parsing per NFC Forum NDEF spec:
     - TNF (Type Name Format): 0x01=WellKnown, 0x02=Media, 0x03=URI, 0x04=External, 0x05=Unknown
     - Record types: URL (`U+<URL>`), Text (`T`), Smart Poster (`Sp`), vCard, etc.
  3. **NDEF Builder**: For writing, construct TLV = `[0x03] [len] [NDEF message]` + `[0xFE]` terminator. Pad to block size.
  4. **Integration**: The TLV block must be written to the correct block(s). For Classic, it goes in sector 0 after the MAD (sector trailer). For Ultralight/NTAG, it goes at the offset indicated by CC (usually after CC bytes at block 0x04+).
- **Risk Assessment**: Medium. NDEF writing can damage card's MAD/trailer if written to wrong blocks. Must add safety checks: verify CC structure before writing NDEF. For Classic, must not overwrite key blocks (sector trailer) or MAD sectors.
- **Testing**: Use an NDEF-formatted tag (e.g., "Shopping list" written by an iPhone). Read with the flipper; verify URL/text is displayed instead of raw hex. Write an NDEF URL; read back with a phone.

---

### NFC-05: Enable FeliCa Listener on PN532

- **Tier**: 2
- **Current State**: In `furi_hal_nfc_pn532.c:296-298`, FeliCa listener mode is rejected with an early return. The comment or guard explicitly puts FeliCa listener in the "not supported on PN532" category.
- **Target State**: PN532 operates as a FeliCa type-F tag (card emulation). An Android phone with NFC TagWriter or FeliCa app can detect and read the emulated card's NDEF data or system data.
- **Reference**: `Adafruit-PN532` `Adafruit_PN532::AS_Target()` — sets FeliCa parameters (NFCID2, PAD, SENSF_RES). `Elechouse PN532` `PN532::tgInitAsTarget()` with `FELICA_BAUD` parameter. Both show how to configure PN532 for FeliCa target mode via `TGINITASTARGET` command.
- **Files to Modify**:
  - `targets/f7/furi_hal/furi_hal_nfc_pn532.c` — Remove the FeliCa listener rejection. Add a `pn532_listener_init_felica()` function that calls `TGINITASTARGET` with FeliCa configuration (mode byte = `0x02` for FeliCa, NFCID2 = 8 bytes, PAD = 8 bytes, SENSF_RES). See `furi_hal_nfc_felica.c` for the expected parameter structure.
  - `targets/f7/furi_hal/furi_hal_nfc_felica.c` — The `furi_hal_nfc_felica_listener_init()` function may need a PN532-specific path that calls the new `pn532_listener_init_felica()` instead of the ST25R3916 path.
  - `lib/nfc/protocols/felica/felica_listener.c` — Verify the listen flow calls the HAL init correctly for PN532.
- **FeliCa Target Mode Parameters** (from PN532 datasheet):
  - `TGINITASTARGET` (0x8C): `[Mode=0x02] [NFCID2t: 8 bytes] [PAD: 8 bytes] [SENSF_RES: 4 bytes (0x12 0xFC 0x00 0x00 for NDEF)]`
  - The PN532 will then respond to FeliCa polling (SENSF_REQ = `0x00FFFFFFFFFFFFFFFF00` → SENSF_RES with NFCID2t).
- **Implementation Approach**:
  1. Follow the existing `tg_init_as_target()` path in `furi_hal_nfc_pn532.c`. Currently it sets mode to `0x01` (ISO14443-4A target) or `0x03` (ISO18092 P2P target). Add case for `0x02` (FeliCa target).
  2. Read the configuration from `FeliCaListenerConfig` struct (defined in `furi_hal_nfc_felica.h` or similar). Set NFCID2, PAD, and SENSF_RES from the config.
  3. After TGINITASTARGET completes, the PN532 enters target mode. The card reader's poll response is handled automatically; the host MCU receives callback when data is exchanged.
- **Risk Assessment**: Low. PN532 natively supports FeliCa target. Risk of breaking existing 4A target mode if mode byte logic is incorrect.
- **Testing**: Use an Android phone with NFC TagWriter. Enable card emulation on flipper (FeliCa emulation). Phone should detect the virtual FeliCa card.

---

### NFC-06: Add EMV Credit Card Reader

- **Tier**: 2
- **Current State**: The EMV poller (`lib/nfc/protocols/emv/emv_poller.c`) exists in the codebase but is unreachable because it requires a working ISO14443-4A layer (parent protocol). Since NFC-01 blocks 4A, EMV cannot activate.
- **Target State**: Once NFC-01 is fixed, EMV poller activates and performs the full PPSE/GPO/AFL/ReadRecords flow to extract card details: PAN (Primary Account Number), expiry date, cardholder name (if present), application data, and track2 equivalent data.
- **Reference**: `BruceDevices firmware` `emv_reader.cpp` — implements PPSE select (`2PAY.SYS.DDF01` / `1PAY.SYS.DDF01`), GPO command, AFL parsing, and record reading. `Momentum-Firmware` `emv_poller.c` has a working implementation.
- **Files to Modify**:
  - `lib/nfc/protocols/emv/emv_poller.c` — May need PN532-specific fixes for secure messaging / MAC handling. The existing code assumes ST25R3916 behavior; some APDU transport expectations may differ on PN532.
  - `targets/f7/furi_hal/furi_hal_nfc_pn532.c` — The 4A I-block forwarding InCommunicateThru path (from NFC-01) must handle the long APDU frames that EMV generates (up to 256-byte response, multi-block if > 256 bytes).
- **EMV Command Flow**:
  1. PPSE: Select `2PAY.SYS.DDF01` (or `1PAY.SYS.DDF01`) via ISO7816-4 SELECT command → get PDOL and application list.
  2. GPO: Build GET PROCESSING OPTIONS command with PDOL data → receive AFL (Application File Locator).
  3. READ RECORD: For each (SFI, record) in AFL, read records → extract tags (5A=PAN, 5F24=expiry, 5F20=cardholder name, 57=track2 equivalent, 9F6C=card transaction data).
  4. CDA (if needed): Combine dynamic authentication using CA public keys (not implemented yet; Stage 2 feature).
- **Implementation Approach**:
  1. After NFC-01 fixes 4A, the existing `emv_poller_activate()` flow should mostly work. The key is ensuring `furi_hal_nfc_poller_trx()` for 4A frames correctly wraps/unwraps I-blocks that contain ISO7816-4 APDUs.
  2. The PN532's 4A I-block format: ISO14443-4A I-block = `[PCB] [CID] [INF...] [CRC]`. The PN532's InDataExchange may or may not handle I-block wrapping automatically. If not, manually wrap/unwrap in `poller_trx()`.
  3. Test with a real EMV card (debit/credit card with contactless).
- **Risk Assessment**: Medium. EMV PDOL parsing and AFL execution require careful state management. Security: PAN data should be masked in display and must never be logged.
- **Testing**: Hold a contactless EMV card to flipper. Card should be detected as EMV, and PAN (masked), expiry, and cardholder name should display.

---

### NFC-07: Add MIFARE Classic UID Write (CUID/CUID2)

- **Tier**: 2
- **Current State**: There is no way to write block 0 (UID and manufacturer data) on CUID/CUID2 Chinese clone cards. These cards support backdoor commands to overwrite the UID even after lock.
- **Target State**: A "Write UID" option in the MIFARE Classic app sends the appropriate backdoor command (0x64 for Gen1A, 0x65 for Gen2/CUID) to write a new 4-byte UID + BCC + manufacturer data to block 0.
- **Reference**: `BruceDevices firmware` `PN532.cpp` `clone()` function — sends backdoor auth then writes block 0 with custom UID. `Momentum-Firmware` `mf_classic_poller.c` backdoor write support.
- **Files to Modify**:
  - `lib/nfc/protocols/mf_classic/mf_classic_poller_sync.c` — Add `mf_classic_poller_sync_write_block_with_uid()` that uses the backdoor path (NFC-02) to authenticate with backdoor command, then write block 0.
  - `applications/main/nfc/scenes/nfc_scene_mf_classic_keys.c` or new scene — Add "Write UID" dialog.
- **Implementation Approach**:
  1. Detect if card is a magic card (Gen1A or Gen2): try backdoor auth (0x64 for Gen1A, 0x65 for Gen2 with known backdoor keys). If auth succeeds, card is writable.
  2. For Gen1A: `[CRC] [0x64] [0x00] [0x00] [0x00]*6` → writes next `InCommunicateThru` data to block 0.
  3. For Gen2/CUID: `[CRC] [0x65] [0x00] [0x00] [key_bytes]` where `key_bytes` is the 6-byte key (default `0x000000000000` or `0xFFFFFF FFFFFF`), then write block 0.
  4. Block 0 format: `[UID0-3] [BCC] [SAK/ATQA] [manufacturer data]`. BCC must be recalculated as `UID0 ^ UID1 ^ UID2 ^ UID3`.
- **Risk Assessment**: Low. Well-known procedure on multiple platforms.
- **Testing**: Acquire CUID/CUID2 cards. Write a custom UID. Read back — verify UID changed. Attempt to write new UID and verify.

---

### NFC-08: Add MIFARE Classic Value Block Operations

- **Tier**: 2
- **Current State**: All blocks are read and written as plain data. Value blocks (used for e-money, transit passes, loyalty points) are not recognized or operated on. The user cannot increment or decrement values.
- **Target State**: The MIFARE Classic app detects value blocks (by format: 4-byte value repeated inverted + address byte) and offers Increment, Decrement, Restore, Transfer operations. These use the standard MIFARE commands: `0xC0` (INC), `0xC1` (DEC), `0xC2` (RESTORE), `0x50` (TRANSFER).
- **Reference**: `PN532-on-STM32` `pn532_mifare.c` — defines `MIFARE_CMD_INC=0xC0`, `MIFARE_CMD_DEC=0xC1`, `MIFARE_CMD_RESTORE=0xC2`, `MIFARE_CMD_TRANSFER=0x50`. `pcsclite` / `libfreefare` have reference value block implementations.
- **Files to Modify**:
  - `lib/nfc/protocols/mf_classic/mf_classic_poller_sync.c` — Add `mf_classic_poller_sync_value_op()` that sends INC/DEC/RESTORE/TRANSFER via existing Crypto1 exchange path. After authentication, the value command is: `[encrypted(cmd)] [encrypted(value_param)]` → response `[encrypted(ACK/NACK)]`.
  - `applications/main/nfc/scenes/` — Add "Value Block" option in Classic card actions that detects value blocks and shows current value, then offers +1/-1/set operations.
- **Implementation Approach**:
  1. The MIFARE value block format: offset 0-3 = value (little-endian int32), offset 4-7 = ~value (inverted), offset 8-9 = value again, offset 10-11 = ~value again, byte 12-15 = address byte (inverted twice). Verify format to detect value block.
  2. INC (0xC0) / DEC (0xC1) take a 4-byte signed value parameter. RESTORE (0xC2) takes no parameter (copies block to internal transfer buffer). TRANSFER (0x50) writes the transfer buffer to the target block.
  3. Commands must be authenticated first (key A or B for the sector). After auth, send the encrypted command + value via standard Crypto1 exchange.
  4. Implementation: re-use `mf_classic_poller_sync_auth()` then `mf_classic_poller_sync_cmd_send()` or equivalent.
- **Risk Assessment**: Low. Standard MIFARE commands. Risk of corrupting value blocks if commands sent to non-value blocks.
- **Testing**: Use a known value block card (some transit passes). Read block, verify value format. Decrement by 1, read back, verify value decreased. Increment back.

---

### NFC-09: FeliCa Authentication Key Recovery / Dictionary

- **Tier**: 3
- **Current State**: FeliCa poller authentication requires the 16-byte card key (CK) to be known. The CK is typically unique per card and set during personalization. There is no key recovery or dictionary attack implementation.
- **Target State**: A FeliCa key dictionary attack tries a set of known default CKs (e.g., all-zeros, FeliCa SDK test keys, common transport system keys). If successful, the user gains read/write access to the FeliCa system data and NDEF areas.
- **Reference**: `Momentum-Firmware` `felica_auth.h` — defines default CKs and provides dictionary attack function. Their implementation authenticates with each CK in sequence until one succeeds.
- **Files to Modify**:
  - `lib/nfc/protocols/felica/felica_poller.c` — Add `felica_poller_dict_attack()` that tries each CK from a built-in list, calling `felica_poller_auth()` for each. Cache successful CK.
  - New: `lib/nfc/protocols/felica/felica_dict.h` — Key list header (similar to `mf_classic_dict.h`).
- **FeliCa Auth Flow**: `felica_poller_auth()` sends `[command=0x0A] [RC=0x00 or 0x01] [CARD_ID] [CK... (16 bytes)]` → card responds with `[mode=0x09] [IDm] [PMM] [response_data]`. If CK is wrong, response is all-zeros or error status.
- **Implementation Approach**:
  1. Build a dictionary of ~20-100 CKs from known defaults (Sony FeliCa SDK, transport cards in Japan/Hong Kong/Singapore, FeliCa Lite default).
  2. For each CK, call `felica_poller_auth()`. Check response validity. On success, store CK and exit.
  3. Performance: each auth attempt ≈ 1 I2C transaction (≈5ms). 100 attempts ≈ 500ms — acceptable.
- **Risk Assessment**: Low. Read-only dictionary attacks cannot corrupt card state.
- **Testing**: Test with a FeliCa Lite card (known default CK = all zeros). Verify CK is found and data areas become readable.

---

### NFC-10: ISO14443-4A P2P (LLCP/SNEP)

- **Tier**: 3
- **Current State**: No peer-to-peer mode is implemented. The PN532 is only used as a reader/writer or tag emulator.
- **Target State**: LLCP (Logical Link Control Protocol) and SNEP (Simple NDEF Exchange Protocol) are implemented, enabling NDEF exchange with Android phones (Android HCE / TagWriter). The flipper appears as an NFC peer and can send/receive NDEF messages (URLs, text, vCards) to/from a phone.
- **Reference**: `Elechouse PN532` `LLCP/` and `SNEP/` directories — complete LLCP+SNEP implementation. Their code handles symmetry, PDU encoding, and connection-oriented transport. `Adafruit-PN532` has simplified SNEP put/get.
- **Files to Modify**:
  - New: `lib/nfc/helpers/llcp.c`, `llcp.h` — LLCP MAC layer (SYMM, PDU exchange, connection management).
  - New: `lib/nfc/helpers/snep.c`, `snep.h` — SNEP client/server for NDEF put/get.
  - Modify: `targets/f7/furi_hal/furi_hal_nfc_pn532.c` — Use TGINITASTARGET with mode `0x03` (DEP passive mode) for P2P. Wrap LLCP PDUs through InCommunicateThru or InDataExchange.
- **LLCP/SNEP Overview**:
  1. `TGINITASTARGET` with mode=0x03 starts Passive F (212 kbps) P2P mode.
  2. PN532 handles DEP framing (SYMM/resynch). Host handles LLCP PDU exchange within DEP data frames.
  3. LLCP: SYMM (0x00) for keepalive, CONNECT (0x10) for connection, DISC (0x11) for disconnect, DM (0x14), I (0x12), RR (0x13), RNR (0x15).
  4. SNEP over LLCP: SNEP PUT (0x10) sends NDEF message, SNEP GET (0x11) requests NDEF message, SNEP response with status.
- **Implementation Approach**:
  1. Port Elechouse's LLCP implementation (about 500 lines C) focusing on PN532's TGINITASTARGET P2P mode.
  2. The LLCP layer handles symmetry interval (default 50ms), connection-oriented I-frames, and aggregation.
  3. SNEP layer builds on LLCP connections. Use default SNEP service access point (SAP=0x04).
  4. Integrate with NDEF parser (NFC-04) for message construction.
- **Risk Assessment**: High. P2P is complex with strict timing requirements. PN532's DEP implementation has known bugs (firmware V1.6+ needed). May not work with all phones.
- **Testing**: Android phone with NFC TagWriter → write NDEF URL to flipper (flipper as Type 4 Tag emulator). Or flipper → send NDEF text to phone.

---

### NFC-11: Improve Dictionary Attack Performance

- **Tier**: 3
- **Current State**: The MIFARE Classic key dictionary has 2311 keys × 2 (key A + key B) = 4622 auth attempts. Each attempt involves an I2C transaction (request → wait → response) taking ~5-10ms. Total ≈ 23-46 seconds worst case for all keys on all sectors. The dict_attack view re-reads the same blocks multiple times.
- **Target State**: Reduced attack time through:
  1. **Key cache**: Skip keys already tried on other sectors of the same card (keys are common across sectors).
  2. **Batch sector read**: After finding a valid key, immediately try it on all remaining sectors before continuing dictionary.
  3. **I2C optimization**: Reduce per-attempt overhead by pipelining requests (if PN532 supports).
  4. **Progress view**: Show real-time key count and found keys count.
- **Reference**: `Momentum-Firmware` `lib/nfc/protocols/mf_classic/mf_classic_poller.c` key cache + optimized dict attack view. Their dict_attack view shows keys tried, found, and per-sector progress.
- **Files to Modify**:
  - `lib/nfc/protocols/mf_classic/mf_classic_poller.c` — Add key cache (a set of keys already found on this card). When a new sector is attacked, check cache first.
  - `lib/nfc/protocols/mf_classic/mf_classic_poller_sync.c` — Implement batch key propagation: when a key is found for sector N, try it on all unattacked sectors immediately.
  - `applications/main/nfc/views/dict_attack.c` — Improve progress display.
- **Implementation Approach**:
  1. Key cache: maintain a `uint8_t found_keys[40][2]` array (max 40 sectors × 2 key types). Before each auth attempt, check if key is already cached. On cache hit, skip attempt.
  2. After finding a key for sector X, run a fast scan of all other unattacked sectors using that same key. This is `mf_classic_poller_sync_try_key()` on each sector.
  3. Display optimization (separate work item): replace the "spinning dots" with actual key counter and found key list.
- **Risk Assessment**: Low. No protocol changes. Pure optimization. Risk: key cache may use too much RAM if implemented naively (40×2×6=480 bytes — negligible).
- **Testing**: Run dict attack on a card with 16 sectors. Verify attack time < 20 seconds. Verify all keys found by comparing with known keys.

---

### NFC-12: MIFARE Plus SL1 Read Support

- **Tier**: 3
- **Current State**: MIFARE Plus poller code exists in `lib/nfc/protocols/mf_plus/` but is unreachable because it requires ISO14443-4A (blocked by NFC-01). Even with 4A working, the existing MIFARE Plus implementation may not correctly handle PN532-specific I-block wrapping.
- **Target State**: MIFARE Plus SL1 cards (security level 1, backward-compatible with MIFARE Classic) can be detected and read. The poller uses standard Crypto1 auth for SL1 access, then reads blocks via the 4A I-block layer.
- **Reference**: `Momentum-Firmware` `lib/nfc/protocols/mf_plus/` — working implementation that handles SL1 authentication fallback and block I/O. Their code correctly switches between 3A (Crypto1) and 4A (I-block) modes based on card response.
- **Files to Modify**:
  - `lib/nfc/protocols/mf_plus/mf_plus_poller.c` — May need PN532-specific path for the 3A→4A transition. SL1 mode uses 3A for auth then switches to 4A for block read — the PN532 must handle this transition without resetting the 3A state.
  - `targets/f7/furi_hal/furi_hal_nfc_pn532.c` — The `poller_trx()` must support the 3A→4A transition. After 3A auth, RATS must be sent (via 3A InDataExchange or InCommunicateThru) to transition to 4A.
- **Implementation Approach**:
  1. MIFARE Plus in SL1 mode: card first responds as ISO14443-3A. After 3A authentication (Crypto1), send RATS to switch to 4A mode. Once in 4A, use I-blocks (via InCommunicateThru) to send READ_BINARY / WRITE_BINARY commands.
  2. The tricky part: after 3A auth, the Crypto1 cipher state must be maintained during the RATS exchange. The PN532's InDataExchange may reset the cipher state when switching to 4A mode. Need to test whether InCommunicateThru preserves cipher state.
  3. Alternative: perform the entire MIFARE Plus session in 3A mode using InCommunicateThru with manually calculated Crypto1 frames (similar to NFC-02). This avoids the 3A→4A transition issue.
- **Risk Assessment**: Medium. MIFARE Plus SL1 behavior is card-dependent. Some Plus cards default to SL3 (fully encrypted 4A) which cannot be read with our approach.
- **Testing**: Acquire a MIFARE Plus SL1 card (e.g., NXP MIFARE Plus EV1). Verify SL1 detection and block read.

---

### NFC-13: NTAG/Ultralight Password Authentication

- **Tier**: 3
- **Current State**: Password-protected NTAG and Ultralight C tags cannot be read. The PWD_AUTH (0x1B) command is not exposed in the HAL or poller layers. Tags with protection bit set in the ACCESS (0x03) / AUTH0 bytes return all zeros for protected blocks.
- **Target State**: The poller detects password protection (reads the ACCESS/AUTH0 configuration), prompts the user for a 4-byte password (or attempts default/all-zeros), sends PWD_AUTH (0x1B), and on success reads the previously protected blocks.
- **Reference**: `Momentum-Firmware` `lib/nfc/protocols/mf_ultralight/mf_ultralight_poller.c` — PWD_AUTH implementation. Their code reads the config block, detects AUTH0 limit, and allows password entry.
- **Files to Modify**:
  - `lib/nfc/protocols/mf_ultralight/mf_ultralight_poller.c` — Add `mf_ul_poller_auth()` that sends PWD_AUTH (0x1B) + 4-byte password → checks PACK response (2 bytes acknowledge). If PACK is valid (check against expected PACK from config block), mark password as authenticated.
  - `applications/main/nfc/scenes/nfc_scene_mf_ultralight_emulate.c` or new scene — Add dialog for password entry (4 hex bytes).
  - `targets/f7/furi_hal/furi_hal_nfc_pn532.c` — The PWD_AUTH command is a standard 3A command; may work through existing InDataExchange path. If not, use InCommunicateThru.
- **Implementation Approach**:
  1. After reading config block (block 0x03 for NTAG21x/UL-C, or block 0x00 + 0x01 for UL-EV1), check AUTH0 byte. If AUTH0 < max_block and PROT bit is set, blocks from AUTH0 to max are password-protected.
  2. PWD_AUTH: send `0x1B [PWD0] [PWD1] [PWD2] [PWD3]` via existing fast command path. Response is `[PACK0] [PACK1]` (2 bytes). Verify PACK matches expected PACK stored in config block (bytes 0x02-0x03 of block 0x03 for NTAG).
  3. After successful PWD_AUTH, protected blocks become readable until power cycle or another PWD_AUTH with wrong password.
- **Risk Assessment**: Low. Standard NTAG command. Risk: some cards (NTAG216) have SET_PASSWORD (0xA5) which is different from PWD_AUTH. Not implementing SET_PASSWORD in this item.
- **Testing**: Use an NTAG213 with password set (via NFC Tools app). Read with flipper — should prompt for password. Enter correct password → protected blocks appear. Enter wrong → access denied.

---

### NFC-14: Investigate ISO15693-3 via InCommunicateThru

- **Tier**: 3
- **Current State**: ISO15693-3 (vicinity cards at 13.56 MHz) is completely unsupported. The PN532 does not have native ISO15693 support.
- **Target State**: Investigation only. Determine whether PN532's InCommunicateThru can drive ISO15693 frames (which use a different subcarrier modulation: ASK 10% or 4.9 MHz subcarrier FSK/ASK). ISO15693 operates at the same 13.56 MHz carrier but uses a different modulation scheme and data rate (26.48 kbps vs 106 kbps for ISO14443).
- **Reference**: `BruceDevices firmware` — their SRIX implementation (NFC-03) uses InCommunicateThru successfully for SRIX which is also a 3A-variant. ISO15693 is fundamentally different — it uses 1-of-4 or 1-of-256 PPM coding, not Miller Manchester.
- **Files to Modify**: None (investigation only). May create `docs/nfc/pn532_iso15693_research.md` if findings warrant.
- **Implementation Approach**:
  1. Read PN532's User Manual section on InCommunicateThru. The command sends data bits via the 3A modulation at 106 kbps. ISO15693 requires 26.48 kbps with 4.9 MHz subcarrier (ASK 10% or FSK). These are physically different signal characteristics.
  2. Likely conclusion: InCommunicateThru cannot change modulation rate or subcarrier frequency. ISO15693 is physically incompatible with PN532's 3A frontend.
  3. However, test empirically: connect an ISO15693 card, send an INVENTORY command (0x01 + 0x00 + 0x00) via InCommunicateThru. Check if any response is received. Bruce firmware's SRIX success suggests some non-3A protocols work, but they all use 106 kbps Miller modulation.
- **Risk Assessment**: Low effort. Conclusion likely negative — PN532 chips are not ISO15693-capable.
- **Testing**: Hold a TI Tag-it HF-I or NXP ICODE SLI near PN532. Run InCommunicateThru with ISO15693 command. No response expected.

---

### NFC-15: Type 4 Tag Emulation Improvements

- **Tier**: 3
- **Current State**: Type 4 Tag emulation works via TGINITASTARGET with mode=0x01 (ISO14443-4A target). However, the NDEF content is hardcoded, and write support may not work. APDU handling is minimal.
- **Target State**: Configurable NDEF content via NDEF parser (NFC-04), write support (phone can write NDEF to flipper), proper ISO7816-4 SELECT + READ BINARY + UPDATE BINARY command handling, and support for multiple NDEF entries.
- **Reference**: `Elechouse PN532` `PN532::EmulateTag()` class methods — handles SELECT by AID, READ BINARY, UPDATE BINARY. Their implementation demonstrates proper File Control Information (FCI) construction.
- **Files to Modify**:
  - `targets/f7/furi_hal/furi_hal_nfc_pn532.c` — The TGINITASTARGET 4A listener path. After target initialization, the listener loop must parse incoming APDUs and respond appropriately.
  - `lib/nfc/protocols/iso14443_4a/iso14443_4a_listener.c` — Extend the APDU handler to support NDEF CC file and NDEF data file reads + updates.
- **Type 4 Tag File Structure** (NFC Forum Type 4 Tag spec):
  - Capability Container (CC) file: File ID `0xE103`, read-only, 12 bytes. Describes maximum NDEF size, read/write access.
  - NDEF file: File ID `0xE104`, read/write. Contains NDEF TLV + data.
  - Select by AID: ISO7816-4 SELECT with AID `0xD2760000850100` (NFC Forum Type 4 Tag AID).
- **Implementation Approach**:
  1. Store CC and NDEF data in a RAM buffer emulating Type 4 Tag memory.
  2. Handle incoming SELECT command: match AID or File ID, return FCI template.
  3. Handle READ BINARY: return requested bytes from CC or NDEF file.
  4. Handle UPDATE BINARY: write incoming bytes to NDEF file buffer (then parse NDEF via NFC-04).
  5. All APDUs wrapped in ISO14443-4A I-blocks, which PN532 handles via TGINITASTARGET data exchange.
- **Risk Assessment**: Medium. Complexity of correct ISO7816-4 state machine. Risk of APDU sequence errors causing phone to fail.
- **Testing**: Write NDEF URL to flipper with Android phone (NFC TagWriter). Read flipper with another phone — should receive the NDEF URL.

---

### NFC-16: Enhanced Error Reporting and Diagnostics

- **Tier**: 3
- **Current State**: PN532 errors are propagated as raw status bytes. The user sees "Error: 0x0B" or similar opaque codes. There is no protocol-level error decoding, debug history, or diagnostics screen.
- **Target State**: Error codes from PN532 (0x00=success, 0x01=timeout, 0x0B=invalid parameter, 0x0D=oversized frame, 0x14=wrong mode, etc.) are decoded to human-readable strings. A diagnostics screen shows recent I2C transactions, last error, and retry count.
- **Reference**: `Adafruit-PN532` `Adafruit_PN532.cpp` — `get_error_code()` returns string. `Elechouse PN532` `PN532::read_status_code()` has error code table. All PN532 libraries include status code lookup.
- **Files to Modify**:
  - `targets/f7/furi_hal/furi_hal_pn532.c` — Add `const char* furi_hal_pn532_strerror(uint8_t code)` with error code table. Log errors at `FURI_LOG_E` level with decoded message.
  - `targets/f7/furi_hal/furi_hal_nfc_pn532.c` — Track last `pn532_status_code` per operation. Maintain circular buffer of last 16 operations (command, status, timestamp) for diagnostics.
  - `applications/main/nfc/scenes/` — Add "NFC Diagnostics" scene showing: last error, retry count, recent command history, chip version, firmware version.
- **PN532 Error Codes** (from PN532 User Manual §7.1):
  - `0x00`: No error
  - `0x01`: Timeout
  - `0x02`: CRC error
  - `0x03`: Parity error
  - `0x04`: Collision detected (during anticollision)
  - `0x05`: Framing error
  - `0x06`: Overrun error
  - `0x07`: RF field error (no card)
  - `0x08`: Protocol error (wrong protocol mode)
  - `0x09`: Buffer overflow
  - `0x0A`: Temperature warning (not error)
  - `0x0B`: Invalid buffer / parameter
  - `0x0C`: Communication buffer full
  - `0x0D`: Oversized frame
  - `0x0E`: Invalid command format
  - `0x0F`: MIFARE Classic authentication fail
- **Implementation Approach**:
  1. Create string table `pn532_error_strings[]` indexed by status code.
  2. In every HAL function that receives a PN532 response, call `furi_hal_pn532_strerror()` when status != 0x00 and log with `FURI_LOG_E("PN532", "...")`.
  3. Maintain `pn532_diag_entry_t diag_ring[16]` with command byte, status, duration_ms, timestamp.
  4. NFC Diagnostics scene reads ring buffer and displays via TextBox or similar.
- **Risk Assessment**: Low. Pure logging/metadata. No protocol changes.
- **Testing**: Introduce a protocol error (e.g., remove tag mid-read). Verify error message is readable (e.g., "Timeout" instead of "Error: 0x01").

---

### NFC-17: MIFARE Classic Extended Dict Attack (Mfkey32 Integration)

- **Tier**: 3
- **Current State**: Reader detection captures nonce pairs (UID, nt0, nt1, nr0, ar0, nr1, ar1) and attempts to solve local keys using nested attack. The mfkey32 flow is partially implemented but lacks polish: nonce output format may not be compatible with external tools, and the "solved keys" display is minimal.
- **Target State**: Captured nonces are displayed in a format compatible with mfkey32v2 tool. User can copy/save nonces for solving on a PC. If local solving succeeds (nested attack works), keys are automatically added to the key dict. A "Mfkey32 Complete" scene shows all solved keys with option to add to dictionary.
- **Reference**: `Momentum-Firmware` `lib/nfc/helpers/mfkey32_logger.c` — sophisticated nonce capture + auto-solving + key injection. Their `nfc_scene_mfkey_complete.c` shows solved keys and allows saving to dict.
- **Files to Modify**:
  - `applications/main/nfc/helpers/mfkey32.c` — Improve nonce output format (standard mfkey32v2 format: `uid nt nr ar` per line). Add option to export to SD card file.
  - `applications/main/nfc/scenes/` — Add `nfc_scene_mfkey_complete.c` showing solved keys, option to add to dictionary.
  - `lib/nfc/protocols/mf_classic/mf_classic_dict.c` — Add `mf_classic_dict_add_keys()` that appends solved keys to the dictionary.
- **Implementation Approach**:
  1. Mfkey32 nonce structure: after reader authentication attempt, we have pairs: `(nt0, nr0, ar0)` and `(nt1, nr1, ar1)`. Combine with known UID and key type → feed to `crypto1_attack_nested()`.
  2. Format for external use: `UID NT NR AR` in hex, one pair per line, as expected by mfkey32v2 (`./mfkey32v2 UID NT NR AR`).
  3. Auto-solving: call `mf_classic_poller_sync_nested_attack()` (already exists). On success, display solved keys in a scrollable list.
  4. "Add to dict" writes keys to `assets/mf_classic_dict_user.txt`.
- **Risk Assessment**: Low. All building blocks exist. Risk: nested attack may not work on all cards (depends on PRNG weaknesses). Some cards (MIFARE Classic EV1 with 3DES random ID) defeat nested attack.
- **Testing**: Use a known MIFARE Classic card. Run "Detect Reader", let a reader authenticate. Verify nonces are captured. Verify keys are solved (if card is vulnerable to nested attack). Verify key is added to dict.

---

### NFC-18: 270-Byte I2C Read Optimization

- **Tier**: 3
- **Current State**: In `furi_hal_pn532.c`, every I2C read requests the full 270-byte buffer (PN532's maximum response size), even when the expected response is only 4-8 bytes (e.g., InListPassiveTarget response for a single card). This adds latency and I2C bus traffic.
- **Target State**: Read only the number of bytes expected, based on the response length field in the PN532 frame (first 2 bytes after the status: LEN + LCS). The I2C read size is dynamically adjusted per transaction.
- **Reference**: Bug #6 from the NFC Bugs Fixed section (2026-05-11). The issue was diagnosed as minor performance impact — this item tracks the fix.
- **Files to Modify**:
  - `targets/f7/furi_hal/furi_hal_pn532.c` — The `pn532_tx_rx()` or equivalent function that handles I2C data exchange. Before reading, determine expected response length from the request type. Or: read 2 bytes first (status + len), then read remaining `len` bytes.
- **Implementation Approach**:
  1. Two-phase read: first read 2 bytes (status + length byte), then compute remaining = length + 1 (checksum) + 1 (postamble). Read that many bytes.
  2. Works with the PN532 I2C read protocol: chip pulls data line low when ready, host reads 1 byte (status), then reads N-1 remaining bytes.
  3. Need to handle the case where we read 2 bytes and the length byte indicates 0-byte payload (status-only response). In that case, read only the trailing checksum byte.
  4. Risk of timing issue: if PN532 hasn't finished writing the response to its TX buffer, the 2-byte peek may see partial data. The PN532 signals data-ready via I2C handshake (nRSTPDN + IRQ), so this should not happen if handshake is respected.
- **Risk Assessment**: Low. Simple performance optimization. Risk: if two-phase read timing is wrong, can get partial frames. Add retry logic: if status byte is not in valid range (0x00-0x7F for success frame), retry up to 3 times with 1ms delay.
- **Testing**: Run the existing NFC unit tests. Measure I2C bus traffic before and after: should see shorter reads for short responses (e.g., InListPassiveTarget response goes from 270 bytes to ~20 bytes).

---

## Implementation Roadmap

### Phase 1 — Fix Critical Protocol Blockers
| Item | Depends On | Estimated Effort |
|---|---|---|
| NFC-01: Fix ISO14443-4A (0x0B Error) | None | 2-3 days |
| NFC-03: Add SRIX4K/SRIX512 | NFC-02 (raw frame path) | 2-3 days |

Phase 1 focus: Unlock the 5+ protocols blocked by NFC-01. While working on NFC-01, the InCommunicateThru raw frame path will also be built, which is prerequisite for NFC-02 (backdoor auth), NFC-03 (SRIX), and NFC-07 (CUID write).

### Phase 2 — Enhance Existing Protocols
| Item | Depends On | Estimated Effort |
|---|---|---|
| NFC-02: Enable Backdoor Auth | NFC-01 (raw frame path) | 1-2 days |
| NFC-04: Add NDEF Support | None (independent) | 2-3 days |
| NFC-05: Enable FeliCa Listener | None (independent) | 1 day |

Phase 2: After the raw frame path is established (from NFC-01), backdoor auth becomes straightforward. NDEF is parallelizable. FeliCa listener is independent and low risk.

### Phase 3 — New Capabilities
| Item | Depends On | Estimated Effort |
|---|---|---|
| NFC-06: EMV Reader | NFC-01 (4A must work) | 3-5 days |
| NFC-07: CUID Write | NFC-02 (backdoor auth) | 1 day |
| NFC-08: Value Block Ops | None (independent) | 1-2 days |

Phase 3: EMV is the most impactful item here — payment card reading is a flagship feature. CUID write and value blocks are smaller additions that build on Phase 2 work.

### Phase 4 — Advanced Features
| Item | Depends On | Estimated Effort |
|---|---|---|
| NFC-10: P2P (LLCP/SNEP) | NFC-01 (4A) + NFC-04 (NDEF) | 5-7 days |
| NFC-11: Dict Attack Perf | None | 1-2 days |
| NFC-12: MIFARE Plus | NFC-01 (4A) | 2-3 days |
| NFC-13: NTAG Password Auth | None | 1 day |
| NFC-09: FeliCa Dict | None | 1 day |
| NFC-14: ISO15693 Investigation | None | 0.5 day |
| NFC-15: Type 4 Tag Emulation | NFC-01 + NFC-04 | 3-5 days |
| NFC-16: Error Diagnostics | None | 1-2 days |
| NFC-17: Mfkey32 Polish | None | 1-2 days |
| NFC-18: I2C Read Optimization | None | 0.5 day |

Phase 4: Items with the highest complexity (P2P, Type 4 Tag emulation) and lowest priority go here. Many smaller items (NFC-09, 11, 13, 16, 17, 18) are independent and can be picked up opportunistically.

---

## Reference Project Quick Reference

| Improvement ID | Primary Reference | Secondary Reference | Key Files |
|---|---|---|---|
| NFC-01 | Flipper-Zero-ESP32-Port | BruceDevices firmware | `nfc_pn532.c`, `iso14443_4a_poller.c`, `PN532.cpp` |
| NFC-02 | Momentum-Firmware | BruceDevices firmware | `mf_classic_poller.c`, `PN532.cpp` |
| NFC-03 | BruceDevices firmware | PN532-on-STM32 | `lib/PN532_SRIX/pn532_srix.cpp`, `srix.h` |
| NFC-04 | Elechouse PN532 | Adafruit-PN532, BruceDevices | `NDEF/` directory, `apdu.cpp`, `Adafruit_PN532.cpp` |
| NFC-05 | Adafruit-PN532 | Elechouse PN532 | `Adafruit_PN532::AS_Target()`, `tgInitAsTarget()` |
| NFC-06 | BruceDevices firmware | Momentum-Firmware | `emv_reader.cpp`, `emv_poller.c` |
| NFC-07 | BruceDevices firmware | Momentum-Firmware | `PN532.cpp clone()`, `mf_classic_poller.c` |
| NFC-08 | PN532-on-STM32 | pn532-lib | `pn532_mifare.c`, MIFARE_CMD constants |
| NFC-09 | Momentum-Firmware | — | `felica_auth.h`, `felica_poller.c` |
| NFC-10 | Elechouse PN532 | Adafruit-PN532 | `LLCP/`, `SNEP/` directories, `Adafruit_PN532::startP2P()` |
| NFC-11 | Momentum-Firmware | — | `mf_classic_poller.c` key cache, dict_attack view |
| NFC-12 | Momentum-Firmware | — | `lib/nfc/protocols/mf_plus/` |
| NFC-13 | Momentum-Firmware | — | `mf_ultralight_poller.c` PWD_AUTH |
| NFC-14 | — | — | PN532 User Manual §7.3.1 (InCommunicateThru) |
| NFC-15 | Elechouse PN532 | — | `PN532::EmulateTag()` class |
| NFC-16 | Adafruit-PN532 | Elechouse PN532 | `get_error_code()`, status code tables |
| NFC-17 | Momentum-Firmware | — | `mfkey32_logger.c`, `nfc_scene_mfkey_complete.c` |
| NFC-18 | — | — | `furi_hal_pn532.c` (self-contained fix) |

---

## Reference Project Paths

| Project | Path |
|---|---|
| Adafruit-PN532 | `F:\FB_V3\diy_flipper_zero.worktrees\Adafruit-PN532` |
| BruceDevices | `F:\FB_V3\diy_flipper_zero.worktrees\firmware` |
| Elechouse PN532 | `F:\FB_V3\diy_flipper_zero.worktrees\PN532` |
| pn532-lib | `F:\FB_V3\diy_flipper_zero.worktrees\pn532-lib` |
| PN532-on-STM32 | `F:\FB_V3\diy_flipper_zero.worktrees\PN532-on-STM32` |
| Flipper-Zero-ESP32-Port | `F:\FB_V3\diy_flipper_zero.worktrees\Flipper-Zero-ESP32-Port` |
| Momentum-Firmware | `F:\FB_V3\Momentum-Firmware` |
| Flipper Firmware Fork | `F:\FB_V3\flipperzero-firmware` |

---

## Future Considerations (Post-Plan)

- **Bluetooth PASSPVT/FeliCa transport**: If FeliCa listener works (NFC-05), explore using the flipper as a FeliCa payment terminal accessory via BLE.
- **PN532 firmware update**: PN532 firmware V1.8 improves P2P stability. Consider updating firmware if P2P (NFC-10) has issues.
- **External antenna mod**: PN532 range is limited (~2cm with PCB antenna). For EMV (NFC-06), external antenna amplifier could improve reliability.
- **ST25R3916 upgrade path**: If PN532 limitations become unacceptable (especially lack of ISO15693, poor P2P), design a ST25R3916 daughterboard as long-term solution.
