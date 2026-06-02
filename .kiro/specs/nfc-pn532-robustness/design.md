# Design Document: NFC PN532 Robustness Fixes

## Overview

Four bugs degrade NFC emulation and scanning reliability on the DIY Flipper Zero (STM32WB55 + PN532 over I2C). This document provides concrete code-level designs for all four fixes.

**Files modified:**
- `targets/f7/furi_hal/furi_hal_nfc_pn532.c` — Bugs 1, 2, 4
- `targets/f7/furi_hal/furi_hal_nfc_pn532.h` — Bug 4 (new function declaration)
- `targets/f7/furi_hal/furi_hal_pn532.c` — Bug 3 (new `poll_iso15693`)
- `targets/f7/furi_hal/furi_hal_pn532.h` — Bug 3 (new function declaration)
- `lib/nfc/nfc_scanner.c` — Bug 3 (probe order)

---

## Architecture

The PN532 HAL is split into two layers:

```
NfcScanner / NFC pollers / NFC listeners
        |
furi_hal_nfc_pn532.c   (protocol-level HAL: listener_wait_event, exchange_internal)
        |
furi_hal_pn532.c       (chip-level driver: InListPassiveTarget, InDataExchange, TgInitAsTarget)
        |
I2C1 @ 0x48 (PN532)
```

---

## Bug 1: 4/7-byte UID Emulation (TgInitAsTarget NFCID1 Truncation)

### Root Cause

In `furi_hal_nfc_pn532_listener_wait_event()`, the ISO14443-3A branch of the `TgInitAsTarget` parameter block construction copies `listener_uid[0..2]` directly into `params[3..5]` (NFCID1) regardless of `listener_uid_len`. For 4-byte and 7-byte UIDs this is wrong: the PN532 needs a Cascade Tag (`0x88`) at `params[3]` and SAK bit 2 set to signal to the reader that the UID continues at the next anticollision level.

### TgInitAsTarget Parameter Block Layout

```
Byte  0:    mode (0x05 = passive 106 kbps, PICC only)
Bytes 1-2:  SENS_RES (ATQA, 2 bytes)
Bytes 3-5:  NFCID1 (3 bytes — always 3 bytes; CT used for cascade)
Byte  6:    SEL_RES (SAK)
Bytes 7-24: FeliCa params (18 bytes, zeroed for ISO14443-3A)
Bytes 25-34: NFCID3t (10 bytes — full UID for ISO-DEP readers)
Byte  35:   Gt_len (0)
Byte  36:   Tk_len (0)  [total: 37 bytes, but PN532 accepts 36]
```

### State Machine

```
set_col_res_data(uid, uid_len, atqa, sak)
        |
        v
listener_configured = true
listener_uid[0..uid_len-1] = uid
listener_uid_len = uid_len
listener_sak = sak
listener_active = false   <-- forces TgInitAsTarget rebuild

        |
        v
listener_wait_event() -- !listener_active branch
        |
        +-- uid_len == 3 --> params[3..5] = uid[0..2], params[6] = sak
        |
        +-- uid_len == 4 --> params[3] = 0x88 (CT)
        |                    params[4..5] = uid[0..1], params[5] = uid[2]
        |                    params[6] = sak | 0x04
        |
        +-- uid_len == 7 --> params[3] = 0x88 (CT)
                             params[4..5] = uid[0..1], params[5] = uid[2]
                             params[6] = 0x04  (cascade, final SAK deferred)
        |
        v (all cases)
params[25..25+uid_len-1] = uid[0..uid_len-1]  (NFCID3t)
        |
        v
TgInitAsTarget(params, 36, timeout_ms)
```

### Concrete Code Change

**File:** `targets/f7/furi_hal/furi_hal_nfc_pn532.c`

Replace the existing `listener_configured` block inside `listener_wait_event()`:

```c
// BEFORE (buggy — always copies uid[0..2] regardless of uid_len):
if(furi_hal_nfc_pn532.listener_configured) {
    params[1] = furi_hal_nfc_pn532.listener_atqa[0];
    params[2] = furi_hal_nfc_pn532.listener_atqa[1];
    params[3] = (furi_hal_nfc_pn532.listener_uid_len >= 1) ?
                    furi_hal_nfc_pn532.listener_uid[0] : 0x00;
    params[4] = (furi_hal_nfc_pn532.listener_uid_len >= 2) ?
                    furi_hal_nfc_pn532.listener_uid[1] : 0x00;
    params[5] = (furi_hal_nfc_pn532.listener_uid_len >= 3) ?
                    furi_hal_nfc_pn532.listener_uid[2] : 0x00;
    params[6] = furi_hal_nfc_pn532.listener_sak;
}
```

