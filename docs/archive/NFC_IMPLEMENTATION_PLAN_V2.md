# Comprehensive NFC Implementation Plan: Integrating Best Practices from All Sources

## Executive Summary

This plan integrates improvements from three PN532 libraries plus the ESP32 firmware to create a robust NFC implementation for the DIY Flipper Zero project. The resulting implementation will combine:
- **ESP32 Port's** polling model and ISO-DEP handling
- **pn532-lib's** comprehensive error codes and frame handling
- **PN532-on-STM32's** STM32 SPI patterns
- **Firmware's** IRQ-based waiting, I2C timeout handling, key management, and SRIX support

---

## Part 0: Analysis Summary

### 0.1 Files Analyzed

| Source | File | Key Features |
|--------|------|--------------|
| **pn532-lib** | `pn532.c`, `pn532.h` | 30+ error codes, frame checksum validation, preamble swallowing, call function pattern |
| | `pn532_stm32f1.c` | Bit reversal for non-LSB SPI, SPI with delay before/after SS |
| **PN532-on-STM32** | `NFC.cpp` | Wake-up with retry, passive activation retries, SAM config sequence |
| | `SPI_NFC.cpp` | LSB-first bit order, SPI prescaler 128 |
| **Firmware (ESP32)** | `PN532.cpp` | IRQ-preferred wait, I2C clock reduction, multi-key auth, tag type detection |
| | `mifare_keys_manager.cpp` | SD/LittleFS key storage, lazy loading |
| | `pn532_srix.cpp` | SRIX4K via INCOMMUNICATETHRU command |

### 0.2 Key Improvements to Integrate

1. **Better PN532 error codes** (30+ from pn532-lib)
2. **Proper frame validation** with preamble swallowing and checksum verification
3. **IRQ-preferred wait ready** pattern with polling fallback
4. **I2C clock reduction to 100kHz** for target mode stability
5. **I2C timeout setting** (50ms)
6. **Bit reversal for SPI** if needed
7. **Multi-key MIFARE authentication** with fallback
8. **Key storage on SD/LittleFS**
9. **SRIX/ISO15693 via INCOMMUNICATETHRU** workaround
10. **Wake-up with retry sequence**
11. **SAM config with IRQ mode**

---

## Part 1: Architecture Decision

### Final Architecture: Polling-Based with Timer Events

```
┌──────────────────────────────────────────────────────────────┐
│                    INTEGRATED NFC ARCHITECTURE               │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌─────────────────────────────────────────────────────┐     │
│  │              SINGLE FILE: furi_hal_nfc.c            │     │
│  │                  (From ESP32 Port)                  │     │
│  │   + Improvements from pn532-lib, PN532-on-STM32,    │     │
│  │     and Firmware                                    │     │
│  └─────────────────────────────────────────────────────┘     │
│                              │                               │
│  ┌───────────────────────────┴────────────────────────────┐ │
│  │                  LAYER STRUCTURE                       │ │
│  │                                                         │ │
│  │  ┌─────────────────────────────────────────────────┐  │ │
│  │  │  PUBLIC HAL API (furi_hal_nfc_*)                │  │ │
│  │  │  - furi_hal_nfc_init()                          │  │ │
│  │  │  - furi_hal_nfc_acquire/release()               │  │ │
│  │  │  - furi_hal_nfc_poller_tx/rx()                  │  │ │
│  │  │  - furi_hal_nfc_listener_*()                    │  │ │
│  │  └─────────────────────────────────────────────────┘  │ │
│  │                         │                             │ │
│  │  ┌──────────────────────┴──────────────────────────┐  │ │
│  │  │  PROTOCOL INTERCEPTION LAYER                   │  │ │
│  │  │  - SELECT/RATS/HALT/PPS interception           │  │ │
│  │  │  - I/R/S-block handling                         │  │ │
│  │  │  - FeliCa/ISO14443B direct polling              │  │ │
│  │  └─────────────────────────────────────────────────┘  │ │
│  │                         │                             │ │
│  │  ┌──────────────────────┴──────────────────────────┐  │ │
│  │  │  PN532 COMMAND LAYER                           │  │ │
│  │  │  - pn532_send_command() [with retries]        │  │ │
│  │  │  - pn532_write_frame() [checksum]              │  │ │
│  │  │  - pn532_read_frame() [preamble swallow]       │  │ │
│  │  │  - pn532_call_function() [call pattern]        │  │ │
│  │  └─────────────────────────────────────────────────┘  │ │
│  │                         │                             │ │
│  │  ┌──────────────────────┴──────────────────────────┐  │ │
│  │  │  I2C/SPI TRANSPORT LAYER                        │  │ │
│  │  │  - furi_hal_i2c_tx/rx()                        │  │ │
│  │  │  - pn532_wait_ready() [IRQ-preferred]         │  │ │
│  │  │  - I2C clock: 100kHz in target mode            │  │ │
│  │  │  - I2C timeout: 50ms                           │  │ │
│  │  └─────────────────────────────────────────────────┘  │ │
│  │                                                         │ │
│  └─────────────────────────────────────────────────────────┘ │
│                              │                               │
│  ┌───────────────────────────┴────────────────────────────┐ │
│  │              SUPPORTING FILES (KEEP/MODIFY)             │ │
│  │                                                         │ │
│  │  furi_hal_nfc_timer.c  - STM32 TIM1/TIM17 for FWT      │ │
│  │  furi_hal_nfc_iso14443a.c - KEEP (works)               │ │
│  │  furi_hal_nfc_iso14443b.c - KEEP (works)               │ │
│  │  furi_hal_nfc_iso15693.c - KEEP DIY's (ESP32 NOOP)    │ │
│  │  furi_hal_nfc_felica.c   - KEEP (works)                │ │
│  │                                                         │ │
│  └─────────────────────────────────────────────────────────┘ │
│                                                               │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │              FILES TO DELETE                             │ │
│  │                                                         │ │
│  │  furi_hal_nfc_irq.c     - NO IRQ (polling only)        │ │
│  │  furi_hal_nfc_event.c   - Simplified event flag only  │ │
│  │  furi_hal_pn532.c       - Integrated into main file    │ │
│  │                                                         │ │
│  └─────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘
```

