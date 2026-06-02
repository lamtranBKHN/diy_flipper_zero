# Handover: MIFARE Classic Alignment Session

**Date:** 2026-06-01
**Branch:** copilot-worktree-2026-04-03T07-50-46
**Build:** `firmware_f7` — passes clean
**Scope:** Selective alignment of MF Classic read/write/emulate with reference projects

---

## What Was Done (P-Series, This Session)

7 changes across 7 files. All build-verified.

### P1.1 — Deauth Sequencing Before Crypto1 Fallback
**File:** `lib/nfc/protocols/mf_classic/mf_classic_poller_i.c:170-174`
**What:** After PN532 native auth fails, call `iso14443_3a_poller_halt()` + `furi_hal_nfc_pn532_mf_deauth()` before falling back to manual Crypto1. Without this, the card stays in an auth'd state and Crypto1 handshake gets garbage nonces.

### P1.2 — Nested Auth CIU Cleanup on Error
**File:** `lib/nfc/protocols/mf_classic/mf_classic_poller_i.c:235-239`
**What:** On auth failure during nested attack, call `furi_hal_nfc_pn532_mf_deauth()` if `is_nested && pn532_is_active()`. Prevents stale CIU register state from corrupting the next auth attempt.

### P1.3 — Per-Sector Auth State Tracking
**Files:**
- `lib/nfc/protocols/mf_classic/mf_classic_poller_i.h:39-42,198-202` — `MfClassicAuthState` enum changed from `{Idle,Passed}` to `{Never,Failed,Passed}`. Added `auth_sector_state[MF_CLASSIC_TOTAL_SECTORS_MAX]` array to `MfClassicPoller` struct.
- `lib/nfc/protocols/mf_classic/mf_classic_poller_i.c` — `auth_sector_state[sector]` set to `Passed` on success, `Failed` on error, reset to `Never` in `halt()`.
- `lib/nfc/protocols/mf_classic/mf_classic_poller.c:2520-2526` — Card-lost path resets entire `auth_sector_state` array to `Never`.

**Why:** Previous `{Idle,Passed}` couldn't distinguish "never tried" from "tried and failed". This matters for dict attack: if sector 5 failed auth with key A, don't retry key A — try key B or move on.

### P1.4 — Sector State Query API
**Files:**
- `lib/nfc/protocols/mf_classic/mf_classic.h:230-237` — New `MfClassicSectorState` struct (`is_read`, `is_authed_a`, `is_authed_b`, `has_valid_ac`) and `mf_classic_get_sector_state()` declaration.
- `lib/nfc/protocols/mf_classic/mf_classic.c:643-676` — Implementation. Checks `is_key_found()` for A/B, parses access bits from sector trailer, verifies all blocks in sector are read.

**Why:** UI scenes currently use raw bitmasks to display sector status. This API provides a clean abstraction. Consumers need updating (see Remaining Work).

### P2.5 — Access Bit Boundary Fix
**File:** `lib/nfc/protocols/mf_classic/mf_classic.c:759`
**What:** `block_num <= 128` → `block_num < 128`. Block 128 is in the 4K boundary zone (uses `(block & 0x0f) / 5` formula), not the 1K zone (`block & 0x03`). The old code misclassified block 128.

### P2.6 — Reset Memset + ISO-DEP Early Reject
**File:** `lib/nfc/protocols/mf_classic/mf_classic.c:74-78`
**What:** `mf_classic_reset()` now does `memset(data, 0, sizeof(MfClassicData))` + `iso14443_3a_alloc()` before calling `iso14443_3a_reset()`. Previously, reset only called `iso14443_3a_reset()` which left stale key/block data in memory.

**Also in** `mf_classic_poller.c:134-155` — ISO-DEP-only cards (SAK bit 6 set, no MF Classic bits) are rejected immediately in `detect_type` handler. Prevents PN532 from attempting crypto1 auth on EMV/Type4Tag cards which would hang indefinitely.

### P2.7 — Key Cache Sector Count Field
**File:** `applications/main/nfc/helpers/mf_classic_key_cache.c:62-66,113-123`
**What:**
- **Save:** Writes `Sector count: N` field (uint32_t) after `Mifare Classic type`, before key maps. Uses `uint32_t sector_num` for type safety with `flipper_format_write_uint32`.
- **Load:** Reads optional `Sector count` with backward compat — defaults to `MF_CLASSIC_TOTAL_SECTORS_MAX` (40) if missing. Clamped to `[1..40]`. Used as loop bound instead of hardcoded 40.

**File format change:** Old files (no `Sector count` field) load fine — fallback handles it. No version bump needed.

### P2.8 — PN532 Force Reinit on Listener Free
**File:** `lib/nfc/protocols/mf_classic/mf_classic_listener.c:667-672`
**What:** On `mf_classic_listener_free()`, if PN532 is active, call `furi_hal_pn532_force_reinit()`. Prevents stale PN532 state from corrupting the next NFC session.

---

## Additional Changes (Prior Sessions, In Working Tree)

The working tree has **248 files changed** (~6000 insertions, ~1800 deletions) across many sessions. Key areas beyond the P-series:

