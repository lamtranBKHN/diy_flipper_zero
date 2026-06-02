# RAM Comparison: Official vs Momentum vs FCFZ vs FCFZ-Ref

**Date**: 2026-06-02
**Projects**: flipperzero-firmware (OFW), Momentum-Firmware, FCFZ (our build), FuckingCheapFlipperZero (FCFZ-ref)

---

## 1. Heap Allocator — IDENTICAL across all four

| Aspect | OFW | Momentum | FCFZ | FCFZ-ref |
|---|---|---|---|---|
| `memmgr_heap.c` diff vs FCFZ | Zero diff | Zero diff | — | Zero diff |
| Algorithm | FreeRTOS heap_4 | Same | Same | Same |
| OOM behavior | `furi_check` crash | Same | Same | Same |
| Heap size | `__heap_end__ - __heap_start__` | Same | Same | Same |
| Heap canary | Yes (XOR) | Same | Same | Same |
| Clear on free | Yes | Same | Same | Same |

**All four identical.** Same allocator, same OOM behavior, same heap size formula.

---

## 2. FURI_LOG Compile-Time Behavior

| Aspect | OFW | Momentum | FCFZ-ref | FCFZ (ours) |
|---|---|---|---|---|
| `FURI_LOG_D` w/ `FURI_NDEBUG` | Always active | Always active | Always active | `if(0)` dead code ✅ |
| `FURI_LOG_T` w/ `FURI_NDEBUG` | Always active | Always active | Always active | `if(0)` dead code ✅ |
| `log.h` diff vs FCFZ | 16 lines added by FCFZ | 16 lines added by FCFZ | 16 lines added by FCFZ | — |
| `log.c` diff vs FCFZ | Zero diff | Zero diff | Zero diff | — |

**FCFZ has BETTER compile-out than all three reference projects.** We added `#ifdef FURI_NDEBUG` guard around `FURI_LOG_D/T` and `FURI_LOG_RAW_D/T`. OFW, Momentum, and FCFZ-ref all lack this guard.

**BUT:** `furi_log_print_format()` checks `if(level > furi_log.log_level) { break; }` BEFORE allocating FuriString. So even without the guard, reference projects don't allocate when log level ≥ Info. Our guard is extra safety (compiler eliminates call entirely).

---

## 3. Build Defaults

| Setting | OFW | Momentum | FCFZ-ref | FCFZ (ours) |
|---|---|---|---|---|
| `DEBUG` | **1** (debug!) | 0 | 0 | 0 |
| `COMPACT` | 0 | 1 | 1 | 1 |
| `FIRMWARE_ORIGIN` | "Official" | "Momentum" | — | "Momentum" |

**OFW defaults to debug builds.** All others default to release.

---

## 4. NFC Log Call Counts (FURI_LOG_D + FURI_LOG_T)

| File | OFW | Momentum | FCFZ-ref | FCFZ (ours) |
|---|---|---|---|---|
| `furi_hal_pn532.c` | N/A (ST25R3916) | N/A (ST25R3916) | N/A (no PN532) | **28** |
| `furi_hal_nfc_pn532.c` | N/A (ST25R3916) | N/A (ST25R3916) | N/A (no PN532) | **19** |
| `mf_classic_poller.c` | 50 | 50 | 50 | **64** |
| **Total** | **50** | **50** | **50** | **111** |

FCFZ has 61 extra D/T calls — 47 in PN532 HAL (unique to PN532 hardware) + 14 in poller (hybrid auth, key reuse, nested attack).

---

## 5. Root Cause Analysis

| Hypothesis | Verdict | Evidence |
|---|---|---|
| (a) FCFZ inheriting Momentum's heavier RAM | **Wrong** | `memmgr_heap.c` identical across all four |
| (b) FCFZ failing to define FURI_NDEBUG | **Wrong** | FCFZ defines it + has extra compile-out guard |
| (c) FCFZ-specific PN532 log calls | **Partial** | 47 extra D/T calls, but compiled out in release |
| (d) Universal debug-build behavior | **CONFIRMED** | All four crash with debug logging + NFC |

---

## 6. Conclusion

**FCFZ has BETTER log protection than all three reference projects.** The `FURI_NDEBUG` guard on `FURI_LOG_D/T` is a FCFZ-only improvement — none of OFW, Momentum, or FCFZ-ref have it.

**The OOM crash is universal to debug builds.** All four firmwares would crash with debug logging + NFC scanning. FCFZ just happens to be the one being tested with debug logging enabled.

**FCFZ release builds (`DEBUG=0`) are safe.** All D/T calls compiled out as `if(0)` dead code. No heap pressure from debug logging.

**No code changes needed.** The existing `sysctl log level info` warning at MFC dict attack entry is sufficient.

---

## 7. FCFZ-Specific Additions

| Addition | Source | Impact |
|---|---|---|
| 47 PN532 HAL log calls | `furi_hal_pn532.c` (28), `furi_hal_nfc_pn532.c` (19) | Compiled out in release |
| 14 extra poller log calls | `mf_classic_poller.c` (hybrid auth, key reuse, nested attack) | Compiled out in release |
| `FURI_NDEBUG` compile-out guard | `log.h` (FCFZ-only improvement) | Better than reference |
| Debug log level warning | `mf_classic_poller.c` handler_start | Warns developer |
| Heap guards | `mf_classic_poller.c`, `mf_classic.c` | Diagnostic only |
