# PN532 ISO15693 (NFC-V) Feasibility Research

## Summary

**Conclusion: ISO15693 is NOT feasible on the PN532.** The PN532's analog frontend and digital baseband are hardwired for ISO14443A/B and FeliCa modulation schemes only. ISO15693 uses fundamentally different physical layer characteristics that the PN532 cannot generate or decode.

## Technical Analysis

### Physical Layer Comparison

| Parameter | ISO14443A (PN532 native) | ISO15693 (NFC-V) |
|-----------|--------------------------|-------------------|
| Carrier frequency | 13.56 MHz | 13.56 MHz |
| Subcarrier | 847 kHz (Manchester) | 423 kHz (ASK) or 423/484 kHz (FSK) |
| Data rate (reader→tag) | 106 kbps | 26.48 kbps |
| Data rate (tag→reader) | 106 kbps | 26.48 or 6.62 kbps |
| Modulation (reader→tag) | 100% ASK (Miller) | 10% or 100% ASK (PPM) |
| Modulation (tag→reader) | Subcarrier load modulation | Subcarrier load modulation |
| Frame format | Short frame, standard frame | SOF, EOF, custom framing |

### Why InCommunicateThru Won't Work

The PN532's `InCommunicateThru` command (0x42) transmits raw bytes using the **currently configured modulation scheme**. After `InListPassiveTarget` with ISO14443A baud rate (0x00), the PN532 is configured for:
- 106 kbps Miller modulation
- 100% ASK
- 847 kHz subcarrier

InCommunicateThru has **no parameter to change the modulation rate or scheme**. All data is transmitted at the modulation configured for the currently selected target. There is no `RFCONFIGURATION` register that changes this — the PN532 datasheet makes no mention of ISO15693 or 26 kbps modulation.

### PN532 Command Set Analysis

Commands checked from PN532 User Manual v1.6:

| Command | Supports ISO15693? | Notes |
|---------|-------------------|-------|
| InListPassiveTarget (0x4A) | No | Baud rate options: 106k (A), 212k (F), 424k (F), 848k. No 26k option |
| InDataExchange (0x40) | No | Works only with inlisted target; inherits modulation from detection |
| InCommunicateThru (0x42) | No | Uses current RF configuration; cannot change modulation |
| RFConfiguration (0x32) | No | Configures retries, field strength, timers. No modulation control |
| InAutoPoll (0x60) | No | Only polls ISO14443A, ISO14443B, FeliCa |
| TgInitAsTarget (0x8C) | No | Only ISO14443A or FeliCa target mode |

### Verification Test Procedure

If hardware verification is desired:

1. Hold an ISO15693 card (e.g., TI Tag-it HF-I, NXP ICODE SLI) near PN532
2. Run `InCommunicateThru` with ISO15693 INVENTORY command `[0x26 0x01 0x00]`
3. Check response: expected behavior is timeout/no response

Source: `srix_tool.cpp` in Bruce firmware shows SRIX works via InCommunicateThru — but SRIX uses ISO14443A-compatible 106 kbps modulation, NOT ISO15693. The SRIX success is due to it being a 3A-variant protocol, not ISO15693.

### Alternative Solutions

1. **Separate ISO15693 reader module**: Add an external ST25R95 or TRF7960 reader connected via SPI/UART
2. **ST25R3916 upgrade path**: Replace PN532 with ST25R3916 (same as original Flipper Zero)
3. **Chameleon Ultra**: Use Chameleon Ultra hardware (already partially supported in app)

### References

- PN532 User Manual Rev. 1.6 — Section 7.3.1 (InCommunicateThru)
- PN532 Application Note AN133910 — RF communication parameters
- ISO/IEC 15693-2: Air interface and initialization
- ST25R3916 datasheet (comparison reference)
