# Firmware Delta Report

**Project:** DIY Flipper Zero (copilot-worktree)  
**Date:** 2026-05-10  
**Reference:** flipperzero-firmware, Momentum-Firmware, FuckingCheapFlipperZero-DIY-Flipper-zero-The-real-on

---

## 1. New Files in DIY (Not in Reference Projects)

### 1.1 PN532 NFC Implementation

| File | Size | Purpose |
|------|------|---------|
| `targets/f7/furi_hal/furi_hal_pn532.c` | 23.32 KB | Low-level PN532 I2C driver |
| `targets/f7/furi_hal/furi_hal_pn532.h` | 0.92 KB | PN532 HAL API |
| `targets/f7/furi_hal/furi_hal_nfc_pn532.c` | 18.83 KB | NFC tech interface for PN532 |
| `targets/f7/furi_hal/furi_hal_nfc_pn532.h` | 1.12 KB | NFC HAL hooks |
| `targets/f7/boards/custom_pn532_board.mk` | 1 line | PN532 board enable flag |

### 1.2 PCF8574 I/O Expander

| File | Size | Purpose |
|------|------|---------|
| `targets/f7/furi_hal/furi_hal_pcf8574.c` | ~1.5 KB | PCF8574 I2C GPIO driver |
| `targets/f7/furi_hal/furi_hal_pcf8574.h` | ~0.5 KB | PCF8574 API |

### 1.3 Modified Input Service

| File | Status | Purpose |
|------|--------|---------|
| `applications/services/input/input.c` | MODIFIED | PCF8574-based button reading |
| `applications/services/input/input.h` | MODIFIED | Added PCF8574 integration |
| `targets/f7/furi_hal/furi_hal_resources.c` | MODIFIED | `.gpio = NULL` for all buttons |

---

## 2. Build Configuration Deltas

### 2.1 fbt_options.py Comparison

| Setting | DIY | Momentum | Official | FuckingCheap |
|---------|-----|----------|----------|--------------|
| `TARGET_HW` | 7 | 7 | 7 | 7 |
| `FIRMWARE_ORIGIN` | "Momentum" | "Momentum" | "Flipper" | "Momentum" |
| `COMPACT` | 1 | 1 | 0 | 1 |
| `DEBUG` | 0 | 0 | 1 | 0 |
| `COPRO_STACK_TYPE` | "ble_light" | "ble_light" | "ble_full" | "ble_light" |
| `COPRO_CUBE_VERSION` | "1.20.0" | "1.20.0" | "1.20.0" | "1.20.0" |

### 2.2 custom_pn532_board.mk

```makefile
# DIY Project - enables PN532
EXTRA_SCONS_VARS = {
    'CPPDEFINES': ['PN532_ENABLED']
}
```

**Reference projects do NOT have this file.**

---

## 3. HAL Implementation Deltas

### 3.1 NFC HAL Delta

#### Official/ Momentum f7 - ST25R3916 path:

```
lib/drivers/st25r3916.c (2.5 KB)
lib/drivers/st25r3916_reg.c (8.33 KB)
lib/drivers/st25r3916_reg.h (59.52 KB)
targets/f7/furi_hal/furi_hal_nfc.c (ST25R3916-based)
targets/f7/furi_hal/furi_hal_nfc_timer.c
targets/f7/furi_hal/furi_hal_nfc_irq.c
targets/f7/furi_hal/furi_hal_nfc_event.c
targets/f7/furi_hal/furi_hal_nfc_iso14443a.c
targets/f7/furi_hal/furi_hal_nfc_iso14443b.c
targets/f7/furi_hal/furi_hal_nfc_iso15693.c
targets/f7/furi_hal/furi_hal_nfc_felica.c
```

#### DIY Project - PN532 path:

```
targets/f7/furi_hal/furi_hal_pn532.c (23.32 KB) ← NEW
targets/f7/furi_hal/furi_hal_nfc_pn532.c (18.83 KB) ← NEW
targets/f7/furi_hal/furi_hal_nfc.c ← MODIFIED to include PN532 backend
targets/f7/furi_hal/furi_hal_nfc_timer.c (may be unused)
```