```c
// AFTER (correct — cascade encoding for 4/7-byte UIDs):
if(furi_hal_nfc_pn532.listener_configured) {
    const uint8_t* uid     = furi_hal_nfc_pn532.listener_uid;
    const uint8_t  uid_len = furi_hal_nfc_pn532.listener_uid_len;
    const uint8_t  sak     = furi_hal_nfc_pn532.listener_sak;

    params[1] = furi_hal_nfc_pn532.listener_atqa[0];
    params[2] = furi_hal_nfc_pn532.listener_atqa[1];

    if(uid_len <= 3) {
        /* 3-byte UID: copy directly, SAK unmodified */
        params[3] = (uid_len >= 1) ? uid[0] : 0x00;
        params[4] = (uid_len >= 2) ? uid[1] : 0x00;
        params[5] = (uid_len >= 3) ? uid[2] : 0x00;
        params[6] = sak;
    } else {
        /* 4 or 7-byte UID: cascade level 1
         * NFCID1[0] = CT (0x88) signals to reader that UID continues.
         * NFCID1[1..3] = uid[0..2] (first 3 bytes of actual UID).
         * SEL_RES bit 2 (0x04) tells reader to proceed to next cascade level. */
        params[3] = 0x88; /* CT — Cascade Tag */
        params[4] = uid[0];
        params[5] = uid[1];
        /* params[5] is NFCID1[2]; uid[2] goes here */
        params[5] = uid[2]; /* overwrite: NFCID1 is [CT, uid[0], uid[1], uid[2]] */
        params[6] = (uid_len == 4) ? (sak | 0x04) : 0x04;
        /* For 7-byte UIDs the PN532 handles cascade level 2 internally
         * using the NFCID3t field; the final SAK is returned at level 2. */
    }
    /* NFCID3t: populate with full UID for ISO-DEP readers (bytes 25..34) */
    for(uint8_t i = 0; i < uid_len && i < 10; i++) {
        params[25 + i] = uid[i];
    }
}
```

> **Note on params[5] assignment:** The NFCID1 field is 3 bytes at indices 3, 4, 5. For cascade mode these are `[CT, uid[0], uid[1]]` — but the PN532 spec places the third NFCID1 byte at index 5, so `params[5] = uid[2]` is correct. The intermediate `params[5] = uid[1]` line above is a typo in the pseudocode; the actual implementation should be:
> ```c
> params[3] = 0x88;   // CT
> params[4] = uid[0];
> params[5] = uid[1]; // NFCID1[2] — note: PN532 uses 3-byte NFCID1 field
> // uid[2] goes into NFCID3t at params[25+2], not into NFCID1
> ```
> The PN532 `TgInitAsTarget` NFCID1 is exactly 3 bytes. For a 4-byte UID `[A, B, C, D]`, cascade level 1 presents `[CT=0x88, A, B]` as NFCID1 and the reader then does a second SELECT to get `[C, D, BCC]`. The PN532 handles the second level using the NFCID3t bytes. Correct final form:

```c
if(uid_len <= 3) {
    params[3] = (uid_len >= 1) ? uid[0] : 0x00;
    params[4] = (uid_len >= 2) ? uid[1] : 0x00;
    params[5] = (uid_len >= 3) ? uid[2] : 0x00;
    params[6] = sak;
} else {
    /* 4 or 7-byte UID: cascade level 1 */
    params[3] = 0x88;   /* CT */
    params[4] = uid[0];
    params[5] = uid[1];
    params[6] = (uid_len == 4) ? (sak | 0x04) : 0x04;
}
/* Always populate NFCID3t with full UID */
for(uint8_t i = 0; i < uid_len && i < 10; i++) {
    params[25 + i] = uid[i];
}
```

### Testing Strategy

- Unit test: build params for uid_len=3, verify `params[3..5] == uid[0..2]`, `params[6] == sak`
- Unit test: build params for uid_len=4, verify `params[3] == 0x88`, `params[6] & 0x04 != 0`, `params[25..28] == uid[0..3]`
- Unit test: build params for uid_len=7, verify `params[3] == 0x88`, `params[6] == 0x04`, `params[25..31] == uid[0..6]`
- Integration: emulate a 4-byte UID card against a real reader, verify reader accepts the card

---

## Bug 2: ISO-DEP Chaining (Multi-Block Responses Silently Truncated)

### Root Cause

In `furi_hal_nfc_pn532_exchange_internal()`, the I-block path calls `furi_hal_pn532_in_data_exchange()` once and immediately reconstructs a single I-block response. If the card's response has the chaining bit set in its PCB byte (`PCB & 0x10 != 0`), the remaining fragments are never fetched. The caller receives only the first fragment.

### How PN532 Signals Chaining

