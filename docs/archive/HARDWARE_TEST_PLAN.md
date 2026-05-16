# Hardware Verification Test Plan
## DIY Flipper Zero (Nucleus Dark MK1) — Firmware v1.0-stable

**Target Hardware**: STM32WB55CGU6 DIY Board (TARGET_HW=7)  
**NFC Module**: PN532 via I2C  
**Firmware Build**: `./fbt TARGET_HW=7 DEBUG=1 COMPACT=1`  
**Build Date**: 2026-05-04  
**Test Date**: _______________  
**Tester**: _______________  

---

## Pre-Flash Checklist

- [ ] Firmware compiled successfully (`dist/f7-DC/firmware.bin` exists)
- [ ] ST-Link or USB cable ready
- [ ] Device battery charged (>50% recommended)
- [ ] PN532 module connected via I2C (SDA/SCL/VCC/GND)
- [ ] PCF8574 I/O expander connected (for buttons)
- [ ] Display (SSD1306) connected via SPI
- [ ] CC1101 Sub-GHz module connected (if testing Sub-GHz)
- [ ] SD card inserted (optional, for file system tests)

---

## Test 1: Boot & Basic Functionality
**Purpose**: Verify the device boots and basic UI works  
**Related Fix**: None (baseline test)

### Steps
1. [ ] Power on the device (hold power button for 2 seconds)
2. [ ] Observe boot animation/splash screen
3. [ ] Wait for main menu to appear
4. [ ] Navigate through menu items using directional buttons
5. [ ] Verify screen updates without freezing or tearing

### Expected Results
- ✅ Device boots within 5 seconds
- ✅ Main menu displays correctly
- ✅ Navigation is responsive (no lag >500ms)
- ✅ No screen corruption or artifacts

### Pass/Fail
- **Result**: [ ] PASS  [ ] FAIL
- **Notes**: _________________________________________________

---

## Test 2: Flipper Lab / Web Interface Connection
**Purpose**: Verify device is recognized by Flipper Lab and mobile apps  
**Related Fix**: BUG-043 — `furi_hal_version_get_model_name()` returns `"Flipper Zero"`

### Steps
1. [ ] Connect device to PC via USB-C cable
2. [ ] Open browser and navigate to `lab.flipper.net`
3. [ ] Click "Connect" in Flipper Lab
4. [ ] Verify device appears in the device list as "Flipper Zero"
5. [ ] Connect to device and check firmware info
6. [ ] (Optional) Test with Flipper mobile app on smartphone

### Expected Results
- ✅ Device is detected by browser (WebUSB prompt appears)
- ✅ Device name shows as "Flipper Zero" (NOT "By LamTran")
- ✅ Firmware version and hardware info displayed correctly
- ✅ Can read/write files via Flipper Lab
- ✅ (Optional) Mobile app connects successfully

### Pass/Fail
- **Result**: [ ] PASS  [ ] FAIL
- **Notes**: _________________________________________________
- **Device Name Shown**: _____________________________________

---

## Test 3: Power Management & Shutdown
**Purpose**: Verify device shuts down without freezing  
**Related Fix**: BUG-001 — Replaced `while(1) {}` with `NVIC_SystemReset()`

### Steps
1. [ ] From main menu, navigate to Settings → Power
2. [ ] Select "Shut Down" or hold power button for 3 seconds
3. [ ] Observe shutdown sequence
4. [ ] Verify device powers off completely (screen goes black, LEDs off)
5. [ ] Wait 5 seconds, then power on again
6. [ ] Verify device boots normally after shutdown

### Expected Results
- ✅ Shutdown completes within 3 seconds
- ✅ No frozen screen during shutdown
- ✅ Device powers off completely (no partial power state)
- ✅ Device boots normally after shutdown
- ✅ No corrupted file system or settings loss

### Pass/Fail
- **Result**: [ ] PASS  [ ] FAIL
- **Notes**: _________________________________________________

---

## Test 4: NFC Scanner Mode Stability
**Purpose**: Verify NFC scanner mode doesn't crash or freeze  
**Related Fix**: BUG-026, BUG-027 — Removed deadlocks, added timeouts

### Steps
1. [ ] From main menu, open NFC app
2. [ ] Select "Read" or "Scan" mode
3. [ ] Hold NFC tag (Mifare Classic, ISO14443A) near PN532 antenna
4. [ ] Observe if tag is detected and read
5. [ ] Exit NFC app and return to main menu
6. [ ] Re-enter NFC app and repeat scan 3 times
7. [ ] Leave NFC scanner running for 60 seconds without a tag
8. [ ] Verify no freeze or watchdog reset occurs