**Key Differences:**
- ST25R3916 is SPI-based; PN532 is I2C-based
- ST25R3916 has 8 hardware interrupt-driven source files; PN532 has 2 polling-based files
- PN532 adds EMV, NTAG4xx, Type4Tag protocols not in official

### 3.2 Sub-GHz HAL Delta

| Feature | DIY | Official | Delta |
|---------|-----|----------|-------|
| File size | 36,449 B | 29,650 B | +6,799 (+23%) |
| Lines | 1001 | 834 | +167 |
| Frequency ranges | Extended | Standard | +281-361, +749-962 |
| Async RX hopping | Yes | No | NEW |
| Rolling counter | Yes | No | NEW |
| Extended range mode | Yes | No | NEW |
| RF switch GPIO | No | Yes | MISSING |
| GDO0 pull-down bias | Yes | No | NEW |

### 3.3 Input HAL Delta

| Component | DIY | Official |
|-----------|-----|----------|
| Button GPIO | NONE (PCF8574) | Direct MCU GPIO |
| Button pins | PCF8574 P0-P5 | PC0-PC3, PC6, PC10-PC13 |
| Vibro | PCF8574 P6 | Direct GPIO (PC1) |
| Buzzer | PCF8574 P7 | Speaker PWM (TIM3) |
| INT handling | PCF8574 INT → PB0 | GPIO interrupts |
| Polling fallback | Yes (if INT fails) | No |

---

## 4. Symbol Deltas

### 4.1 api_symbols.csv Version Comparison

| Project | Version | Total Symbols | Delta vs Official |
|---------|---------|---------------|-------------------|
| DIY f7 | 87.5 | ~4163 | +20 unique |
| Momentum f7 | 87.1 | ~4163 | Standard |
| Official f7 | 87.1 | ~4163 | Baseline |
| Momentum f18 | 87.1 | ~3243 | -920 (excluded) |

### 4.2 Unique Symbols in DIY (not in other f7)

```csv
lib/momentum/momentum.h
lib/rgb_backlight.h
applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h
applications/main/archive/helpers/archive_helpers_ext.h
applications/main/subghz/subghz_fap.h
```

### 4.3 Missing from DIY vs Official

The following symbols exist in official f7 but are NOT in DIY's api_symbols.csv (or are excluded):

```
# These may be missing due to PN532/ST25R3916 switch
furi_hal_nfc_timer.c related symbols
furi_hal_nfc_irq.c related symbols
furi_hal_nfc_event.c related symbols
```

---

## 5. Application Layer Deltas

### 5.1 NFC Application

Both DIY and official use the same high-level application architecture in `applications/main/nfc/`:
- Same scene structure
- Same poller/listener patterns
- Same device storage format

**Delta is ONLY in the HAL backend** (PN532 vs ST25R3916).

### 5.2 Input Application

| Aspect | DIY | Official |
|--------|-----|----------|
| Input service | Modified to use PCF8574 | Standard GPIO |
| Debounce logic | Same | Same |
| Key mapping | Same | Same |

---

## 6. File Deletion/Missing Analysis

### 6.1 Files NOT in DIY (vs Official)

| File | Reason |
|------|--------|
| `lib/drivers/st25r3916*` | Replaced by PN532 driver |
| `targets/f7/furi_hal/furi_hal_nfc_timer.c` | May be unused with PN532 |
| `targets/f7/furi_hal/furi_hal_nfc_irq.c` | May be unused with PN532 |
| `targets/f7/furi_hal/furi_hal_nfc_event.c` | May be unused with PN532 |

### 6.2 Files NOT in DIY (vs Momentum f18)

| File | Reason |
|------|--------|
| `targets/f18/` directory | DIY uses f7 target with modifications |
| `targets/f18/target.json` | DIY doesn't have f18 configuration |

---

## 7. Conditional Compilation Deltas

### 7.1 PN532 Feature Flag

```c
// In various files, wrapped with:
#ifdef PN532_ENABLED
    // PN532-specific code
#else
    // ST25R3916 code path
#endif
```

### 7.2 PCF8574 Feature Flag