The PN532 `InDataExchange` response strips the ISO14443-4 PCB byte and returns only the INF payload. However, the PN532 sets **bit 6 of its own status byte** (the first byte of the raw PN532 response, before `furi_hal_pn532_in_data_exchange` strips it) to `0x40` when the card indicated chaining. Since `furi_hal_pn532_in_data_exchange` already strips the PN532 status byte, the chaining signal must be detected differently.

**Correct approach:** After `in_data_exchange` returns the INF payload, reconstruct the PCB byte that the card would have sent. The PN532 internally tracks the block number; the reconstructed PCB for the response is `0x02 | (block_num & 1)`. The chaining bit in the card's original PCB is reflected in the PN532 status byte bit 6 (`0x40`). We need to expose this from `in_data_exchange`.

**Simpler approach (used here):** Modify `furi_hal_pn532_in_data_exchange` to return the raw PN532 status byte alongside the payload, or check the status byte before stripping. The status byte is the first byte of the PN532 response payload (after the command echo byte `0x41`). Bit 6 (`0x40`) of the status byte indicates the card sent a chained response.

### Data Flow

```
exchange_internal() receives I-block from upper layer
        |
        v
Strip PCB header, extract INF bytes
        |
        v
furi_hal_pn532_in_data_exchange(INF)
        |
        v
PN532 response: [status_byte][card_data...]
  status_byte bit 6 (0x40) = card indicated chaining
        |
        +-- chaining bit clear --> assemble single I-block, return
        |
        +-- chaining bit set  --> save fragment, send R(ACK), loop
                |
                v
        furi_hal_pn532_in_data_exchange(R(ACK))
                |
                v
        Append fragment payload to assembled buffer
                |
                v
        Check chaining bit again --> loop or exit
                |
                v
        Reconstruct final I-block from assembled buffer, return
```

### Interface Change: Expose PN532 Status Byte

Add a new internal function in `furi_hal_pn532.c` that returns the raw PN532 status byte alongside the payload:

**File:** `targets/f7/furi_hal/furi_hal_pn532.h` — add declaration:

```c
/**
 * InDataExchange with raw PN532 status byte returned.
 * @param pn532_status  Output: raw PN532 status byte (bit 6 = 0x40 means card chaining active)
 * All other parameters identical to furi_hal_pn532_in_data_exchange.
 */
FuriHalPn532Error furi_hal_pn532_in_data_exchange_ex(
    uint8_t target_number,
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len,
    uint8_t* pn532_status);
```

**File:** `targets/f7/furi_hal/furi_hal_pn532.c` — implement by refactoring the existing `furi_hal_pn532_in_data_exchange`:

```c
FuriHalPn532Error furi_hal_pn532_in_data_exchange_ex(
    uint8_t target_number,
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len,
    uint8_t* pn532_status) {

    if(tx_len > PN532_MAX_TX_PAYLOAD - 3) return FuriHalPn532ErrorComm;

    uint8_t cmd[PN532_MAX_TX_PAYLOAD];
    cmd[0] = PN532_CMD_IN_DATA_EXCHANGE;
    cmd[1] = target_number;
    memcpy(&cmd[2], tx_data, tx_len);

    uint8_t response[PN532_MAX_RX_FRAME];
    size_t response_len = 0;

    FuriHalPn532Error err = pn532_exchange(
        cmd, tx_len + 2,
        PN532_CMD_IN_DATA_EXCHANGE + 1,
        response, sizeof(response), &response_len,
        PN532_TIMEOUT_EXCHANGE_MS);

    if(err != FuriHalPn532ErrorNone) return err;
    if(response_len < 2) return FuriHalPn532ErrorInvalidFrame;

    /* response[0] = 0x41 (cmd echo), response[1] = PN532 status byte */
    const uint8_t status = response[1];
    if(pn532_status) *pn532_status = status;

    if(status != 0x00 && (status & 0x3F) != 0x00) {
        /* Non-zero lower 6 bits = PN532 error code */
        FURI_LOG_W(TAG, "InDataExchange status 0x%02X: %s", status,
                   furi_hal_pn532_strerror(status & 0x3F));
        return FuriHalPn532ErrorComm;
    }

    const size_t payload_len = response_len - 2;
    if(payload_len > rx_size) return FuriHalPn532ErrorBufferOverflow;
    memcpy(rx_data, &response[2], payload_len);
    if(rx_len) *rx_len = payload_len;
    return FuriHalPn532ErrorNone;
}

/* Original function now delegates to _ex, ignoring status byte */
FuriHalPn532Error furi_hal_pn532_in_data_exchange(
    uint8_t target_number,
    const uint8_t* tx_data, size_t tx_len,
    uint8_t* rx_data, size_t rx_size, size_t* rx_len) {
    return furi_hal_pn532_in_data_exchange_ex(
        target_number, tx_data, tx_len, rx_data, rx_size, rx_len, NULL);
}
```

