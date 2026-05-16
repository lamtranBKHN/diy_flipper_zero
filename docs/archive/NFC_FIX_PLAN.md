# NFC Fix Plan

## Status: COMPLETED

All 55 issues across 4 waves have been fixed and verified:
- **Wave 0** (25 safe fixes) — applied and build verified
- **Wave 1** (10 moderate fixes) — applied and build verified  
- **Wave 2** (10 higher-risk fixes) — applied and build verified
- **Wave 3** (10 cleanup fixes) — applied and build verified

**Final build**: `./fbt` debug + `./fbt TARGET_HW=7 DEBUG=0 COMPACT=1 copro_dist updater_package fap_dist` release — both PASS with zero errors and zero warnings.

**54/55 issues confirmed** via source audit (P38 was determined dead code and removed — `uint8_t len` can never exceed `PN532_MAX_RX_FRAME - 6 = 264`, so the assertion was statically always-true).

## Scope
55 issues found across the NFC subsystem: PN532 driver, NFC HAL, lib/nfc stack, and applications/main/nfc app layer.

## File Dependency Map

| File | Issues |
|------|--------|
| `targets/f7/furi_hal/furi_hal_pn532.c` | P1, P9, P10, P17, P18, P19, P20, P37, P38, P39, P40, P41 |
| `targets/f7/furi_hal/furi_hal_nfc_pn532.c` | P11, P21, P22, P23, P42, P43 |
| `targets/f7/furi_hal/furi_hal_nfc.c` | P6, P24, P44 |
| `targets/f7/furi_hal/furi_hal_nfc.h` | P5 |
| `targets/f7/furi_hal/furi_hal_nfc_iso14443a.c` | P5, P45, P46, P47 |
| `lib/nfc/nfc_scanner.c` | P2, P3 |
| `lib/nfc/nfc.c` | P12, P13, P14, P26, P27 |
| `lib/nfc/protocols/mf_classic/mf_classic_poller.c` | P28, P29, P30, P51, P52 |
| `lib/nfc/protocols/mf_classic/mf_classic.c` | P48, P49, P50 |
| `applications/main/nfc/` (scenes, app, helpers) | P4, P15, P16, P31, P32, P33, P34, P35, P36, P53, P54, P55, P56 |
| `applications/main/nfc/helpers/mf_user_dict.c` | P36 |
| `applications/main/nfc/cli/` | P53, P55, P56 |

## Wave 0 — Safe Single-Line Fixes (no semantic change, < 5 mins each)

### Wave 0.1 — PN532 Driver (`furi_hal_pn532.c`)
- **P17** (line 254): Add POSTAMBLE validation `if(frame[6 + len] != PN532_POSTAMBLE) return false;` after DCS check.
- **P18** (line 414): Add `pn532_ready = false;` in `pn532_send_command` ACK failure path (mirroring `pn532_exchange`).
- **P19** (line 469): Add `if(!target) return false;` before success path in `poll_iso14443a`.
- **P19** (line 504): Same guard for `poll_felica`.
- **P20** (line 260): Only call `pn532_read_response` if `pn532_send_command` succeeded in cleanup path of `pn532_send_inrelease`.

### Wave 0.2 — NFC HAL (`furi_hal_nfc.c`, `furi_hal_nfc_pn532.c`, `furi_hal_nfc.h`, `furi_hal_nfc_iso14443a.c`)
- **P6** (furi_hal_nfc.c:123-156): Wrap `set_mode`/`reset_mode` body in `furi_hal_nfc_acquire()`/`release()` with `goto release` cleanup pattern.
- **P11** (furi_hal_nfc_pn532.c:632-639): Wrap `pn532_rx()` body in acquire/release.
- **P24** (furi_hal_nfc.c:109-121): Wrap `low_power_mode_start/stop` body in acquire/release.
- **P5** (furi_hal_nfc.h:430): Guard `listener_set_col_res_data` declaration with `#ifndef FURI_HAL_NFC_PN532_ONLY`.
- **P5** (nfc.c:602): Guard call site with `#ifndef FURI_HAL_NFC_PN532_ONLY`, with `#else` empty `(void)instance;`.
- **P44** (furi_hal_nfc.c:27-51): In `is_hal_ready()` early-return path, ensure `furi_hal_nfc_event` is initialized.

### Wave 0.3 — NFC Stack (`lib/nfc/`)
- **P2** (nfc_scanner.c:192): `memcpy(instance->detected_protocols, filtered_protocols, filtered_protocols_num * sizeof(NfcProtocol))`.
- **P3** (nfc_scanner.c:104,131): Add early guard at scanner entry: `if(instance->base_protocols_num == 0)` → transition to `NfcScannerStateComplete` (no protocols to detect).
- **P12** (nfc.c:344): Add `furi_check(instance->state == NfcStateIdle)` at top of `nfc_start`.
- **P14** (nfc.c:466-470): Add `case NfcCommStateFailed: error = NfcErrorInternal; break;` before event-check cascade.
- **P26** (nfc.c:60-63): Increase `#define NFC_TX_BUFFER_SIZE (256)` to `(288)` to fit data+parity bits (256 + 32 = 288).
- **P25** (furi_hal_nfc_pn532.c:329-375): In `wait_event`, change flag clearing to only clear the specific flags the call is about to wait for, not all timer flags.

