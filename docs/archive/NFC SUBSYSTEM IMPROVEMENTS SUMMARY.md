# NFC Subsystem Fix Summary (PN532 V3 on STM32WB55)

The following critical bugs in the NFC Hardware Abstraction Layer (HAL) were identified and fixed to stabilize the DIY Flipper Zero (UBYTE board):

## 1. I2C Handle Double-Release Fix
- **File:** `targets/f7/furi_hal/furi_hal_pn532.c`
- **Issue:** The `furi_hal_pn532_init()` function was calling `furi_hal_i2c_release()` in its cleanup block despite not having acquired the handle itself (the caller handles the lock). 
- **Impact:** Releasing an unacquired lock corrupted the Furi OS mutex state, leading to subsequent `furi_check` assertion failures and system crashes during I2C operations.
- **Fix:** Removed the erroneous release call.

## 2. Buffer Truncation & Memory Corruption Fix
- **File:** `targets/f7/furi_hal/furi_hal_pn532.c`
- **Issue:** The receive buffer `rx` in the internal `pn532_read_response()` function had been reduced to 32 bytes.
- **Impact:** When reading NFC tags with payloads larger than ~24 bytes (e.g., ISO-DEP APDUs, standard Mifare sectors, or NDEF records), the subsequent `memcpy` would read up to 255 bytes from a 32-byte stack buffer. This caused out-of-bounds reads of stack memory, leading to Undefined Behavior (UB), silent data corruption, or hard faults.
- **Fix:** Restored the `rx` buffer size to `PN532_MAX_RX_FRAME` (255 bytes).

## 3. Recursive I2C Lock Acquisition Fix
- **File:** `targets/f7/furi_hal/furi_hal_pn532.c`
- **Issue:** The public API `furi_hal_pn532_read_response()` was attempting to acquire the I2C handle internally.
- **Impact:** Higher-level functions (like `furi_hal_nfc_pn532_listener_rx`) already hold the I2C lock before calling this function. Re-acquiring the same lock on the same bus triggered a `furi_check(handle->bus->current_handle == NULL)` failure, causing an immediate crash when entering NFC Listener mode.
- **Fix:** Refactored the function to delegate to the internal `pn532_read_response()` which assumes the lock is already held.

## Subsystem Map (NFC)
- **Top Level:** `applications/main/nfc/` - UI and application logic.
- **Middleware:** `lib/nfc/` - Protocol stacks (ISO14443, FeliCa, etc.) and device logic.
- **Backend Wrapper:** `targets/f7/furi_hal/furi_hal_nfc_pn532.c` - Maps generic Furi NFC HAL calls to PN532-specific commands.
- **Hardware Driver:** `targets/f7/furi_hal/furi_hal_pn532.c` - Low-level PN532 I2C framing and register management.
- **I/O Layer:** `targets/f7/furi_hal/furi_hal_i2c.c` - STM32WB55 I2C peripheral management.