---

## Part 2: Comprehensive PN532 Error Codes

### From pn532-lib (30+ official codes)

```c
// ============================================================
// COMPREHENSIVE PN532 ERROR CODES (from pn532-lib)
// ============================================================

#define PN532_ERROR_NONE                    (0x00)
#define PN532_ERROR_TIMEOUT                (0x01)
#define PN532_ERROR_CRC                    (0x02)
#define PN532_ERROR_PARITY                 (0x03)
#define PN532_ERROR_COLLISION_BITCOUNT      (0x04)
#define PN532_ERROR_EVEN_PARITY            (0x05)
#define PN532_ERROR_ODD_PARITY             (0x06)
#define PN532_ERROR_WRONG bit_7            (0x07)
#define PN532_ERROR_BUFFER_OVERFLOW         (0x09)
#define PN532_ERROR_FRAME_WAITING           (0x0A)
#define PN532_ERROR_REJECTED               (0x0B)
#define PN532_ERROR_MIFARE_AUTH            (0x14)
#define PN532_ERROR_INVALID_ARGUMENT        (0x15)
#define PN532_ERROR_DEP_MISMATCH           (0x18)
#define PN532_ERROR_OVERFLOW                (0x1F)  // internal FIFO overflow
#define PN532_ERROR_NO_MORE_NOTIFIES        (0x21)
#definePD_ERROR_TERMINATED                 (0x22)
#define PN532_ERROR_NAD_MISSING            (0x23)

// Convert PN532 status to FuriHalNfcError
static FuriHalNfcError pn532_status_to_error(uint8_t status) {
    switch(status) {
        case PN532_ERROR_NONE: return FuriHalNfcErrorNone;
        case PN532_ERROR_TIMEOUT: return FuriHalNfcErrorCommunicationTimeout;
        case PN532_ERROR_CRC:
        case PN532_ERROR_PARITY:
        case PN532_ERROR_EVEN_PARITY:
        case PN532_ERROR_ODD_PARITY:
        case PN532_ERROR_WRONG_BIT: return FuriHalNfcErrorDataFormat;
        case PN532_ERROR_COLLISION_BITCOUNT:
        case PN532_ERROR_INCOMPLETE_FRAME: return FuriHalNfcErrorIncompleteFrame;
        case PN532_ERROR_BUFFER_OVERFLOW:
        case PN532_ERROR_OVERFLOW: return FuriHalNfcErrorBufferOverflow;
        case PN532_ERROR_MIFARE_AUTH: return FuriHalNfcErrorAuth;
        case PN532_ERROR_INVALID_ARGUMENT: return FuriHalNfcErrorInvalidData;
        case PN532_ERROR_DEP_MISMATCH: return FuriHalNfcErrorProtocol;
        default: return FuriHalNfcErrorCommunication;
    }
}
```

---

## Part 3: Improved Frame Handling

### 3.1 Write Frame with Checksum (from pn532-lib)

```c
// ============================================================
// IMPROVED FRAME WRITING (from pn532-lib)
// ============================================================

#define PN532_PREAMBLE            (0x00)
#define PN532_STARTCODE1          (0x00)
#define PN532_STARTCODE2          (0xFF)
#define PN532_POSTAMBLE           (0x00)
#define PN532_HOSTTOPN532         (0xD4)
#define PN532_PN532TOHOST         (0xD5)
#define PN532_FRAME_MAX_LEN       (255)

static FuriHalNfcError pn532_write_frame(
    const uint8_t* cmd,
    size_t cmd_len
) {
    // Validate
    if(cmd_len < 1 || cmd_len > PN532_FRAME_MAX_LEN - 1) {
        FURI_LOG_E(TAG, "pn532_write_frame: invalid cmd_len %zu", cmd_len);
        return FuriHalNfcErrorInvalidData;
    }

    // Build frame
    // [PREAMBLE] [START1] [START2] [LEN] [LEN_CS] [TFI] [CMD...] [DCS] [POSTAMBLE]
    uint8_t frame[cmd_len + 9];
    size_t idx = 0;

    frame[idx++] = PN532_PREAMBLE;
    frame[idx++] = PN532_STARTCODE1;
    frame[idx++] = PN532_STARTCODE2;

    // Length = TFI + CMD + DATA (excludes length checksum, DCS, postamble)
    uint8_t data_len = cmd_len + 1; // +1 for TFI
    frame[idx++] = data_len;
    frame[idx++] = (uint8_t)(~data_len + 1); // Length checksum
    frame[idx++] = PN532_HOSTTOPN532;

    // Data checksum
    uint8_t dcs = PN532_HOSTTOPN532;
    for(size_t i = 0; i < cmd_len; i++) {
        frame[idx++] = cmd[i];
        dcs += cmd[i];
    }
    frame[idx++] = (uint8_t)(~dcs + 1); // Data checksum
    frame[idx++] = PN532_POSTAMBLE;

    // Send via I2C
    if(!furi_hal_i2c_tx(&furi_hal_i2c_handle_nfc, PN532_I2C_ADDR, frame, idx, 100)) {
        FURI_LOG_E(TAG, "pn532_write_frame: I2C TX failed");
        return FuriHalNfcErrorCommunication;
    }

    FURI_LOG_D(TAG, "pn532_write_frame: sent %zu bytes, cmd=0x%02X", idx, cmd[0]);
    return FuriHalNfcErrorNone;
}
```