| Area | Files | Summary |
|---|---|---|
| PN532 HAL | `furi_hal_pn532.c/h`, `furi_hal_nfc_pn532.c/h` | Full PN532 driver: I2C transport, CIU register access, InCommunicateThru, background ready poller |
| NFC Scanner | `nfc_scanner.c` | Multi-protocol scan with PN532 probe order |
| MF Classic Poller | `mf_classic_poller.c` | Dict attack, nested attack, backdoor detection, key reuse |
| MF Ultralight | `mf_ultralight_poller.c` | Fast read, counter, auth |
| ISO14443-4 Chaining | `iso14443_4_layer.c/h` | Full I-block/R-block/WTX chaining |
| Type 4 Tag | `type_4_tag_listener_i.c`, `type_4_tag_poller_i.c` | NDEF read/write/listen |
| EMV | `emv_poller_i.c` | Payment card detection |
| Sub-GHz | `subghz_tx_rx_worker.c` | TX/RX buffer write checks |
| Key Cache | `mf_classic_key_cache.c` | Sector count field (P2.7) |
| Loader | `loader_menu.c` | Menu file caching |
| I2C Config | `furi_hal_i2c_config.c` | I2C3 disabled (furi_crash guard) |
| RFID HAL | `furi_hal_rfid.c` | Dead DMA code removed |
| API Symbols | `targets/f7/api_symbols.csv` | Updated for PN532 APIs |

---

## Remaining Work

### Hardware Validation (Requires Physical Device)
- **P1.3 runtime test:** Clone PN532 + MIFARE Classic 1K — verify 0x13 error → reinit → Crypto1 → read succeeds
- **P1.4 consumer audit:** Update `nfc_scene_mf_classic_dict_attack.c` and related render scenes to use `mf_classic_get_sector_state()` instead of raw bitmask checks
- **P2.7 backward compat:** Verify old key cache files (without `Sector count`) load correctly on device

### Known Limitations (PN532 Hardware)
- **No listener mode:** `set_mode(FuriHalNfcModeListener)` returns error. MF Classic emulation shows "Not Supported".
- **No ISO15693-3/Slix/ST25TB:** Disabled (no PN532 native support).
- **InCommunicateThru only for auth:** MF Classic auth uses CIU register writes + InCommunicateThru. InDataExchange only for PN532 native auth.
- **No HW reset:** Bus desync recovery via I2C re-probe only (no RST pin on clone modules).

---

## Build Commands

```bash
# Load toolchain (Windows)
scripts\toolchain\fbtenv.cmd

# Debug build
./fbt

# Release build (matches CI)
./fbt TARGET_HW=7 DEBUG=0 COMPACT=1 copro_dist updater_package fap_dist

# Clean build (what we verified)
./fbt -c firmware_f7

# Flash via ST-Link
./fbt flash

# Flash over USB (DFU mode)
./fbt flash_usb_full
```

---

## Key Architecture Decisions

1. **Selective alignment, not migration.** Current implementation has 14 verified improvements over reference projects. Migration would lose them.

2. **PN532-native auth with Crypto1 fallback.** Hybrid path: try `furi_hal_nfc_pn532_mf_auth()` first (fast, hardware-accelerated), fall back to manual Crypto1 on failure. Guarded by `#if !PN532_NATIVE_AUTH_DISABLED`.

3. **Per-sector auth state, not global.** `auth_sector_state[40]` tracks auth result per sector. `MfClassicAuthStateNever=0` means `memset(..., 0, ...)` correctly resets all sectors.

4. **Key cache format is backward-compatible.** `Sector count` field is optional on load. Old files default to 40 sectors. No version bump.

5. **150ms I2C timeout floor.** All I2C transactions on shared I2C1 bus (PN532 + PCF8574 + OLED) use minimum 150ms timeout. Sub-150ms only for single-byte ready-status reads (10ms/30ms) protected by 150ms fallback.

---

## File Index (P-Series Changes Only)

| File | Lines Changed | Purpose |
|---|---|---|
| `lib/nfc/protocols/mf_classic/mf_classic.c` | +48 -8 | P2.5 boundary fix, P2.6 reset memset, P1.4 sector state API, get_key zero-return guard |
| `lib/nfc/protocols/mf_classic/mf_classic.h` | +9 | P1.4 MfClassicSectorState struct + function declaration |
| `lib/nfc/protocols/mf_classic/mf_classic_poller_i.c` | +98 -45 | P1.1 deauth, P1.2 nested cleanup, P1.3 per-sector state, hybrid auth improvements |
| `lib/nfc/protocols/mf_classic/mf_classic_poller_i.h` | +8 -2 | P1.3 auth state enum + auth_sector_state array |
| `lib/nfc/protocols/mf_classic/mf_classic_poller.c` | +227 -80 | P1.3 card-lost reset, ISO-DEP reject, fail retry limit, dict attack improvements |
| `lib/nfc/protocols/mf_classic/mf_classic_listener.c` | +22 -10 | P2.8 force reinit, listener reset on alloc, memcpy writes |
| `applications/main/nfc/helpers/mf_classic_key_cache.c` | +11 -3 | P2.7 sector count save/load |
