# Firmware Architecture Comparison Report

**Project:** DIY Flipper Zero (copilot-worktree)  
**Date:** 2026-05-10  
**Reference Firmwares:** Momentum-Firmware, flipperzero-firmware, FuckingCheapFlipperZero-DIY-Flipper-zero-The-real-on

---

## 1. Repository Relationship Graph

```
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                         FLIPPER ZERO FIRMWARE FAMILY                                │
├─────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                     │
│   ┌─────────────────────────────────────────────────────────────┐                  │
│   │          OFFICIAL FLIPPER ZERO FIRMWARE                     │                  │
│   │          (flipperzero-firmware)                              │                  │
│   │          TARGET_HW=7 / TARGET_HW=18                          │                  │
│   │          STM32WB55 / STM32F7                                 │                  │
│   └─────────────────────┬───────────────────────────────────────┘                  │
│                         │                                                            │
│                         │ fork                                                        │
│                         ▼                                                            │
│   ┌─────────────────────────────────────────────────────────────┐                  │
│   │          MOMENTUM FIRMWARE                                   │                  │
│   │          (F:\FB_V3\Momentum-Firmware)                        │                  │
│   │          TARGET_HW=7 (f7) / TARGET_HW=18 (f18 DIY)           │                  │
│   │          Base: Official FZ + custom features                 │                  │
│   │          + Asset packs, + Extended protocols, + Spoofing      │                  │
│   └──────┬──────────────────────────────┬───────────────────────┘                  │
│          │                              │                                            │
│          │ fork                          │ fork                                       │
│          ▼                              ▼                                            │
│   ┌──────────────┐           ┌────────────────────────┐                           │
│   │ FuckingCheap │           │   CURRENT DIY PROJECT   │                           │
│   │ (same as     │           │   (copilot-worktree)    │                           │
│   │  Momentum)   │           │   TARGET_HW=7           │                           │
│   └──────────────┘           │   + PN532 NFC support  │                           │
│                              │   + PCF8574 expander   │                           │
│                              │   + CC1101 extended    │                           │
│                              └────────────────────────┘                           │
│                                                                                     │
└─────────────────────────────────────────────────────────────────────────────────────┘
```

**Key Insight:** Current DIY project is a ** Momentum fork** with hardware-specific modifications (PN532, PCF8574) that are NOT present in any other reference project.

---

## 2. Hardware Architecture Comparison

### 2.1 Official Flipper Zero (f7)

```
┌──────────────────────────────────────────────────────────────────────────┐
│                         FLIPPER ZERO (f7)                                │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│    ┌─────────────┐     ┌─────────────┐     ┌─────────────┐              │
│    │  CC1101     │     │  ST25R3916  │     │  SPI Bus D  │              │
│    │  Sub-GHz    │     │  NFC        │     │  Display    │              │
│    │  (SPI R)    │     │  (SPI R)    │     │  SD Card    │              │
│    └──────┬──────┘     └──────┬──────┘     └──────┬──────┘              │
│           │                   │                   │                      │
│           │     SPI Bus R ◄───┴────────┬──────────┘                      │
│           │                           │                                   │
│           │                    ┌──────┴──────┐                           │
│           │                    │  SPI1       │                           │
│           │                    │  (STM32WB)  │                           │
│           │                    └──────┬──────┘                           │
│           │                           │                                   │
│           ▼                           ▼                                   │
│    ┌─────────────┐           ┌─────────────┐                             │
│    │ Direct GPIO │           │ Direct GPIO │                             │
│    │ Buttons     │           │ Buttons     │                             │
│    │ Vibro/Buzz  │           │             │                             │
│    └─────────────┘           └─────────────┘                             │
│                                                                          │
│    Button pins: PC0-PC3, PC6, PC10-PC13 (6 buttons direct)               │
│    No I2C expander needed                                                │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

### 2.2 DIY Project (Current - PN532 + PCF8574)

```
┌──────────────────────────────────────────────────────────────────────────┐
│                    DIY PROJECT (PN532 + PCF8574)                         │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│    ┌─────────────┐     ┌─────────────┐     ┌─────────────┐              │
│    │  CC1101     │     │  PN532      │     │  OLED       │              │
│    │  Sub-GHz    │     │  NFC        │     │  Display    │              │
│    │  (SPI1)     │     │  (I2C1)     │     │  (I2C1)     │              │
│    └──────┬──────┘     └──────┬──────┘     └──────┬──────┘              │
│           │                   │                   │                      │
│           │           I2C1 ◄──┴───────────────────┘                      │
│           │                                                                   │
│    ┌──────┴──────┐    ┌─────────────┐                                      │
│    │  SPI1       │    │  PCF8574    │◄── I2C1 @ 0x20                       │
│    │  (STM32WB)  │    │  I/O Expander│                                     │
│    └──────┬──────┘    └──────┬──────┘                                      │
│           │                  │                                               │
│           │         ┌────────┴────────┐                                     │
│           │         │ Buttons (P0-P5)  │                                     │
│           │         │ Vibro (P6)       │                                     │
│           │         │ Buzzer (P7)     │                                     │
│           │         │ INT → PB0       │                                     │
│           │         └─────────────────┘                                     │
│           │                                                                │
│    ┌──────┴──────┐                                                         │
│    │  SD Card    │                                                         │
│    │  (SPI1)     │                                                         │
│    └─────────────┘                                                         │
│                                                                          │
│    ** HARDWARE SHARED ON SPI1: CC1101, Display, SD Card **                 │
│    ** I2C1 SHARED: PN532, OLED, PCF8574 **                                │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