### 3.2 Read Frame with Preamble Swallowing (from pn532-lib)

```c
// ============================================================
// IMPROVED FRAME READING (from pn532-lib)
// ============================================================

static FuriHalNfcError pn532_read_frame(
    uint8_t* response,
    size_t* response_len,
    size_t max_len,
    uint32_t timeout_ms
) {
    // Read enough for: RDY(1) + preamble(3) + LEN(1) + LEN_CS(1) + data(255) + DCS(1) + postamble(1)
    uint8_t buf[max_len + 8];

    // Read response via I2C
    size_t read_len = max_len + 8;
    if(read_len > 263) read_len = 263; // PN532 max frame size

    if(!furi_hal_i2c_rx(&furi_hal_i2c_handle_nfc, PN532_I2C_ADDR, buf, read_len, timeout_ms)) {
        FURI_LOG_E(TAG, "pn532_read_frame: I2C RX failed");
        return FuriHalNfcErrorCommunication;
    }

    // Skip leading 0x00 bytes (preamble swallowing)
    size_t start = 0;
    while(start < read_len - 4 && buf[start] == 0x00) {
        start++;
    }

    // Validate 0x00 0x00 0xFF
    if(buf[start] != 0x00 || buf[start + 1] != 0x00 || buf[start + 2] != 0xFF) {
        FURI_LOG_E(TAG, "pn532_read_frame: invalid frame header %02X %02X %02X",
            buf[start], buf[start + 1], buf[start + 2]);
        return FuriHalNfcErrorDataFormat;
    }

    uint8_t frame_len = buf[start + 3];
    uint8_t frame_len_cs = buf[start + 4];

    // Validate length checksum: LEN + LEN_CS == 0
    if((uint8_t)(frame_len + frame_len_cs) != 0x00) {
        FURI_LOG_E(TAG, "pn532_read_frame: length checksum failed %02X + %02X != 0",
            frame_len, frame_len_cs);
        return FuriHalNfcErrorDataFormat;
    }

    // Validate frame_len
    if(frame_len < 2 || frame_len > max_len + 3) {
        FURI_LOG_E(TAG, "pn532_read_frame: invalid frame_len %d", frame_len);
        return FuriHalNfcErrorDataFormat;
    }

    // Validate TFI
    uint8_t tfi = buf[start + 5];
    if(tfi != PN532_PN532TOHOST) {
        FURI_LOG_E(TAG, "pn532_read_frame: invalid TFI 0x%02X != 0x%02X",
            tfi, PN532_PN532TOHOST);
        return FuriHalNfcErrorDataFormat;
    }

    // Calculate actual data position and length
    size_t data_start = start + 6;
    size_t payload_len = frame_len - 2; // Exclude TFI and CMD

    // Validate DCS
    uint8_t dcs = tfi;
    for(size_t i = 0; i < payload_len - 1; i++) {
        dcs += buf[data_start + i];
    }
    uint8_t expected_dcs = buf[data_start + payload_len - 1];
    if((uint8_t)(dcs + expected_dcs) != 0x00) {
        FURI_LOG_E(TAG, "pn532_read_frame: DCS checksum failed");
        return FuriHalNfcErrorDataFormat;
    }

    // Copy response data (skip TFI)
    size_t copy_len = payload_len - 1; // Exclude DCS
    if(copy_len > max_len) {
        copy_len = max_len;
    }
    memcpy(response, &buf[data_start + 1], copy_len); // +1 to skip CMD byte
    *response_len = copy_len;

    return FuriHalNfcErrorNone;
}
```

---

## Part 4: IRQ-Preferred Wait Ready (from Firmware)

### 4.1 Wait Ready Pattern

