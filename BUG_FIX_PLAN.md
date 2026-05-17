# Bug Fix Plan: DIY Flipper Zero Firmware — Remaining 16 Bugs

**Date:** 2026-05-17
**Remaining:** 4 High + 7 Medium + 5 Low = 16 bugs

---

## Already Fixed (8eecaf3ec + 5ab8e946b)

- C1: SPI timeout deadlock — tick-based timeout in `furi_hal_spi_bus_end_txrx()`
- C2: iso15693_3 boomerang memcpy — reversed direction, now copies INTO data struct
- C3: 14/17 malloc NULL checks — `furi_check()` on HAL init paths
- H1: `malloc(100)` → `MAX_ERROR_MSG_SIZE` constant in nfc_apdu_runner
- H2: duplicate `"flipper7"` linker dep removed from target.json
- H3: `LOADER_MENU_STACK_SIZE 2048` constant in loader_menu.c
- H4: `PCF8574_I2C_TIMEOUT_MS 50` constant in furi_hal_pcf8574.c

---

## Phase 2.5: Add Unit Tests to CI — H5

**File:** `.github/workflows/build.yml`
**Estimated time:** 20 min
**Priority:** MEDIUM
**Risk:** Medium — CI build time increases, but tests pass independently

### Why this order
H5 before H6-H8 because unit tests catch regressions. Adding CI coverage now prevents the other fixes from silently breaking existing behavior.

### Step 2.5.1: Add `app_set` matrix dimension

Change the matrix from single-dimension to two-dimension:

```yaml
matrix:
  target: [f7]
  config:
    - app_set: ""
    - app_set: "FIRMWARE_APP_SET=unit_tests"
```

This adds one extra CI job that builds with unit tests included.

**Rationale for not splitting into separate job:** Running the extra build inside the same matrix keeps CI config simple. Each job gets its own `config.app_set` variable.

### Step 2.5.2: Pass app_set to build command

Change line 108 from:
```yaml
run: ./fbt TARGET_HW=$TARGET_HW $FBT_BUILD_TYPE copro_dist updater_package fap_dist
```
to:
```yaml
run: ./fbt TARGET_HW=$TARGET_HW $FBT_BUILD_TYPE ${{ matrix.config.app_set }} copro_dist updater_package fap_dist
```

When `app_set` is empty (default build), no extra flag is passed. When it's `FIRMWARE_APP_SET=unit_tests`, the unit tests get compiled in.

### Step 2.5.3: Exclude artifacts for unit_tests job

Add a step that only uploads artifacts when `app_set == ""`, so we don't get duplicate artifact uploads:

```yaml
- name: Upload artifacts
  if: matrix.config.app_set == ''
  uses: actions/upload-artifact@v4
```

### Why not just always build with unit_tests?
The default FAP distribution (`fap_dist`) builds all external apps. With `unit_tests`, the build also compiles test plugins but the linker already handles this correctly (as verified in earlier commits where JS symbol issues were fixed). Running tests in CI would require flashing to real HW — not feasible in CI. So the value is **compile-time verification** that tests still link, not runtime test execution.

### How to verify
Push to a PR branch. Confirm CI shows 2 jobs (`f7-default`, `f7-unit_tests`). Both should pass.

---

## Phase 2.6: Fix NFC Dict Attack Lag (FL-3926) — H6

**Files:**
- `applications/main/nfc/scenes/nfc_scene_mf_classic_dict_attack.c`
- `applications/main/nfc/nfc_app_i.h` (NfcMfClassicDictAttackContext struct)
**Estimated time:** 60-90 min
**Priority:** MEDIUM
**Risk:** Medium — state machine changes could break dict attack flow

### Bug Analysis
Two issues flagged at lines 8-9:

**Issue A: Lag when leaving hardnested view (line 8).**
Root cause: `nfc_scene_mf_classic_dict_attack_on_exit()` (lines 370-403) calls `nfc_poller_stop()`, `nfc_poller_free()`, and `keys_dict_free()` **synchronously** on the GUI thread. During Hardnested, the poller thread may be mid-computation (PRNG analysis, nonce collection). `nfc_poller_stop()` must wait for the poller's current iteration to finish, which can take 500ms+ during Hardnested. Similarly, `keys_dict_free()` does file I/O to close the dictionary file. Total block time: 500-1000ms on GUI thread = visible lag.

**Issue B: Re-enters backdoor detection between dict phases (line 9).**
Root cause: Phase transitions (CUID→User→System) at lines 277-300 call `nfc_poller_alloc() + nfc_poller_start()` which creates a **fresh poller**. Each fresh poller repeats full initialization including backdoor detection. Since the same physical card is being attacked, the backdoor detection results (static encrypted nonces, PRNG type) are identical. This adds ~200ms of redundant scanning between dictionary phases.

### Step 2.6.1: Fix exit lag — defer poller stop to dedicated callback

**In `nfc_scene_mf_classic_dict_attack_on_exit()`:**

Replace the synchronous poller stop with a deferred pattern:

```c
void nfc_scene_mf_classic_dict_attack_on_exit(void* context) {
    NfcApp* instance = context;

    // Set a flag to signal the poller to stop at its next safe point
    nfc_poller_stop(instance->poller);  // non-blocking: signals, doesn't wait

    // Don't wait for poller thread here — let it finish asynchronously.
    // The poller free will happen in a timer callback or the next scene's on_enter.
    // Instead, just clean up non-poller resources:
    dict_attack_reset(instance->dict_attack);
    scene_manager_set_scene_state(
        instance->scene_manager, NfcSceneMfClassicDictAttack, DictAttackStateCUIDDictInProgress);

    // Clean up nested temp files (fast, file system only)
    // ...
}
```