### Chaining Assembly in exchange_internal

**File:** `targets/f7/furi_hal/furi_hal_nfc_pn532.c` — replace the I-block path inside `exchange_internal()`:

```c
// BEFORE (buggy — single exchange, no chaining):
if(hdr_len < send_len) {
    err = furi_hal_pn532_in_data_exchange(
        furi_hal_nfc_pn532.target.target_number,
        &tx_bytes[hdr_len], send_len - hdr_len,
        rx_payload, PN532_MAX_FRAME_SIZE, &rx_len);
}
if(err == FuriHalPn532ErrorNone && rx_len > 0) {
    // Rebuild I-block response: [PCB] [data] [CRC]
    uint8_t resp_pcb = 0x02 | (furi_hal_nfc_pn532.iso_dep_block_num & 1);
    furi_hal_nfc_pn532.iso_dep_block_num ^= 1;
    // ... assemble and return
}
```

```c
// AFTER (correct — chaining assembly loop):
#define PN532_STATUS_CHAINING 0x40  /* PN532 status bit: card indicated chaining */

if(hdr_len < send_len) {
    /* Assembled buffer lives in a local stack array.
     * PN532_MAX_FRAME_SIZE (192) is the hard cap. */
    uint8_t assembled[PN532_MAX_FRAME_SIZE];
    size_t  assembled_len = 0;
    uint8_t pn532_status  = 0;

    err = furi_hal_pn532_in_data_exchange_ex(
        furi_hal_nfc_pn532.target.target_number,
        &tx_bytes[hdr_len], send_len - hdr_len,
        assembled, sizeof(assembled), &assembled_len,
        &pn532_status);

    /* Chaining loop: while PN532 signals more fragments, send R(ACK) */
    while(err == FuriHalPn532ErrorNone &&
          assembled_len > 0 &&
          (pn532_status & PN532_STATUS_CHAINING)) {

        /* R(ACK): PCB = 0xA2 | (block_num & 1), no INF bytes */
        uint8_t rack = (uint8_t)(0xA2U | (furi_hal_nfc_pn532.iso_dep_block_num & 1U));

        uint8_t frag[PN532_MAX_FRAME_SIZE];
        size_t  frag_len = 0;
        pn532_status = 0;

        err = furi_hal_pn532_in_data_exchange_ex(
            furi_hal_nfc_pn532.target.target_number,
            &rack, 1,
            frag, sizeof(frag), &frag_len,
            &pn532_status);

        if(err != FuriHalPn532ErrorNone || frag_len == 0) break;

        /* Overflow guard */
        if(assembled_len + frag_len > sizeof(assembled)) {
            furi_hal_nfc_pn532.last_error = FuriHalPn532ErrorBufferOverflow;
            furi_hal_nfc_pn532.last_error_tick = furi_get_tick();
            return FuriHalNfcErrorBufferOverflow;
        }

        memcpy(&assembled[assembled_len], frag, frag_len);
        assembled_len += frag_len;
    }

    if(err == FuriHalPn532ErrorNone && assembled_len > 0) {
        /* Copy assembled payload into scratch for I-block reconstruction */
        memcpy(rx_payload, assembled, assembled_len);
        rx_len = assembled_len;

        /* Rebuild I-block: [PCB][assembled_payload][CRC] */
        uint8_t resp_pcb = (uint8_t)(0x02U | (furi_hal_nfc_pn532.iso_dep_block_num & 1U));
        furi_hal_nfc_pn532.iso_dep_block_num ^= 1U;

        memmove(&furi_hal_nfc_pn532.scratch[1], furi_hal_nfc_pn532.scratch, rx_len);
        furi_hal_nfc_pn532.scratch[0] = resp_pcb;
        size_t iob_len = 1 + rx_len;
        if(iob_len + 2 > PN532_MAX_FRAME_SIZE) iob_len = PN532_MAX_FRAME_SIZE - 2;
        const uint16_t crc = furi_hal_nfc_pn532_crc_a(furi_hal_nfc_pn532.scratch, iob_len);
        furi_hal_nfc_pn532.scratch[iob_len]     = (uint8_t)(crc & 0xFFU);
        furi_hal_nfc_pn532.scratch[iob_len + 1] = (uint8_t)(crc >> 8U);

        furi_hal_nfc_pn532_prepare_rx(furi_hal_nfc_pn532.scratch, iob_len + 2, false, false);
        return furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532ErrorNone, true);
    }
    /* Fall through to error handling */
} else {
    err = FuriHalPn532ErrorComm;
}
```

### Testing Strategy

