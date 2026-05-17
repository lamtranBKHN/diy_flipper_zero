# Bug Fix Plan: DIY Flipper Zero Firmware

**Date:** 2026-05-17
**Total bugs:** 23 new + 8 unstaged fixes
**Estimated effort:** 4-6 hours (23 new bugs) + commit (8 unstaged)

---

## Phase 0: Commit Existing Fixes (10 min)

**Step 0.1:** Commit the 8 already-fixed bugs (13 files, +217/-59 lines)

```
git add -A
git commit -m "fix: 8 bugs — sizeof pointer, PCF8574 cooldown, PN532 ACK, InCommunicateThru timeout, I-block PCB, I2C3 crash, SSD1306 guard, menu cache"
```

**Why first:** These are already implemented and tested. Leaving them unstaged risks losing work. Clean baseline before new fixes.

**Files affected:**
- `AGENTS.md`
- `applications/main/nfc/scenes/nfc_scene_mf_classic_dict_attack.c`
- `applications/services/loader/loader_menu.c`
- `lib/nfc/SConscript`
- `lib/nfc/protocols/mf_classic/mf_classic_poller.c`
- `scripts/fastfap.py`
- `scripts/fbt_tools/fbt_extapps.py`
- `targets/f7/api_symbols.csv`
- `targets/f7/furi_hal/furi_hal_i2c_config.c`
- `targets/f7/furi_hal/furi_hal_nfc_pn532.c`
- `targets/f7/furi_hal/furi_hal_pcf8574.c`
- `targets/f7/furi_hal/furi_hal_pn532.c`
- `targets/f7/furi_hal/furi_hal_resources.h`
- `lib/nfc/helpers/ndef_write.c` (new)
- `lib/nfc/helpers/ndef_write.h` (new)

---

## Phase 1: Critical Bugs — Must Fix (1-2 hours)

### Step 1.1: Fix SPI timeout deadlock (C1)
**File:** `targets/f7/furi_hal/furi_hal_spi.c:93-102`

**Change:** Replace `UNUSED(timeout)` and infinite busy-wait loops with tick-based timeout:

```c
static void furi_hal_spi_bus_end_txrx(const FuriHalSpiBusHandle* handle, uint32_t timeout) {
    uint32_t start = furi_get_tick();
    while(LL_SPI_GetTxFIFOLevel(handle->bus->spi) != LL_SPI_TX_FIFO_EMPTY) {
        if(furi_get_tick() - start >= timeout) return; // timeout — drain will happen on next call
    }
    start = furi_get_tick();
    while(LL_SPI_IsActiveFlag_BSY(handle->bus->spi)) {
        if(furi_get_tick() - start >= timeout) return;
    }
    while(LL_SPI_GetRxFIFOLevel(handle->bus->spi) != LL_SPI_RX_FIFO_EMPTY) {
        LL_SPI_ReceiveData8(handle->bus->spi);
    }
}
```

**Why this order:** This is the single highest-impact bug. SPI1 deadlock freezes the entire system (display + CC1101 + SD). Every ViewPort lockup warning traces to this. Fixing this first eliminates the most severe failure mode.

**Risk:** Low. Timeout is already passed by callers; we just need to use it. The function is `void` so we can't return error — draining on next call is acceptable.

**Test:** Build, flash, stress-test SPI bus (rapid display refresh + SD I/O + sub-GHz hop).

---

### Step 1.2: Fix iso15693_3 boomerang memcpy (C2)
**File:** `lib/nfc/protocols/iso15693_3/iso15693_3.c:97-114`

**Analysis:** The function `iso15693_3_load_security_legacy()` reads legacy format security data. The current code:
1. malloc's `legacy_data` (line 97)
2. Reads hex values into `legacy_data` from file (line 98)
3. Parses first byte into lock_bits (lines 102-103)
4. memcpy FROM `data->block_security` INTO `&legacy_data[1]` (lines 106-109) — **this is backwards**
5. Sets `loaded = true` (line 111)
6. Frees `legacy_data` (line 114) — **the memcpy'd data is lost**

**Fix:** The intent is to populate `data->block_security` from the legacy file format. The memcpy should be:

```c
memcpy(
    simple_array_get_data(data->block_security),  // destination: existing array
    &legacy_data[1],                               // source: legacy file data (skip lock byte)
    simple_array_get_count(data->block_security)); // size
```

**Why this order:** This is a data corruption bug. Any ISO15693 card saved in legacy format and reloaded will have its block security data silently replaced with garbage (the memcpy copies FROM the existing array INTO a temp buffer that is then freed). This means the card's security state is lost on reload.

**Risk:** Medium. Need to verify the simple_array is already allocated with the correct size before memcpy. Check `simple_array_get_count(data->block_security)` matches `value_count - 1`.

**Test:** Save an ISO15693 card in legacy format, reload, verify block security matches original.

---

### Step 1.3: Add NULL checks after malloc (C3) — 17 instances

**Strategy:** Batch fix all 17 instances in a single commit. Pattern:

