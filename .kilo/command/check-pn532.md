# Check PN532 Guard Compliance

Audit that all PN532-only code paths are properly guarded with `#ifndef FURI_HAL_NFC_PN532_ONLY`.

## Files to check
Run grep for bare `#ifdef PN532_ENABLED` usage (should use `FURI_HAL_NFC_PN532_ONLY` instead):

```
rg "#ifdef PN532_ENABLED" --type-add 'c:*.{c,h}' -t c
```

## Files needing PN532 guards
- `targets/f7/furi_hal/furi_hal_nfc.c`
- `targets/f7/furi_hal/furi_hal_nfc_iso14443a.c`
- `targets/f7/furi_hal/furi_hal_nfc_iso14443b.c`
- `targets/f7/furi_hal/furi_hal_nfc_iso15693.c`
- `targets/f7/furi_hal/furi_hal_nfc_felica.c`
- `targets/f7/furi_hal/furi_hal_nfc_event.c`
- `targets/f7/furi_hal/furi_hal_nfc_i.h`
- `lib/nfc/nfc.c`
- `lib/nfc/nfc_scanner.c`

## Verify PN532-only flag consistency
Cross-check `FURI_HAL_NFC_PN532_ONLY` vs `PN532_ENABLED`:

```
rg "FURI_HAL_NFC_PN532_ONLY" -l
rg "PN532_ENABLED" -l
```

The board config (`custom_pn532_board.mk`) sets `PN532_ENABLED`. The HAL uses `FURI_HAL_NFC_PN532_ONLY`. These must be in sync.