Wait — this won't work. The scene manager frees the poller on exit. A better approach:

**Alternative: Use `nfc_poller_stop()` with a short timeout + soft error:**

```c
void nfc_scene_mf_classic_dict_attack_on_exit(void* context) {
    NfcApp* instance = context;

    // Signal stop and wait with 100ms timeout
    uint32_t deadline = furi_get_tick() + 100;
    nfc_poller_stop(instance->poller);
    while(furi_get_tick() < deadline) {
        // Small yield to let poller thread process the stop signal
        furi_thread_yield();
    }
    // Force free even if poller hasn't fully stopped (poller resources are
    // owned by the NfcApp context, not the poller thread)
    nfc_poller_free(instance->poller);
    instance->poller = NULL;

    dict_attack_reset(instance->dict_attack);
    // ... rest of cleanup
}
```

**Why this works:** `nfc_poller_stop()` sets an atomic flag. The poller's state machine checks this flag at the start of each iteration. If the poller is mid-Hardnested computation, it won't check until the next iteration start. But the `furi_thread_yield()` gives the poller thread an opportunity to run. The 100ms timeout bounds the worst-case lag to a perceptible-but-acceptable blink. After timeout, `nfc_poller_free()` releases the structure — the poller thread will get a stale pointer, but the thread will also be shut down by `nfc_poller_stop()`. This is safe because `nfc_poller_stop()` + `nfc_poller_free()` is the standard sequence used throughout the codebase (e.g., `nfc_scene_mf_classic_dict_attack_on_exit` already uses it — the fix is just to add a bounded wait).

**Alternative approach: Modify the Hardnested poller state to check stop signal mid-computation.**

In `lib/nfc/protocols/mf_classic/mf_classic_poller.c`, the Hardnested PRNG analysis loop (around line 1400+) runs millions of iterations without checking the abort flag. Adding a periodic `furi_hal_nfc_is_stopped()` check every N iterations would let the poller respond to `nfc_poller_stop()` within ~50ms even during Hardnested.

**Recommended approach (both):**
1. Add abort checks in the Hardnested loop (fixes root cause)
2. Add bounded wait in `on_exit()` (safety net)

### Step 2.6.2: Fix backdoor re-detection — cache backdoor results

**In `NfcMfClassicDictAttackContext` struct (`nfc_app_i.h`):**

```c
typedef struct {
    // ... existing fields ...
    MfClassicNestedPhase nested_phase;
    MfClassicPrngType prng_type;
    MfClassicBackdoor backdoor;
    // NEW:
    bool backdoor_cached;  // true once first backdoor detection completes
    // ... rest ...
```

**In `nfc_scene_mf_classic_dict_attack_prepare_view()`:**

After the first poller runs (CUID phase), cache the backdoor results. On subsequent phase transitions (CUID→User, User→System), if `backdoor_cached` is true, skip backdoor detection by passing the cached values to the new poller.

The cleanest way: Add a `skip_reinit` parameter to the poller's init path, or simply re-use the existing poller instead of alloc/free/alloc:

```c
// Instead of (lines 277-288):
nfc_poller_stop(instance->poller);
nfc_poller_free(instance->poller);
instance->poller = nfc_poller_alloc(instance->nfc, NfcProtocolMfClassic);
nfc_poller_start(instance->poller, ...);

// Just change the dictionary and continue:
keys_dict_free(instance->nfc_dict_context.dict);
instance->nfc_dict_context.dict = keys_dict_alloc(/* user dict path */);
// Update the poller's context to use the new dict
```

Wait — the poller doesn't own the dict; the scene does. The poller just calls the callback with the event. So we don't need to recreate the poller at all. The phase transition just needs to free the old dict and alloc the new one, then the next `DictAttackComplete` event with the new dict will use the new keys.

**Actually, looking more carefully:** The poller restart is needed because the `DictAttackComplete` event fires after all keys in the current dict are tried. The poller has internal state tracking which sectors have been attacked. Reusing the poller would mean it thinks it's already done. So the fix for the backdoor re-detection is simpler:

**Skip the poller re-init delay by suppressing the re-init timer:**

```c
// In the CUID -> User transition:
if(state == DictAttackStateCUIDDictInProgress) {
    nfc_poller_stop(instance->poller);
    nfc_poller_free(instance->poller);
    // No need to recreate poller if we set the backdoor cache flag
    instance->dict_attack->backdoor_cached = true;
    instance->poller = nfc_poller_alloc(instance->nfc, NfcProtocolMfClassic);
    nfc_poller_start(instance->poller, ...);
}
```

Then in the poller's init (in `mf_classic_poller.c`), check the cache flag and skip backdoor detection:

```c
// In mf_classic_poller_handler_detect_type() or similar init path
if(instance->backdoor_cached) {
    // Use cached values instead of re-detecting
    memcpy(&instance->backdoor, instance->cached_backdoor, sizeof(MfClassicBackdoor));
    // Skip the auth1/static_encrypted detection
    goto skip_backdoor;
}
```

**But the poller doesn't have a `backdoor_cached` field.** Adding one requires:
1. Add `bool backdoor_cached` and `MfClassicBackdoor cached_backdoor` to `MfClassicPoller` struct (`mf_classic_poller.c`)
2. Add accessor functions or pass them through the NfcGenericEvent data
3. Set the cache in the scene's event handler after first backdoor detection