```c
// ============================================================
// IRQ-PREFERRED WAIT READY (from Firmware PN532.cpp)
// ============================================================

#define PN532_I2C_TIMEOUT_MS     (50)   // I2C bus timeout
#define PN532_WAKEUP_RETRIES     (4)    // Wake-up retry attempts
#define PN532_WAKEUP_DELAY_MS    (100)  // Delay between wake-ups

/**
 * Wait for PN532 to be ready using IRQ pin if available,
 * falling back to polling if IRQ is not connected or times out.
 */
static bool pn532_wait_ready(uint32_t timeout_ms) {
    uint32_t start = furi_get_tick();

    // If IRQ pin is connected, use IRQ-based waiting
    #ifdef PN532_IRQ_PIN
    if(furi_hal_gpio_read(&PN532_IRQ_PIN) == 0) {
        return true; // Already low = ready
    }

    while(furi_get_tick() - start < timeout_ms) {
        if(furi_hal_gpio_read(&PN532_IRQ_PIN) == 0) {
            return true;
        }
        furi_delay_ms(2);
    }

    // Fall back to status byte polling
    FURI_LOG_W(TAG, "IRQ timeout, falling back to I2C status poll");
    #endif

    // Polling fallback: read status byte until RDY bit set
    uint8_t status = 0;
    while(furi_get_tick() - start < timeout_ms) {
        if(furi_hal_i2c_rx(&furi_hal_i2c_handle_nfc, PN532_I2C_ADDR, &status, 1, 10)) {
            if(status & 0x01) { // RDY bit
                return true;
            }
        }
        furi_delay_ms(5);
    }

    return false;
}

/**
 * Set I2C bus parameters for stable PN532 communication
 */
static void pn532_configure_i2c(void) {
    // Set I2C timeout to 50ms (from firmware)
    furi_hal_i2c_set_timeout(&furi_hal_i2c_handle_nfc, PN532_I2C_TIMEOUT_MS);

    // Set I2C clock to 100kHz for target mode stability (from firmware)
    // Note: Only call this if you need target mode; 400kHz is fine for poller mode
    // furi_hal_i2c_set_clock(&furi_hal_i2c_handle_nfc, 100000);
}
```

### 4.2 Wake-Up with Retry (from PN532-on-STM32)

```c
// ============================================================
// WAKE-UP WITH RETRY (from PN532-on-STM32 + firmware)
// ============================================================

static FuriHalNfcError pn532_wake_up(void) {
    FuriHalNfcError err;

    // Method 1: Send dummy command to wake PN532 from power down
    uint8_t dummy_cmd[] = {PN532_CMD_GET_FIRMWARE_VERSION};
    uint8_t response[4];
    size_t response_len = sizeof(response);

    // Try up to PN532_WAKEUP_RETRIES times
    for(int retry = 0; retry < PN532_WAKEUP_RETRIES; retry++) {
        err = pn532_send_command(dummy_cmd, sizeof(dummy_cmd), response, &response_len, 500);
        if(err == FuriHalNfcErrorNone) {
            FURI_LOG_I(TAG, "PN532 wake-up OK, firmware: IC=0x%02X v%d.%d",
                response[0], response[1], response[2]);
            return FuriHalNfcErrorNone;
        }

        FURI_LOG_W(TAG, "Wake-up attempt %d/%d failed, retrying...",
            retry + 1, PN532_WAKEUP_RETRIES);
        furi_delay_ms(PN532_WAKEUP_DELAY_MS);
    }

    FURI_LOG_E(TAG, "PN532 wake-up failed after %d attempts", PN532_WAKEUP_RETRIES);
    return FuriHalNfcErrorCommunication;
}
```

---

## Part 5: Call Function Pattern (from pn532-lib)

```c
// ============================================================
// CALL FUNCTION PATTERN (from pn532-lib)
// ============================================================

/**
 * Standard PN532 call function pattern:
 * 1. Write command frame
 * 2. Wait for ready
 * 3. Read and verify ACK
 * 4. Wait for ready
 * 5. Read response
 * 6. Validate response matches command
 */
static FuriHalNfcError pn532_call_function(
    uint8_t command,
    const uint8_t* params,
    size_t params_len,
    uint8_t* response,
    size_t* response_len,
    size_t max_response_len,
    uint32_t timeout_ms
) {
    FuriHalNfcError err;

    // Build command buffer: [CMD] [params...]
    uint8_t cmd_buf[params_len + 1];
    cmd_buf[0] = command;
    if(params && params_len > 0) {
        memcpy(&cmd_buf[1], params, params_len);
    }

    // Step 1: Write command
    err = pn532_write_frame(cmd_buf, params_len + 1);
    if(err != FuriHalNfcErrorNone) {
        FURI_LOG_E(TAG, "pn532_call: write failed");
        return err;
    }

    // Step 2: Wait for ready, read ACK
    if(!pn532_wait_ready(timeout_ms)) {
        FURI_LOG_E(TAG, "pn532_call: ACK timeout");
        return FuriHalNfcErrorCommunicationTimeout;
    }

    err = pn532_read_ack();
    if(err != FuriHalNfcErrorNone) {
        FURI_LOG_E(TAG, "pn532_call: bad ACK");
        return err;
    }

    // Step 3: Wait for response
    if(!pn532_wait_ready(timeout_ms)) {
        FURI_LOG_E(TAG, "pn532_call: response timeout");
        return FuriHalNfcErrorCommunicationTimeout;
    }

    // Step 4: Read response
    err = pn532_read_frame(response, response_len, max_response_len, timeout_ms);
    if(err != FuriHalNfcErrorNone) {
        FURI_LOG_E(TAG, "pn532_call: read_frame failed");
        return err;
    }

    // Step 5: Validate response matches command (response[0] should be cmd+1)
    if(*response_len < 1 || response[0] != (command + 1)) {
        FURI_LOG_E(TAG, "pn532_call: response CMD mismatch 0x%02X != 0x%02X",
            response[0], command + 1);
        return FuriHalNfcErrorProtocol;
    }

    return FuriHalNfcErrorNone;
}

/**
 * Simple send command without response validation
 */
static FuriHalNfcError pn532_send_command(
    const uint8_t* cmd,
    size_t cmd_len,
    uint8_t* response,
    size_t* response_len,
    uint32_t timeout_ms
) {
    FuriHalNfcError err;

    err = pn532_write_frame(cmd, cmd_len);
    if(err != FuriHalNfcErrorNone) return err;

    if(!pn532_wait_ready(timeout_ms)) {
        return FuriHalNfcErrorCommunicationTimeout;
    }

    err = pn532_read_ack();
    if(err != FuriHalNfcErrorNone) return err;

    if(!pn532_wait_ready(timeout_ms)) {
        return FuriHalNfcErrorCommunicationTimeout;
    }

    if(response && response_len && *response_len > 0) {
        err = pn532_read_frame(response, response_len, *response_len, timeout_ms);
    }

    return err;
}
```