### 2.3 Momentum f18 (DIY Target - Radio-less)

```
┌──────────────────────────────────────────────────────────────────────────┐
│                    MOMENTUM f18 (DIY TARGET)                             │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│    ┌─────────────┐           ┌─────────────┐                             │
│    │  (EMPTY)    │           │  SPI Bus D  │                             │
│    │  No CC1101  │           │  Display    │                             │
│    │  No NFC     │           │  SD Card    │                             │
│    └─────────────┘           └──────┬──────┘                             │
│                                     │                                     │
│                              ┌──────┴──────┐                              │
│                              │  SPI1       │                              │
│                              │  (STM32WB)  │                              │
│                              └──────┬──────┘                              │
│                                     │                                     │
│                                     ▼                                     │
│                              ┌─────────────┐                             │
│                              │ Direct GPIO │                              │
│                              │ Buttons     │                              │
│                              │ Vibro/Buzz  │                              │
│                              └─────────────┘                              │
│                                                                          │
│    ** ALL RADIO HARDWARE EXCLUDED IN target.json **                       │
│    ** No Sub-GHz, No NFC, No RFID, No Infrared, No iButton **            │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

---

## 3. SPI Bus Architecture Comparison

### 3.1 SPI Pin Mapping

| SPI Bus | DIY Project | Official f7 | Momentum f18 |
|---------|-------------|-------------|--------------|
| **SPI1 (Shared)** | | | |
| SCK | PA5 | PA5 | PA5 |
| MISO | PA6 | PA6 | PA6 |
| MOSI | PA7 | PA7 | PA7 |
| CC1101 CS | **PA4** | PA4 (GPIOD Pin 0 on f7) | N/A |
| SD Card CS | PA10 | PA10 (GPIOC Pin 12) | PA10 |
| Display CS | **PA2** | PA3 (GPIOC Pin 11) | PA3 |
| Display DI | **PB6** | PB1 | PB1 |
| Display RST | **PB7** | PB0 | PB0 |
| NFC CS | **PE4** | PE4 | N/A |
| CC1101 G0 | **PA1** | PA1 | N/A |

### 3.2 I2C Bus Mapping

| I2C Bus | DIY Project | Official f7 | Momentum f18 |
|---------|-------------|-------------|--------------|
| **I2C1** | | | |
| SCL | PB6/PB7 | PB6/PB7 | PB6/PB7 |
| SDA | PA7/PA8 | PA7/PA8 | PA7/PA8 |
| PN532 | **0x48 (7-bit)** | N/A | N/A |
| OLED | 0x3C (SH1106) | N/A | N/A |
| PCF8574 | **0x20** | N/A | N/A |

---

## 4. HAL Architecture - NFC Comparison

### 4.1 NFC Stack Layers

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          NFC APPLICATION LAYER                          │
│              applications/main/nfc/ (shared across all)                 │
└─────────────────────────────────┬───────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                          NFC PROTOCOL LAYER                             │
│              lib/nfc/ (nfc_poller, nfc_listener, scenes)                 │
│              Supported: ISO14443-3A/B, ISO14443-4A/B, ISO15693,          │
│              FeliCa, MfUltralight, MfClassic, MfDesfire, EMV*,          │
│              NTAG4xx*, Type4Tag* (*DIY-only extensions)                 │
└─────────────────────────────────┬───────────────────────────────────────┘
                                  │
                                  ▼
┌────────────────────────────────┐ ┌──────────────────────────────────────┐
│     DIY PROJECT                 │ │     OFFICIAL / MOMENTUM              │
│     furi_hal_nfc_pn532.c       │ │     furi_hal_nfc_st25r3916.c          │
│     furi_hal_pn532.c           │ │     lib/drivers/st25r3916/            │
├────────────────────────────────┤ ├──────────────────────────────────────┤
│     I2C @ 0x48                 │ │     SPI                               │
│     PN532 chip                 │ │     ST25R3916 chip                   │
│     Poll-only interface        │ │     Interrupt-driven                 │
│     18.83 KB + 23.32 KB        │ │     Direct FIFO access                │
└────────────────────────────────┘ └──────────────────────────────────────┘
```