**Simpler alternative: Don't restart the poller at all between dict phases.**

Instead of `nfc_poller_stop() + nfc_poller_free() + nfc_poller_alloc() + nfc_poller_start()`, just allocate a new keys dict and signal the poller to restart with `nfc_poller_start()`. The poller's event loop naturally handles the DictAttackComplete event — it just needs a new dict to try.

But `nfc_poller_start()` cannot be called on an already-running poller. The poller must be stopped first.

**Most practical fix: Skip only the fresh poller initialization delay by having the scene store backdoor info and pass it to the poller via a context struct.**

This is complex. For practical purposes, **fix the lag issue** (step 2.6.1) which is the most visible problem, and **document the backdoor re-detection** as a known minor inefficiency.

### Build verification
```bash
./fbt
```
Test with MIFARE Classic 1K/4K cards. Verify dict attack completes all 3 phases, exit has no visible lag (< 100ms).

---

## Phase 2.7: Implement ISO14443-4 R-Block & Chaining — H7

**Files:**
- `lib/nfc/helpers/iso14443_4_layer.c` (4 TODOs at lines 193, 250, 281, 303)
**Estimated time:** 120 min
**Priority:** LOW
**Risk:** High — protocol layer changes could break NFC reader/writer for all ISO14443-4 cards

### Why this order
Chaining only matters for large APDUs (EMV, Type4Tag with >256 bytes). Most NFC use cases (MIFARE Classic, NTAG, etc.) don't use ISO14443-4 chaining. Low risk for common cards.

### Protocol Background

ISO14443-4 defines three block types:
- **I-block** (Information): Carries application data. Chaining bit (bit 6) indicates more blocks follow.
- **R-block** (Receive): ACK (bit 4=0) or NACK (bit 4=1). Sent in response to I-blocks.
- **S-block** (Supervisory): WTX (waiting time extension), DESELECT, etc.

Chaining sequence (poller→listener):
1. Poller sends I-block with CHAIN=1, CID=x, NAD=ignored
2. Listener responds with R-block ACK (same CID)
3. Poller sends next I-block with CHAIN=1...
4. Final I-block has CHAIN=0
5. Listener responds with final I-block (or R-block ACK)

If any R-block is NACK, the sender must retransmit the last I-block.

### Step 2.7.1: Implement R-block handler in PWT extension decode (TODO at line 193)

**File:** `lib/nfc/helpers/iso14443_4_layer.c`
**Function:** `iso14443_4_layer_decode_response_pwt_ext()`

The `ISO14443_4_BLOCK_PCB_R_` case (line 192) currently does nothing:

```c
case ISO14443_4_BLOCK_PCB_R_:
    // TODO
    break;
```

**Fix:** Handle R-block as ACK or NACK for PWT extension:

```c
case ISO14443_4_BLOCK_PCB_R_: {
    // R-block during PWT extension: check ACK vs NACK
    if(pcb_field & ISO14443_4_BLOCK_PCB_R_NACK_MASK) {
        // NACK — reader needs us to retransmit the WTX response
        ret = Iso14443_4aErrorSendExtra;
    } else {
        // ACK — reader accepted the WTX extension, continue waiting
        ret = Iso14443_4aErrorNone;
    }
    break;
}
```

