# NFC Implementation Comparison: DIY Flipper Zero vs ESP32 Port vs Momentum Firmware

## Project Overview

| Aspect | DIY Flipper Zero (Current) | Flipper-Zero-ESP32-Port | Momentum-Firmware |
|--------|----------------------------|-------------------------|-------------------|
| **MCU** | STM32WB55CGU6 | ESP32-S3 | STM32WB55 (official) |
| **NFC Chip** | PN532 | PN532 | **ST25R3916** |
| **Comm Interface** | I2C | I2C | **SPI** |
| **I2C/SPI Address** | **0x48** (7-bit) | **0x24** (7-bit) | N/A (SPI chip select) |
| **Files** | Modular (8+ files) | Monolithic (1 file) | Modular (8+ files) |
| **Timers** | STM32 TIM1/TIM17 | ESP32 `esp_timer` | STM32 TIM1/TIM17 |
| **ISO15693** | Full support | Not supported | Full support |

---

## 1. Architecture Comparison

### DIY Flipper Zero - Modular Architecture

```
targets/f7/furi_hal/
├── furi_hal_nfc.c              # Main HAL orchestrator
├── furi_hal_nfc_pn532.c        # PN532 backend & protocol bridging
├── furi_hal_pn532.c            # Low-level PN532 I2C driver
├── furi_hal_nfc_iso14443a.c     # ISO14443A tech implementation
├── furi_hal_nfc_iso14443b.c     # ISO14443B tech implementation
├── furi_hal_nfc_iso15693.c     # ISO15693 tech implementation
├── furi_hal_nfc_felica.c        # FeliCa tech implementation
├── furi_hal_nfc_event.c         # Event queue management
├── furi_hal_nfc_timer.c         # Hardware timer management
├── furi_hal_nfc_irq.c           # GPIO IRQ handler
├── furi_hal_nfc_i.h             # Private HAL types
└── furi_hal_nfc_tech_i.h        # Technology interface definitions
```

### ESP32 Port - Monolithic Architecture

```
components/furi_hal/
└── furi_hal_nfc.c              # All NFC HAL in one file (~1541 lines)
```

### Momentum-Firmware - Modular Architecture (ST25R3916)

```
targets/f7/furi_hal/
├── furi_hal_nfc.c              # Main HAL orchestrator (ST25R3916)
├── furi_hal_nfc_timer.c         # Hardware timer management (TIM1/TIM17)
├── furi_hal_nfc_event.c         # Event/IRQ management
├── furi_hal_nfc_irq.c           # GPIO IRQ handler
├── furi_hal_nfc_iso14443a.c     # ISO14443A tech implementation
├── furi_hal_nfc_iso14443b.c     # ISO14443B tech implementation
├── furi_hal_nfc_iso15693.c     # ISO15693 tech implementation
├── furi_hal_nfc_felica.c        # FeliCa tech implementation
├── furi_hal_nfc_i.h             # Private HAL types
└── furi_hal_nfc_tech_i.h        # Technology interface definitions

lib/drivers/
├── st25r3916.c                  # ST25R3916 driver (FIFO, IRQ)
├── st25r3916_reg.c              # ST25R3916 register definitions
└── st25r3916.h                  # ST25R3916 header
```

---

## 2. Key Differences

### 2.1 NFC Hardware & Communication

| Project | NFC Chip | Interface | Notes |
|---------|----------|-----------|-------|
| DIY Flipper Zero | PN532 | I2C @ 0x48 | 7-bit address; IRQ on GPIO |
| ESP32 Port | PN532 | I2C @ 0x24 | 7-bit address (different!) |
| Momentum-Firmware | **ST25R3916** | **SPI** | Original Flipper Zero chip |

### 2.2 Timer Implementation

