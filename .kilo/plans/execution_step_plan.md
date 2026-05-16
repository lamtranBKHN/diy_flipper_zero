# NFC Implementation - Step-by-Step Execution Plan

## Legend
- ✅ **Done** (Phase 1 complete)
- 🚧 **In Progress**
- ⏳ **Pending**
- ❌ **Blocked** (dependency not met)

---

## Phase 2 — Enhance Existing Protocols

### Item 2A: NFC-02 — MIFARE Classic Backdoor Auth on PN532

**Goal**: Enable backdoor auth (0x64/0x65) for magic cards on PN532 via InCommunicateThru.

| Step | File | Action | Est. Effort |
|------|------|--------|-------------|
| 2A.1 | `furi_hal_pn532.h` | Add `furi_hal_pn532_mf_backdoor_auth()` and `furi_hal_pn532_mf_backdoor_write()` declarations | 5 min |
| 2A.2 | `furi_hal_pn532.c` | Implement backdoor auth: send 0x64 (Gen1A) or 0x65 (Gen2) with Crypto1-encrypted payload via InCommunicateThru. Auth flow: `[0x64/0x65, block, key_type, key(6), uid(4)]`. On success, card accepts raw write to block 0. | 30 min |
| 2A.3 | `furi_hal_pn532.c` | Implement `furi_hal_pn532_mf_backdoor_write_block0()`: after backdoor auth, write UID+SAK+ATQA+BCC to block 0 via InCommunicateThru followed by standard write to block 0. | 20 min |
| 2A.4 | `mf_classic_poller.c` | Remove `#ifndef FURI_HAL_NFC_PN532_ONLY` guard around backdoor detection (line 235-247). Add PN532-specific backdoor auth path using new HAL functions instead of raw txrx_custom_parity. | 30 min |
| 2A.5 | `mf_classic_poller_sync.c` | Add `mf_classic_poller_sync_write_block_with_uid()` that: (1) detects magic card via backdoor detection, (2) uses backdoor auth to authenticate, (3) writes block 0 with new UID+SAK+ATQA. | 20 min |
| 2A.6 | Build + test | Verify compilation; test with Gen1A/Gen2 CUID card | 10 min |

**Total**: ~2 hours

### Item 2B: NFC-04 — NDEF Extraction and Writing

**Goal**: Parse and display NDEF messages (URL, text) from MIFARE Classic, Ultralight, NTAG. Enable writing NDEF URI.

| Step | File | Action | Est. Effort |
|------|------|--------|-------------|
| 2B.1 | `lib/nfc/helpers/ndef.h` | Create NDEF record structures: `NdefRecord` (TNF, type, id, payload), `NdefMessage` (max 4 records). Declare `ndef_parse()`, `ndef_uri_build()`, `ndef_text_build()`. | 15 min |
| 2B.2 | `lib/nfc/helpers/ndef.c` | Implement NDEF TLV parser: scan block data starting at offset 4 (Ultralight/NTAG CC + NDEF), parse TLV `0x03` (NDEF message), extract records with TNF/type/id/payload. Handle URL (`U` prefix + URI code), text (`T` + lang + text). | 45 min |
| 2B.3 | `lib/nfc/helpers/ndef.c` | Implement NDEF URI builder: construct TLV `[0x03][len][NDEF header][URI prefix code][URI bytes]` + `[0xFE]` terminator. Handle padding to 4-byte block boundaries. | 20 min |
| 2B.4 | `applications/main/nfc/plugins/supported_cards/ndef.c` | Create NDEF card plugin: on `verify()` check for TLV `0x03` at expected offset. On `parse()` extract NDEF records and display URL/text in summary. Register in plugin table. | 30 min |
| 2B.5 | `applications/main/nfc/scenes/` | Add "Write NDEF URL" scene: text input for URL, selects NDEF-capable card type, writes via existing sync write functions. | 30 min |
| 2B.6 | Build + test | Verify compilation; test with NDEF-formatted tag | 10 min |

**Total**: ~2.5 hours

### Item 2C: NFC-05 — FeliCa Listener on PN532

**Goal**: Enable FeliCa card emulation via TGINITASTARGET with FeliCa parameters.