- Unit test: single-fragment response (status byte bit 6 clear) — verify 1 call to `in_data_exchange_ex`, assembled payload equals fragment
- Unit test: 2-fragment chain — verify 2 calls, assembled payload equals concatenation of both fragments
- Unit test: 3-fragment chain — verify 3 calls, assembled payload equals concatenation of all 3
- Unit test: overflow — fragments that would exceed 192 bytes total return `FuriHalNfcErrorBufferOverflow`
- Integration: read a card that returns chained APDU responses (e.g. large SELECT response)

---

## Bug 3: ISO15693 Polling Not Implemented

### Root Cause

`NfcProtocolIso15693_3` is excluded from `pn532_probe_order[]` in `nfc_scanner.c` with the comment "causes I2C deadlock". That deadlock was caused by a now-fixed bug in the I2C retry/drain logic. Additionally, `furi_hal_pn532_poll_iso15693()` does not exist in `furi_hal_pn532.c`, so even adding it to the probe order would crash.

### ISO15693 InListPassiveTarget Protocol

The PN532 supports ISO15693 via `InListPassiveTarget` with `BrTy = 0x05`. The initiator data is a 2-byte INVENTORY command: `[0x26, 0x01]` (flags=0x26: inventory flag + nb_slots=1, mask_len=0x01).

**Response format** (from PN532 `InListPassiveTarget` response):
```
[0x4B]          -- response code (cmd+1)
[NbTg=1]        -- number of targets found
[Tg=1]          -- target number
[DSFID(1)]      -- Data Storage Format Identifier
[UID(8)]        -- UID, LSB-first (PN532 wire format)
```
Total minimum response length: 1 (cmd) + 1 (NbTg) + 1 (Tg) + 1 (DSFID) + 8 (UID) = 12 bytes.

The UID must be reversed to MSB-first before storing in `FuriHalPn532Target.uid`.

### State Machine

```
nfc_scanner probes NfcProtocolIso15693_3
        |
        v
furi_hal_pn532_poll_iso15693(target)
        |
        v
Build InListPassiveTarget command:
  [0x4A, 0x01, 0x05, 0x26, 0x01]
  cmd=0x4A, MaxTg=1, BrTy=0x05 (ISO15693), InitData=[0x26,0x01]
        |
        v
pn532_exchange(cmd, 5, 0x4B, response, 32, &resp_len, POLL_MS)
        |
        +-- error or resp_len < 12 --> return false
        |
        +-- response[2] == 0 (NbTg=0) --> return false (no card)
        |
        +-- response[2] >= 1 --> parse:
                DSFID = response[3]
                UID[0..7] = response[4..11] (LSB-first from PN532)
                target->uid[i] = response[4 + 7 - i]  (reverse to MSB-first)
                target->uid_len = 8
                return true
```

### Concrete Code Changes

**File:** `targets/f7/furi_hal/furi_hal_pn532.h` — add declaration:

```c
/**
 * Poll for ISO15693 (vicinity) cards using InListPassiveTarget BrTy=0x05.
 * @param target  Output struct; uid[0..7] is MSB-first (reversed from wire).
 * @return true if a card was found and target populated.
 */
bool furi_hal_pn532_poll_iso15693(FuriHalPn532Target* target);
```

**File:** `targets/f7/furi_hal/furi_hal_pn532.c` — add implementation after `furi_hal_pn532_poll_iso14443b`:

```c
bool furi_hal_pn532_poll_iso15693(FuriHalPn532Target* target) {
    if(target) memset(target, 0, sizeof(*target));

    /* InListPassiveTarget: MaxTg=1, BrTy=0x05 (ISO15693 at 26 kbps),
     * InitiatorData = [0x26, 0x01] = INVENTORY request, 1 time slot */
    uint8_t cmd[] = {PN532_CMD_IN_LIST_PASSIVE, 0x01, 0x05, 0x26, 0x01};
    uint8_t response[32] = {0};
    size_t  response_len = 0;

    FuriHalPn532Error error = pn532_exchange(
        cmd,
        sizeof(cmd),
        PN532_CMD_IN_LIST_PASSIVE + 1,
        response,
        sizeof(response),
        &response_len,
        PN532_TIMEOUT_POLL_MS);

    if(error != FuriHalPn532ErrorNone) return false;

    /* Minimum valid response: [0x4B][NbTg][Tg][DSFID][UID(8)] = 12 bytes */
    if(response_len < 12) return false;

    /* response[0] = 0x4B (cmd echo), response[1] = NbTg */
    if(response[1] == 0) return false; /* no card found */

    if(!target) return true; /* caller only wanted presence check */

    /* response[2] = Tg (target number, 1-based)
     * response[3] = DSFID
     * response[4..11] = UID, LSB-first (PN532 wire format per ISO15693) */
    target->target_number = response[2];
    target->uid_len = 8;

    /* Reverse UID from LSB-first (wire) to MSB-first (Flipper convention) */
    for(int i = 0; i < 8; i++) {
        target->uid[i] = response[4 + (7 - i)];
    }

    /* Store DSFID in atqa[0] for upper layers that need it */
    target->atqa[0] = response[3]; /* DSFID */
    target->atqa[1] = 0x00;
    target->sak = 0; /* not applicable for ISO15693 */

    FURI_LOG_D(TAG, "ISO15693 card found, UID: %02X%02X%02X%02X%02X%02X%02X%02X",
               target->uid[0], target->uid[1], target->uid[2], target->uid[3],
               target->uid[4], target->uid[5], target->uid[6], target->uid[7]);

    return true;
}
```