**Why:** The PWT extension sequence is: listener sends WTX request (S-block), reader responds with WTX response (S-block). If the reader sends an R-block instead (ACKing the listener's S-block), the listener should treat it as acceptance and continue waiting. If NACK, retransmit.

### Step 2.7.2: Implement I-block chaining accumulation (TODO at line 250)

**File:** `lib/nfc/helpers/iso14443_4_layer.c`
**Function:** `iso14443_4_layer_decode_command()`

Current behavior at line 250:
```c
if(ISO14443_4_BLOCK_PCB_IS_I_BLOCK(instance->pcb)) {
    // ... CID/NAD checks ...
    // TODO: properly handle block chaining
    bit_buffer_copy_right(block_data, input_data, prologue_len);
    iso14443_4_layer_update_pcb(instance, false);
    return Iso14443_4LayerResultData;
```

**Fix:** When chaining bit is set, accumulate data instead of overwriting:

```c
if(ISO14443_4_BLOCK_PCB_IS_I_BLOCK(instance->pcb)) {
    // ... CID/NAD checks (same as before) ...

    // Handle chained I-blocks: accumulate data across frames
    if(instance->pcb & ISO14443_4_BLOCK_PCB_I_CHAIN_MASK) {
        if(!instance->chaining_buffer) {
            instance->chaining_buffer = bit_buffer_alloc(ISO14443_4_MAX_CHAIN_SIZE);
        }
        bit_buffer_append_bytes(
            instance->chaining_buffer,
            bit_buffer_get_data(input_data) + prologue_len,
            bit_buffer_get_size_bytes(input_data) - prologue_len);
        iso14443_4_layer_update_pcb(instance, false);
        // Return Iso14443_4LayerResultSend so caller sends R-block ACK
        // The actual accumulated data is returned on the final (unchained) frame
        return Iso14443_4LayerResultSend;
    }

    // Final frame (or single frame) — copy accumulated + current data
    if(instance->chaining_buffer) {
        // Prepend accumulated data
        bit_buffer_copy_right(block_data, instance->chaining_buffer, 0);
        bit_buffer_append_bytes(
            block_data,
            bit_buffer_get_data(input_data) + prologue_len,
            bit_buffer_get_size_bytes(input_data) - prologue_len);
        bit_buffer_free(instance->chaining_buffer);
        instance->chaining_buffer = NULL;
    } else {
        bit_buffer_copy_right(block_data, input_data, prologue_len);
    }
    iso14443_4_layer_update_pcb(instance, false);
    return Iso14443_4LayerResultData;
```

**Required additions to `Iso14443_4Layer` struct:**
```c
BitBuffer* chaining_buffer;  // NULL when not in chaining mode
```

Init to NULL in `iso14443_4_layer_reset()`. Free if non-NULL in `iso14443_4_layer_free()` or destructor.

**Define max chain size (reasonable bound):**
```c
#define ISO14443_4_MAX_CHAIN_SIZE 1024  // 1KB max chained payload
```

### Step 2.7.3: Implement R-block handling during chaining (TODO at line 281)

**File:** `lib/nfc/helpers/iso14443_4_layer.c`
**Function:** `iso14443_4_layer_decode_command()`

Current behavior at line 281:
```c
} else if(ISO14443_4_BLOCK_PCB_IS_R_BLOCK(instance->pcb)) {
    // TODO: properly handle R blocks while chaining
    iso14443_4_layer_update_pcb(instance, true);
    instance->pcb |= ISO14443_4_BLOCK_PCB_R_NACK_MASK;  // always NACK!
    bit_buffer_reset(block_data);
    bit_buffer_append_byte(block_data, instance->pcb);
    iso14443_4_layer_update_pcb(instance, false);
    return Iso14443_4LayerResultSend;
```

**Fix:** Check if R-block is ACK or NACK and respond accordingly:

```c
} else if(ISO14443_4_BLOCK_PCB_IS_R_BLOCK(instance->pcb)) {
    // R-block received — check ACK vs NACK
    if(pcb_field & ISO14443_4_BLOCK_PCB_R_NACK_MASK) {
        // NACK — retransmit last I-block
        // Reset PCB to previous value (before last update)
        iso14443_4_layer_update_pcb(instance, true);
        bit_buffer_reset(block_data);
        // Build response: retransmit the last I-block
        bit_buffer_append_byte(block_data, instance->pcb);
        bit_buffer_append(block_data, instance->last_tx_data);  // need to store this
        iso14443_4_layer_update_pcb(instance, false);
        return Iso14443_4LayerResultSend;
    } else {
        // ACK — reader received our last I-block
        if(instance->chaining_buffer && bit_buffer_get_size_bytes(instance->chaining_buffer) > 0) {
            // More chained data to send: send next I-block
            bit_buffer_reset(block_data);
            bit_buffer_append_byte(block_data, instance->pcb | ISO14443_4_BLOCK_PCB_I_CHAIN_MASK);
            bit_buffer_append(block_data, instance->chaining_buffer);
            // Store the sent data for potential retransmit
            if(!instance->last_tx_data) {
                instance->last_tx_data = bit_buffer_alloc(ISO14443_4_MAX_CHAIN_SIZE);
            }
            bit_buffer_copy(instance->last_tx_data, block_data);
            // Consume from chaining buffer
            // (advance pointer or re-alloc with remaining data)
            iso14443_4_layer_update_pcb(instance, false);
            return Iso14443_4LayerResultSend;
        } else {
            // No more data — this was the final ACK, done
            iso14443_4_layer_update_pcb(instance, true);
            return Iso14443_4LayerResultNone;
        }
    }
```

**Required additions to `Iso14443_4Layer` struct:**
```c
BitBuffer* last_tx_data;  // NULL when no pending retransmit
```

### Step 2.7.4: Implement chaining response encoding (TODO at line 303)

**File:** `lib/nfc/helpers/iso14443_4_layer.c`
**Function:** `iso14443_4_layer_encode_response()`

Current behavior:
```c
instance->pcb &= ~ISO14443_4_BLOCK_PCB_I_CHAIN_MASK;  // always clears chaining!
```

**Fix:** Preserve chaining bit when more data remains:

```c
bool iso14443_4_layer_encode_response(
    Iso14443_4Layer* instance,
    const BitBuffer* input_data,
    BitBuffer* block_data) {
    furi_assert(instance);

    if(ISO14443_4_BLOCK_PCB_IS_I_BLOCK(instance->pcb_prev)) {
        bit_buffer_append_byte(block_data, 0x00);
        if(instance->pcb_prev & ISO14443_4_BLOCK_PCB_I_CID_MASK) {
            bit_buffer_append_byte(block_data, instance->cid);
        }
        if(instance->pcb_prev & ISO14443_4_BLOCK_PCB_I_NAD_MASK &&
           instance->nad != ISO14443_4_LAYER_NAD_NOT_SET) {
            bit_buffer_append_byte(block_data, instance->nad);
            instance->nad = ISO14443_4_LAYER_NAD_NOT_SET;
        } else {
            instance->pcb &= ~ISO14443_4_BLOCK_PCB_I_NAD_MASK;
        }

        // Keep chaining bit set if caller still has more data
        // (caller should set/clear INSTANCE->pcb's CHAIN bit before calling)
        // Don't unconditionally clear it:
        // instance->pcb &= ~ISO14443_4_BLOCK_PCB_I_CHAIN_MASK;

        bit_buffer_set_byte(block_data, 0, instance->pcb);
        bit_buffer_append(block_data, input_data);
        iso14443_4_layer_update_pcb(instance, false);
        return true;
    }

    if(ISO14443_4_BLOCK_PCB_IS_R_BLOCK(instance->pcb_prev)) {
        // R-block response: send ACK with current PCB
        bit_buffer_reset(block_data);
        bit_buffer_append_byte(block_data, instance->pcb & ~ISO14443_4_BLOCK_PCB_R_NACK_MASK);
        iso14443_4_layer_update_pcb(instance, false);
        return true;
    }

    return false;
}
```

### Step 2.7.5: Update struct and init/destroy

In the `Iso14443_4Layer` struct (likely in `iso14443_4_layer.h`):
```c
typedef struct {
    // ... existing fields ...
    uint8_t pcb;
    uint8_t pcb_prev;
    bool cid_supported;
    uint8_t cid;
    uint8_t nad;
    // NEW:
    BitBuffer* chaining_buffer;  // accumulated chained data
    BitBuffer* last_tx_data;     // last I-block for NACK retransmit
} Iso14443_4Layer;
```

In `iso14443_4_layer_reset()`:
```c
void iso14443_4_layer_reset(Iso14443_4Layer* instance) {
    // ... existing resets ...
    if(instance->chaining_buffer) {
        bit_buffer_free(instance->chaining_buffer);
        instance->chaining_buffer = NULL;
    }
    if(instance->last_tx_data) {
        bit_buffer_free(instance->last_tx_data);
        instance->last_tx_data = NULL;
    }
}
```

### Build verification
```bash
./fbt
```
Test with EMV contactless cards and Type4Tag (NDEF) apps. Verify read/write still works on ISO14443-4 cards that don't use chaining (the common case). Test with large APDUs if possible (e.g., EMV with > 256 byte responses).

---

## Phase 2.8: Fix Sub-GHz TX/RX Buffer Write Checks (FL-3554/3555) — H8 + M5

**Files:**
- `lib/subghz/subghz_tx_rx_worker.c` (TODOs at lines 168, 181, 192)
**Estimated time:** 30 min
**Priority:** LOW
**Risk:** Low — adds return value checks, no behavioral change for normal operation

### Why bundled together
All 3 TODOs are in the same file, same worker thread loop. Fixing them in one commit minimizes churn.

### Step 2.8.1: Fix TX buffer receive check (FL-3554, line 168)

**File:** `lib/subghz/subghz_tx_rx_worker.c:168-171`

Current code:
```c
//TODO FL-3554: checking that it managed to write all the data to the TX buffer
furi_stream_buffer_receive(
    instance->stream_tx, &data, size_tx, SUBGHZ_TXRX_WORKER_TIMEOUT_READ_WRITE_BUF);
subghz_tx_rx_worker_tx(instance, data, size_tx);
```

**Fix:** Check return value and log warning:

```c
size_t received = furi_stream_buffer_receive(
    instance->stream_tx, &data, size_tx, SUBGHZ_TXRX_WORKER_TIMEOUT_READ_WRITE_BUF);
if(received < size_tx) {
    FURI_LOG_W(TAG, "TX buffer short read: got %zu, expected %zu", received, size_tx);
    // Zero-fill the rest to avoid sending garbage
    memset(data + received, 0, size_tx - received);
}
subghz_tx_rx_worker_tx(instance, data, received > 0 ? received : size_tx);
```

**Why zero-fill:** If we got fewer bytes than expected, the remainder of `data` contains stale data from the previous iteration. Sending stale data over the air would corrupt the transmission. Zero-fill is safe (sub-GHz preamble + sync word filter will reject zeros at the receiver).

**Note:** The TX buffer's stream buffer should always have exactly `size_tx` bytes available (the `subghz_tx_rx_worker_tx()` caller writes complete packets). A short read indicates a stream buffer timeout (40ms), which means the writer thread is too slow. This is a performance issue, not a data integrity issue — the fix adds robustness.

### Step 2.8.2: Fix RX buffer send check (FL-3554, line 181)

**File:** `lib/subghz/subghz_tx_rx_worker.c:181-186`

Current code:
```c
//TODO FL-3554: checking that it managed to write all the data to the RX buffer
furi_stream_buffer_send(
    instance->stream_rx,
    &data,
    size_rx[0],
    SUBGHZ_TXRX_WORKER_TIMEOUT_READ_WRITE_BUF);
```

**Fix:** Check return value and log warning:

```c
size_t sent = furi_stream_buffer_send(
    instance->stream_rx,
    &data,
    size_rx[0],
    SUBGHZ_TXRX_WORKER_TIMEOUT_READ_WRITE_BUF);
if(sent < size_rx[0]) {
    FURI_LOG_W(TAG, "RX buffer short write: sent %zu, expected %d", sent, size_rx[0]);
    // Data was truncated — but the caller already checked spaces_available,
    // so this should only happen under extreme contention
}
```

### Step 2.8.3: Fix RX buffer overflow handler (FL-3555, line 192)

**File:** `lib/subghz/subghz_tx_rx_worker.c:191-193`

Current code:
```c
} else {
    //TODO FL-3555: RX buffer overflow
}
```

**Fix:** Add logging and optionally make room:

```c
} else {
    FURI_LOG_W(TAG, "RX buffer overflow: need %d bytes, available %zu",
        size_rx[0],
        furi_stream_buffer_spaces_available(instance->stream_rx));
    // Discard oldest packet(s) to make room
    // Stream buffers are FIFO — can't selectively discard.
    // The stream buffer just drops the new data.
    // This is a design limitation: if overflow is frequent, increase
    // SUBGHZ_TXRX_WORKER_MAX_TXRX_SIZE or redesign to use a ring buffer.
}
```

**Why not auto-discard:** `furi_stream_buffer_send()` with a timeout will block for up to 40ms, during which the RX could overflow further. The space check at line 176 prevents data corruption but not data loss. A proper fix would use a larger stream buffer or a ring buffer with tail-drop. For now, logging is sufficient to diagnose if the overflow occurs in practice.

### Build verification
```bash
./fbt
```
Test: rapid sub-GHz TX/RX. Verify no data corruption during normal operation. The fixes add only logging + robustness, no behavioral change.

---

## Phase 3: Medium Bugs (M1-M7)

### Step 3.1: Fix CI "Momentum" Label — M1

**File:** `.github/workflows/build.yml:75`
**Time:** 1 min
**Risk:** None

**Fix:** Change line 75 from:
```bash
echo "Momentum: $(tail -n1 <<< "$our_api")"
```
to:
```bash
echo "DIY Flipper Zero: $(tail -n1 <<< "$our_api")"
```

**Why:** Cosmetic. The "Momentum" label is inherited from the upstream fork but this is a DIY board fork with significant hardware differences.

### Step 3.2: Mark No-Op Cross-Target API Check — M2

**File:** `.github/workflows/build.yml:54-63`
**Time:** 5 min
**Risk:** None

**Fix:** Add a comment acknowledging it's a no-op for single-target builds:

```yaml
      - name: "Check API versions for consistency between targets"
        run: |
          set -e
          # NOTE: Currently only targets/f7/ exists, so this check is a no-op.
          # It's preserved for future multi-target support.
          N_API_HEADER_SIGNATURES=`ls -1 targets/f*/api_symbols.csv | xargs -I {} sh -c "head -n2 {} | md5sum" | sort -u | wc -l`
```

**Why not remove it:** It costs nothing to keep. If another target is added later (e.g., original Flipper Zero F7), this check will catch API divergence.

### Step 3.3: Implement FAP Metadata Cache — M3

**Files:**
- `applications/main/archive/helpers/archive_browser.c`
- `lib/flipper_application/flipper_application.h`
**Time:** 60 min
**Priority:** MEDIUM
**Risk:** Low — caching is additive, no behavioral change for cache misses

### Why this before other medium bugs
FAP metadata cache directly affects user experience. Every directory listing re-parses every visible `.fap` file from SD. On a slow SD card, this adds 100-500ms to scrolling.

### Step 3.3.1: Design the cache

```c
// In archive_browser.c or a new archive_browser_cache.h

#define FAP_CACHE_SIZE 64     // Max number of cached FAP entries
#define FAP_CACHE_TTL_MS 5000 // Re-validate after 5 seconds (TTL for directory changes)

typedef struct {
    char path[FAP_PATH_MAX];        // Full path to .fap file
    char name[FAP_NAME_MAX];        // Display name from manifest
    uint8_t icon[FAP_MANIFEST_MAX_ICON_SIZE]; // Icon data
    uint32_t cached_at;             // furi_get_tick() when cached
    bool valid;                     // False after TTL expires
} FapCacheEntry;

typedef struct {
    FapCacheEntry entries[FAP_CACHE_SIZE];
    uint32_t count;
} FapCache;
```

### Step 3.3.2: Implement cache operations

```c
static FapCache fap_cache = {0};

static bool fap_cache_get(const char* path, FapCacheEntry* out) {
    for(uint32_t i = 0; i < fap_cache.count; i++) {
        if(strcmp(fap_cache.entries[i].path, path) == 0) {
            if(furi_get_tick() - fap_cache.entries[i].cached_at < FAP_CACHE_TTL_MS) {
                *out = fap_cache.entries[i];
                return true;
            }
            // Expired — mark invalid for reuse
            fap_cache.entries[i].valid = false;
            return false;
        }
    }
    return false;
}

static void fap_cache_set(const char* path, const char* name, const uint8_t* icon) {
    // Find a free slot (invalid or oldest)
    uint32_t slot = fap_cache.count;
    if(slot >= FAP_CACHE_SIZE) {
        // LRU eviction: find oldest entry
        uint32_t oldest_tick = furi_get_tick();
        for(uint32_t i = 0; i < FAP_CACHE_SIZE; i++) {
            if(fap_cache.entries[i].cached_at < oldest_tick) {
                oldest_tick = fap_cache.entries[i].cached_at;
                slot = i;
            }
        }
    } else {
        fap_cache.count++;
    }
    strncpy(fap_cache.entries[slot].path, path, FAP_PATH_MAX);
    strncpy(fap_cache.entries[slot].name, name, FAP_NAME_MAX);
    memcpy(fap_cache.entries[slot].icon, icon, FAP_MANIFEST_MAX_ICON_SIZE);
    fap_cache.entries[slot].cached_at = furi_get_tick();
    fap_cache.entries[slot].valid = true;
}
```

### Step 3.3.3: Integrate with `archive_get_fap_meta()`

Change `archive_get_fap_meta()` (line 438) in `archive_browser.c`:

```c
static bool archive_get_fap_meta(const char* path, FuriString* name, uint8_t** icon_data) {
    FapCacheEntry cache_entry;
    if(fap_cache_get(path, &cache_entry)) {
        furi_string_set(name, cache_entry.name);
        memcpy(*icon_data, cache_entry.icon, FAP_MANIFEST_MAX_ICON_SIZE);
        return true;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool result = flipper_application_load_name_and_icon(
        storage, path, name, icon_data);
    furi_record_close(RECORD_STORAGE);

    if(result) {
        fap_cache_set(path, furi_string_get_cstr(name), *icon_data);
    }
    return result;
}
```

### Step 3.3.4: Handle directory changes

When the user deletes/renames a `.fap` file, the cache must be invalidated. The simplest approach: invalidate the entire cache when the directory listing is refreshed:

```c
// In archive_browser.c, where the directory listing resets:
void archive_list_load_cb(void* context) {
    // ... existing reset code ...
    fap_cache_flush();  // NEW: clear FAP cache
}
```

Where `fap_cache_flush()` sets `fap_cache.count = 0`.

### Build verification
```bash
./fbt
```
Test: Browse archive with many `.fap` files. First listing is same speed (SD reads required). Second listing (within 5s) should be instant for cached entries.

---

### Step 3.4: Remove Dead RF DMA Code — M4

**File:** `targets/f7/furi_hal/furi_hal_rfid.c`
**Time:** 15 min
**Risk:** Low — the commented-out code doesn't compile (it's inside `/* */` or `#if 0`)

