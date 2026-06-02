# NFC Protocol Feature Completeness Matrix

_Last updated: 2026-05-27_

This document catalogs all 17 NFC protocols supported in the firmware and their feature completeness across the four main user-facing capabilities: Info display, MoreInfo (raw/diagnostic dump), Emulate (card emulation), and Write (data write/modify).

## Legend

| Symbol | Meaning |
|--------|---------|
| ✅ | Implemented (non-empty scene handler) |
| ❌ | Not implemented (empty stub or missing) |
| — | Not applicable / hardware-limited |
| via X | Delegates to protocol X's implementation |

## Feature Matrix

| # | Protocol | Info | MoreInfo | Emulate | Write | Features Flag |
|---|---|---|---|---|---|---|
| 1 | **ISO14443-3A** (NFC-A) | ✅ | ✅ | ✅ UID | ❌ | `EmulateUid \| EditUid \| MoreInfo` |
| 2 | **ISO14443-3B** (NFC-B) | ✅ | ✅ | ❌ | ❌ | `MoreInfo` |
| 3 | **ISO14443-4A** (ISO-DEP) | ✅ | ✅ | ✅ UID | ❌ | `EmulateUid \| EditUid \| MoreInfo` |
| 4 | **ISO14443-4B** | ✅ | ✅ | ❌ | ❌ | `MoreInfo` |
| 5 | **ISO15693-3** (NFC-V) | ✅ | ✅ | ✅ Full | ❌ | `EmulateFull \| EditUid \| MoreInfo` |
| 6 | **FeliCa** | ✅ | ✅ | ✅ Full | ❌ | `EmulateFull \| MoreInfo` |
| 7 | **MIFARE Ultralight** | ✅ | ✅ | ✅ Full | ✅ | `EmulateFull \| MoreInfo \| Write` |
| 8 | **MIFARE Classic** | ✅ | ✅ | ✅ Full | ✅ | `EmulateFull \| MoreInfo \| Write` |
| 9 | **MIFARE Plus** | ✅ | ✅ | ✅ UID (via 4A) | ❌ | `EmulateUid \| MoreInfo` |
| 10 | **MIFARE DESFire** | ✅ | ✅ | ✅ UID (via 4A) | ❌ | `EmulateUid \| MoreInfo` |
| 11 | **ST25TB** | ✅ | ✅ | ❌ | ❌ | `MoreInfo` |
| 12 | **NTAG 4xx** | ✅ | ✅ | ✅ UID (via 4A) | ❌ | `EmulateUid \| MoreInfo` |
| 13 | **Type 4 Tag** | ✅ | ✅ | ✅ Full | ✅ | `EmulateFull \| MoreInfo \| Write` |
| 14 | **Slix** | ✅ | ✅ | ✅ Full | ❌ | `EmulateFull \| MoreInfo` |
| 15 | **EMV** (payment) | ✅ | ✅ | ❌ | ❌ | `MoreInfo` |
| 16 | **SRIX** | ✅ | ✅ | ❌ | ❌ | `MoreInfo` |
| 17 | **Jewel/Topaz** | ✅ | ✅ | ❌ | ✅ | `EditUid \| MoreInfo \| Write` |

## Summary

| Feature | Protocols with ✅ | Protocols with ❌ |
|---------|------------------|-------------------|
| **Info** | 17 / 17 | — |
| **MoreInfo** | **17 / 17** | — |
| **Emulate** | **11 / 17** | 3B, 4B, ST25TB, SRIX, EMV, Jewel |
| **Write** | **4 / 17** | 3A, 3B, 4A, 4B, 15693-3, FeliCa, Plus, DESFire, ST25TB, NTAG, Slix, EMV, SRIX |

## Gap Analysis

### Feature Flag Consistency

All `.features` flags correctly match implemented scene handlers. No inconsistencies found.

### Remaining Gaps by Priority