#### DIY Flipper Zero & Momentum (Hardware Timers - Same HW, Different Chip)
```c
// Both DIY and Momentum use identical hardware timer structure
static const FuriHalNfcTimerConfig furi_hal_nfc_timers[FuriHalNfcTimerCount] = {
    [FuriHalNfcTimerFwt] = {
        .timer = TIM1,
        .bus = FuriHalBusTIM1,
        .event = FuriHalNfcEventInternalTypeTimerFwtExpired,
        .irq_id = FuriHalInterruptIdTim1UpTim16,
        .irq_type = TIM1_UP_TIM16_IRQn,
    },
    [FuriHalNfcTimerBlockTx] = {
        .timer = TIM17,
        .bus = FuriHalBusTIM17,
        .event = FuriHalNfcEventInternalTypeTimerBlockTxExpired,
        .irq_id = FuriHalInterruptIdTim1TrgComTim17,
        .irq_type = TIM1_TRG_COM_TIM17_IRQn,
    },
};
```

**Key Difference**: DIY uses these timers with PN532 I2C commands, while Momentum uses them with ST25R3916 SPI registers.

#### ESP32 Port (Software Timers)
```c
// components/furi_hal/furi_hal_nfc.c
static esp_timer_handle_t fwt_timer = NULL;
static esp_timer_handle_t block_tx_timer = NULL;

static void fwt_timer_cb(void* arg) {
    if(nfc_event_flags) {
        furi_event_flag_set(nfc_event_flags, FuriHalNfcEventTimerFwtExpired);
    }
}

void furi_hal_nfc_timer_fwt_start(uint32_t time_fc) {
    uint64_t us = ((uint64_t)time_fc * 1000ULL) / 13560ULL;
    esp_timer_stop(fwt_timer);
    esp_timer_start_once(fwt_timer, us);
}
```

### 2.3 Initialization Sequence

#### DIY Flipper Zero (PN532 I2C)
```
1. Acquire I2C mutex
2. Probe PN532 at 0x48
3. Get firmware version
4. Configure SAM (Security Access Module)
5. Write TxControl register
6. Enable IRQ callback
```

#### Momentum-Firmware (ST25R3916 SPI)
```
1. Acquire SPI mutex
2. Set chip to default state
3. Verify chip ID (ST25R3916)
4. Initialize GPIO ISR
5. Turn on oscillator & verify
6. Measure voltage (ADC)
7. Configure regulators based on voltage
8. Configure antenna tuning
9. Perform regulator calibration
10. Enable external/internal load modulation
```

**Key Insight**: Momentum's initialization is much more comprehensive, including:
- Oscillator health check
- Voltage measurement for 3V vs 5V operation
- Antenna tuning calibration
- Load modulation configuration

#### ESP32 Port (PN532 I2C)
```
1. i2c_master_init()
2. Get firmware version
3. Configure SAM
4. InListPassiveTarget for polling
```

### 2.4 Event System

#### DIY Flipper Zero (Thread Flags)
```c
// Polling-based with GPIO fallback
static bool pn532_wait_ready_with_timeout(uint32_t timeout_ms) {
    while(furi_get_tick() - start < timeout_ms) {
        uint32_t flags = furi_thread_flags_get();
        if(flags & FuriHalNfcEventInternalTypeAbort) return false;
        if(flags & FuriHalNfcEventInternalTypeIrq) { ... return true; }
        // GPIO fallback
        if(furi_hal_gpio_read(&gpio_nfc_irq_rfid_pull) == 0) { ... }
        furi_delay_ms(1);
    }
}
```

#### ESP32 Port (Event Flags)
```c
FuriHalNfcEvent furi_hal_nfc_poller_wait_event(uint32_t timeout_ms) {
    uint32_t flags = furi_event_flag_wait(
        nfc_event_flags, NFC_EVENT_ALL_BITS, FuriFlagWaitAny | FuriFlagNoClear, timeout_ms);
    if(flags & FuriFlagError) return FuriHalNfcEventTimeout;
    furi_event_flag_clear(nfc_event_flags, flags & NFC_EVENT_ALL_BITS);
    return (FuriHalNfcEvent)(flags & NFC_EVENT_ALL_BITS);
}
```

### 2.4 Listener Mode Implementation

#### DIY Flipper Zero (PN532)
- Uses `furi_hal_nfc_pn532_listener_*` functions
- Event queue (`furi_hal_nfc_pn532_queue_push/pop`) for event buffering
- Queue capacity: `PN532_EVENT_QUEUE_CAPACITY = 8`
- **Note**: Queue can overflow and silently drop events (critical bug)