### 4.2 NFC Protocol Support Matrix

| Protocol | DIY (PN532) | Official | Momentum | Momentum f18 |
|----------|-------------|----------|----------|-------------|
| ISO14443-3A | ✓ | ✓ | ✓ | **EXCLUDED** |
| ISO14443-3B | ✓ | ✓ | ✓ | **EXCLUDED** |
| ISO14443-4A | ✓ | ✓ | ✓ | **EXCLUDED** |
| ISO14443-4B | ✓ | ✓ | ✓ | **EXCLUDED** |
| ISO15693-3 | ✓ | ✓ | ✓ | **EXCLUDED** |
| FeliCa | ✓ | ✓ | ✓ | **EXCLUDED** |
| MfUltralight | ✓ | ✓ | ✓ | **EXCLUDED** |
| MfClassic | ✓ | ✓ | ✓ | **EXCLUDED** |
| MfPlus | ✓ | ✓ | ✓ | **EXCLUDED** |
| MfDesfire | ✓ | ✓ | ✓ | **EXCLUDED** |
| EMV | ✓ | ✗ | ✓ | **EXCLUDED** |
| NTAG4xx | ✓ | ✗ | ✓ | **EXCLUDED** |
| Type4Tag | ✓ | ✗ | ✓ | **EXCLUDED** |

**Note:** DIY project adds 3 protocols (EMV, NTAG4xx, Type4Tag) not in official firmware.

---

## 5. HAL Architecture - Sub-GHz Comparison

### 5.1 Sub-GHz Stack Layers

```
┌─────────────────────────────────────────────────────────────────────────┐
│                      SUB-GHz APPLICATION LAYER                          │
│              applications/main/subghz/                                  │
└─────────────────────────────────┬───────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                      SUB-GHz PROTOCOL LAYER                              │
│              lib/subghz/protocols/                                      │
│  DIY: 179 files    Official: 111 files    Momentum: 185 files           │
└─────────────────────────────────┬───────────────────────────────────────┘
                                  │
                                  ▼
┌────────────────────────────────┐ ┌──────────────────────────────────────┐
│     DIY PROJECT                 │ │     OFFICIAL / MOMENTUM              │
│     furi_hal_subghz.c           │ │     furi_hal_subghz.c                │
│     (36,449 bytes, 1001 lines)  │ │     (29,650 bytes, 834 lines)         │
├────────────────────────────────┤ ├──────────────────────────────────────┤
│  + Extended frequency range     │ │  + Standard frequency range          │
│  + Async RX hopping            │ │  + RF switch GPIO control             │
│  + Rolling counter manip       │ │                                      │
│  + Extended range mode         │ │                                      │
│  + GDO0 floating-edge fix      │ │                                      │
│  - No RF switch support        │ │  + gpio_rf_sw_0 for antenna path     │
└────────────────────────────────┘ └──────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                      CC1101 DRIVER LAYER                                │
│              lib/drivers/cc1101.c (identical across all - 5,812 bytes) │
└─────────────────────────────────┬───────────────────────────────────────┘
                                  │
                                  ▼
┌────────────────────────────────┐
│     CC1101 TRANSCEIVER         │
│     SPI1 @ PA4 (CS), PA1 (GDO0)│
└────────────────────────────────┘
```