| Priority | Gap | Protocols | Rationale |
|---|---|---|---|
| **LOW** | No Emulate | ISO14443-3B, ISO14443-4B | No listener HAL support for Type B on PN532 or ST25R3916 |
| **LOW** | No Emulate | ST25TB, SRIX | ST25R3916-only (no PN532 support). Would need listener implementation |
| **LOW** | No Emulate | EMV | No legitimate use case for emulating payment cards |
| **LOW** | No Emulate | Jewel/Topaz | PN532 hardware cannot emulate Jewel (no native support) |
| **LOW** | No Write | 13 protocols | Most don't have write-capable poller FSMs. Writing to vicinity cards (ISO15693-3), FeliCa, or EMV/SRIX tags is uncommon |

### Intentional Omissions

- **EMV Emulate**: Payment card emulation has no legitimate use case in this firmware. Intentionally omitted.
- **ISO15693-3 & Slix Write**: Vicinity cards are typically factory-programmed. Write would require poller FSM additions with limited practical benefit.
- **ST25TB Emulate**: ST25R3916-only protocol; PN532 build excludes it entirely via `SConscript`.
- **Jewel/Topaz Emulate**: PN532 cannot emulate Jewel/Topaz (no native `InListPassiveTarget` support for BrTy=0x04 in target mode).

## File Reference

Each protocol's feature flags and scene handlers are defined in:

```
applications/main/nfc/helpers/protocol_support/<protocol>/<protocol>.c
```

The corresponding render functions (Info, MoreInfo dump) live in:

```
applications/main/nfc/helpers/protocol_support/<protocol>/<protocol>_render.c
```

### MoreInfo Dump Implementations

| Protocol | Dump Function | Content |
|---|---|---|
| ISO14443-3A | `nfc_render_iso14443_3a_dump()` | UID, ATQA, SAK, UID length, struct hex dump |
| ISO14443-3B | `nfc_render_iso14443_3b_dump()` | ATQB (UID, AppData, ProtoInfo), struct hex dump |
| ISO14443-4A | `nfc_render_iso14443_4a_dump()` | ATS (TL, T0 flags, TA1, TB1, TC1 flags), historical bytes, ATS hex dump, + 3A base dump |
| ISO14443-4B | `nfc_render_iso14443_4b_dump()` | Delegates to 3B dump |
| ISO15693-3 | `nfc_render_iso15693_3_info()` | Block dump (Full format) |
| FeliCa | (render function) | IDm, PMm, block data |
| MIFARE Ultralight | (render function) | OTP, lock bytes, page data |
| MIFARE Classic | (render function) | Access conditions, block data |
| MIFARE Plus | (render function) | 4A ATS + block data |
| MIFARE DESFire | (render function) | 4A ATS + application data |
| **ST25TB** | `nfc_render_st25tb_dump()` | UID, type, block count, system OTP, block dump |
| NTAG 4xx | (render function) | 4A ATS + NDEF/memory data |
| Type 4 Tag | (render function) | NDEF/applet data |
| Slix | (render function) | Block dump |
| EMV | `nfc_render_emv_dump()` | UID, ATQA/SAK, ATS, AID, PDOL, AFL, PAN, AIP |
| SRIX | `nfc_render_srix_dump()` | 512-byte hex dump (128 blocks × 4 bytes) |
| Jewel/Topaz | (render function) | HR0, HR1, UID, ATQA |

## Hardware Constraints

This firmware targets a **DIY board with STM32WB55CGU6 + PN532 (I2C)**. Some protocols are hardware-limited:

- **PN532 builds** (`FURI_HAL_NFC_PN532_ONLY`): No listener mode for Type B, ST25TB, SRIX. No vicinity card support (ISO15693-3, Slix excluded from compilation).
- **ST25R3916 builds** (original Flipper Zero): Supports all protocols including listener mode for Type B, ST25TB, ISO15693-3.

See `AGENTS.md` for detailed PN532 limitations.