#### ESP32 Port (PN532)
- Direct TgInitAsTarget / TgGetData polling
- Polling slices of 200ms to honor abort requests
- No event queue - direct command/response pattern

#### Momentum-Firmware (ST25R3916)
- Uses `furi_hal_nfc_listener_*` functions
- Direct register access for field detection
- Field ON/OFF event detection via ST25R3916 interrupts
- No event queue - uses direct IRQ handling

---

## 3. ISO-DEP (ISO14443-4) Handling

### DIY Flipper Zero (`furi_hal_nfc_pn532.c`)

```c
// Lines 511-517: RATS interception
if(sel_cmd == 0xE0U && furi_hal_nfc_pn532.has_ats) {
    furi_hal_nfc_pn532_prepare_rx(
        furi_hal_nfc_pn532.last_ats, furi_hal_nfc_pn532.last_ats_len, true, false);
    furi_hal_nfc_pn532.iso_dep_active = true;
    return furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532ErrorNone, true);
}

// Lines 531-575: I-block / S-block / R-block handling
if(furi_hal_nfc_pn532.iso_dep_active) {
    uint8_t pcb = tx_bytes[0];
    if((pcb & 0xE2) == 0x02) {
        // I-block: strip PCB and pass payload to PN532
        ...
    } else if((pcb & 0xC0) == 0xC0) {
        // S-block: PN532 handles internally
        ...
    }
}
```

### ESP32 Port (`furi_hal_nfc.c`)

```c
// Lines 739-752: RATS interception
if(tx_bytes >= 2 && tx_data[0] == 0xE0 &&
   pn532_iso_dep_active && pn532_cached_ats_len > 0) {
    memcpy(pn532_rx_buf, pn532_cached_ats, pn532_cached_ats_len);
    crc_a_append(pn532_rx_buf, pn532_cached_ats_len);
    pn532_rx_bits = (pn532_cached_ats_len + 2) * 8;
    pn532_iso_dep_mode = true;
    ...
}

// Lines 792-845: I-block/R-block/S-block handling with more complete implementation
if(pn532_iso_dep_mode && payload_len >= 1) {
    saved_pcb = payload[0];
    if((saved_pcb & 0xC0) == 0xC0) {
        // S-block handling (WTX, DESELECT)
        ...
    } else if((saved_pcb & 0xC0) == 0x80) {
        // R-block handling - more complete than DIY version
        ...
    }
}
```

### Momentum-Firmware (ST25R3916)
- ST25R3916 handles ISO-DEP natively via FIFO
- No protocol interception needed - chip handles RF framing
- More direct data exchange via `st25r3916_write_fifo()` / `st25r3916_read_fifo()`

---

## 4. ISO15693 Support

### DIY Flipper Zero
- Full polling implementation via `furi_hal_pn532_poll_iso15693()`
- Target mode via separate listener functions
- **Actually works** with PN532 hardware

### ESP32 Port
```c
/* NOTE: The PN532 does NOT support ISO15693 (NFC-V).
 * It only supports ISO14443A, ISO14443B, and FeliCa.
 * These functions return success as no-ops so the stack doesn't crash,
 * but ISO15693 cards cannot be read or emulated with PN532 hardware. */

FuriHalNfcError furi_hal_nfc_iso15693_listener_tx_sof(void) {
    return FuriHalNfcErrorNone;  // No-op!
}
```

**Critical Issue**: ESP32 port explicitly documents that ISO15693 is NOT supported, while DIY version has working implementation.

### Momentum-Firmware
- Full implementation via `furi_hal_nfc_iso15693.c`
- ST25R3916 supports NFC-V natively
- Uses ST25R3916's ISO15693 protocol handling

---

## 5. Critical Bugs in DIY Version

### Bug 1: Event Queue Overflow (PN532-specific)
```c
// furi_hal_nfc_pn532.c:50-52
static bool furi_hal_nfc_pn532_queue_push(FuriHalNfcEvent event) {
    if(furi_hal_nfc_pn532.event_count >= PN532_EVENT_QUEUE_CAPACITY) return false; // Silent drop!
}
```
- **Affected**: DIY (PN532) and ESP32 (PN532) - both use queues
- **Not affected**: Momentum (ST25R3916) - uses direct IRQ handling, no queue