```c
// Before:
ptr = malloc(size);
ptr->field = value;  // potential NULL deref

// After (init path — should never fail):
ptr = malloc(size);
furi_check(ptr);
ptr->field = value;

// After (runtime path — can fail gracefully):
ptr = malloc(size);
if(!ptr) return SomeError;
ptr->field = value;
```

**Files to fix (in order of criticality):**

1. `targets/f7/src/update.c:58, 80, 153` — firmware update path, crash = bricked device
2. `applications/services/loader/loader_menu.c:79, 269, 452` — menu crash = UX failure
3. `lib/nfc/protocols/mf_classic/mf_classic_poller.c:34, 1115, 1636` — NFC crash during card read
4. `targets/f7/furi_hal/furi_hal_nfc_event.c:11` — NFC event handling
5. `targets/f7/furi_hal/furi_hal_memory.c:33` — memory subsystem init
6. `targets/f7/furi_hal/furi_hal_ibutton.c:38` — iButton init
7. `targets/f7/furi_hal/furi_hal_adc.c:92` — ADC init
8. `targets/f7/furi_hal/furi_hal_serial_control.c:266` — serial control init
9. `targets/f7/ble_glue/ble_glue.c:73` — BLE init
10. `targets/f7/ble_glue/ble_app.c:82` — BLE app init
11. `targets/f7/ble_glue/gap.c:544` — GAP init

**Why this order:** Update path first (bricking risk), then user-facing (menu, NFC), then HAL init (all use `furi_check` since init should never fail), then BLE (less critical).

**Risk:** Low. `furi_check()` is the standard pattern in this codebase. For init paths, OOM is unrecoverable anyway — crashing is better than silent corruption.

**Test:** Build, verify no compilation warnings. Runtime test requires inducing OOM (hard on embedded).

---

## Phase 2: High Bugs — Should Fix (1-2 hours)

### Step 2.1: Fix malloc(100) magic numbers (H1)
**File:** `applications/external/nfc_apdu_runner/nfc_worker.c:291, 346, 398`

```c
#define APDU_ERROR_MSG_MAX 100
// ...
worker->error_message = malloc(APDU_ERROR_MSG_MAX);
snprintf(worker->error_message, APDU_ERROR_MSG_MAX, ...);
```

**Why:** Simple fix, eliminates 3 magic numbers, prevents future buffer overflow if format string grows.

---

### Step 2.2: Remove duplicate linker dependency (H2)
**File:** `targets/f7/target.json:52`

Remove the second `"flipper7"` entry. One-liner.

**Why:** Trivial fix, cleans up technical debt.

---

### Step 2.3: Define thread stack size constants (H3)
**Files:** `loader_menu.c:66`, `nfc_worker.c:582`

```c
#define LOADER_MENU_STACK_SIZE 2048
#define NFC_WORKER_STACK_SIZE 8192
```

**Why:** Simple fix, enables compile-time validation and future stack analysis.

---

### Step 2.4: Define PCF8574 I2C timeout constant (H4)
**File:** `targets/f7/furi_hal/furi_hal_pcf8574.c`

```c
#define PCF8574_I2C_TIMEOUT_MS 50
```

Replace all 4 instances of `50` with the constant.

**Why:** Simple fix, single source of truth for timeout value.

---

### Step 2.5: Add unit tests to CI (H5)
**File:** `.github/workflows/build.yml`

Add a matrix entry or separate job:

```yaml
matrix:
  target: [f7]
  app_set: [default, unit_tests]
# ...
- run: ./fbt TARGET_HW=$TARGET_HW FIRMWARE_APP_SET=${{ matrix.app_set }} copro_dist
```

**Why:** Zero automated test coverage is a major risk. This catches regressions before they reach devices.

**Risk:** Medium. May increase CI time. Consider running unit_tests only on `dev` branch pushes, not every PR.

---

### Step 2.6: Fix NFC dict attack lag (H6) — FL-3926
**File:** `applications/main/nfc/scenes/nfc_scene_mf_classic_dict_attack.c:8-9`

**Requires investigation first.** The TODO mentions:
1. Lag when leaving hardnested dict attack view
2. Re-entering backdoor detection between user and system dictionary

**Approach:**
1. Profile the exit path — identify what's blocking
2. Add state flag to skip backdoor detection when transitioning between dictionaries
3. Test with real MIFARE Classic cards

**Why deferred:** Requires real hardware testing and understanding of the hardnested algorithm's state machine.

---

### Step 2.7: Implement ISO14443-4 R-block handling (H7)
**File:** `lib/nfc/helpers/iso14443_4_layer.c:193, 250, 281, 303`

**Requires protocol spec review.** The TODOs at lines 193, 250, 281, 303 all reference missing R-block and block chaining handling.

**Approach:**
1. Read ISO14443-4 spec sections on R-blocks and chaining
2. Implement `ISO14443_4_BLOCK_PCB_R_` case at line 193
3. Add chaining state machine for multi-block transfers

**Why deferred:** Requires deep protocol knowledge. Low priority unless specific cards fail.

---

### Step 2.8: Fix sub-GHz RX buffer overflow (H8) — FL-3555
**File:** `lib/subghz/subghz_tx_rx_worker.c:192`