### 5.2 Sub-GHz Frequency Range Comparison

| Range | DIY Project | Official Flipper | YARD Stick One |
|-------|-------------|------------------|----------------|
| Low | 281-361 MHz | 299-348 MHz | 300-348 MHz |
| Medium | 378-481 MHz | 386-464 MHz | 387-464 MHz |
| High | 749-962 MHz | 778-928 MHz | 779-928 MHz |

**DIY extends** both low-end (281 vs 299) and high-end (962 vs 928) beyond official specs, matching YARD Stick One ranges.

---

## 6. Input/Button Architecture Comparison

### 6.1 Button Handling Flow

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         OFFICIAL FLIPPER ZERO                           │
│                         (Direct GPIO approach)                          │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│    Physical Buttons ──► MCU GPIO Pins ──► furi_hal_gpio ISR ──► input.c │
│    (PC0-PC3,           (hardware              (interrupt              (debounce,   │
│     PC6,              interrupt)              driven)                   dispatch)   │
│     PC10-PC13)                                                               │
│                                                                          │
│    Vibro ──► Direct GPIO (PC1) ──► furi_hal_vibro.c                     │
│    Buzzer ──► Speaker PWM (TIM3) ──► furi_hal_speaker.c                 │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│                    DIY PROJECT (PCF8574 approach)                        │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│    Physical Buttons ──► PCF8574 I/O Expander ──► I2C1 ──► input.c        │
│    (P0-P5,             (0x20)                    @0x20       (read,       │
│     active-low)                                                    debounce,   │
│                                                                    dispatch)   │
│                             │                                           │
│                             ├──► INT Pin (PB0/EXTI0) ──► interrupt      │
│                             │                                           │
│                             └──► Polling fallback (if INT unstable)      │
│                                                                          │
│    Vibro ──► PCF8574 P6 ──► furi_hal_vibro.c ──► furi_hal_pcf8574_write│
│    Buzzer ──► PCF8574 P7 ──► CLI only (on/off)                          │
│                                                                          │
│    button_to_pcf_pin[] = {2, 1, 4, 3, 0, 5} // Back, Down, OK, Left, Up, Right│
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 6.2 PCF8574 Pin Configuration

| PCF8574 Pin | Function | Direction | Notes |
|-------------|----------|----------|-------|
| P0 | Button Up | Input | Active-low |
| P1 | Button Down | Input | Active-low |
| P2 | Button Left | Input | Active-low |
| P3 | Button Right | Input | Active-low |
| P4 | Button OK | Input | Active-low |
| P5 | Button Back | Input | Active-low |
| P6 | Vibro Motor | Output | Active-high |
| P7 | Buzzer | Output | Active-high |

### 6.3 InputPin Configuration (DIY)

```c
// furi_hal_resources.c
const InputPin input_pins[] = {
    {.gpio = NULL, .key = InputKeyUp,    .inverted = true, .name = "Up"},    // PCF P0
    {.gpio = NULL, .key = InputKeyDown,  .inverted = true, .name = "Down"},  // PCF P1
    {.gpio = NULL, .key = InputKeyLeft,  .inverted = true, .name = "Left"},  // PCF P3
    {.gpio = NULL, .key = InputKeyRight, .inverted = true, .name = "Right"}, // PCF P2
    {.gpio = NULL, .key = InputKeyOk,     .inverted = true, .name = "Ok"},    // PCF P4
    {.gpio = NULL, .key = InputKeyBack,  .inverted = true, .name = "Back"},  // PCF P5
};
```

**Note:** `.gpio = NULL` signals that input is handled via PCF8574, not direct MCU GPIO.

---

## 7. Build System Architecture

### 7.1 TARGET_HW Configuration

| Target | Hardware | MCU | NFC | Sub-GHz | IR | iButton |
|--------|----------|-----|-----|---------|-----|---------|
| **f7** (all projects) | Standard FZ | STM32WB55 | ST25R3916 | CC1101 | Yes | Yes |
| **f18** (Momentum/Official) | Radio-less | STM32WB55 | Excluded | Excluded | Excluded | Excluded |
| **DIY (current)** | Custom | STM32WB55 | PN532 | CC1101 | Yes | Yes |