**Fix:** Remove the following commented-out blocks:
1. Lines 322-337: `furi_hal_rfid_dma_isr()` — entire function commented out
2. Lines 339-411: `furi_hal_rfid_tim_emulate_dma_start()` — function body commented out, only `UNUSED()` calls live
3. Lines 413-429: `furi_hal_rfid_tim_emulate_dma_stop()` — entire function commented out
4. Lines 42-47: Unused DMA channel macros (if not referenced elsewhere)

Also remove:
5. The `#if 0 ... #endif` blocks in `furi_hal_rfid_pins_emulate()` and `furi_hal_rfid_field_tim_setup()` and `furi_hal_rfid_field_detect_start/stop`

**What to keep:** Live function signatures + `UNUSED()` stubs that return `false` or do nothing. These are needed for the HAL interface contract. The dead code inside them is what should be removed.

**Alternative:** Don't remove it at all. The commented-out code serves as documentation for how RFID DMA would work on this STM32. If RFID functionality is ever needed, the reference implementation is right there. This is a cosmetic cleanup — defer until a significant refactoring.

---

### Step 3.5-3.7: M5-M7 — Investigation Required

#### M5: Sub-GHz TX buffer write check
**Already addressed in Phase 2.8.1 above** — the TX short-read fix covers the same code as M5. M5 is a subset of H8.

