# Firmware Implementation Plan

**Project:** DIY Flipper Zero (copilot-worktree)  
**Date:** 2026-05-10  
**Status:** Based on comprehensive cross-firmware analysis

---

## 1. Executive Summary

The current DIY project has **unique hardware adaptations** (PN532 NFC, PCF8574 buttons) that are **not present in any reference firmware**. This creates both opportunities and risks.

### 1.1 Key Findings

| Finding | Severity | Action Required |
|---------|----------|------------------|
| No f18 DIY target defined | HIGH | Create `targets/f18/` matching hardware config |
| PN532 is unique to this project | HIGH | Document behavior differences vs ST25R3916 |
| PCF8574 is unique to this project | MEDIUM | Ensure stability, add tests |
| Sub-GHz has extended features | MEDIUM | Validate against official baseline |
| FuckingCheap = Momentum (no unique value) | LOW | Deprecate reference or consolidate |

### 1.2 Recommended Priority Order

```
1. Create proper f18 target (CRITICAL)
2. Add PN532 vs ST25R3916 behavior tests (CRITICAL)
3. Stabilize PCF8574 INT handling (HIGH)
4. Align Sub-GHz with Momentum baseline (MEDIUM)
5. Document all hardware quirks (MEDIUM)
6. Create regression test suite (MEDIUM)
```

---

## 2. Critical Issues

### 2.1 Issue: No f18 DIY Target

**Problem:** Current project uses `TARGET_HW=7` with modifications, but Momentum/Official have a proper `TARGET_HW=18` for DIY hardware.

**Current State:**
- `TARGET_HW=7` in `fbt_options.py`
- No `targets/f18/` directory
- PN532 enabled via `custom_pn532_board.mk` flag

**Recommended Solution:**

```bash
# Create targets/f18/ based on Momentum f18 structure
# but with DIY-specific hardware (PN532, PCF8574)
```

**Action Items:**
- [ ] Create `targets/f18/target.json` with exclusion list for radio modules NOT on DIY board
- [ ] Create `targets/f18/furi_hal/furi_hal_resources.h` with DIY pin mappings
- [ ] Create `targets/f18/furi_hal/furi_hal_spi_config.h` for SPI1
- [ ] Copy `targets/f7/api_symbols.csv` to `targets/f18/api_symbols.csv` and adjust
- [ ] Update `fbt_options.py` to allow `TARGET_HW=18`
- [ ] Test build with `TARGET_HW=18`

**Files to Create:**
```
targets/f18/
├── target.json                    # Hardware exclusion list
├── api_symbols.csv               # API symbols for f18
├── furi_hal/
│   ├── furi_hal_resources.c
│   ├── furi_hal_resources.h
│   ├── furi_hal_spi_config.c
│   └── furi_hal_spi_config.h
├── ble_glue/                      # May be empty for f18
├── fatfs/
├── inc/
├── platform_specific/
├── src/
│   ├── main.c
│   ├── stm32wb55_startup.c
│   ├── dfu.c
│   └── recovery.c
├── stm32wb55xx_flash.ld
├── stm32wb55xx_ram_fw.ld
└── application_ext.ld
```

### 2.2 Issue: NFC Behavior Differences (PN532 vs ST25R3916)

**Problem:** PN532 is fundamentally different from ST25R3916:
- Polling vs interrupt-driven
- I2C vs SPI
- Different protocol implementations

**Risk Assessment:**

| Aspect | PN532 | ST25R3916 | Risk |
|--------|-------|-----------|------|
| Interface | I2C | SPI | Different timing |
| IRQ | No (polling) | Yes (interrupt) | Slower response |
| FIFO | 64 bytes | Larger | Different throughput |
| Protocol | Firmware-based | Hardware-based | Different behavior |

**Recommended Solution:**

1. **Document all known differences** in `NFC_IMPLEMENTATION_PLAN_V2.md`
2. **Add behavior tests** comparing PN532 to ST25R3916
3. **Create fallback handling** for unsupported protocols

**Critical Tests Required:**