### 7.2 f18 Target Exclusion List (Momentum/Official)

```json
// targets/f18/target.json
{
    "excluded_sources": [
        "furi_hal_infrared.c",
        "furi_hal_nfc.c", "furi_hal_nfc_timer.c", "furi_hal_nfc_irq.c",
        "furi_hal_nfc_event.c", "furi_hal_nfc_iso15693.c",
        "furi_hal_nfc_iso14443a.c", "furi_hal_nfc_iso14443b.c",
        "furi_hal_nfc_felica.c",
        "furi_hal_rfid.c",
        "furi_hal_subghz.c"
    ],
    "excluded_headers": [
        "furi_hal_infrared.h", "furi_hal_nfc.h", "furi_hal_rfid.h",
        "furi_hal_subghz.h", "furi_hal_ibutton.h", "furi_hal_subghz_configs.h"
    ],
    "excluded_modules": ["nfc", "lfrfid", "subghz", "ibutton", "infrared"]
}
```

### 7.3 DIY Project Custom Board Configuration

```makefile
# targets/f7/boards/custom_pn532_board.mk
EXTRA_SCONS_VARS = {
    'CPPDEFINES': ['PN532_ENABLED']
}
```

---

## 8. Directory Structure Comparison

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           DIRECTORY STRUCTURE COMPARISON                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  Component          │ DIY  │ Momentum │ Official │ FuckingCheap │           │
│  ─────────────────────────────────────────────────────────────────────────  │
│  applications/     │  ✓   │    ✓     │    ✓     │      ✓       │           │
│  applications_user/ │  ✓   │    ✓     │    ✓     │      ✓       │           │
│  assets/            │  ✓   │    ✓     │    ✓     │      ✓       │           │
│  documentation/     │  ✓   │    ✓     │    ✓     │      ✓       │           │
│  furi/              │  ✓   │    ✓     │    ✓     │      ✓       │           │
│  lib/               │  ✓   │    ✓     │    ✓     │      ✓       │           │
│  scripts/           │  ✓   │    ✓     │    ✓     │      ✓       │           │
│  site_scons/        │  ✓   │    ✓     │    ✓     │      ✓       │           │
│  targets/           │  f7  │  f7,f18  │  f7,f18  │     f7       │           │
│  ─────────────────────────────────────────────────────────────────────────  │
│  UNIQUE TO DIY:                                                             │
│  targets/f7/furi_hal/furi_hal_pn532.c/h    (PN532 NFC driver)               │
│  targets/f7/furi_hal/furi_hal_nfc_pn532.c/h                               │
│  targets/f7/furi_hal/furi_hal_pcf8574.c/h  (PCF8574 expander)              │
│  targets/f7/boards/custom_pn532_board.mk                                  │
│  ─────────────────────────────────────────────────────────────────────────  │
│  UNIQUE TO MOMENTUM:                                                        │
│  lib/momentum/                      (Momentum settings, asset packs)      │
│  applications/main/momentum_app/    (Momentum settings app)                │
│  ─────────────────────────────────────────────────────────────────────────  │
│  UNIQUE TO FUCKINGCHEAP: (same as Momentum, just named differently)          │
│  No unique files - same as Momentum                                        │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 9. File Size Comparison

### 9.1 Key HAL Files

| File | DIY | Official | Delta |
|------|-----|----------|-------|
| furi_hal_subghz.c | 36,449 B | 29,650 B | +6,799 (+23%) |
| furi_hal_pn532.c | 23,320 B | N/A | NEW |
| furi_hal_nfc_pn532.c | 18,830 B | N/A | NEW |
| furi_hal_pcf8574.c | ~1,500 B | N/A | NEW |
| st25r3916 driver | N/A | ~75 KB total | NOT USED |
| CC1101 driver | 5,812 B | 5,812 B | Identical |

### 9.2 Sub-GHz Protocol Count

| Project | Protocol Files | Notes |
|---------|---------------|-------|
| DIY Project | 179 | +62% vs official |
| Momentum | 185 | +67% vs official |
| Official | 111 | Baseline |

---

## 10. API Symbols Comparison

### 10.1 api_symbols.csv Versions

