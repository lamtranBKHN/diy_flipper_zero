---
description: NFC subsystem development on DIY board (PN532/I2C)
mode: subagent
permission:
  edit:
    "targets/f7/furi_hal/furi_hal_nfc*": allow
    "targets/f7/furi_hal/furi_hal_pn532*": allow
    "lib/nfc/**": allow
    "applications/main/nfc/**": allow
    "*": ask
---
NFC subsystem development agent. Loads `project-intro.md` and `nfc-system.md` for context. Focus areas: PN532 driver, NFC HAL, nfc protocol stack, nfc application.