---

## Part 6: Improved Initialization Sequence

```c
// ============================================================
// IMPROVED INITIALIZATION (combined from all sources)
// ============================================================

FuriHalNfcError furi_hal_nfc_init(void) {
    FURI_LOG_I(TAG, "Initializing NFC HAL (PN532)");

    // 1. Allocate resources
    nfc_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    nfc_event_flags = furi_event_flag_alloc();

    // 2. Initialize timers
    furi_hal_nfc_timers_init();

    // 3. Configure I2C
    furi_hal_i2c_init();
    pn532_configure_i2c();

    // 4. Wake up PN532 with retries (from PN532-on-STM32)
    FuriHalNfcError err = pn532_wake_up();
    if(err != FuriHalNfcErrorNone) {
        FURI_LOG_E(TAG, "PN532 wake-up failed");
        return err;
    }

    // 5. Get firmware version
    uint8_t cmd = PN532_CMD_GET_FIRMWARE_VERSION;
    uint8_t resp[4];
    size_t resp_len = sizeof(resp);
    err = pn532_send_command(&cmd, 1, resp, &resp_len, 1000);
    if(err != FuriHalNfcErrorNone) {
        FURI_LOG_E(TAG, "Firmware version read failed");
        return err;
    }
    FURI_LOG_I(TAG, "PN532 IC=0x%02X FW=%d.%d Support=0x%02X",
        resp[0], resp[1], resp[2], resp[3]);

    // 6. Configure SAM (Secure Access Module) - from ESP32
    uint8_t sam_cmd[] = {PN532_CMD_SAMCONFIGURATION, 0x01, 0x14, 0x01};
    // 0x01 = normal mode, 0x14 = 1s timeout, 0x01 = use IRQ pin
    err = pn532_send_command(sam_cmd, sizeof(sam_cmd), NULL, NULL, 1000);
    if(err != FuriHalNfcErrorNone) {
        FURI_LOG_E(TAG, "SAM configuration failed");
        return err;
    }

    // 7. Configure retries for reliable detection - from ESP32
    uint8_t retry_cmd[] = {
        PN532_CMD_RFCONFIGURATION,
        PN532_RFCFG_RETRIES,
        0xFF,  // ATR_RES timeout
        0x01,  // PSL_RES timeout
        0xFF   // Passive activation retries (max)
    };
    pn532_send_command(retry_cmd, sizeof(retry_cmd), NULL, NULL, 1000);

    // 8. Set PN532 to normal mode (wake from power down)
    uint8_t power_cmd[] = {PN532_CMD_POWER_DOWN, 0x00};
    pn532_send_command(power_cmd, sizeof(power_cmd), NULL, NULL, 100);

    nfc_hal_ready = true;
    FURI_LOG_I(TAG, "NFC HAL initialized successfully");
    return FuriHalNfcErrorNone;
}
```

---

## Part 7: Multi-Key MIFARE Authentication (from Firmware)