**Requires investigation.** The TODO flags potential overflow but doesn't specify the exact condition.

**Approach:**
1. Trace the RX path to find buffer size and write point
2. Add boundary check: `if(write_pos >= buffer_size) return;`
3. Test with high RF activity

---

## Phase 3: Medium Bugs — Nice to Have (30-60 min)

### Step 3.1: Fix CI "Momentum" label (M1)
**File:** `.github/workflows/build.yml:75`

Change `"Momentum:"` to `"DIY:"` or project-specific label.

### Step 3.2: Remove no-op cross-target API check (M2)
**File:** `.github/workflows/build.yml:54-63`

Either remove the step or add a comment explaining it's vestigial.

### Step 3.3: Remove dead RF DMA code (M4)
**File:** `targets/f7/furi_hal/furi_hal_rfid.c:322-399`

Delete the commented-out function body. If needed later, it's in git history.

### Step 3.4-3.7: M5-M7 (TX check, file leak, double-start)
These require investigation similar to H6-H8. Defer until specific issues manifest.

---

## Phase 4: Low Bugs — Cleanup (15-30 min)

### Step 4.1-4.5: L1-L5
All are TODOs with no immediate impact. Fix when working on the affected subsystems.

---

## Completed Phases

- **Phase 0:** Committed (5ab8e946b) — 8 pre-existing bug fixes
- **Phase 1.1:** C1 SPI timeout — **DONE** (8eecaf3ec)
- **Phase 1.2:** C2 iso15693 memcpy — **DONE** (8eecaf3ec)
- **Phase 1.3:** C3 malloc NULL checks — **DONE** (14 of 17, 8eecaf3ec)
- **Phase 2.1:** H1 malloc magic — **DONE** (submodule 6d03fe32f)
- **Phase 2.2:** H2 duplicate dep — **DONE** (8eecaf3ec)
- **Phase 2.3:** H3 stack size — **DONE** (8eecaf3ec)
- **Phase 2.4:** H4 PCF8574 timeout — **DONE** (8eecaf3ec)

## Pending Phases

- **Phase 2.5:** H5 unit tests in CI — NOT STARTED
- **Phase 2.6:** H6 NFC dict attack lag (FL-3926) — NOT STARTED (needs investigation)
- **Phase 2.7:** H7 ISO14443-4 chaining — NOT STARTED (needs protocol spec)
- **Phase 2.8:** H8 Sub-GHz RX overflow (FL-3555) — NOT STARTED
- **Phase 3:** Medium bugs (M1-M7) — NOT STARTED
- **Phase 4:** Low bugs (L1-L5) — NOT STARTED

## Remaining: 4 High + 7 Medium + 5 Low = 16 bugs

```
Phase 0 (commit existing) → Phase 1 (critical) → Phase 2 (high) → Phase 3 (medium) → Phase 4 (low)
```

**Why this order:**

1. **Phase 0 first:** Already-done work must be preserved. Unstaged changes are at risk.

2. **Phase 1 before Phase 2:** Critical bugs can cause device bricking (C1 SPI deadlock), data corruption (C2 boomerang memcpy), or hard faults (C3 NULL deref). These are active threats on every device run.

3. **Within Phase 1:**
   - C1 (SPI) first because it affects ALL SPI1 devices (display, CC1101, SD) — the most shared resource
   - C2 (memcpy) second because it silently corrupts saved card data
   - C3 (NULL checks) third because OOM is rare on 256KB RAM, but when it happens, the crash is unrecoverable

4. **Phase 2 before Phase 3:** High bugs have functional impact (buffer overflow, no test coverage, protocol incompleteness). Medium bugs are mostly maintainability and CI cosmetics.

5. **Within Phase 2:**
   - H1-H4 are simple one-line fixes — do them first for quick wins
   - H5 (CI tests) requires workflow changes — do after simple fixes
   - H6-H8 require investigation — do last, may need hardware testing

6. **Phase 3-4:** Low-impact, can be deferred indefinitely without risk.

---

## Build Verification After Each Phase

After each phase, run:
```bash
./fbt TARGET_HW=7 DEBUG=0 COMPACT=1 copro_dist updater_package fap_dist
```

Verify:
- Build succeeds with zero warnings
- `git diff --exit-code` passes (no generated file drift)
- Binary size doesn't increase significantly (COMPACT=1 should keep it stable)

---

## Risk Assessment

| Phase | Risk | Rollback |
|-------|------|----------|
| 0 (commit) | None | `git reset --hard HEAD~1` |
| 1.1 (SPI timeout) | Low — timeout may trigger on slow devices | Revert single file |
| 1.2 (memcpy) | Medium — may break legacy card reload | Revert single file, test with saved cards |
| 1.3 (NULL checks) | Low — furi_check is standard pattern | Revert all 17 files |
| 2.1-2.4 (simple fixes) | None | Trivial reverts |
| 2.5 (CI tests) | Medium — may break CI if tests fail | Revert workflow change |
| 2.6-2.8 (investigation needed) | High — unknown scope | May require multiple reverts |
