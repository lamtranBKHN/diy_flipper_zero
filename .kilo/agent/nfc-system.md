# NFC Subsystem (PN532-only)

## Architecture
- **PN532** on I2C1 at 0x24. No HW IRQ — polling only.
- HAL: `targets/f7/furi_hal/furi_hal_nfc*.c` + `furi_hal_pn532.c`.
- Protocol stack: `lib/nfc/` (nfc_scanner, nfc_poller, nfc_listener, protocols/).
- App: `applications/main/nfc/`.

## PN532 Limitations
- **No listener (card emulation) mode**: `set_mode(FuriHalNfcModeListener)` → error.
- **No RF field control**: poller_field_on/off are no-ops.
- **Scanner offers**: ISO14443-3A, ISO14443-3B, FeliCa only. ISO15693, ST25TB, etc. return error.
- **FIFO**: 64 bytes vs ST25R3916's larger buffer.
- **I2C**: 100kHz, no HW RST pin (bus desync = permanent, use retry only).
- **270-byte read**: every I2C response reads full buffer regardless of payload.

## Key HAL Files
| File | Role |
|------|------|
| `furi_hal_nfc.c` | Dispatcher: acquire/release mutex, route to PN532 |
| `furi_hal_nfc_pn532.c` | PN532 backend: set_mode, tx, rx, poll |
| `furi_hal_pn532.c` | Low-level PN532 protocol: frame encode/decode, I2C |
| `furi_hal_nfc_iso14443a.c` | ISO14443A tech ops (routes to PN532 at runtime) |
| `furi_hal_nfc_iso14443b.c` | ISO14443B stubs (return error) |
| `furi_hal_nfc_iso15693.c` | ISO15693 stubs (return error) |
| `furi_hal_nfc_felica.c` | FeliCa stubs (return error) |
| `furi_hal_nfc_irq.c` | No-op IRQ stubs (correct — PN532 has no HW IRQ) |

## Bug History
- **6 bugs fixed (2026-05-11)**: mutex race in wrappers (P1), PN532 missing mutex (P2), scanner filtered to A only (P3), listener col_res_data ST25 access (P4), low_power_mode missing furi_check (P5), 270-byte read inefficiency (P6).
- **55 NFC bugs fixed (2026-05-13)**: 4 critical (uint8 overflow, memcpy byte-count, div-by-zero, double-free), 1 linker, 12 high, 18 medium, 20 low. See CLAUDE.md for full list.

## Mutex Discipline
- `furi_hal_nfc_acquire()`/`furi_hal_nfc_release()` wraps PN532 HAL ops.
- Tech functions (iso14443a, etc.) own their own locking.
- Wrappers (`poller_tx`, `poller_rx`, etc.) do NOT acquire — tech funcs do.
- Pattern: `furi_hal_nfc_acquire()` → check PN532 active → call PN532 → `furi_hal_nfc_release()` via `goto release`.

## Tests
- Unit tests in `applications/debug/unit_tests/tests/nfc/` (auto-discovered by `*.c` glob).
- Run on real HW only (no mock/simulator).
- `test_robustness.c` (7 test cases) validates error enums, timeouts, frame sizes, diagnostic API.