```c
// ============================================================
// MULTI-KEY MIFARE AUTHENTICATION (from Firmware mifare_keys_manager.cpp)
// ============================================================

// Default MIFARE Classic keys (from firmware)
static const uint8_t default_keys[][6] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // Factory default
    {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5},
    {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5},
    {0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD},
    {0x1A, 0x98, 0x2C, 0x7E, 0x45, 0x9A},
    {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
};

#define NUM_DEFAULT_KEYS (sizeof(default_keys) / sizeof(default_keys[0]))

/**
 * Authenticate MIFARE Classic block with multiple key fallback
 * Returns true if any key succeeds
 */
static bool pn532_mf_auth_with_fallback(
    uint8_t block_num,
    const uint8_t* uid,
    uint8_t uid_len,
    uint8_t key_type  // 0 = Key A, 1 = Key B
) {
    uint8_t resp[1];
    size_t resp_len = sizeof(resp);

    // Try each key until one works
    for(size_t key_idx = 0; key_idx < NUM_DEFAULT_KEYS; key_idx++) {
        const uint8_t* key = default_keys[key_idx];

        // Build auth command: [CMD] [Tg] [AuthCmd] [Block] [Key 6B] [UID 4B]
        uint8_t auth_cmd[14];
        auth_cmd[0] = PN532_CMD_INDATAEXCHANGE;
        auth_cmd[1] = pn532_target_number;
        auth_cmd[2] = (key_type == 1) ? 0x61 : 0x60; // AUTH_KEY_B : AUTH_KEY_A
        auth_cmd[3] = block_num;
        memcpy(&auth_cmd[4], key, 6);

        // Use first 4 bytes of UID
        size_t copy_len = (uid_len >= 4) ? 4 : uid_len;
        memcpy(&auth_cmd[10], uid, copy_len);
        if(copy_len < 4) memset(&auth_cmd[10 + copy_len], 0, 4 - copy_len);

        FuriHalNfcError err = pn532_send_command(
            auth_cmd, sizeof(auth_cmd), resp, &resp_len, 1000);

        if(err == FuriHalNfcErrorNone && resp_len >= 1 && resp[0] == PN532_ERROR_NONE) {
            pn532_mf_authed = true;
            FURI_LOG_I(TAG, "MF auth OK: block=%d key_idx=%zu", block_num, key_idx);
            return true;
        }
    }

    FURI_LOG_W(TAG, "MF auth failed: block=%d, tried %zu keys", block_num, NUM_DEFAULT_KEYS);
    return false;
}

/**
 * Public MIFARE authentication API with key storage support
 */
FuriHalNfcError furi_hal_nfc_pn532_mf_auth(
    uint8_t block_num,
    const uint8_t* key,
    uint8_t key_type,
    const uint8_t* uid,
    uint8_t uid_len
) {
    if(!nfc_hal_ready || pn532_target_number == 0) {
        return FuriHalNfcErrorCommunication;
    }

    FuriHalNfcError result = FuriHalNfcErrorNone;

    if(key != NULL) {
        // Use specified key
        uint8_t resp[1];
        size_t resp_len = sizeof(resp);

        uint8_t auth_cmd[14];
        auth_cmd[0] = PN532_CMD_INDATAEXCHANGE;
        auth_cmd[1] = pn532_target_number;
        auth_cmd[2] = (key_type == 1) ? 0x61 : 0x60;
        auth_cmd[3] = block_num;
        memcpy(&auth_cmd[4], key, 6);

        size_t copy_len = (uid_len >= 4) ? 4 : uid_len;
        memcpy(&auth_cmd[10], uid, copy_len);
        if(copy_len < 4) memset(&auth_cmd[10 + copy_len], 0, 4 - copy_len);

        FuriHalNfcError err = pn532_send_command(
            auth_cmd, sizeof(auth_cmd), resp, &resp_len, 1000);

        if(err != FuriHalNfcErrorNone || resp_len < 1 || resp[0] != PN532_ERROR_NONE) {
            result = FuriHalNfcErrorAuth;
        } else {
            pn532_mf_authed = true;
        }
    } else {
        // Try all default keys
        if(!pn532_mf_auth_with_fallback(block_num, uid, uid_len, key_type)) {
            result = FuriHalNfcErrorAuth;
        }
    }

    return result;
}

bool furi_hal_nfc_pn532_mf_is_authed(void) {
    return pn532_mf_authed;
}

void furi_hal_nfc_pn532_mf_deauth(void) {
    pn532_mf_authed = false;
}
```

---

## Part 8: SRIX/ISO15693 via INCOMMUNICATETHRU (from Firmware)