| Test | Pass Criteria |
|------|---------------|
| ISO14443-3A read | Same UID read as official |
| ISO14443-4A read | APDU exchange works |
| ISO15693 read | Same as official |
| EMV read | Card detected, select working |
| Read range | Within 80% of official |

### 2.3 Issue: PCF8574 INT Stability

**Problem:** PCF8574 INT handler may be unreliable, with polling fallback.

**Current Implementation (input.c:170-199):**
```c
// Pseudocode
furi_hal_pcf8574_init();
if (furi_hal_pcf8574_attach_int(callback) != FURI_OK) {
    // Fall back to polling
    furi_timer_start(polling_timer, 50ms);
}
```

**Recommended Solution:**

1. **Add hardware debounce** if not present
2. **Log INT failures** for diagnostics
3. **Measure polling overhead** and document impact on power consumption

**Action Items:**
- [ ] Verify INT pin (PB0/EXTI0) is correctly configured
- [ ] Add oscilloscope measurement of INT signal
- [ ] Test button response time in both INT and polling modes
- [ ] Document worst-case polling latency

---

## 3. Medium Priority Items

### 3.1 Sub-GHz Alignment with Momentum

**Problem:** DIY Sub-GHz has +23% larger code with extended features. Need to verify changes don't break compatibility.

**DIY Extensions vs Official:**

| Feature | DIY | Official | Test Required |
|---------|-----|----------|----------------|
| Extended freq (281-361 MHz) | ✓ | ✗ | Validate regulation compliance |
| Extended freq (749-962 MHz) | ✓ | ✗ | Validate regulation compliance |
| Async RX hopping | ✓ | ✗ | Test frequency switch stability |
| Rolling counter | ✓ | ✗ | Verify protocol correctness |
| GDO0 pull-down | ✓ | ✗ | Verify no false interrupts |
| No RF switch | ✓ | ✗ | Test antenna path (may be lossy) |

**Recommended Actions:**
- [ ] Compare `furi_hal_subghz.c` with Momentum to identify drift
- [ ] Test all extended-frequency protocols for correct output
- [ ] Benchmark async RX hopping switching time
- [ ] Verify extended protocols (KeeLoq variants, Somfy, etc.)

### 3.2 Asset Pack Compatibility

**Problem:** DIY inherits from Momentum which has extensive theming. Need to verify all assets work.

**Current Status:**
- Asset packs in `assets/` (dolphin animations, icons)
- Custom themes may reference hardware that doesn't exist (e.g., RGB LED)

**Recommended Actions:**
- [ ] Verify RGB backlight integration (lib/rgb_backlight.h exists)
- [ ] Test custom animations load correctly
- [ ] Document hardware limitations vs visual features

### 3.3 Documentation Gaps

**Current Documentation State:**

| Document | Status | Action |
|----------|--------|--------|
| `CLAUDE.md` | ✓ | OK |
| `AGENTS.md` | ✓ | Has PN532/PCF8574 details |
| `FW_ARCHITECTURE.md` | ✓ | 1310 lines, comprehensive |
| `NFC_IMPLEMENTATION_PLAN_V2.md` | ✓ | Has PN532 roadmap |
| `HARDWARE_TEST_PLAN.md` | ✓ | Needs update |

**Recommended Actions:**
- [ ] Add PN532-specific test cases to `HARDWARE_TEST_PLAN.md`
- [ ] Add PCF8574 test cases
- [ ] Create `HARDWARE_ QuirKS.md` documenting SPI contention, INT fallback

---

## 4. Low Priority Items

### 4.1 FuckingCheap Reference Cleanup

**Problem:** `FuckingCheapFlipperZero-DIY-Flipper-zero-The-real-on` is identical to Momentum firmware. Using it as a reference provides no unique value.

**Recommended:** Remove from reference list or mark as "Momentum = FuckingCheap"

### 4.2 API Symbol Cleanup

**DIY Unique Symbols:**
```
lib/momentum/momentum.h
lib/rgb_backlight.h
applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h
applications/main/archive/helpers/archive_helpers_ext.h
applications/main/subghz/subghz_fap.h
```