**File:** `lib/nfc/nfc_scanner.c` — add `NfcProtocolIso15693_3` to probe order:

```c
// BEFORE:
static const NfcProtocol pn532_probe_order[] = {
    NfcProtocolIso14443_3a,
    NfcProtocolIso14443_3b,
    NfcProtocolFelica,
    // NfcProtocolIso15693_3 omitted for PN532 (unsupported, causes I2C deadlock)
};

// AFTER:
static const NfcProtocol pn532_probe_order[] = {
    NfcProtocolIso14443_3a,
    NfcProtocolIso14443_3b,
    NfcProtocolFelica,
    NfcProtocolIso15693_3,  /* I2C deadlock resolved; poll_iso15693 now implemented */
};
```

### Testing Strategy

- Unit test: parse a synthetic INVENTORY response with known UID bytes, verify MSB-first reversal
- Unit test: response shorter than 12 bytes returns false without crash
- Unit test: response with NbTg=0 returns false
- Integration: present an ISO15693 card, verify scanner detects `NfcProtocolIso15693_3`
- Regression: ISO14443-3A, ISO14443-3B, FeliCa probes unaffected (timing unchanged)

---

## Bug 4: FeliCa Emulation Uses Hardcoded NFCID2t

### Root Cause

In `furi_hal_nfc_pn532_listener_wait_event()`, the FeliCa branch builds a hardcoded `params[]` array with NFCID2t bytes `{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}`. The `listener_uid` stored by `set_col_res_data()` is never used in the FeliCa path. Any reader that checks the NFCID2t (all real FeliCa readers do) will reject the emulated card.

### State Additions

**File:** `targets/f7/furi_hal/furi_hal_nfc_pn532.c` — add fields to `FuriHalNfcPn532State`:

```c
typedef struct {
    // ... existing fields ...

    /* FeliCa emulation NFCID2t (8 bytes).
     * Set by furi_hal_nfc_pn532_listener_set_felica_params().
     * Used by listener_wait_event() FeliCa branch. */
    uint8_t felica_nfcid2t[8];
    bool    felica_nfcid2t_configured;
} FuriHalNfcPn532State;
```

Also add `felica_nfcid2t_configured = false` to `furi_hal_nfc_pn532_reset()`.

### New Function

**File:** `targets/f7/furi_hal/furi_hal_nfc_pn532.h` — add declaration:

```c
/**
 * Configure the NFCID2t for FeliCa listener (emulation) mode.
 * Must be called before furi_hal_nfc_pn532_listener_wait_event().
 * Forces re-initialisation of TgInitAsTarget on the next wait_event call.
 * @param nfcid2t  8-byte NFCID2t of the FeliCa card being emulated.
 */
void furi_hal_nfc_pn532_listener_set_felica_params(const uint8_t* nfcid2t);
```

**File:** `targets/f7/furi_hal/furi_hal_nfc_pn532.c` — add implementation:

```c
void furi_hal_nfc_pn532_listener_set_felica_params(const uint8_t* nfcid2t) {
    furi_check(nfcid2t);
    memcpy(furi_hal_nfc_pn532.felica_nfcid2t, nfcid2t, 8);
    furi_hal_nfc_pn532.felica_nfcid2t_configured = true;
    /* Force TgInitAsTarget to re-run with the new NFCID2t on next wait_event */
    furi_hal_nfc_pn532.listener_active = false;
    FURI_LOG_D(TAG, "FeliCa NFCID2t configured: %02X%02X%02X%02X%02X%02X%02X%02X",
               nfcid2t[0], nfcid2t[1], nfcid2t[2], nfcid2t[3],
               nfcid2t[4], nfcid2t[5], nfcid2t[6], nfcid2t[7]);
}
```

### FeliCa Branch Change in listener_wait_event

**File:** `targets/f7/furi_hal/furi_hal_nfc_pn532.c`