### Bug 2: I2C Handle Ownership Confusion (PN532-specific)
```c
// furi_hal_pn532.c:293-294
// Note: I2C handle should already be acquired by caller (furi_hal_nfc_pn532_backend_init)
// Do NOT acquire here - it breaks PN532 detection
```
- **Affected**: DIY (PN532) - confusing ownership model
- **Not affected**: ESP32 (PN532) - different I2C handling
- **Not affected**: Momentum (ST25R3916) - uses SPI, different model

### Bug 3: Timer "Already Running" Check (Hardware Timer Issue)
```c
// furi_hal_nfc_timer.c:139-141 (before fix)
furi_check(!furi_hal_nfc_timer_is_running(timer));  // Crashes if running!
// Fixed: Auto-stop if already running
```

---

## 6. Features Comparison

| Feature | DIY (PN532) | ESP32 (PN532) | Momentum (ST25R3916) |
|---------|-------------|---------------|---------------------|
| **ISO15693** | Full support | Not supported (no-op) | Full support |
| **ISO14443B** | Via poller_tx interception | Direct InListPassiveTarget | Full native support |
| **FeliCa** | Via poller_tx interception | Direct InListPassiveTarget | Full native support |
| **Abort slicing** | No - can get stuck | Yes (200ms) | Yes (interrupt-driven) |
| **R-block handling** | Incomplete (PN532 handles) | Complete | Native chip support |
| **PPS interception** | No | Yes | Native chip support |
| **Voltage measurement** | No | No | Yes (ADC-based) |
| **Antenna tuning** | No | No | Yes (calibrated) |
| **Oscillator check** | No | No | Yes (OSC interrupt) |

---

## 7. PN532 vs ST25R3916 Command Sets

### PN532 Commands (DIY & ESP32)

| Command | DIY Version | ESP32 Port |
|--------|-------------|------------|
| GET_FIRMWARE_VERSION | 0x02 | 0x02 |
| SAM_CONFIGURATION | 0x14 | 0x14 |
| RFCONFIGURATION | 0x32 | 0x32 |
| IN_LIST_PASSIVE | 0x4A | 0x4A |
| IN_DATA_EXCHANGE | 0x40 | 0x40 |
| TG_INIT_AS_TARGET | 0x8C | 0x8C |
| TG_GET_DATA | 0x86 | 0x86 |
| TG_SET_DATA | 0x8E | 0x8E |
| POWER_DOWN | **Missing** | 0x16 |
| IN_RELEASE | **Missing** | 0x52 |
| IN_COMMUNICATE_THRU | **Missing** | 0x42 |
| IN_JUMP_FOR_DEP | **Missing** | 0x56 |
| TG_RESPONSE_TO_INIT | **Missing** | 0x90 |

### ST25R3916 Commands (Momentum)

Momentum uses ST25R3916 direct commands instead of PN532 commands:

```c
// Example ST25R3916 commands
ST25R3916_CMD_SET_DEFAULT          // Reset to default state
ST25R3916_CMD_TRANSMIT_WITHOUT_CRC  // Send data without CRC
ST25R3916_CMD_TRANSMIT_WITH_CRC    // Send data with CRC
ST25R3916_CMD_EOF                   // End of frame
ST25R3916_CMD_STOP                  // Stop current operation
ST25R3916_CMD_MEASURE_VDD           // Measure supply voltage
ST25R3916_CMD_ADJUST_REGULATORS    // Calibrate regulators
```

**Note**: ST25R3916 and PN532 are completely different chips with different command sets. The ST25R3916 is the original Flipper Zero NFC chip and offers more direct control over the RF protocol.

---

## 8. Communication Patterns

### DIY Version - PN532 I2C (Modular)
```c
// furi_hal_pn532.c - Separate functions
pn532_write_frame()      // Build and send PN532 frame
pn532_read_ack()          // Wait and verify ACK
pn532_read_response()     // Read and parse response
pn532_exchange()          // Full command/response cycle with retries
```