| Project | API Version | Unique Symbols |
|---------|-------------|----------------|
| DIY f7 | 87.5 | +momentum.h, +rgb_backlight.h, +subghz_fap.h |
| Momentum f7 | 87.1 | Standard set |
| Official f7 | 87.1 | Standard set |
| Momentum f18 | 87.1 | Missing NFC/SubGHz/RFID/IR/iButton libs |

### 10.2 Unique Symbols in DIY (not in other f7)

```
lib/momentum/momentum.h
lib/rgb_backlight.h
applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h
applications/main/archive/helpers/archive_helpers_ext.h
applications/main/subghz/subghz_fap.h
```

---

## 11. Boot Flow Comparison

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              BOOT FLOW (ALL PROJECTS)                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│   STM32 Reset                                                               │
│       │                                                                     │
│       ▼                                                                     │
│   ROM Bootloader (checks boot pins)                                         │
│       │                                                                     │
│       ▼                                                                     │
│   Flash @ 0x08000000 → main()                                               │
│       │                                                                     │
│       ├──► furi_init()                                                      │
│       │         │                                                           │
│       │         ▼                                                           │
│       │    Check RTC boot_mode register                                     │
│       │         │                                                           │
│       │         ├──► [Dfu mode]  → flipper_boot_dfu_exec()                  │
│       │         ├──► [Update]    → flipper_boot_update_exec()               │
│       │         └──► [Normal]    → init_task()                              │
│       │                                                                     │
│       ▼                                                                     │
│   furi_hal_init_early()                                                     │
│       │                                                                     │
│       ├──► (DIY) furi_hal_pcf8574_init()     ← PCF8574 I2C expander init   │
│       ├──► (DIY) furi_hal_pn532_init()       ← PN532 NFC init              │
│       └──► (All) Standard HAL early init                                    │
│       │                                                                     │
│       ▼                                                                     │
│   flipper_init()                                                             │
│       │                                                                     │
│       ▼                                                                     │
│   Start Services:                                                           │
│   storage → desktop → gui → notification → cli → bt → rpc → loader →         │
│   dialogs → power                                                            │
│       │                                                                     │
│       ▼                                                                     │
│   furi_run() → FreeRTOS Scheduler                                           │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 12. Known Issues & Quirks

| Issue | Location | Severity | Notes |
|-------|----------|----------|-------|
| SPI1 bus contention | view_port.c:208 | Medium | Display + CC1101 + SD share SPI1 |
| PN532 polling only | furi_hal_pn532.c | Low | No interrupt-driven RX |
| PCF8574 INT instability | input.c:170-199 | Low | Falls back to polling |
| No RF switch support | furi_hal_subghz.c | Low | Software-only path selection |
| GDO0 floating edge | furi_hal_subghz.c | Low | Pull-down bias when CC1101 absent |

---

## 13. Summary Tables

### 13.1 Feature Matrix

| Feature | DIY | Momentum f7 | Momentum f18 | Official | FuckingCheap |
|---------|-----|-------------|--------------|----------|--------------|
| PN532 NFC | ✓ | ✗ | ✗ | ✗ | ✗ |
| ST25R3916 NFC | ✗ | ✓ | ✗ | ✓ | ✓ |
| CC1101 Sub-GHz | ✓ | ✓ | ✗ | ✓ | ✓ |
| Extended freq | ✓ | ✓ | ✗ | ✗ | ✓ |
| PCF8574 buttons | ✓ | ✗ | ✗ | ✗ | ✗ |
| Direct GPIO | ✗* | ✓ | ✓ | ✓ | ✓ |
| Asset packs | ✓ | ✓ | ✓ | ✗ | ✓ |
| Extended protocols | ✓ | ✓ | ✗ | ✗ | ✓ |
| JS SDK | ✓ | ✓ | ✓ | ✓ | ✓ |

*DIY uses PCF8574, not direct GPIO for buttons

### 13.2 File Count by Category

| Category | DIY | Official | Momentum |
|----------|-----|----------|----------|
| HAL files | ~80 | ~75 | ~75 |
| NFC-related | 8 (includes PN532) | 6 (ST25R3916) | 6 |
| Sub-GHz protocols | 179 | 111 | 185 |
| Applications | 250+ | 200+ | 250+ |

---

*End of Architecture Comparison Report*