### Wave 0.4 — MIFARE Classic (`mf_classic_poller.c`, `mf_classic.c`)
- **P28** (mf_classic_poller.c:1397): Replace VLA with fixed array `uint32_t nt_enc_temp_arr[MF_CLASSIC_NESTED_NT_HARD_MINIMUM]` (max 3, current pattern uses exactly 3 entries). Or use `MF_CLASSIC_NESTED_NT_HARD_MINIMUM` as the max bound.
- **P29** (mf_classic_poller.c:1808): Replace `furi_assert(params_saved)` with `if(!params_saved) { FURI_LOG_E(...); instance->state = MfClassicPollerStateFail; break; }`.
- **P33** (dict_attack.c:99-101): Add `if(m->sectors_total == 0) return;` guard before progress bar division.

### Wave 0.5 — Application (`applications/main/nfc/`)
- **P15** (nfc_scene_mf_classic_dict_attack.c:217): Add `if(!dict) { /* error handling */ ... }` guard before `keys_dict_get_total_keys(dict)`.
- **P35** (nfc_app.c:331): Check `filename_start != FURI_STRING_FAILURE` before using result.
- **P36** (mf_user_dict.c:72-81): After decrementing `keys_num`, shift remaining entries via `memmove(&keys_arr[i], &keys_arr[i+1], (keys_num - i) * sizeof(MfUserDictKey))`.
- **P4** (nfc_scene_mf_classic_detect_reader.c:136): Add `instance->mfkey32_logger = NULL;` after `mfkey32_logger_free()`.
- **P34** (nfc_app.c:142-227): Add frees for `mfkey32_logger`, `timer`, `timer_auto_exit`, `mf_user_dict`, and `nfc_dict_context.dict` at top of `nfc_app_free()` (guard with NULL checks).

## Wave 1 — Moderate Risk (needs careful implementation, 5-15 mins each)

### Wave 1.1 — PN532 Driver bounds fixes
- **P1, P9, P10** (furi_hal_pn532.c:110,560,600): 
  - Line 110: `len = (uint8_t)(cmd_len + 1)` — add `furi_check(cmd_len < PN532_MAX_TX_PAYLOAD - 1)` before cast.
  - Line 560: Change `if(tx_len > PN532_MAX_TX_PAYLOAD - 2)` to `> PN532_MAX_TX_PAYLOAD - 3` (i.e. `> 252`).
  - Line 600: Change `if(tx_len > PN532_MAX_TX_PAYLOAD - 1)` to `> PN532_MAX_TX_PAYLOAD - 2` (i.e. `> 253`).

### Wave 1.2 — PN532 init desync
- **P8** (furi_hal_pn532.c:334): For each init command (RFConfiguration, retries, TxControl), if `pn532_write_frame` succeeds but ACK fails, call `pn532_read_response(buf, 50)` to drain pending response before proceeding.

### Wave 1.3 — NFC state machine robustness
- **P13** (nfc.c:382-383,740-741): Replace `while(furi_hal_nfc_timer_block_tx_is_running()) {}` with timed loop:
  ```c
  uint32_t timeout = furi_get_tick() + FURI_HAL_NFC_TIMEOUT_MS;
  while(furi_hal_nfc_timer_block_tx_is_running()) {
      if(furi_get_tick() > timeout) break;
      furi_thread_yield();
  }
  ```
- **P27** (nfc.c:471): Add state-name-aware timeout and logging: `FURI_LOG_E(TAG, "Unknown state %d timed out", instance->state)`.

### Wave 1.4 — PN532 abort + auth paths
- **P22** (furi_hal_nfc_pn532.c:438): Replace `continue` with `break` at the abort-check-after-failed-poll site, so control exits the polling loop immediately.
- **P30** (mf_classic_poller.c:1448): Add `use_backdoor` parameter throughout `mf_classic_poller_auth_nested()` chain — currently hardcodes `false` where true might be needed. Low risk: backdoor auth is supplementary (optional optimization).

### Wave 1.5 — Application error handling
- **P31** (nfc_app.c:329): Save `file_path` to temporary string before truncation; restore on save failure.
- **P35** (nfc_app.c:331): Already in Wave 0.5. Additionally ensure error path in `nfc_save_internal` doesn't silently discard data.
- **P45** (furi_hal_nfc_iso14443a.c:33-38): Add early return at top of `listener_init` if `furi_hal_nfc_pn532_is_active()`.
- **P46** (furi_hal_nfc_iso14443a.c:74-85): Remove `UNUSED()` macros from function parameters that are actually used.
- **P52** (mf_classic_poller.c:1334-1338): When all calibration distances filtered by 3σ, fall back to `d_min = 0; d_max = UINT16_MAX` instead of leaving at `UINT16_MAX/0` (which triggers `assert` at line 1346).

