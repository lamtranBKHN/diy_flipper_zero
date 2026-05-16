# Project Debugging Report: DIY Flipper Zero (Nucleus Dark MK1) Worktree

## Overview
This report summarizes the debugging analysis of the Flipper Zero firmware adaptation for the custom UBYTE STM32WB55CGU6 hardware, focusing on the NFC subsystem transition from ST25R3916 to PN532 over I2C. The worktree contains uncommitted changes implementing stability fixes and protocol alignments.

## Project Status
- **Build Status**: ✅ Successful (firmware compiles without errors)
- **API Consistency**: ✅ Consistent (single api_symbols.csv target)
- **Lint Status**: ❌ Violations detected (clang-format issues in u8g2 library)
- **Hardware Target**: UBYTE STM32WB55CGU6 with PN532 NFC module

## Key Findings

### NFC Subsystem Improvements
- **Major Changes**: Removed ST25R3916 support, implemented PN532-only HAL
- **Stability Enhancements**: Added polling fallback for IRQ issues, capability guards
- **Protocol Support**: ISO14443A/FeliCa full support, ISO15693/ISO14443B limited
- **Application Hardening**: Dynamic UI filtering, graceful error handling

### Identified Issues
1. **Code Formatting**: Numerous clang-format violations in `lib/u8g2/u8x8_d_ssd1306.c` (third-party library)
2. **Potential I2C Conflicts**: Documentation discrepancies in I2C pin mappings (SCL/SDA assignments)
3. **SPI Contention**: Shared SPI1 bus may cause ViewPort lockups (display/CC1101/SD card)

### Diagnostic Results
- **Build Check**: Passed (CDB generated successfully)
- **API Hash Check**: Passed (1 unique hash across targets)
- **Lint Errors**: 500+ formatting violations in u8g2 library

## Visual Elements

### Change Statistics
```
33 files changed, 1308 insertions(+), 2455 deletions(-)
```

### Key Modified Files
- `targets/f7/furi_hal/furi_hal_nfc.c`: 878 deletions (removed ST25R3916 code)
- `targets/f7/furi_hal/furi_hal_nfc_pn532.c`: 347 additions (new PN532 implementation)
- `targets/f7/furi_hal/furi_hal_i2c_config.c`: 125 modifications (I2C bus configuration)
- `applications/main/nfc/helpers/protocol_support/nfc_protocol_support.c`: 90 modifications (UI filtering)

### Work Tree Structure (Sample)
```
copilot-worktree-2026-04-03T07-50-46/
├── applications/
│   ├── external/
│   ├── main/nfc/
│   └── system/js_app/
├── lib/
│   ├── mbedtls/
│   ├── nanopb/
│   └── nfc/
├── targets/f7/
│   ├── furi_hal/
│   └── api_symbols.csv
└── NFC SUBSYSTEM IMPROVEMENTS SUMMARY.md
```

## Conclusions
The project is in a stable state with successful builds and consistent APIs. The NFC subsystem has been successfully adapted for PN532 hardware with appropriate fallbacks and protections. Minor formatting issues exist but do not affect functionality. I2C pin alignment should be verified against hardware measurements to prevent potential bus conflicts.

## Recommendations
1. Run `./fbt format_all` to resolve clang-format violations in u8g2 library
2. Verify I2C pin mappings on actual hardware (continuity/I2C scan)
3. Monitor SPI1 bus contention in runtime logs for lockup warnings
4. Commit changes after verification to ensure consistency