**Recommended:** Verify these are intentionally different from Momentum baseline.

### 4.3 SPI Contention Documentation

**Known Issue (AGENTS.md):**
```
ViewPort lockup warnings at view_port.c:208 are expected on DIY board
due to SPI1 bus sharing (display + CC1101 + SD)
```

**Recommended Actions:**
- [ ] Add debugging hooks to measure SPI hold times
- [ ] Document recommended transaction sizes
- [ ] Create example showing proper SPI release timing

---

## 5. Regression Tracking

### 5.1 Known Regressions vs Official

| Issue | Severity | Workaround | Fix Priority |
|-------|----------|------------|--------------|
| ViewPort lockup on SPI contention | Medium | Avoid long SPI transactions in draw callback | MEDIUM |
| PN532 polling overhead | Low | Accept reduced NFC response speed | LOW |
| PCF8574 polling fallback | Low | Monitor INT stability | LOW |
| No RF switch (Sub-GHz) | Low | Accept reduced range | IF NEEDED |

### 5.2 Features Missing vs Momentum

| Feature | DIY | Momentum | Gap |
|---------|-----|----------|-----|
| f18 target | ✗ | ✓ | Create f18 |
| ST25R3916 NFC | ✗ | ✓ | Not needed (PN532 is alternative) |
| Full Sub-GHz | ~same | ✓ | Aligned |
| Asset packs | ✓ | ✓ | OK |

---

## 6. Testing Plan

### 6.1 NFC Testing

```python
# Test matrix for PN532
test_cases = [
    ("ISO14443-3A", "Mifare Ultralight", "Read UID"),
    ("ISO14443-3A", "Mifare Classic 1K", "Read/Write"),
    ("ISO14443-3A", "Mifare Desfire", "Select App"),
    ("ISO14443-4A", "ISO14443-4 card", "APDU exchange"),
    ("ISO14443-3B", "ISO14443-3B card", "Read UID"),
    ("ISO15693", "NFC-V tag", "Read blocks"),
    ("FeliCa", "FeliCa card", "Read without encryption"),
    ("EMV", "Contactless Visa", "Select PSE"),  # DIY extension
]

# Compare with official firmware
# Pass: Same UID, same read time (±20%)
# Fail: Different UID, timeout, corruption
```

### 6.2 Button Testing

```python
test_cases = [
    ("Up", "Press and release"),
    ("Down", "Press and release"),
    ("Left", "Press and release"),
    ("Right", "Press and release"),
    ("OK", "Press and release"),
    ("Back", "Press and release"),
    ("Long press", "Hold OK for 3 seconds"),
    ("Multi-key", "Hold Back + Down"),
]

# Verify:
# - Correct key mapped
# - Debounce works (no double-trigger)
# - Response time < 50ms
```

### 6.3 Sub-GHz Testing

```python
test_cases = [
    ("Standard range", "433.92 MHz OOK", "Receive known signal"),
    ("Extended low", "300 MHz OOK", "Receive if hardware supports"),
    ("Extended high", "915 MHz OOK", "Receive if hardware supports"),
    ("Async hopping", "Fast frequency switch", "Verify no dropped packets"),
    ("Protocol decode", "Keeloq", "Verify correct decoding"),
]
```

---

## 7. Risk Matrix

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| PN532 instability | LOW | HIGH | Extensive testing, fallback to polling |
| PCF8574 INT failure | MEDIUM | MEDIUM | Polling fallback, monitoring |
| SPI contention lockup | MEDIUM | MEDIUM | Shorter transactions, release SPI quickly |
| Extended freq violation | LOW | HIGH | Document regulatory compliance responsibility |
| API drift from Momentum | MEDIUM | LOW | Periodic sync with upstream |

---

## 8. Implementation Roadmap

### Phase 1: Critical (Week 1)