### ESP32 Version - PN532 I2C (Combined)
```c
// furi_hal_nfc.c - One function does all
pn532_send_command(cmd, cmd_len, response, response_len, timeout_ms) {
    // Build frame
    // i2c_master_write_to_device()
    // pn532_wait_ready()
    // Read ACK
    // pn532_wait_ready()
    // Read response
}
```

### Momentum Version - ST25R3916 SPI (Direct Register Access)
```c
// lib/drivers/st25r3916.c - Direct FIFO/RF control
st25r3916_write_fifo(handle, tx_data, tx_bits);  // Write to TX FIFO
st25r3916_read_fifo(handle, rx_data, size, &bits); // Read from RX FIFO
st25r3916_direct_cmd(handle, ST25R3916_CMD_TRANSMIT_WITHOUT_CRC);
st25r3916_get_irq(handle);                         // Read IRQ status
st25r3916_mask_irq(handle, mask);                  // Set IRQ mask
```

**Key Difference**: Momentum's ST25R3916 uses SPI with direct FIFO access, offering lower latency and more control compared to PN532's I2C command/response protocol.

---

## 9. Key Insights: Why DIY Uses PN532

The DIY board uses PN532 instead of ST25R3916 because:

1. **Cost**: PN532 modules are cheaper and more readily available
2. **I2C**: Simpler interface than SPI for DIY projects
3. **Integration**: PN532 has built-in protocol handling (ISO14443, FeliCa)
4. **DIY-friendly**: Easier to interface with minimal external components

However, this comes with trade-offs:
- PN532 doesn't support ISO15693 natively (DIY has workarounds)
- PN532's protocol interception is complex
- Less control over RF parameters

---

## 10. Summary: Issues to Fix in DIY Version

### High Priority
1. **PN532 I2C Address**: DIY uses `0x48`, ESP32 uses `0x24` - verify which is correct for your hardware
2. **ISO15693**: DIY has it, ESP32 doesn't - keep DIY's implementation
3. **Abort handling**: ESP32's 200ms slicing is more responsive
4. **Event queue overflow**: Add warning log when queue is full

### Medium Priority
1. **Add missing PN532 commands**: POWER_DOWN, IN_RELEASE, IN_COMMUNICATE_THRU, IN_JUMP_FOR_DEP, TG_RESPONSE_TO_INIT
2. **R-block handling**: Verify PN532 handles internally
3. **PPS interception**: Add support for PPS (Protocol and Parameter Selection)

### Low Priority
1. **BOARD_HAS_NFC stub**: ESP32's approach of graceful degradation
2. **FeliCa ISO-DEP**: DIY may need the more complete SDD frame handling from ESP32

### From Momentum-Firmware (Future Considerations)
1. **Voltage measurement**: Add ADC-based voltage monitoring
2. **Antenna tuning**: Implement calibration for optimal RF performance
3. **Oscillator health check**: Add startup verification
4. **Field detection**: Use ST25R3916's EFD (External Field Detector) registers

---

## 11. File Location Reference

### DIY Flipper Zero (Current Project)
- Main HAL: `targets/f7/furi_hal/furi_hal_nfc.c`
- PN532 Driver: `targets/f7/furi_hal/furi_hal_pn532.c`
- PN532 Backend: `targets/f7/furi_hal/furi_hal_nfc_pn532.c`
- Tech Implementations: `targets/f7/furi_hal/furi_hal_nfc_*.c`
- Header: `targets/furi_hal_include/furi_hal_nfc.h`

### ESP32 Port (Reference Project)
- All NFC HAL: `components/furi_hal/furi_hal_nfc.c` (1541 lines)
- Header: `components/furi_hal/furi_hal_nfc.h`

### Momentum-Firmware (Reference Project)
- Main HAL: `targets/f7/furi_hal/furi_hal_nfc.c`
- Timer: `targets/f7/furi_hal/furi_hal_nfc_timer.c`
- Events: `targets/f7/furi_hal/furi_hal_nfc_event.c`
- IRQ: `targets/f7/furi_hal/furi_hal_nfc_irq.c`
- Tech Implementations: `targets/f7/furi_hal/furi_hal_nfc_*.c`
- ST25R3916 Driver: `lib/drivers/st25r3916.c`
- ST25R3916 Registers: `lib/drivers/st25r3916_reg.c`