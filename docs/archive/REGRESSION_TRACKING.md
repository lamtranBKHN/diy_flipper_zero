# Regression & Bug Tracking Log
## DIY Flipper Zero Firmware — Post-Flash Testing

**Firmware Version**: v1.0-stable (2026-05-04)  
**Target Hardware**: STM32WB55CGU6 (TARGET_HW=7)  
**Build Command**: `./fbt TARGET_HW=7 DEBUG=1 COMPACT=1`  

---

## How to Use This Document

1. **When a bug is found during testing**, create a new entry below
2. **Include all relevant details**: steps to reproduce, expected vs actual behavior, severity
3. **Assign a priority**: P0 (Critical), P1 (High), P2 (Medium), P3 (Low)
4. **Track status**: OPEN → INVESTIGATING → FIXED → VERIFIED
5. **Link to original bug ID** if this is a regression of a previously fixed issue

---

## Bug Reports

### REG-001: [Title]
- **Date Found**: YYYY-MM-DD
- **Found By**: [Tester Name]
- **Severity**: P0|P1|P2|P3
- **Status**: OPEN
- **Related Bug ID**: BUG-XXX (if regression)
- **Component**: [NFC|Sub-GHz|I2C|USB|Power|UI|Other]
- **Description**: [Brief description of the issue]
- **Steps to Reproduce**:
  1. Step 1
  2. Step 2
  3. Step 3
- **Expected Behavior**: [What should happen]
- **Actual Behavior**: [What actually happens]
- **Workaround**: [If any]
- **Fix Applied**: [Description of fix, PR link, commit hash]
- **Verified By**: [Name] | **Date**: YYYY-MM-DD

---

### REG-002: [Title]
- **Date Found**: 
- **Found By**: 
- **Severity**: 
- **Status**: OPEN
- **Component**: 
- **Description**: 
- **Steps to Reproduce**:
  1. 
- **Expected Behavior**: 
- **Actual Behavior**: 
- **Workaround**: 
- **Fix Applied**: 
- **Verified By**:  | **Date**: 

---

## Summary Statistics

| Status | Count |
|--------|-------|
| OPEN | 0 |
| INVESTIGATING | 0 |
| FIXED | 0 |
| VERIFIED | 0 |
| WONTFIX | 0 |

| Severity | Count |
|----------|-------|
| P0 (Critical) | 0 |
| P1 (High) | 0 |
| P2 (Medium) | 0 |
| P3 (Low) | 0 |

---

## Severity Definitions

| Level | Description | Response Time |
|-------|-------------|---------------|
| **P0 - Critical** | System crash, hard fault, data loss, bricking risk | Immediate fix required |
| **P1 - High** | Feature broken, workaround unavailable, frequent occurrence | Fix within 24 hours |
| **P2 - Medium** | Feature degraded, workaround available, occasional occurrence | Fix within 1 week |
| **P3 - Low** | Cosmetic issue, minor bug, rare occurrence | Fix when convenient |

---

## Regression Analysis

### Bugs Fixed That Could Regress

| Bug ID | Description | Fix Applied | Regression Risk | Notes |
|--------|-------------|-------------|-----------------|-------|
| BUG-001 | Power shutdown freeze | NVIC_SystemReset() | Low | Core system change |
| BUG-004 | NFC timer ISR hard fault | pin reference fix | Low | Simple pointer fix |
| BUG-014 | PN532 RX buffer overflow | Cap at 255 bytes | Medium | Hardware limit change |
| BUG-023 | Sub-GHz DMA ISR freeze | MAX_ITERATIONS=1024 | Low | Failsafe addition |
| BUG-026 | NFC listener deadlock | Removed acquire/release | Medium | State machine change |
| BUG-027 | NFC timer spin-loop | Added timeout | Low | Timeout addition |
| BUG-029 | NFC scanner memcpy | Fixed size calc | Low | Simple math fix |
| BUG-030 | NFC scanner cooperative stop | Added state checks | Medium | Loop structure change |
| BUG-040 | GPIO SFP strcpy overflow | snprintf() | Low | Bounds check addition |
| BUG-041 | NFC poller race condition | Added 5ms delay | Low | Timing change |
| BUG-042 | CCID deinit memory leak | Fixed free target | Low | Memory management |
| BUG-043 | Flipper Lab rejection | Model name fix | None | String change only |

---

## Notes & Observations

### Hardware-Specific Issues
- [Document any issues specific to the DIY board hardware]

### Environmental Factors
- [Document any issues related to temperature, power supply, EMI, etc.]

### Firmware Version History
| Version | Date | Changes | Stable? |
|---------|------|---------|---------|
| v1.0-stable | 2026-05-04 | 11 bug fixes applied | Testing |