```c
// BEFORE (buggy — hardcoded NFCID2t):
if(furi_hal_nfc_pn532.tech == FuriHalNfcTechFelica) {
    uint8_t params[] = {0x02, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                        0x07, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x12, 0xFC, 0x00, 0x00};
    err = furi_hal_pn532_tg_init_as_target(params, sizeof(params), slice_ms);
}
```

```c
// AFTER (correct — uses configured NFCID2t or spec-compliant default):
if(furi_hal_nfc_pn532.tech == FuriHalNfcTechFelica) {
    /* FeliCa TgInitAsTarget params layout (21 bytes):
     * [0]     mode = 0x02 (passive 212/424 kbps)
     * [1..8]  NFCID2t (8 bytes) — the FeliCa card's IDm
     * [9..16] PAD (8 bytes, zeroed)
     * [17..18] SENSF_RES system code (0x12, 0xFC = NFC-F generic)
     * [19..20] reserved (0x00, 0x00) */
    uint8_t params[21];
    memset(params, 0, sizeof(params));
    params[0] = 0x02; /* mode: passive FeliCa */

    /* Select NFCID2t: use configured value if available, else NFC-F default.
     * Default {0x01, 0xFE, ...} uses manufacturer code 0x01 and random
     * prefix 0xFE per NFC-F specification section 6.2.1. */
    static const uint8_t felica_nfcid2t_default[8] =
        {0x01, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    const uint8_t* nfcid2t = furi_hal_nfc_pn532.felica_nfcid2t_configured ?
                              furi_hal_nfc_pn532.felica_nfcid2t :
                              felica_nfcid2t_default;

    memcpy(&params[1], nfcid2t, 8);  /* NFCID2t at bytes 1..8 */
    params[17] = 0x12;               /* SENSF_RES system code high byte */
    params[18] = 0xFC;               /* SENSF_RES system code low byte  */

    err = furi_hal_pn532_tg_init_as_target(params, sizeof(params), slice_ms);
}
```

### State Machine

```
furi_hal_nfc_pn532_listener_set_felica_params(nfcid2t)
        |
        v
felica_nfcid2t[0..7] = nfcid2t[0..7]
felica_nfcid2t_configured = true
listener_active = false   <-- forces TgInitAsTarget rebuild

        |
        v
listener_wait_event() -- !listener_active, tech == FeliCa
        |
        +-- felica_nfcid2t_configured == true  --> params[1..8] = felica_nfcid2t
        |
        +-- felica_nfcid2t_configured == false --> params[1..8] = {0x01,0xFE,...}
        |
        v
TgInitAsTarget(params, 21, slice_ms)
        |
        v
listener_active = true
return FuriHalNfcEventFieldOn | FuriHalNfcEventListenerActive
```

### Testing Strategy

- Unit test: call `set_felica_params({0xAA,...})`, verify `felica_nfcid2t_configured == true` and `listener_active == false`
- Unit test: build FeliCa params with configured NFCID2t, verify `params[1..8] == nfcid2t`
- Unit test: build FeliCa params without configuration, verify `params[1] == 0x01` and `params[2] == 0xFE`
- Unit test: call `set_felica_params(N1)` then `set_felica_params(N2)`, verify params use N2
- Integration: emulate a FeliCa card against a real reader, verify reader accepts the card with correct NFCID2t

---

## Data Models

### FuriHalNfcPn532State (updated)

```c
typedef struct {
    bool active;
    bool target_valid;
    bool listener_active;
    FuriHalNfcMode mode;
    FuriHalNfcTech tech;
    FuriHalPn532Target target;
    uint32_t target_tick;
    uint8_t rx_buffer[PN532_MAX_FRAME_SIZE];
    uint8_t scratch[PN532_MAX_FRAME_SIZE];
    size_t rx_bits;
    FuriHalNfcEvent event_queue[PN532_EVENT_QUEUE_CAPACITY];
    size_t event_head;
    size_t event_count;
    FuriHalPn532Error last_error;
    FuriHalNfcPn532Result last_result;
    uint32_t last_error_tick;
    uint8_t mf_auth_key[6];
    uint8_t mf_auth_key_type;
    bool mf_auth_key_valid;
    uint8_t cached_ats[20];
    size_t cached_ats_len;
    bool iso_dep_mode;
    bool needs_relist;
    uint8_t iso_dep_block_num;
    bool mf_authed;

    /* Listener (target) mode — ISO14443-3A */
    uint8_t listener_uid[10];
    uint8_t listener_uid_len;
    uint8_t listener_atqa[2];
    uint8_t listener_sak;
    bool listener_configured;

    /* Listener (target) mode — FeliCa (Bug 4) */
    uint8_t felica_nfcid2t[8];          /* NEW: configured NFCID2t */
    bool    felica_nfcid2t_configured;  /* NEW: true after set_felica_params called */
} FuriHalNfcPn532State;
```