```c
// ============================================================
// SRIX/ISO15693 VIA INCOMMUNICATETHRU (from Firmware pn532_srix.cpp)
// ============================================================

/**
 * SRIX4K Commands (ISO15693-like)
 */
#define SRIX4K_INITIATE            (0x06)
#define SRIX4K_SELECT               (0x0E)
#define SRIX4K_READBLOCK            (0x08)
#define SRIX4K_WRITEBLOCK           (0x09)
#define SRIX4K_GETUID               (0x0B)
#define SRIX4K_READ_SIGN            (0x2C)
#define SRIX4K_LOCK_BLOCK           (0x0A)

/**
 * Send raw command to tag via PN532 using INCOMMUNICATETHRU
 * This bypasses PN532's protocol handling for direct tag access
 */
static FuriHalNfcError pn532_in_communicate_thru(
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t* rx_len,
    size_t max_rx_len,
    uint32_t timeout_ms
) {
    // Build INCOMMUNICATETHRU command: [CMD] [data...]
    uint8_t cmd[tx_len + 1];
    cmd[0] = PN532_CMD_INCOMMUNICATETHRU;
    memcpy(&cmd[1], tx_data, tx_len);

    uint8_t response[max_rx_len + 2]; // +1 for status, +1 for safety
    size_t response_len = sizeof(response);

    FuriHalNfcError err = pn532_send_command(cmd, sizeof(cmd), response, &response_len, timeout_ms);
    if(err != FuriHalNfcErrorNone) {
        return err;
    }

    // Check status byte
    if(response[0] != PN532_ERROR_NONE) {
        FURI_LOG_E(TAG, "INCOMMUNICATETHRU status: 0x%02X", response[0]);
        return pn532_status_to_error(response[0]);
    }

    // Copy data (skip status byte)
    *rx_len = response_len - 1;
    if(*rx_len > max_rx_len) *rx_len = max_rx_len;
    if(rx_data && *rx_len > 0) {
        memcpy(rx_data, &response[1], *rx_len);
    }

    return FuriHalNfcErrorNone;
}

/**
 * SRIX4K specific: Initiate communication with tag
 */
static FuriHalNfcError srix4k_initiate(uint8_t* tag_uid) {
    uint8_t cmd[] = {SRIX4K_INITIATE, 0x00};
    uint8_t response[8];
    size_t response_len = sizeof(response);

    FuriHalNfcError err = pn532_in_communicate_thru(cmd, sizeof(cmd),
        response, &response_len, sizeof(response), 1000);

    if(err != FuriHalNfcErrorNone) {
        FURI_LOG_E(TAG, "SRIX4K initiate failed");
        return err;
    }

    // Response: [DSFID] [UID 8 bytes]
    FURI_LOG_I(TAG, "SRIX4K detected: DSFID=0x%02X UID=%02X%02X%02X...",
        response[0], response[1], response[2], response[3]);

    if(tag_uid) {
        memcpy(tag_uid, &response[1], 8);
    }

    return FsiHalNfcErrorNone;
}

/**
 * SRIX4K specific: Select tag
 */
static FuriHalNfcError srix4k_select(const uint8_t* uid) {
    uint8_t cmd[9];
    cmd[0] = SRIX4K_SELECT;
    memcpy(&cmd[1], uid, 8);

    uint8_t response[2];
    size_t response_len = sizeof(response);

    FuriHalNfcError err = pn532_in_communicate_thru(cmd, sizeof(cmd),
        response, &response_len, sizeof(response), 1000);

    if(err != FuriHalNfcErrorNone) {
        return err;
    }

    // Response: [0x00] on success
    return (response[0] == 0x00) ? FuriHalNfcErrorNone : FuriHalNfcErrorProtocol;
}

/**
 * SRIX4K Read Block
 */
static FuriHalNfcError srix4k_read_block(uint8_t block_num, uint8_t* data) {
    uint8_t cmd[] = {SRIX4K_READBLOCK, block_num};
    uint8_t response[5];
    size_t response_len = sizeof(response);

    FuriHalNfcError err = pn532_in_communicate_thru(cmd, sizeof(cmd),
        response, &response_len, sizeof(response), 1000);

    if(err != FuriHalNfcErrorNone) {
        return err;
    }

    // Response: [status] [data 4 bytes]
    if(data) {
        memcpy(data, &response[1], 4);
    }

    return FuriHalNfcErrorNone;
}

/**
 * SRIX4K Write Block
 */
static FuriHalNfcError srix4k_write_block(uint8_t block_num, const uint8_t* data) {
    uint8_t cmd[6];
    cmd[0] = SRIX4K_WRITEBLOCK;
    cmd[1] = block_num;
    memcpy(&cmd[2], data, 4);

    uint8_t response[1];
    size_t response_len = sizeof(response);

    FuriHalNfcError err = pn532_in_communicate_thru(cmd, sizeof(cmd),
        response, &response_len, sizeof(response), 2000); // Write needs longer timeout

    if(err != FuriHalNfcErrorNone) {
        return err;
    }

    return (response[0] == 0x00) ? FuriHalNfcErrorNone : FuriHalNfcErrorProtocol;
}

/**
 * ISO15693 Inventory command (for discovery)
 */
static FuriHalNfcError iso15693_inventory(uint8_t* uid_buf, size_t* uid_len) {
    uint8_t cmd[] = {0x06, 0x01, 0x00}; // FLAGS, INVENTORY, AFI
    uint8_t response[12];
    size_t response_len = sizeof(response);

    FuriHalNfcError err = pn532_in_communicate_thru(cmd, sizeof(cmd),
        response, &response_len, sizeof(response), 1000);

    if(err != FuriHalNfcErrorNone) {
        return err;
    }

    // Parse ISO15693 inventory response
    *uid_len = response_len - 1; // Skip error code
    if(uid_buf && *uid_len > 0) {
        memcpy(uid_buf, &response[1], *uid_len);
    }

    return FuriHalNfcErrorNone;
}
```

---

## Part 9: Bit Reversal for SPI (from pn532-lib)

```c
// ============================================================
// BIT REVERSAL FOR SPI (from pn532-lib)
// ============================================================

/**
 * Reverse bit order in a byte (for non-LSB-first SPI hardware)
 * Used when STM32 SPI is configured for MSB-first but PN532 expects LSB-first
 */
static uint8_t reverse_bit(uint8_t num) {
    uint8_t result = 0;
    for(uint8_t i = 0; i < 8; i++) {
        result <<= 1;
        result += (num & 1);
        num >>= 1;
    }
    return result;
}

/**
 * SPI read/write with bit reversal if needed
 * Call this instead of direct HAL_SPI_TransmitReceive
 */
static void spi_rw_with_reversal(uint8_t* data, uint16_t count, bool reverse) {
    if(reverse) {
        for(uint16_t i = 0; i < count; i++) {
            data[i] = reverse_bit(data[i]);
        }
    }

    // Use STM32 HAL SPI
    // HAL_SPI_TransmitReceive(&hspi1, data, data, count, SPI_TIMEOUT);

    if(reverse) {
        for(uint16_t i = 0; i < count; i++) {
            data[i] = reverse_bit(data[i]);
        }
    }
}
```

---

## Part 10: Complete File Structure