```
□ Create targets/f18/ directory structure
□ Create targets/f18/target.json
□ Create targets/f18/furi_hal/furi_hal_resources.h
□ Update fbt_options.py for TARGET_HW=18
□ Test build with TARGET_HW=18
□ Verify PN532 initialization on boot
□ Verify PCF8574 button reading on boot
```

### Phase 2: Testing (Week 2)

```
□ Create NFC test suite (ISO14443, ISO15693, FeliCa)
□ Run NFC tests, compare with official
□ Create button test suite
□ Run button tests, measure latency
□ Test Sub-GHz standard protocols
□ Test Sub-GHz extended protocols
```

### Phase 3: Stabilization (Week 3)

```
□ Fix any NFC issues found in testing
□ Fix any button issues found in testing
□ Optimize SPI transactions
□ Add INT stability monitoring
□ Document all quirks in HARDWARE_QUIRKS.md
```

### Phase 4: Optimization (Week 4)

```
□ Profile power consumption
□ Optimize polling intervals
□ Benchmark NFC read times
□ Test extended temperature range
□ Final regression testing
```

---

## 9. File Change Summary

### 9.1 New Files Required

```
targets/f18/target.json                           # Hardware exclusion list
targets/f18/api_symbols.csv                        # API symbols
targets/f18/furi_hal/furi_hal_resources.c         # DIY pin definitions
targets/f18/furi_hal/furi_hal_resources.h         # DIY pin macros
targets/f18/furi_hal/furi_hal_spi_config.c         # SPI config
targets/f18/furi_hal/furi_hal_spi_config.h         # SPI config
targets/f18/ble_glue/                              # Empty or minimal
targets/f18/fatfs/sd_glue.c                        # SD card config
targets/f18/src/main.c                             # Entry point
targets/f18/src/stm32wb55_startup.c                # Startup
targets/f18/inc/stm32wb55xx.h                     # Device headers
HARDWARE_QUIRKS.md                                 # Document unique issues
```

### 9.2 Files to Modify

```
fbt_options.py                                    # Add TARGET_HW=18 support
site_scons/commandline.scons                       # Add f18 to enum (if needed)
HARDWARE_TEST_PLAN.md                              # Add PN532/PCF8574 tests
```

### 9.3 Files to Verify

```
targets/f7/furi_hal/furi_hal_pn532.c              # Still compiles
targets/f7/furi_hal/furi_hal_nfc_pn532.c           # Still compiles
targets/f7/furi_hal/furi_hal_pcf8574.c            # Still compiles
applications/services/input/input.c                # PCF8574 init OK
```

---

## 10. Success Criteria

### 10.1 Build Success

| Criterion | Target | Measurement |
|-----------|--------|-------------|
| TARGET_HW=7 build | ✓ | `./fbt` completes |
| TARGET_HW=18 build | ✓ | `./fbt TARGET_HW=18` completes |
| FAP build | ✓ | `./fbt fap_dist` completes |

### 10.2 Functional Success

| Criterion | Target | Measurement |
|-----------|--------|-------------|
| NFC read | < 500ms | Time to read Mifare UID |
| Button response | < 50ms | Key press to event |
| Sub-GHz RX | Same as official | Known signal decode |
| Boot time | < 10s | Cold start to menu |

### 10.3 Stability Success

| Criterion | Target | Measurement |
|-----------|--------|-------------|
| NFC read success | > 95% | 100 reads, same card |
| Button detection | > 99% | 1000 presses |
| No SPI lockup | 0 | 1 hour stress test |

---

## 11. References

| Document | Path | Notes |
|----------|------|-------|
| Architecture | `FIRMWARE_COMPARISON_ARCHITECTURE.md` | This analysis |
| Delta Report | `FIRMWARE_DELTA_REPORT.md` | File-by-file changes |
| Current HAL | `targets/f7/furi_hal/` | Live implementation |
| Momentum f18 | `F:\FB_V3\Momentum-Firmware\targets\f18\` | Reference target |
| Official f7 | `F:\FB_V3\flipperzero-firmware\targets\f7\` | Baseline |

---

*End of Implementation Plan*