```c
// In input.c
#ifdef PCF8574_ENABLED
    // PCF8574-based button reading
    furi_hal_pcf8574_read();
#else
    // Standard GPIO reading
#endif
```

---

## 8. Build Output Deltas

### 8.1 Firmware Size Comparison

| Component | DIY | Official | Delta |
|-----------|-----|----------|-------|
| furi_hal | Larger | Baseline | +PN532 +PCF8574 |
| NFC stack | Different | ST25R3916 | Different chip |
| Applications | Same | Same | Identical |

### 8.2 Linker Script

All projects use same STM32WB55 linker scripts:
- `stm32wb55xx_flash.ld` (1MB firmware)
- `stm32wb55xx_ram_fw.ld`
- `application_ext.ld`

---

## 9. Pin Mapping Deltas

### 9.1 SPI1 Pins

| Signal | DIY | Official | Momentum f18 |
|---------|-----|----------|--------------|
| CC1101 CS | PA4 | PA4 (GPIOD0 on f7) | N/A |
| CC1101 GDO0 | PA1 | PA1 | N/A |
| Display CS | PA2 | PA3 (f7 uses GPIOC11) | PA3 |
| Display DI | PB6 | PB1 | PB1 |
| Display RST | PB7 | PB0 | PB0 |
| SD CS | PA10 | PA10 (GPIOC12) | PA10 |
| NFC CS | PE4 | PE4 | N/A |

### 9.2 I2C1 Pins

| Signal | DIY | Official |
|---------|-----|----------|
| SCL | PB6/PB7 | PB6/PB7 |
| SDA | PA7/PA8 | PA7/PA8 |
| PN532 | 0x48 | N/A |
| PCF8574 | 0x20 | N/A |
| OLED | 0x3C | N/A |

---

## 10. Code Change Patterns

### 10.1 PN532 Integration Pattern

```c
// furi_hal_nfc.c
#include "furi_hal_nfc_pn532.h"  // Conditionally included

void furi_hal_nfc_init(void) {
#ifdef PN532_ENABLED
    furi_hal_pn532_init();
#else
    // ST25R3916 init
#endif
}
```

### 10.2 PCF8574 Button Pattern

```c
// input.c
void input_init(void) {
#ifdef PCF8574_ENABLED
    furi_hal_pcf8574_init();
    // Attach INT callback
#else
    // Standard GPIO init
#endif
}
```

---

## 11. Risk Assessment

### 11.1 High Risk Deltas

| Delta | Risk | Impact |
|-------|------|--------|
| NFC chip change | HIGH | PN532 is fundamentally different from ST25R3916 |
| I2C vs SPI NFC | HIGH | Different protocol timing, polling vs interrupt |
| PCF8574 button | MEDIUM | Software debounce may differ from hardware interrupts |

### 11.2 Medium Risk Deltas

| Delta | Risk | Impact |
|-------|------|--------|
| Extended freq range | MEDIUM | May violate local regulations |
| Sub-GHz async hopping | MEDIUM | New code paths not tested in official |
| No RF switch | LOW | Software-only path selection may be less efficient |

### 11.3 Low Risk Deltas

| Delta | Risk | Impact |
|-------|------|--------|
| GDO0 pull-down | LOW | Prevents floating edge when CC1101 absent |
| Polling fallback | LOW | Degrades to polling if INT fails |

---

## 12. Testing Recommendations

### 12.1 NFC Testing

1. Test all ISO14443-3A cards (Mifare Ultralight, Classic, Desfire)
2. Test ISO14443-3B cards
3. Test ISO15693 (many official tests use this)
4. Test FeliCa
5. Test EMV (DIY extension)
6. Compare read ranges with official firmware

### 12.2 Button Testing

1. Test all 6 buttons for correct key mapping
2. Test debounce behavior
3. Test INT vs polling fallback
4. Test long-press and short-press detection

### 12.3 Sub-GHz Testing

1. Test standard frequency ranges
2. Test extended frequency ranges (DIY extension)
3. Test async RX hopping (if implemented)
4. Compare range with official firmware

---

*End of Delta Report*