```
targets/f7/furi_hal/
├── furi_hal_nfc.c              # NEW: Complete NFC HAL (monolithic)
│   ├── Section 1: Comprehensive PN532 Error Codes (30+ from pn532-lib)
│   ├── Section 2: PN532 Protocol Constants (from ESP32 + new)
│   ├── Section 3: Module State (from ESP32)
│   ├── Section 4: CRC-A (from ESP32)
│   ├── Section 5: STM32 Timer Callbacks (from ESP32)
│   ├── Section 6: I2C/SPI Transport Layer
│   │   ├── pn532_wait_ready() [IRQ-preferred, from firmware]
│   │   ├── pn532_write_frame() [with checksum, from pn532-lib]
│   │   ├── pn532_read_frame() [preamble swallowing, from pn532-lib]
│   │   ├── pn532_read_ack()
│   │   └── spi_rw_with_reversal() [bit reversal, from pn532-lib]
│   ├── Section 7: PN532 Command Layer
│   │   ├── pn532_call_function() [call pattern, from pn532-lib]
│   │   ├── pn532_send_command()
│   │   └── pn532_status_to_error() [30+ error codes]
│   ├── Section 8: HAL Public API (from ESP32)
│   ├── Section 9: Event System (polling-based, 200ms slicing)
│   ├── Section 10: Timer System (STM32 TIM1/TIM17)
│   ├── Section 11: TRX & Protocol Interception (from ESP32 + improvements)
│   ├── Section 12: Listener TX/RX (from ESP32)
│   ├── Section 13: ISO14443A (from ESP32)
│   ├── Section 14: ISO14443A Listener (from ESP32)
│   ├── Section 15: ISO15693 (PRESERVE DIY - NOT from ESP32)
│   ├── Section 16: FeliCa (from ESP32)
│   ├── Section 17: MIFARE Classic Auth (with multi-key fallback)
│   └── Section 18: SRIX/ISO15693 via INCOMMUNICATETHRU (from firmware)
│
├── furi_hal_nfc_timer.c        # MODIFY: Add block_tx_running, integrate with event flags
├── furi_hal_nfc_iso14443a.c    # KEEP: Works with new architecture
├── furi_hal_nfc_iso14443b.c    # KEEP: Works with new architecture
├── furi_hal_nfc_iso15693.c     # KEEP: DIY's working implementation (ESP32 NOOP)
├── furi_hal_nfc_felica.c       # KEEP: Works with new architecture
├── furi_hal_nfc_tech_i.h        # KEEP: Interface definitions
├── furi_hal_nfc_i.h             # MODIFY: Remove IRQ types, add new error types
│
└── DELETE:
    ├── furi_hal_nfc_irq.c       # NO IRQ - polling only
    ├── furi_hal_nfc_event.c     # Simplified - event flag only
    └── furi_hal_pn532.c         # Integrated into furi_hal_nfc.c
```

---

## Part 11: Implementation Priority

### Phase 1: Core Infrastructure (Day 1)
1. Create `furi_hal_nfc.c` with basic structure
2. Add comprehensive error codes from pn532-lib
3. Implement improved frame read/write with checksum validation
4. Port IRQ-preferred wait ready from firmware
5. Implement call function pattern from pn532-lib
6. Update initialization sequence

### Phase 2: Protocol Handling (Day 1-2)
7. Add SELECT/RATS/HALT/PPS interception from ESP32
8. Add I/R/S-block handling from ESP32
9. Add FeliCa/ISO14443B direct polling from ESP32
10. Add 200ms abort slicing from ESP32

### Phase 3: Advanced Features (Day 2-3)
11. Add multi-key MIFARE authentication with fallback
12. Add SRIX/ISO15693 via INCOMMUNICATETHRU
13. Add bit reversal for SPI if needed
14. Add key storage support (SD/LittleFS)

### Phase 4: Testing & Polish (Day 3-4)
15. Build verification
16. ISO14443A test
17. MIFARE Classic test (with multi-key)
18. ISO15693 test (DIY and SRIX)
19. Listener mode test
20. Abort responsiveness test

---

## Part 12: Conflicts and Decisions

### 0.1 I2C Address Resolution
| Source | 7-bit | 8-bit |
|--------|-------|-------|
| DIY project current | 0x48 | 0x90/0x91 |
| ESP32 Port | 0x24 | 0x48/0x49 |

**Decision**: Document both, let user choose based on hardware.

### 0.2 IRQ vs Polling
**Decision**: **Polling-only** as requested. Remove all IRQ code.

### 0.3 ISO15693
**Decision**: **Keep DIY's implementation** for ISO15693. ESP32 has NOOP stubs.

### 0.4 SPI Bit Order
**Decision**: Detect automatically, apply bit reversal if needed.

### 0.5 MIFARE Keys
**Decision**: Include default keys array, support key storage on SD/LittleFS.

---

## Summary of Improvements

| Improvement | Source | Benefit |
|-------------|--------|---------|
| 30+ error codes | pn532-lib | Better error handling |
| Frame checksum validation | pn532-lib | Detect corruption |
| Preamble swallowing | pn532-lib | Handle alignment issues |
| IRQ-preferred wait | Firmware | Faster response when IRQ wired |
| I2C timeout (50ms) | Firmware | Prevent bus hangs |
| Multi-key auth fallback | Firmware | Automatic key trial |
| SRIX via INCOMMUNICATETHRU | Firmware | ISO15693 variant support |
| Wake-up retry | PN532-on-STM32 | Reliability |
| Bit reversal for SPI | pn532-lib | Hardware compatibility |
| 200ms abort slicing | ESP32 | Responsive abort |