### FuriHalPn532Target (unchanged)

No changes to this struct. The `uid[10]` field is large enough for ISO15693 8-byte UIDs. The `atqa[0]` field is repurposed to carry DSFID for ISO15693 targets (upper layers that need DSFID can read it from there).

---

## Error Handling

| Bug | Error Condition | Return Value |
|-----|----------------|--------------|
| Bug 1 | `uid_len > 10` (impossible via API) | Clamped to 10 by NFCID3t loop |
| Bug 2 | `InDataExchange` fails mid-chain | Loop exits, `finalize_exchange` returns comm error |
| Bug 2 | Assembled payload > 192 bytes | `FuriHalNfcErrorBufferOverflow` immediately |
| Bug 3 | PN532 I2C error during ISO15693 poll | `false` returned, PN532 state unchanged |
| Bug 3 | Response too short (< 12 bytes) | `false` returned, no buffer overread |
| Bug 4 | `set_felica_params(NULL)` | `furi_check(nfcid2t)` asserts (programming error) |

---

## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid executions of a system — essentially, a formal statement about what the system should do. Properties serve as the bridge between human-readable specifications and machine-verifiable correctness guarantees.*

### Property 1: Cascade Tag for Multi-Byte UIDs

*For any* UID of length 4 or 7, when `listener_wait_event()` builds the `TgInitAsTarget` parameter block, `params[3]` SHALL equal `0x88` (Cascade Tag).

**Validates: Requirements 2.1, 2.2**

### Property 2: SAK Cascade Bit for Multi-Byte UIDs

*For any* UID of length 4 or 7 and any SAK value, when `listener_wait_event()` builds the `TgInitAsTarget` parameter block, `params[6]` SHALL have bit 2 set (`params[6] & 0x04 != 0`).

**Validates: Requirements 2.1, 2.2**

### Property 3: NFCID3t Populated with Full UID

*For any* UID of length 4 or 7, when `listener_wait_event()` builds the `TgInitAsTarget` parameter block, `params[25 + i]` SHALL equal `uid[i]` for all `i` in `[0, uid_len)`.

**Validates: Requirements 2.3**

### Property 4: 3-Byte UID Regression

*For any* 3-byte UID and any SAK value, when `listener_wait_event()` builds the `TgInitAsTarget` parameter block, `params[3..5]` SHALL equal `uid[0..2]` and `params[6]` SHALL equal `sak` without modification.

**Validates: Requirements 2.4, 3.1**

### Property 5: Chained Response Assembly

*For any* sequence of N fragments (N ≥ 2) where each fragment except the last has the PN532 chaining status bit set, when `exchange_internal()` completes, the payload returned to the caller SHALL equal the byte-for-byte concatenation of all N fragment payloads in order.

**Validates: Requirements 5.1, 5.2**

### Property 6: Single-Fragment No Extra Calls

*For any* single-fragment ISO-DEP response (PN532 chaining status bit clear), when `exchange_internal()` completes, the number of `InDataExchange` calls issued SHALL equal exactly 1.

**Validates: Requirements 6.1**

### Property 7: ISO15693 UID Byte Reversal

*For any* 8-byte UID in LSB-first wire format (as returned by the PN532 INVENTORY response), when `furi_hal_pn532_poll_iso15693()` parses the response, `target->uid[i]` SHALL equal `wire_uid[7 - i]` for all `i` in `[0, 8)`.

**Validates: Requirements 8.2**

### Property 8: FeliCa NFCID2t Round-Trip

*For any* 8-byte NFCID2t value `N`, when `furi_hal_nfc_pn532_listener_set_felica_params(N)` is called and `listener_wait_event()` subsequently builds the FeliCa `TgInitAsTarget` parameter block, `params[1 + i]` SHALL equal `N[i]` for all `i` in `[0, 8)`.

**Validates: Requirements 11.2**

### Property 9: FeliCa set_felica_params Resets listener_active

*For any* 8-byte NFCID2t value, when `furi_hal_nfc_pn532_listener_set_felica_params()` is called, `furi_hal_nfc_pn532.listener_active` SHALL be `false` immediately after the call returns.

**Validates: Requirements 11.4**

### Property 10: FeliCa Last-Write-Wins

*For any* two distinct 8-byte NFCID2t values `N1` and `N2`, when `set_felica_params(N1)` is called followed by `set_felica_params(N2)`, the `TgInitAsTarget` parameter block built by the next `listener_wait_event()` call SHALL use `N2`, not `N1`.

**Validates: Requirements 11.2, 11.4**

---