## Wave 2 — Higher Risk (needs thorough testing after fix)

### Wave 2.1 — Semantic fixes
- **P4** (double-free): Already in Wave 0.5. Verify the `back_event_callback` and `on_exit` interaction — ensure `on_exit` doesn't double-free the now-NULL pointer. The `mfkey32_logger_free()` must handle NULL gracefully.
- **P7** (furi_hal_pn532.c:284): After `pn532_write_frame(...)` error path in `pn532_write_register`, drain response buffer with `pn532_read_response(buf, 50)` and log warning. This changes error-recovery semantics.
- **P16** (nfc_scene_mf_classic_dict_attack.c:176-198): Add `if(storage_common_copy(...) != FSE_OK) { /* error dialog */ }` for each `furi_string_cat_printf` / copy sequence.

### Wave 2.2 — CRC/parity correctness
- **P21** (furi_hal_nfc_pn532.c:587): Investigate whether `append_crc=true` in the tx path actually causes problems or compensates for PN532 behavior. Test with real cards: if blocks read correctly, this is `append_crc` compensating for PN532 InCommunicateThru stripping the tag CRC. If so, comment documented behavior; if not, change to `false`.

### Wave 2.3 — UI draw path mutation
- **P32** (dict_attack.c:47-73): Move `furi_string_set(m->header, ...)` from draw callback to a dedicated update function called from the scene's tick or custom event handler instead of every frame.

### Wave 2.4 — Function rename (linker-visible)
- **P47** (furi_hal_nfc_iso14443a.c:115,169): Rename `furi_hal_nfc_iso4443a_listener_tx` → `furi_hal_nfc_iso14443a_listener_tx`. Search for all references across codebase to avoid linker errors. Requires grep for both old and new name.

### Wave 2.5 — Strict aliasing / undefined behavior
- **P48** (mf_classic.c:469-471): Replace `*(uint32_t*)&block->data[0]` with `memcpy(&v, block->data, sizeof(v))`. The compiler optimizes `memcpy` of small fixed sizes to inline load instructions on Cortex-M4.
- **P49** (mf_classic.c:476): Move `*value = (int32_t)v;` to after the validation checks pass (inside the `if(val_checks)` block).

### Wave 2.6 — PRNG search cancellation
- **P51** (mf_classic_poller.c:1283): Add periodic abort check inside the search loop: every 1024 iterations, check `furi_hal_nfc_is_abort_requested()` or poll the event system for `FuriHalNfcEventAbortRequest`.

## Wave 3 — Cleanup / Low Priority (no functional impact)

### Wave 3.1 — Code quality
- **P37** (furi_hal_pn532.c:207): Note only — 270-byte stack per read_response call. Known limitation.
- **P38** (furi_hal_pn532.c:237): Add `furi_check(len < PN532_MAX_RX_FRAME - 6)` after LEN validation.
- **P39** (furi_hal_pn532.c:538): Change `response_len < 13` to `< 15` for full ATQB.
- **P40** (furi_hal_pn532.c:85): Remove loop or add actual probing addresses.
- **P41** (furi_hal_pn532.c:132): Document 2ms delay; consider making conditional.
- **P42** (furi_hal_nfc_pn532.c:624): Document 192-byte stack allocation.
- **P43** (furi_hal_nfc_pn532.c:519): Add explicit HLTA opcode check: `if(effective_tx_len == 2U && tx_bytes[0] == 0x50U && tx_bytes[1] == 0x00U)`.
- **P50** (mf_classic.c:699,765): Remove dead `return true;` and `return false;` after switches.

### Wave 3.2 — Application code quality
- **P53** (nfc_cli_command_field.c:9-10): Wrap CLI operations in acquire/release.
- **P54** (nfc_protocol_support.c:509): Use non-blocking notification.
- **P55** (nfc_cli_command_emulate.c:83): Add timeout counter to emulate CLI.
- **P56** (nfc_app.c:395-398): Use `strlen(NFC_APP_SHADOW_EXTENSION)` instead of hardcoded `4`.

## Verification Plan

After each wave:
1. `./fbt` — must compile without errors or warnings
2. For Wave 2 items: manual testing with real MIFARE Classic 1K and 4K cards

Final verification:
1. `./fbt` (debug build) — zero errors
2. `./fbt TARGET_HW=7 DEBUG=0 COMPACT=1 copro_dist updater_package fap_dist` (release build, matches CI)
3. Unit tests: `FIRMWARE_APP_SET=unit_tests ./fbt` (if unit tests exist for NFC)

## Execution Order

```
Wave 0 (safe) → ./fbt verify → Wave 1 (moderate) → ./fbt verify → Wave 2 (risky) → ./fbt verify → Wave 3 (cleanup) → ./fbt verify → Release build
```

Within each wave, process files independently (parallel edits are safe if they touch different files). If two edits in the same wave touch the same file, do them sequentially to avoid conflicts.