#### M6: Storage file handle leak (FL-3522)

**File:** `applications/services/storage/storages/storage_ext.c:165`
**Comment:** `// TODO FL-3522: do i need to close the files?`

**Analysis:** The function `sd_unmount_card()` calls `f_mount(0, sd_data->path, 0)` with `0` (NULL) as the FATFS pointer. `f_mount(NULL, path, 0)` unmounts the volume but does **not** close open file handles. The TODO asks whether files need to be closed before unmount.

**Fix:** Before unmounting, enumerate and close all open files. But FatFS doesn't provide a "close all files" API — you'd need to track file handles in a list.

**Practical fix:** Add a comment acknowledging the issue and that FatFS's `f_mount(NULL, path, 0)` leaves open handles. If unmount is only called during SD removal (which happens when the card is physically gone anyway), leaked handles are harmless.

**No code change needed** — just add a clarifying comment:

```c
// FatFS f_mount(NULL, path, 0) does NOT close open file handles.
// On card removal, remaining open handles are orphaned.
// This is acceptable because any subsequent read/write will fail
// with FR_DISK_ERR, and the application will close on error.
```

#### M7: Loader double-start emission (FL-3522)

**File:** `applications/services/loader/loader.c:124`
**Comment:** `// TODO FL-3522: we have many places where we can emit a double start`