| Step | File | Action | Est. Effort |
|------|------|--------|-------------|
| 2C.1 | `furi_hal_nfc_pn532.c` | In `furi_hal_nfc_pn532_set_mode()`, remove the FeliCa listener early-return (line ~296-298). Add FeliCa target mode path using TGINITASTARGET with mode=0x02 (FeliCa), NFCID2t (8 bytes), PAD (8 bytes), SENSF_RES (4 bytes: 0x12 0xFC 0x00 0x00). | 20 min |
| 2C.2 | `furi_hal_nfc_felica.c` | In `furi_hal_nfc_felica_listener_init()`, add PN532-aware branch that calls PN532 `tgInitAsTarget` with FeliCa params. | 15 min |
| 2C.3 | `furi_hal_nfc_pn532.c` | Update `listener_wait_event()` to handle FeliCa data exchange (TgGetData/TgSetData) same as ISO14443-4A, but without I-block wrapping (FeliCa uses its own frame format). | 15 min |
| 2C.4 | Build + test | Verify compilation; test with Android phone NFC | 10 min |

**Total**: ~1 hour

---

## Phase 3 — New Capabilities

### Item 3A: NFC-06 — EMV Credit Card Reader

**Goal**: Read EMV payment cards (PAN, expiry, cardholder name). DEPENDS ON NFC-01 (4A working).

| Step | File | Action | Est. Effort |
|------|------|--------|-------------|
| 3A.1 | Verify 4A | Test that an ISO14443-4A card (e.g., DESFire) is detected and RATS/ATS completes. Fix any remaining 4A issues found during testing. | 1 hr |
| 3A.2 | `emv_poller.c` | Add PN532-specific APDU transport: ensure I-block handling in `exchange_internal` works for EMV APDUs (which can be up to 256 bytes). Add multi-block chaining support if needed. | 1 hr |
| 3A.3 | `apps/main/nfc/helpers/emv_parser.c` | Implement EMV TLV parser: parse BER-TLV from card responses (tags: 4F=AID, 50=AppLabel, 57=Track2, 5A=PAN, 5F20=CardholderName, 5F24=Expiry, 9F12=AppPreferredName). | 1 hr |
| 3A.4 | `apps/main/nfc/scenes/emv.c` | Create EMV display scene: show masked PAN, expiry, cardholder name, AID, transaction history. | 45 min |
| 3A.5 | Build + test | Test with real EMV contactless card | 30 min |

**Total**: ~4 hours

### Item 3B: NFC-07 — CUID Write

**Goal**: Write UID to Chinese clone cards (CUID/CUID2/Gen1A). DEPENDS ON NFC-02.

| Step | File | Action | Est. Effort |
|------|------|--------|-------------|
| 3B.1 | `mf_classic_poller_sync.c` | Implement in existing sync API: `mf_classic_poller_sync_cuid_write()` — detect card type, backdoor auth, write block 0 with new UID. | 30 min |
| 3B.2 | `nfc_app scenes` | Add "Write UID" option in MIFARE Classic card action menu. | 15 min |
| 3B.3 | Build + test | Test with CUID card | 10 min |

**Total**: ~1 hour

### Item 3C: NFC-08 — MIFARE Classic Value Block Operations

**Goal**: Recognize and operate on value blocks (INC/DEC/RESTORE/TRANSFER).

| Step | File | Action | Est. Effort |
|------|------|--------|-------------|
| 3C.1 | `mf_classic_poller_i.c` | Add `mf_classic_poller_value_op()`: after sector auth, send INC(0xC0), DEC(0xC1), RESTORE(0xC2), TRANSFER(0x50) commands through existing encrypted exchange. | 30 min |
| 3C.2 | `mf_classic_poller_sync.c` | Add `mf_classic_poller_sync_value_read()` (detect value block format), `mf_classic_poller_sync_value_increment()`, `mf_classic_poller_sync_value_decrement()`. | 30 min |
| 3C.3 | Build + test | Test with a value block on a known card | 15 min |

**Total**: ~1.25 hours

---

## Phase 4 — Advanced Features (Independent Items)

### Item 4A: NFC-13 — NTAG/Ultralight Password Auth

**Goal**: Read password-protected tags.

| Step | File | Action | Est. Effort |
|------|------|--------|-------------|
| 4A.1 | `mf_ultralight_poller_i.c` | Add `mf_ul_poller_pwd_auth()`: read AUTH0/PROT config from block 0x03, if auth required, call PWD_AUTH(0x1B) with 4-byte password, check PACK response. | 30 min |
| 4A.2 | `nfc_scenes` | Add password input scene for protected tags. | 15 min |
| 4A.3 | Build + test | Test with password-protected NTAG | 10 min |

### Item 4B: NFC-11 — Dictionary Attack Performance

**Goal**: Cache found keys across sectors to reduce I2C traffic.