### Expected Results
- ✅ NFC app opens without crash
- ✅ Tag detection works (shows tag type and UID)
- ✅ Can exit NFC app cleanly (no freeze on exit)
- ✅ Re-entering NFC app works multiple times
- ✅ Scanner idles for 60s without freezing
- ✅ No hard fault or reboot during testing

### Pass/Fail
- **Result**: [ ] PASS  [ ] FAIL
- **Notes**: _________________________________________________
- **Tag Types Tested**: ______________________________________

---

## Test 5: I2C Bus Stability (PCF8574 + PN532 Coexistence)
**Purpose**: Verify I2C buttons work while NFC is active  
**Related Fix**: BUG-014 — Capped PN532 RX frame to 255 bytes

### Steps
1. [ ] Power on device and verify PCF8574 buttons respond
2. [ ] Press each button connected to PCF8574 (verify in GPIO test)
3. [ ] Open NFC app and start scanning
4. [ ] While NFC is scanning, press PCF8574 buttons
5. [ ] Verify buttons still respond during NFC operation
6. [ ] Hold an NFC tag near antenna while pressing buttons
7. [ ] Exit NFC app and verify button response returns to normal

### Expected Results
- ✅ All PCF8574 buttons respond correctly at idle
- ✅ Buttons remain responsive during NFC scanning
- ✅ No I2C bus errors or timeouts
- ✅ NFC tag detection works simultaneously with button presses
- ✅ No system crash or freeze during combined I2C activity

### Pass/Fail
- **Result**: [ ] PASS  [ ] FAIL
- **Notes**: _________________________________________________
- **Number of PCF8574 Inputs Tested**: _________________________

---

## Test 6: Sub-GHz TX/RX Stability
**Purpose**: Verify Sub-GHz DMA TX completes without ISR freeze  
**Related Fix**: BUG-023 — Added failsafe counter (MAX_ITERATIONS=1024)

### Prerequisites
- CC1101 module must be connected and functional
- (Optional) Second Sub-GHz device or SDR for RX verification

### Steps
1. [ ] From main menu, open Sub-GHz app
2. [ ] Select "Transmit" → "Custom" or "RAW"
3. [ ] Load a test signal file (or create a simple one)
4. [ ] Start transmission
5. [ ] Observe transmission completes without freezing
6. [ ] Verify device returns to menu after TX completes
7. [ ] Repeat TX 5 times in succession
8. [ ] (Optional) Verify signal is actually transmitted using SDR or receiver

### Expected Results
- ✅ Sub-GHz app opens without crash
- ✅ TX starts and completes within expected time
- ✅ No freeze during or after TX
- ✅ Device returns to menu after each TX
- ✅ 5 consecutive TXs complete without failure
- ✅ (Optional) Signal detected on receiver/SDR

### Pass/Fail
- **Result**: [ ] PASS  [ ] FAIL
- **Notes**: _________________________________________________
- **Frequency Tested**: ______________________________________

---

## Test 7: GPIO SFP (EEPROM) Viewer with Long Strings
**Purpose**: Verify GPIO EEPROM viewer handles long strings without crash  
**Related Fix**: BUG-040 — Replaced 5× `strcpy()` with `snprintf()`

### Steps
1. [ ] Connect I2C EEPROM (24Cxx series) to I2C bus
2. [ ] From main menu, open GPIO app
3. [ ] Select "EEPROM Reader" or "SFP Viewer"
4. [ ] Start scan/read of EEPROM
5. [ ] If EEPROM contains strings longer than 64 characters, verify they display correctly
6. [ ] Verify no crash or corruption when viewing long strings
7. [ ] Exit app and verify device is stable

### Expected Results
- ✅ EEPROM reader opens without crash
- ✅ Long strings (>64 chars) display correctly (truncated if needed)
- ✅ No heap corruption or crash
- ✅ Can exit app cleanly
- ✅ Device remains stable after test

### Pass/Fail
- **Result**: [ ] PASS  [ ] FAIL
- **Notes**: _________________________________________________
- **EEPROM Model**: __________________________________________

---

## Test 8: NFC Protocol Detection Race Condition
**Purpose**: Verify NFC poller detect doesn't crash on rapid state changes  
**Related Fix**: BUG-041 — Added 5ms delay between nfc_start() and nfc_stop()