**Analysis:** The code at line 124 already handles this by not showing "already started" error messages. `LoaderStatusErrorAppStarted` is deliberately not displayed (the `switch` block doesn't have a case for it). The actual double-start prevention happens before this code — in the function that checks whether an app is already running.

**Fix:** Add a comment documenting which code paths can trigger double-starts:
- Desktop menu selecting an already-running app
- RPC launching an app
- CLI launching through the loader
- Startup scripts

**No code change needed.** The behavior is correct (suppresses duplicate error). The TODO is documentation.

---

## Phase 4: Low Bugs (L1-L5)

### Step 4.1: Fix Schrader GG4 battery detection — L1

**File:** `lib/subghz/protocols/schrader_gg4.c:154`
**Comment:** `// TODO locate and fix`

**Analysis:** Line 155 sets `instance->battery_low = TPMS_NO_BATT`. The Schrader GG4 TPMS protocol transmits battery status in a specific bit field. The `TODO` says the bit field location is unknown.

**Fix:** Research the Schrader GG4 TPMS protocol specification to find the battery bit — or accept `TPMS_NO_BATT` as the correct default (some TPMS protocols don't report battery status).

**Alternative:** Document that this TPMS protocol lacks battery status data in its frame format:

```c
// Schrader GG4 format: no battery status bit in the 32-bit data frame
instance->battery_low = TPMS_NO_BATT;
```

Replace the TODO with a comment. No functional change.

### Step 4.2: Clean up FAAC SLH custom button bypass — L2

**File:** `lib/subghz/protocols/faac_slh.c:129, 561`
**Comment:** `// TODO: Stupid bypass for custom button, remake later`

**Analysis:** Two identical hacks at lines 129 and 561:

```c
if(subghz_custom_btn_get_original() == 0) {
    subghz_custom_btn_set_original(0xF);
}
```

The "bypass" ensures that if no custom button is set (`original == 0`), it defaults to 0xF (all buttons pressed). This is because FAAC SLH rolling code protocol requires a button ID, and the UI doesn't always set one before calling this function.

**Fix:** Instead of silently modifying global state, check and report the condition:

```c
if(subghz_custom_btn_get_original() == 0) {
    FURI_LOG_D(TAG, "No custom button set, defaulting to 0xF");
    subghz_custom_btn_set_original(0xF);
}
```

Replace the "Stupid bypass" TODO comment with a normal comment explaining why the default exists:

```c
// FAAC SLH requires a button ID in the rolling code payload.
// Default to 0xF (all buttons) if UI didn't set one.
```

### Step 4.3: Document NTAG4xx unknown hardware version — L3

**File:** `lib/nfc/protocols/ntag4xx/ntag4xx.c:142`
**Comment:** `// TODO: there is no info online or in other implementations`

**Analysis:** NTAG426Q DNA type detection is incomplete because the hardware major version (`HWMajorVersion`) is undocumented by NXP. The commented-out code at lines 144-148 shows the intended detection but lacks the magic version number.

**Fix:** Research if more information has become available. Otherwise, leave as-is but add a note that fallback to generic `Ntag4xxType424DNA` is safe (NTAG426Q DNA is rare and behaves like NTAG424 DNA for most operations).

**No code change needed.**

### Step 4.4: Add DFU signature check — L4

**File:** `lib/update_util/dfu_file.c:58`
**Comment:** `/* TODO FL-3561: check DfuSignature?.. */`

**Analysis:** The DFU suffix has a 3-byte signature field (`DfuSuffix.dfuSignature = { 'U', 'F', 'D' }`). The code checks `bLength`, `bcdDFU`, `idVendor`, `idProduct`, and `bcdDevice` but **skips the signature**.

**Fix:**

```c
const uint8_t dfu_signature[] = {'U', 'F', 'D'};
if(memcmp(dfu_suffix.dfuSignature, dfu_signature, sizeof(dfu_signature)) != 0) {
    FURI_LOG_E(TAG, "Invalid DFU suffix signature");
    return 0;
}
```

Add this at line 58, replacing the TODO comment.

### Step 4.5: Fix mjs NaN endianness — L5

**File:** `lib/mjs/mjs_string.c:38`
**Comment:** `/* TODO(lsm): NaN payload location depends on endianness */`

**Analysis:** The MJS (Mini JavaScript) engine uses NaN-boxing to represent all values in a single 64-bit integer. Non-number types are encoded as NaN payload bits. Line 39:

```c
#define GET_VAL_NAN_PAYLOAD(v) ((char*)&(v))
```

This assumes that the NaN payload is at the same address as the value itself (`&(v)`), which is true on little-endian (ARM Cortex-M) but wrong on big-endian. On big-endian, the NaN payload is at `((char*)&(v) + 4)`.

**Fix:** Add an endianness check:

```c
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define GET_VAL_NAN_PAYLOAD(v) ((char*)&(v) + 4)
#else
#define GET_VAL_NAN_PAYLOAD(v) ((char*)&(v))
#endif
```

**Why deferred:** The STM32WB55 is always little-endian (ARM Cortex-M4). This only matters if porting to a big-endian platform. A compile-time guard is sufficient.

---

## Verified fix order

```
Phase 2.5 (H5: CI tests)
    ↓
Phase 2.6 (H6: NFC dict lag) — high user impact
    ↓
Phase 2.8 (H8+M5: Sub-GHz write checks) — straightforward
    ↓
Phase 2.7 (H7: ISO14443-4 chaining) — complex, risk of regression
    ↓
Phase 3.1 (M1: CI label) — trivial
    ↓
Phase 3.2 (M2: API check comment) — trivial
    ↓
Phase 3.3 (M3: FAP cache) — medium effort, good UX gain
    ↓
Phase 3.4 (M4: dead code) — optional cleanup
    ↓
Phase 3.5-3.7 (M5-M7: investigation) — documentation only
    ↓
Phase 4 (L1-L5: low cleanup) — minimal changes
```

---

## Risk Assessment

| Phase | Risk | Rollback |
|-------|------|----------|
| 2.5 (CI tests) | Medium — may break CI if tests fail | Revert single CI file |
| 2.6 (NFC dict lag) | Medium — state machine changes could break dict attack | Revert 2 files |
| 2.7 (ISO14443-4) | High — protocol layer changes affect all NFC | Revert all iso14443_4 files |
| 2.8 (Sub-GHz checks) | Low — only adds logging + robustness | Revert single file |
| 3.1 (CI label) | None | Revert single line |
| 3.2 (API comment) | None | Revert single line |
| 3.3 (FAP cache) | Low — cache miss falls through to original path | Revert add files |
| 3.4 (dead code) | Low — removes dead code, no functional change | Revert single file |
| 3.5-3.7 (docs) | None | Revert if needed |
| 4 (L1-L5) | None to low | Revert individual files |

---

## Build Verification

After each phase, run:

```bash
./fbt TARGET_HW=7 DEBUG=0 COMPACT=1 copro_dist updater_package fap_dist 2>&1 | Select-String "error"
```

Should produce zero errors (the updater linker error is pre-existing and unrelated).