| Step | File | Action | Est. Effort |
|------|------|--------|-------------|
| 4B.1 | `mf_classic_poller.c` | Add `found_keys_cache[40][2]` array. Before each auth attempt, check cache. On key found, try on all unattacked sectors immediately. | 30 min |
| 4B.2 | Build + test | Compare attack time before/after | 15 min |

### Item 4C: NFC-09 — FeliCa Auth Dictionary

**Goal**: Attempt known default CKs for FeliCa.

| Step | File | Action | Est. Effort |
|------|------|--------|-------------|
| 4C.1 | `felica_poller.c` | Add `felica_poller_dict_attack()`: iterate 10-20 known default CKs (all-zeros, FeliCa SDK test keys, transit system keys). Try CK via existing `felica_poller_auth()`. | 30 min |
| 4C.2 | Build + test | Test with FeliCa Lite card (default CK=all zeros) | 10 min |

### Item 4D: NFC-16 — Error Diagnostics

**Goal**: Decode PN532 status codes to human-readable strings.

| Step | File | Action | Est. Effort |
|------|------|--------|-------------|
| 4D.1 | `furi_hal_pn532.c` | Add `const char* furi_hal_pn532_strerror(uint8_t code)` with table of 16 PN532 error codes (0x00-0x0F). | 15 min |
| 4D.2 | `furi_hal_pn532.h` | Declare strerror function. | 5 min |
| 4D.3 | `furi_hal_nfc_pn532.c` | In exchange_internal, log decoded error string on failure. | 10 min |
| 4D.4 | `nfc scenes` | Add "NFC Diagnostics" scene showing last error. | 15 min |

### Item 4E: NFC-18 — I2C Read Optimization

**Goal**: Read only expected bytes instead of full 270-byte buffer.

| Step | File | Action | Est. Effort |
|------|------|--------|-------------|
| 4E.1 | `furi_hal_pn532.c` | In `pn532_exchange()`, after reading status byte, read 2 bytes (status+len), compute remaining `len+1` bytes, read that many. Fallback to 270-byte read on error. | 20 min |

### Item 4F: NFC-17 — Mfkey32 Polish

**Goal**: Better nonce display and auto-solving.

| Step | File | Action | Est. Effort |
|------|------|--------|-------------|
| 4F.1 | `mfkey32_logger.c` | Format nonces for mfkey32v2 tool compatibility (`UID NT NR AR`). | 15 min |
| 4F.2 | `nfc_scenes/mfkey_complete.c` | Show solved keys with "Add to dict" button. | 20 min |

### Item 4G: NFC-14 — ISO15693 Investigation

**Goal**: Determine if InCommunicateThru can drive ISO15693.

| Step | File | Action | Est. Effort |
|------|------|--------|-------------|
| 4G.1 | Research | Read PN532 UM §7.3.1, test INVENTORY cmd via InCommunicateThru. Write findings to `docs/nfc/pn532_iso15693.md`. | 30 min |

---

## Current Phase: Phase 2 Execution Order

```
Step 2A (NFC-02) ──┐
                   ├── Step 3B (NFC-07, depends on 2A)
Step 2B (NFC-04) ──┤
                   └── Step 3A (NFC-06, best with working 4A + NDEF)
Step 2C (NFC-05) ──┐ (independent)
```

**Execution plan**:
1. **NFC-02 first** (unlocks NFC-07)
2. **NFC-04 second** (unlocks NFC-15)
3. **NFC-05 third** (independent but small)
4. Then Phase 3 items
5. Phase 4 items in any order (all independent)

---

## Reference Code Snippets

### Backdoor Auth via InCommunicateThru (NFC-02)
```c
uint8_t cmd[] = {0x42, 0x64, block, key_type, k0, k1, k2, k3, k4, k5};
// InCommunicateThru raw frame: [0x42] [0x64/0x65] [block] [key_type] [key(6)]
// No UID needed — PN532 sends raw bytes to selected target
```

### NDEF URL Builder (NFC-04)
```c
// TLV = [0x03] [len] [NDEF msg] [0xFE]
// NDEF msg = [0xD1] [len] [0x55] [1+url_len] [URI_code] [url...]
// 0xD1 = MB|ME|IL=0|SR=1|TNF=1 (Well-known)
// 0x55 = 'U' (URI record type)
```

### FeliCa Listener TGINITASTARGET (NFC-05)
```c
uint8_t felica_params[] = {
    0x02,       // mode = FeliCa target
    NFCID2[0..7], // 8 bytes
    PAD[0..7],  // 8 bytes of padding
    0x12, 0xFC, 0x00, 0x00  // SENSF_RES
};
furi_hal_pn532_tg_init_as_target(felica_params, sizeof(felica_params), 1000);
```