### Steps
1. [ ] Open NFC app
2. [ ] Start "Read" mode (auto-detect protocol)
3. [ ] Hold Mifare Classic tag near antenna
4. [ ] Immediately remove tag after detection starts
5. [ ] Repeat step 3-4 rapidly 10 times
6. [ ] Verify no crash or freeze during rapid tag insertions/removals
7. [ ] Leave tag on antenna for 10 seconds, then remove
8. [ ] Verify app handles removal gracefully

### Expected Results
- ✅ Rapid tag insertions/removals don't crash the app
- ✅ Protocol detection works correctly when tag is present
- ✅ App recovers gracefully when tag is removed mid-detection
- ✅ No memory leak or resource exhaustion after 10 cycles
- ✅ Device remains stable throughout test

### Pass/Fail
- **Result**: [ ] PASS  [ ] FAIL
- **Notes**: _________________________________________________

---

## Test 9: Long-Running Stability (30-Minute Burn-In)
**Purpose**: Verify no memory leaks or degradation over extended use  
**Related Fix**: All fixes (comprehensive stability check)

### Steps
1. [ ] Power on device and note battery level
2. [ ] Run through NFC scan → Sub-GHz TX → GPIO test cycle 5 times
3. [ ] Leave device idle on main menu for 10 minutes
4. [ ] Run NFC scan continuously for 5 minutes
5. [ ] Run Sub-GHz RX (listen mode) for 5 minutes
6. [ ] Navigate through all menu items
7. [ ] Check battery level and compare to start

### Expected Results
- ✅ No crashes during 30-minute test
- ✅ No screen corruption or display issues
- ✅ All functions remain responsive throughout
- ✅ No unexpected reboots or watchdog resets
- ✅ Battery drain is reasonable (<5% for 30 min idle)

### Pass/Fail
- **Result**: [ ] PASS  [ ] FAIL
- **Notes**: _________________________________________________
- **Start Battery**: ______%  **End Battery**: ______%

---

## Test 10: USB CDC/VCOM Serial Communication
**Purpose**: Verify serial console works over USB  
**Related Fix**: BUG-042 — Fixed CCID deinit memory leak (indirectly affects USB)

### Steps
1. [ ] Connect device to PC via USB-C
2. [ ] Open serial terminal (PuTTY, screen, minicom)
3. [ ] Select correct COM port (check Device Manager on Windows)
4. [ ] Set baud rate to 115200, 8N1, no flow control
5. [ ] Verify CLI prompt appears
6. [ ] Type `help` and verify command list is displayed
7. [ ] Type `device_info` and verify hardware info
8. [ ] Disconnect and reconnect USB
9. [ ] Verify serial console recovers automatically

### Expected Results
- ✅ Serial console connects successfully
- ✅ CLI commands respond correctly
- ✅ Device info displays correct hardware (STM32WB55, PN532)
- ✅ Console recovers after USB disconnect/reconnect
- ✅ No garbled output or missed characters

### Pass/Fail
- **Result**: [ ] PASS  [ ] FAIL
- **Notes**: _________________________________________________
- **COM Port Used**: _________________________________________

---

## Overall Summary

| Test | Description | Result | Notes |
|------|-------------|--------|-------|
| 1 | Boot & Basic Functionality | [ ] | |
| 2 | Flipper Lab / Web Interface | [ ] | |
| 3 | Power Management & Shutdown | [ ] | |
| 4 | NFC Scanner Stability | [ ] | |
| 5 | I2C Bus Stability | [ ] | |
| 6 | Sub-GHz TX/RX Stability | [ ] | |
| 7 | GPIO SFP Long Strings | [ ] | |
| 8 | NFC Protocol Race Condition | [ ] | |
| 9 | 30-Minute Burn-In | [ ] | |
| 10 | USB CDC Serial | [ ] | |

### Final Assessment
- **Tests Passed**: ____ / 10
- **Tests Failed**: ____ / 10
- **Tests Skipped**: ____ / 10

### Overall Result
- [ ] **Firmware is stable and ready for production**
- [ ] **Minor issues found — see notes above**
- [ ] **Critical issues found — firmware needs fixes**

### Critical Issues (if any)
1. _________________________________________________
2. _________________________________________________
3. _________________________________________________

### Recommendations
_________________________________________________________
_________________________________________________________
_________________________________________________________

---

## Sign-Off

**Tested By**: ________________________  **Date**: _______________  
**Reviewed By**: ______________________  **Date**: _______________  
**Approval**: [ ] Approved  [ ] Needs Revision  [ ] Rejected
