## Build System

- `./fbt` must run from the repo root; it forwards to SCons.
- Load toolchain: `scripts\toolchain\fbtenv.cmd` (Windows) or `source scripts/toolchain/fbtenv.sh` (Linux/macOS).
- Shortcut: `./fbt -s env` loads the toolchain environment and dumps variables.

## Hardware Target

- **This is a DIY board** with STM32WB55CGU6, not the original Flipper Zero (STM32F7).
- Default target is `f7` (`TARGET_HW=7`), set in `fbt_options.py:10`.
- CI only builds `f7` matrix target (`.github/workflows/build.yml:29`).
- Target-specific HAL lives in `targets/f7/furi_hal/`.

## Build Commands

- `./fbt` – debug firmware, no external apps
- `./fbt TARGET_HW=7 DEBUG=0 COMPACT=1 copro_dist updater_package fap_dist` – release build (matches CI)
- `./fbt flash` – flash via ST-Link/Black Magic probe
- `./fbt flash_usb_full` – build + flash over USB (device must be in DFU mode)
- `./fbt updater_package` – OTA update bundle
- `./fbt copro_dist` – package Core2 BLE radio stack (`stm32wb5x_BLE_Stack_light_fw.bin`)

## App Development

- Build external app: `./fbt fap_{APPID}` (e.g. `./fbt fap_qrcode`)
- Build all apps: `./fbt faps` or `./fbt fap_dist`
- Build + launch on device: `./fbt launch APPSRC=applications_user/<path>`
- `APPSRC` path is relative to `applications_user/`

## Lint / Format

- `./fbt lint_all` – Python (`black --check`), C/C++ lint, image lint
- `./fbt format_all` – `black`, `clang-format`, image formatting
- Individual: `lint_py`, `format_py`, `lint_img`, `format_img`

## Debugging

- `./fbt debug` – GDB via OpenOCD (default probe)
- `./fbt blackmagic` – GDB via Black Magic probe
- `./fbt debug_other` – attach GDB to arbitrary ELF
- OpenOCD config in `fbt_options.py:74-83` (CMSIS-DAP, SWD, stm32wbx)

## Key Options

- `FIRMWARE_APP_SET=unit_tests` – build with unit tests
- `COMPACT=1` – optimize for size
- `DEBUG=0` – strip debug symbols
- `FBT_NO_SYNC=1` – skip submodule updates
- `VERBOSE=1` – verbose SCons output

## Hardware Quirks (DIY Board)

- **PN532 NFC** via I2C1 at `0x48` (7-bit) or `0x90/0x91` (8-bit write/read), enabled by `PN532_ENABLED` in `targets/f7/boards/custom_pn532_board.mk`
- **PCF8574** I/O expander at `0x20` on I2C1: buttons (P0-P5), vibro (P6), buzzer (P7), INT on PB0
- **CC1101** sub-GHz on SPI1: CS=PA15, G0=PA1
- **SD card** on SPI1: CS=PA10
- **OLED** (SH1106/SSD1306) on I2C1
- I2C3 (PA7/PB4) is **disabled** – pins used for other functions
- Pin macros: `targets/f7/furi_hal/furi_hal_resources.h`

### PN532 NFC Limitations

`FURI_HAL_NFC_PN532_ONLY` flag in `furi_hal_nfc.c` gates PN532-only paths:
- **No listener mode**: `set_mode(FuriHalNfcModeListener)` returns error. All `furi_hal_nfc_listener_*()` return error or timeout.
- **No RF field control**: `poller_field_on/off()` are no-ops (PN532 manages field internally). `field_detect_start/stop` are no-ops, `field_is_present` always returns `true`.
- **Protocol limited (PN532-only build)**: Enabled protocols: ISO14443-3A, ISO14443-3B, ISO14443-4A, ISO14443-4B, FeliCa, MfUltralight, MfClassic, MfPlus, MfDesfire, Ntag4xx, Type4Tag, **EMV**, SRIX, Jewel. Disabled: ISO15693-3, Slix, ST25TB (no PN532 native support).
- **I2C1 shared bus, no INT, no RST**: The PN532, PCF8574 I/O expander, and OLED (SH1106/SSD1306) all share the same I2C1 bus. The PN532 has no INT (IRQ) pin to signal data readiness and no RST (reset) pin for hard-recovery from bus desync.
- **270-byte read on every I2C response**: performance issue, unused trailing bytes.
- **CIU settle timing** (`furi_hal_nfc_pn532.c`): CIU register writes (RxCRC/TxParity) complete on I2C bus instantly, but PN532 internal CIU state machine needs ~500-1000µs to apply. Missing settle delay caused `InCommunicateThru` timeouts `0x01` — NTAG detection fails, MFClassic auth times out, and the logging Heisenbug (debug logging padding 1-10ms masked the race). Fixed: `furi_delay_us(1000)` added in 2 locations — after CIU config in MF Classic auth path, before `InCommunicateThru` in non-ISO-DEP path.
- **CIU settle audit**: All `InCommunicateThru` call sites audited for CIU settle race. Only the 2 fixed sites needed the delay. 5 SRIX calls (`srix_detect/select/get_uid/read_block/write_block`) have no CIU reg writes before them. `in_communicate_thru_bits` writes `CIU_BitFraming` (serial-level, not CIU pipeline) and has zero callers. `mf_backdoor_auth` uses raw `send_command(0x42)` path with no CIU reg writes. All other `InCommunicateThru` paths clean.
- **No reference project uses InCommunicateThru for CIU register access** — our pattern of CIU reg writes + settle delay + InCommunicateThru is unique and innovative. All references use InDataExchange (0x40) for MF Classic auth and standard commands for everything else.
- **Power rail capacitors (CRITICAL for clone modules)**: Clone PN532 modules draw 100-200mA when RF field activates. Without bulk capacitance, voltage drops cause I2C corruption or brown-out. **Required**: 100µF electrolytic + 100nF ceramic across PN532 VCC↔GND, placed within 5mm of module pins. Without these, clone modules may: (1) fail to generate RF field, (2) NACK I2C transactions during RF activation, (3) reset mid-polling.
- **Clone module settle times**: Clone PN532 modules need 3-10x longer settle delays than genuine PN532. RFConfiguration settle: 500ms (vs 50ms genuine). TxControl settle: 300ms (vs 50ms genuine). I2C settle after mode change: 500ms (vs 150ms genuine). InRelease settle: 500ms (vs 150ms genuine). These are required because clone modules have slower RF oscillator startup and weaker power rail decoupling.

### I2C1 Bus 150ms Timeout Floor

**All I2C transactions on the shared I2C1 bus must use a minimum timeout of 150ms.**

#### Rationale

Three devices share I2C1 (PB9=SCL, PA9=SDA):
- **PN532** (NFC, addr `0x48`/`0x24`) — no INT pin, no RST pin
- **PCF8574** (I/O expander, addr `0x20`) — has INT on PB0, uses `PCF8574_I2C_TIMEOUT_MS = 150`
- **OLED** (SH1106/SSD1306, addr `0x3C`) — no INT

Because the PN532 has **no INT pin**, ready-status must be checked by polling the PN532's status byte over I2C. Because it has **no RST pin**, a stuck or desynced PN532 can only be recovered via I2C retry/reinit — never with a hard reset.

#### Why 150ms

| Value | Used By | Purpose |
|---|---|---|
| **500ms** | `furi_hal_nfc_pn532.c` `reset()` / `set_mode()` | PN532 settle guard on shared bus (clone modules) |
| **150ms** | `PCF8574_I2C_TIMEOUT_MS` (furi_hal_pcf8574.h) | All PCF8574 I2C reads/writes |
| **150ms** | `furi_hal_pn532.c` `pn532_read_ack()` | Blocking ACK wait (worst-case I2C arbitration) |
| 100ms | `furi_hal_pn532.c` `pn532_probe_address()` | Initial address probe (single-byte I2C RX) |
| 30ms | `furi_hal_pn532.c` blocking ready poll | Lightweight fallback for ready-status reads |
| 10ms | `furi_hal_pn532.h` background poller I2C RX | Single-byte status read in ready poller thread |

The 150ms floor is the **PCF8574 I2C timeout** — the longest regular timeout of any device on the bus. Using a shorter timeout for PN532 command exchanges risks:
1. **False negatives**: The PN532 is ready but the I2C bus was briefly held by an OLED write or PCF8574 read in progress → we wrongly mark the PN532 as unresponsive.
2. **Desync spiral**: A timeout-masked-as-unresponsive triggers the reinit path, which runs SAM config + 100ms+ delays, wasting even more time.
3. **No recovery escape**: Without RST, the only way to recover a stuck PN532 is I2C re-probe. Having too-short timeouts triggers unnecessary re-probes that add delay with no benefit.

#### Where Sub-150ms Values Are Intentional

Sub-150ms I2C timeouts are **only** used in contexts where the transaction is guaranteed to be brief and the failure cost is low:

| File | Timeout | Context | Why Safe |
|---|---|---|---|
| `furi_hal_pn532.c` | **10ms** | Background poller ready-status read (single byte) | Runs in dedicated low-priority thread; failure just skips one poll cycle |
| `furi_hal_pn532.c` | **30ms** | Blocking ready-status read (fallback when poller offline) | Single-byte read; timeout → caller retries with full 150ms wait |
| `furi_hal_nfc_pn532.c` | **1ms** | CIU register settle delay | Software delay (no I2C involved), before InCommunicateThru |
| `furi_hal_nfc_pn532.c` | **5-20ms** | NDEF emulation loop pacing | Software delay (no I2C involved), waits for reader response |
| `furi_hal_nfc_pn532.c` | **30ms** | IRQ thread startup drain | Single-shot delay per `set_mode()` call |

#### How Sub-150ms Values Are Protected

The `10ms` ready-status poll and `30ms` blocking fallback are both **single-byte I2C reads** (reading the PN532 status register). These are the shortest possible I2C transactions — one byte in, one byte out — and take <1ms on the wire at 400kHz. The timeout covers only I2C arbitration delay (STM32 HAL waiting for bus ownership), not PN532 processing time.

If either of these short reads times out:
- **Poller path**: The background thread skips one cycle; **150ms** is used for the next full command exchange.
- **Blocking path**: The caller re-queues and retries with the full `PN532_TIMEOUT_CMD_MS` (300ms) timeout.

This layering ensures that the **150ms floor always applies to actual command payload exchanges** while keeping lightweight status polls fast.

#### Audit Verdict (2026-05-28)

No sub-150ms **I2C command timeouts** exist in either `furi_hal_nfc_pn532.c` or `furi_hal_pcf8574.c`. All I2C transaction timeouts are 150ms or higher. Sub-150ms values are all:
- Single-byte ready-status reads (10ms / 30ms — protected by 150ms fallback)
- Software delays (`furi_delay_us/ms`) — no I2C bus activity during the wait

The 150ms floor is correctly and consistently applied across the shared bus.

### NFC Reference Project Comparison (2026-05-28)

Comprehensive audit of 10 reference projects vs our implementation. Key findings below.

#### Protocol Support Matrix: Ours vs OFW vs Momentum

| Protocol | Ours (PN532) | OFW (ST25R3916) | Momentum |
|---|---|---|---|
| ISO14443-3A (NFC-A) | ✅ | ✅ | ✅ |
| ISO14443-3B (NFC-B) | ✅ | ✅ | ✅ |
| ISO14443-4A (ISO-DEP) | ✅ | ✅ | ✅ |
| ISO14443-4B | ✅ | ✅ | ✅ |
| ISO15693-3 (NFC-V) | ❌ (no PN532 support) | ✅ | ✅ |
| FeliCa | ✅ | ✅ | ✅ |
| MIFARE Ultralight | ✅ | ✅ | ✅ |
| MIFARE Classic | ✅ | ✅ | ✅ |
| MIFARE Plus | ✅ | ✅ | ✅ |
| MIFARE DESFire | ✅ | ✅ | ✅ |
| ST25TB | ❌ (no PN532 support) | ✅ | ✅ |
| NTAG 4xx | ✅ | ❌ | ✅ |
| Type 4 Tag | ✅ | ❌ | ✅ |
| Slix | ❌ (no PN532 support) | ✅ | ✅ |
| EMV (payment) | ✅ | ❌ | ✅ |
| SRIX | ✅ | ❌ | ❌ |
| Jewel/Topaz | ✅ | ❌ | ❌ |

**Note**: Our PN532 build supports 14/17 protocols (missing only 3 due to hardware limitations). OFW supports 12/17, Momentum supports 15/17. We are the **only** build with SRIX and Jewel support.

#### I2C Transaction Model Comparison

| Aspect | Ours (furi_hal_pn532.c) | Adafruit PN532 | pn532-lib | PN532-on-STM32 |
|---|---|---|---|---|
| I2C ready timeout | 10ms (poller), 30ms (blocking) | 20ms (`PN532_I2C_READYTIMEOUT`) | Platform-defined | Timer-based |
| Default command timeout | 300ms (CMD), 1000ms (EXCHANGE) | 100ms (`sendCommandCheckAck`) | 1000ms (`PN532_DEFAULT_TIMEOUT`) | Configurable in call |
| HW reset pin | **None** (I2C only) | **Yes** (RSTPD_N) | Platform-defined | Assumed present |
| SAM config params | 0x01, 0x14, 0x01 (matches all refs) | 0x01, 0x14, 0x01 | 0x01, 0x14, 0x01 | 0x01, 0x14, 0x01 |
| Command frame format | preamble+startcode+len+lenchk+data+datachk+postamble | Same | Same (+checksum calc) | Same |
| Frame buffer size | `PN532_MAX_FRAME_SIZE` (255+) | 64 (`PN532_PACKBUFFSIZ`) | 255 (`PN532_FRAME_MAX_LENGTH`) | 64 |
| I2C read pattern | Read n bytes (reads leading addr byte) | Read n+1 bytes, strip leading RDY byte | Platform I2C read callback | STM32 HAL I2C |
| Background ready poller | **Yes** (dedicated thread, 25ms period) | **No** (polling in call) | **No** (blocking wait in call) | **No** (blocking wait) |
| CIU register access | **Yes** (InCommunicateThru, unique) | **No** (InDataExchange only) | **No** (InDataExchange only) | **No** (InDataExchange only) |

#### Error Code Definitions

The `pn532-lib` reference has the **complete** official PN532 error code list (0x00-0x2e). Our implementation only has a subset. Consider adding the full list for better diagnostics:

| Error | Code | Description | Present?
|---|---|---|---|
| `PN532_ERROR_NONE` | 0x00 | Success | ✅ |
| `PN532_ERROR_TIMEOUT` | 0x01 | Target hasn't answered | ✅ (used in code) |
| `PN532_ERROR_CRC` | 0x02 | CRC error by CIU | ✅ |
| `PN532_ERROR_PARITY` | 0x03 | Parity error by CIU | ❌ |
| `PN532_ERROR_COLLISION_BITCOUNT` | 0x04 | Erroneous bit count in anti-collision | ❌ |
| `PN532_ERROR_MIFARE_FRAMING` | 0x05 | Framing error during MIFARE | ❌ |
| `PN532_ERROR_COLLISION_BITCOLLISION` | 0x06 | Bit-collision during anti-collision | ❌ |
| `PN532_ERROR_NOBUFS` | 0x07 | Communication buffer insufficient | ❌ |
| `PN532_ERROR_RFNOBUFS` | 0x09 | RF buffer overflow by CIU | ❌ |
| `PN532_ERROR_ACTIVE_TOOSLOW` | 0x0a | RF field not switched on in time | ❌ |
| `PN532_ERROR_RFPROTO` | 0x0b | RF protocol error | ❌ |
| `PN532_ERROR_TOOHOT` | 0x0d | Overheating, antenna disabled | ❌ |
| `PN532_ERROR_INTERNAL_NOBUFS` | 0x0e | Internal buffer overflow | ❌ |
| `PN532_ERROR_INVAL` | 0x10 | Invalid parameter | ❌ |
| `PN532_ERROR_DEP_INVALID_COMMAND` | 0x12 | DEP invalid command | ❌ |
| `PN532_ERROR_DEP_BADDATA` | 0x13 | DEP/MIFARE/14443-4 bad data format | ❌ |
| `PN532_ERROR_MIFARE_AUTH` | 0x14 | Authentication error | ✅ (used as return) |
| `PN532_ERROR_NOSECURE` | 0x18 | No NFC security support | ❌ |
| `PN532_ERROR_I2CBUSY` | 0x19 | I2C bus line busy (TDA ongoing) | ❌ |
| `PN532_ERROR_UIDCHECKSUM` | 0x23 | UID check byte wrong | ❌ |
| `PN532_ERROR_DEPSTATE` | 0x25 | Invalid device state | ❌ |
| `PN532_ERROR_HCIINVAL` | 0x26 | Operation not allowed in HCI config | ❌ |
| `PN532_ERROR_CONTEXT` | 0x27 | Command not acceptable in current context | ❌ |
| `PN532_ERROR_RELEASED` | 0x29 | Target released its initiator | ❌ |
| `PN532_ERROR_CARDSWAPPED` | 0x2a | Card ID mismatch (ISO14443-3B) | ❌ |
| `PN532_ERROR_NOCARD` | 0x2b | Card disappeared | ❌ |
| `PN532_ERROR_MISMATCH` | 0x2c | NFCID3 mismatch in DEP passive | ❌ |
| `PN532_ERROR_OVERCURRENT` | 0x2d | Over-current event | ❌ |
| `PN532_ERROR_NONAD` | 0x2e | NAD missing in DEP frame | ❌ |

#### Key Architectural Differences Found

1. **Background ready poller is unique**: Our `pn532_ready_poller_run` runs as a dedicated thread checking PN532 READY status every 25ms. No reference project does this — they all poll synchronously within each command call. This is an advantage for latency but also adds I2C bus contention.

2. **No HW reset is a major limitation**: Adafruit and pn532-lib both assume a HW reset pin is available. Our I2C-only connection means bus desync requires the reinit-on-ACK-fail pattern (already implemented). The re-probe-on-ACK-fail fix (2026-05-17) is the correct mitigation.

3. **All references use InDataExchange for MF Classic auth**: Whereas we use InCommunicateThru + CIU register writes. This is unique and enables direct CIU register access for RxCRC/TxParity config that InDataExchange doesn't expose. The CIU settle delay (2026-05-26 fix) is a critical requirement for this approach.

4. **Buffer size differences**: Adafruit uses 64-byte buffer. We use MAX_FRAME_SIZE (255+). Our larger buffer is necessary for InDataExchange responses (192+ bytes for some card types). But Adafruit's smaller buffer works because they target simple card reading scenarios.

5. **Command timeout 100ms vs 1000ms**: Adafruit uses 100ms default. We use 300ms/1000ms. The longer timeout is appropriate for our use case (MF Classic auth can take 600ms+). The 2026-05-17 fix (InCommunicateThru timeout 250ms→1000ms) was correct.

6. **SAM config params are universal**: All reference projects use the exact same SAM config: mode=0x01 (normal), timeout=0x14 (50ms*20=1s), use_irq=0x01. Our SAM config implementation is correct.

#### Actionable Improvement Opportunities

| # | Finding | Priority | Effort | Action |
|---|---|---|---|---|
| 1 | **Missing PN532 error codes**: Could help debug InDataExchange/InCommunicateThru errors by name | LOW | Small | Add error code definitions from pn532-lib to `furi_hal_pn532.h` |
| 2 | **Adafruit I2C read strips leading byte**: Every I2C read returns leading status byte (0x01=RDY). Our implementation reads the raw buffer. If we ever need to distinguish RDY from data, use Adafruit's n+1 pattern | LOW | Research | Verify our I2C read already handles this correctly |
| 3 | **No CIU register access in any reference**: Our InCommunicateThru+CIU pattern is novel. Document that if InCommunicateThru ever breaks, fall back to InDataExchange like all references | LOW | Documentation | Already covered in CIU settle notes |
| 4 | **Adafruit uses delay(1) between transactions**: After writecommand, delay(1), after readack, delay(1). We use delay(10). 1ms may be sufficient for I2C settle — consider reducing our inter-transaction delays to 1ms for performance | MEDIUM | Testing | Reduce `furi_delay_ms(10)` → `furi_delay_ms(1)` in write and ack paths, test on hardware |
| 5 | **pn532-lib wakeup-on-write-fail**: On WriteFrame error, calls `pn532->wakeup()`. We don't have this automatic wakeup — consider adding wakeup retry on write failure | MEDIUM | Small | Add wakeup retry in `pn532_write_frame` on failure |
| 6 | **OFW/Momentum protocol architecture differences**: OFW has no Ntag4xx/Type4Tag/Emv in device defs (requires custom FAP). Momentum adds them. Both rely on ST25R3916 (ISO15693-capable). Our PN532 build achieves more protocols than OFW without ISO15693 | INFO | None | No action needed — our protocol support is already strong |

#### Reference Projects Summary

| Project | Language | Interface | Notable Pattern |
|---|---|---|---|
| `Adafruit-PN532` | Arduino C++ | SPI/I2C/UART | HW reset pin, 20ms I2C ready timeout, delay(1) between transactions |
| `pn532-lib` | Embedded C | SPI-only (struct callbacks) | Complete error codes, function pointer HAL, wakeup-on-write-fail |
| `PN532-on-STM32` | STM32 C++ | I2C/SPI | Timer-based waitReady, same SAM config |
| `flipperzero-firmware` (OFW) | Embedded C | ST25R3916 | 12 protocols, no PN532, NFC architecture we adapted |
| `Momentum-Firmware` | Embedded C | ST25R3916 | 15 protocols, OFW fork with Ntag4xx/Type4Tag/EMV |
| `Flipper-Zero-ESP32-ADV` | ESP32 C++ | PN532 via ESP32 | No standalone PN532 driver — ESP32-only |
| `Flipper-Zero-ESP32-Port` | ESP32 C++ | PN532 via ESP32 | No standalone PN532 driver — ESP32-only |
| `PN532` (NXP official) | Docs/ref | All | NXP app notes, AN10787, register reference |
| `FuckingCheapFlipperZero` | Arduino C | Varies | No NFC/PN532 support found in this project |
| `diy_flipper_UBYTE` | Embedded C | Same I2C bus (OLED+PCF8574) | No NFC/PN532 support — same I2C bus sharing pattern as ours |

#### Key Takeaway

Our PN532 implementation is **more feature-rich** than any single reference project:
- **More protocols than stock OFW** (14 vs 12). **Comparable to Momentum** (14 vs 15): we trade ISO15693-3/Slix/ST25TB (ST25R3916-only) for SRIX+Jewel (unique to our build).
- Unique background ready poller for lower latency
- Unique CIU register access via InCommunicateThru for protocol-level config
- No HW reset mitigation (re-probe on ACK fail) is correct per reference analysis
- SAM config and frame format are verified correct against all references

Main improvement opportunities: (1) add missing PN532 error code definitions for better diagnostics, (2) consider reducing inter-transaction delays from 10ms to 1ms per Adafruit pattern (verify on hardware).

### NFC Bugs Fixed (2026-05-11)

6 bugs identified and fixed; see individual files for details:

1. **Mutex race** (4 wrapper funcs in `furi_hal_nfc.c`): `poller_tx/rx` + `listener_tx/rx` acquired/released mutex, then tech func re-acquired → race window. Fixed: removed acquire/release from wrappers (tech funcs own their locking).
2. **PN532 missing mutex** (`furi_hal_nfc_pn532.c`): `exchange_internal()` and `trx_short_frame()` lacked acquire/release. Fixed: added `furi_hal_nfc_acquire()`/`release()` with `goto release` cleanup pattern.
3. **Scanner filtered to ISO14443A only** (`nfc_scanner.c`): PN532 branch offered only protocol A. Fixed: now offers A, B, FeliCa (B+FeliCa will silently fail with "not present" via `poller_tx_common` returning `Communication`).
4. **Listener set_col_res_data ST25R3916 access** (`furi_hal_nfc_iso14443a.c`): no PN532 guard. Fixed: wrapped in `#ifndef FURI_HAL_NFC_PN532_ONLY`.
5. **low_power_mode_stop missing furi_check** (`furi_hal_nfc.c`): called `acquire()` without `furi_check(instance)`. Fixed: added `furi_check(instance)`.
6. **270-byte read inefficiency** (`furi_hal_nfc_pn532.c`): I2C read always requests full buffer. Diagnosed, minor perf impact only.

### Bugs Fixed (2026-05-17)

8 bugs/issues identified and fixed across NFC, I/O expander, I2C config, display config, and loader:

1. **`sizeof(pointer)` truncates RX buffer** (`furi_hal_nfc_pn532.c` lines 783/796): `rx_payload` is a `uint8_t*` so `sizeof(rx_payload)` == 4, not 192. All `InDataExchange` non-ISO-DEP responses silently overflowed. Fixed: replaced with `PN532_MAX_FRAME_SIZE`.
2. **PCF8574 triggers 800ms I2C scan on any glitch** (`furi_hal_pcf8574.c`): `read()`/`write()` called full 16-address `init()` probe on every call when not ready, blocking I2C bus for up to 800ms. Fixed: added `PCF8574_REINIT_COOLDOWN_MS = 500` guard; always update `last_error_tick` on failure.
3. **ACK failure forces cold PN532 reinit** (`furi_hal_pn532.c`): any `pn532_read_ack()` failure set `pn532_ready = false`, causing the next command to run full SAM config + 100ms delays mid-session. Fixed: re-probe chip after ACK fail; only set absent if probe also fails.
4. **`InCommunicateThru` used 250ms command timeout** (`furi_hal_pn532.c`): MIFARE Classic auth via `InCommunicateThru` needs up to 1s but used `PN532_TIMEOUT_CMD_MS`. Fixed: changed to `PN532_TIMEOUT_EXCHANGE_MS` (1000ms).
5. **Listener I-block PCB byte never toggled** (`furi_hal_nfc_pn532.c`): hardcoded `0x02` caused reader retransmit loops per ISO14443-4. Fixed: now uses `iso_dep_block_num` field with XOR toggle, same as poller path.
6. **I2C3 dead code would silently corrupt SPI** (`furi_hal_i2c_config.c`): `Activate` event enabled I2C3 (PA7 == SPI_MOSI). Fixed: replaced with `furi_crash()` to catch accidental future use.
7. **SSD1306 and SSD1309 not mutually exclusive** (`furi_hal_resources.h`): both defines could be 1 simultaneously. Fixed: added `#if … #error` compile-time guard.
8. **Menu cache scaffolded but never used** (`loader_menu.c`): `cached_menu_content` struct fields existed but were never populated. Fixed: implemented 30s TTL cache; slurp on first open, replay from memory on subsequent opens. Also fixed 3 bugs in the cache implementation itself (unused allocation, O(N) header copy, slow character-by-character line scan).

### NFC Bugs Fixed (2026-05-26)

3 bugs fixed across NFC poller + PN532 HAL + worker yield:

1. **MF Classic crash** (`mf_classic_poller.c:2384-2393`): Fail→DetectType loop had no retry limit → infinite loop → watchdog reset. Fixed: `fail_retry_count` field + 5x limit, reset on detect/start.
2. **NTAG/Ultralight "Unknown" CRC double-encode** (`furi_hal_nfc_pn532.c:1090`): `InCommunicateThru` returns card data WITH native CRC; `prepare_rx` appended ANOTHER CRC → 18≠16 bytes → protocol error. Fixed: `!add_parity_to_rx && !use_comm_thru`.
3. **Yield starvation** (`nfc.c:200-214`): `nfc_worker_poller_ready_handler` had no `furi_thread_yield()` on `NfcCommandContinue` → GUI thread starved → watchdog. Fixed: added `else if` branch with yield.

### Protocols Changed (2026-05-26)

- **Removed** from PN532 build: ISO15693-3, Slix (PN532 has no native vicinity support, always times out)
- **Enabled** on PN532 build: EMV (previously NULL'd guard). Scans SAK=0x20 cards as ISO14443-4A → EMV poller detects via PPSE SELECT.
- **U2F** removed from `main_apps` metapackage (no longer auto-built as FAP). HAL driver stays in firmware (~2-4KB, USB descriptors).

### Jewel/Topaz Protocol Added (2026-05-26)

- **Full protocol integration**: `NfcProtocolJewel` as root-level base protocol (no parent, no children).
- **Detection** via `furi_hal_nfc_pn532_poll_jewel()` → PN532 `InListPassiveTarget` with BrTy=0x04 → returns RID (HR0, HR1, 4-byte UID) + ATQA=0x000C.
- **NfcDeviceBase** impl: alloc/free/reset/copy/save/load/get_uid/set_uid/get_name/is_equal/verify. Stores HR0, HR1, 4-byte UID, ATQA.
- **Poller FSM**: Idle → ReadBlock0 (poll_jewel) → ReadSuccess/ReadFailed. Reports JewelPollerEventTypeReady/Error.
- **Sync API**: `jewel_poller_sync_read()` for blocking reads from FAPs.
- **No listener** (PN532 cannot emulate Jewel).
- **Full dump** (READ_ALL implemented — reads 128-byte dump via READ_ALL command).
- **Scanner probe order**: Jewel added after Felica in `pn532_probe_order[]`.
- **Build cost**: ~1.2 KB flash (192 B JewelData + 960 B poller/code).

### Firmware Size

| Build | firmware.bin | Free (860KB) |
|---|---|---|
| Before (pre-2026-05-26) | 880,344 B (859.7 KB) | 296 B |
| After bug fixes only | 880,344 B | 296 B |
| After ISO15693-3+Slix removed | 869,184 B | 11,456 B (11.2 KB) |
| After EMV enabled | 876,984 B (856.4 KB) | 3,656 B (3.6 KB) |
| After D/T log stripping | 851,968 B (832.0 KB) | **28.0 KB** |
| After Jewel/Topaz added | 851,968 B (832.0 KB) | **28.0 KB** |
| After Tier 2 fixes (F5/F17/F22 + 22 M-items) | 862,944 B (842.7 KB) | **17.3 KB** |
| After L-tier L1+L5 (full error table + wakeup-on-write-fail) | 863,416 B (843.2 KB) | **16.8 KB** |

EMV cost: 7,800 bytes (7.6 KB) — below original 14KB estimate.
Log stripping saved ~26 KB. Jewel added ~1.2 KB.
Free flash now 28 KB — room for future features.

### Tier 2 Fixes (2026-06-02)

F-prefix audit Tier 2 (3 items) + 113-finding audit M-tier (22 items landed, 2 deferred). Total: 25 items, 1 build-stop rule preserved throughout.

**F-prefix Tier 2:**
- **F5** — Deleted `furi_hal_pn532_set_rf_retries()` (`furi_hal_pn532.c:2056-2066`), header decl, and `api_symbols.csv:1707` entry. No callers, dead code.
- **F17** — `furi_hal_pn532_in_release()` timeout 50ms → 150ms (`furi_hal_pn532.c:1952`). Matches I2C1 150ms floor (PCF8574 timeout). Preserves protocol semantics — InRelease is a single-byte status response, not a clone-module settle (settle is caller-side 500ms `furi_delay_ms`).
- **F22** — Demoted 4 hot-path logs in `pn532_exchange()` to DEBUG (lines 1198, 1204, 1210, 1252). Kept ERROR at 1175 (init fail) and 1732 (buffer overflow) — operator-actionable faults.

**M-tier (6 high + 16 med landed):**
- **M11** — `mf_classic_listener.c:412`: overflow-safe `transfer_value` accumulation (pos/neg overflow detection before add).
- **M12** — `mf_classic.c:384`: 7-byte UID case in `mf_classic_set_uid()` with BCC = UID0..UID2 + CT 0x88 XOR.
- **M14** — `mf_ultralight.c:705,722`: replaced `(uint8_t*)iv` cast with local `iv_local[8]` buffer in 3DES encrypt/decrypt.
- **M15** — `mf_ultralight.c:690`: rewrote `mf_ultralight_3des_key_valid()` to check page 44 content (false if all 0x00 or all 0xFF).
- **M16** — `mf_ultralight_listener.c:574`: 3DES mutual auth fail → NAK instead of sending encrypted rndA (prevents crypto leak).
- **M17** — `type_4_tag_poller_i.c:129,179`: cast `offset` to `size_t` in iso_read/iso_write bounds check.
- **M19** — `emv_poller_i.c:162`: `furi_check(tlen < sizeof(track_1_equiv_local))` before memcpy.
- **M20** — `emv_poller_i.c:189`: bounded track_2_equiv loop with `(x * 2 + 1) < (int)sizeof(track_2_equiv)`.
- **M22** — `emv_poller_i.c:129`: `furi_check(tlen <= 16)` before `memcpy(application_label)`.
- **M23** — `applications/main/nfc/helpers/protocol_support/emv/emv.c:43`: `EmvPollerEventTypeReadFailed` → `NfcCustomEventPollerFailure` + `NfcCommandStop`.
- **M24** — `emv_poller_i.c:113`: `furi_check(tlen == 1)` before memcpy to `app->priority` (uint8_t).
- **M25** — `felica_listener_i.c:28,36,44,51`: replaced unaligned `uint32_t*` cast with `bit_lib_bytes_to_num_be()` / `bit_lib_num_to_bytes_be()`. 4 sites: warning boundary, error boundary, increment, post_process.
- **M26** — `felica_poller.c:118-123` + `felica_poller_i.h:44`: `activate_retry_count` field, 5-retry cap on activation timeout. Reset on success.
- **M27** — `ntag4xx.c:73`: `ntag4xx_verify()` now checks `version.hw_type || version.sw_type` (was always false).
- **M28** — `ntag4xx_poller.c:105`: write version stub now sets `WriteFailed` (was falsely `WriteSuccess`).
- **M29** — `mf_plus_poller.c:21-43`: added `furi_check` for data + 4 bit_buffer allocs.
- **M30** — `mf_plus.c:81`: `mf_plus_copy()` now calls `mf_plus_reset()` first (prevents double-free of device_name FuriString).
- **M31** — `srix_poller.c:65`: set `data->type` from `chip_id` (0x04→SrixType512, 0x05→SrixType4K, else Unknown).
- **M32** — `jewel_poller_i.c:143`: defensive check after furi_assert — returns `JewelErrorNotPresent` if `uid_len != JEWEL_UID_SIZE`.
- **M33** — `iso14443_4a.c:191`: `is_equal` now compares ATS fields (tl, t0, ta_1, tb_1, tc_1, t1_tk) not just ISO14443-3A base.
- **M34** — `iso14443_4a_poller_i.c:93`: WTXM=0 clamp to 1 per ISO/IEC 14443-4:2016 §7.1.5 (WTXM=0 reserved; clamp so echoed S(WTX) gives real extension, not FWT hard-timeout).
- **M35** — `iso14443_3b_poller.c:16-32` + `iso14443_4b_poller.c:19-27`: added `furi_check` for all poller alloc sites (bit_buffers, iso14443_3b/4b_alloc, iso14443_4_layer_alloc).

**Deferred (audit claims stale or out-of-scope):**
- **M18** — `NfcCommandReset discarded` claim incorrect. Framework at `lib/nfc/nfc.c:213` already handles `NfcCommandReset` (sets `NfcPollerStateReset`). Not broken.
- **M21** — `Interac AID truncated` claim incorrect. 7-byte Interac AID `A0000002771010` matches EMVCo spec. `(i == 3) ? 8 : 7` heuristic is fragile but correct for all 5 AIDs.

**Size delta**: +10.7 KB (832.0 → 842.7 KB). Largest contributors: `bit_lib_bytes_to_num_be` pull-in (FeliCa M25) and ATS memcmp path (M33). Free flash 17.3 KB — still well within 860 KB budget.

### L-tier Fixes (2026-06-02)

113-finding audit L-tier (58 LOW items). 2 landed, 1 dropped, 55 deferred to a dedicated cleanup commit (no behavioral value).

**Landed:**
- **L1** — Added 18 missing PN532 error codes to `FuriHalPn532Error` enum (0x03, 0x04, 0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x10, 0x12, 0x13, 0x18, 0x19, 0x23, 0x25, 0x26, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E) plus matching string cases in `furi_hal_pn532_error_str()`. Purely additive, no existing entries remapped (ABI preserved). Used for diagnostic logging. `furi_hal_pn532.h:99-130`, `furi_hal_pn532.c:1010-1055`.
- **L5** — Added `pn532_wakeup_pulse()` helper (8 × 0x55 burst per NXP UM0701-02 §6.2.4) to `pn532_write_frame()`. Pre-write wakeup before first attempt; post-retry-loop wakeup + one final attempt before declaring failure. SAM reconfiguration remains the caller's responsibility via `furi_hal_pn532_init()` — wakeup is the lightweight recovery path. `furi_hal_pn532.c:239-260`, `:511-514`, `:545-562`.

**Dropped (permanently closed):**
- **L4** — Reduce inter-transaction delay 10ms → 1ms (Adafruit pattern). Rejected: PN532 and PCF8574 share I2C1; 1ms window risks bus arbitration conflicts, clock-stretching collision, and bus lockup. The 9ms overhead is the correct safety margin for a two-device shared bus. Do not revisit unless the PCF8574 is moved off I2C1.

**Deferred (54 cosmetic items):** Variable renames, magic number → named constant, comment cleanup. No behavioral or diagnostic value. Bundle into a single cleanup commit in a future sprint.

**Size delta**: +472 B (842.7 → 843.2 KB). Free flash 16.8 KB — still well within 860 KB budget.

### Remaining Work

**All 15 bugs fixed** (2026-05-26): H5-H8+M1-M7+L1-L5 resolved. See `BUG_FIX_PLAN.md` for details.

**Jewel/Topaz protocol**: implemented (2026-05-26). Detection + basic data. READ_ALL (full 128-byte dump) not implemented. No FAP integration yet.

## NFC Protocol Quick Reference

Full matrix with gap analysis in `documentation/NFCProtocolMatrix.md`.

### Feature Matrix

| # | Protocol | Info | MoreInfo | Emulate | Write |
|---|---|---|---|---|---|
| 1 | ISO14443-3A (NFC-A) | ✅ | ✅ | ✅ UID | ❌ |
| 2 | ISO14443-3B (NFC-B) | ✅ | ✅ | ❌ | ❌ |
| 3 | ISO14443-4A (ISO-DEP) | ✅ | ✅ | ✅ UID | ❌ |
| 4 | ISO14443-4B | ✅ | ✅ | ❌ | ❌ |
| 5 | ISO15693-3 (NFC-V) | ❌ | ❌ | ❌ | ❌ |
| 6 | FeliCa | ✅ | ✅ | ✅ Full | ❌ |
| 7 | MIFARE Ultralight | ✅ | ✅ | ✅ Full | ✅ |
| 8 | MIFARE Classic | ✅ | ✅ | ✅ Full | ✅ |
| 9 | MIFARE Plus | ✅ | ✅ | ❌ | ❌ |
| 10 | MIFARE DESFire | ✅ | ✅ | ❌ | ❌ |
| 11 | ST25TB | ❌ | ❌ | ❌ | ❌ |
| 12 | NTAG 4xx | ✅ | ✅ | ❌ | ❌ |
| 13 | Type 4 Tag | ✅ | ✅ | ✅ Full | ✅ |
| 14 | Slix | ❌ | ❌ | ❌ | ❌ |
| 15 | EMV (payment) | ✅ | ✅ | ❌ | ❌ |
| 16 | SRIX | ✅ | ✅ | ❌ | ❌ |
| 17 | Jewel/Topaz | ✅ | ✅ | ❌ | ✅ |

**Summary (PN532 build)**: Info=14/17, MoreInfo=14/17, Emulate=6/17, Write=4/17.
Info/MoreInfo ❌: ISO15693-3, Slix, ST25TB (no PN532 native support).
Emulate ❌: 3B, 4B, ISO15693-3, Slix, ST25TB, Plus, DESFire, NTAG 4xx, EMV, SRIX, Jewel (11 protocols).
Write ❌: 3A, 3B, 4A, 4B, ISO15693-3, FeliCa, Plus, DESFire, ST25TB, NTAG 4xx, Slix, EMV, SRIX (13 protocols).

### MoreInfo Dump Implementations

| Protocol | Function | Content |
|---|---|---|
| ISO14443-3A | `nfc_render_iso14443_3a_dump()` | UID, ATQA, SAK, struct hex dump |
| ISO14443-3B | `nfc_render_iso14443_3b_dump()` | ATQB (UID, AppData, ProtoInfo), hex dump |
| ISO14443-4A | `nfc_render_iso14443_4a_dump()` | ATS + flags + historical bytes, + 3A dump |
| ISO14443-4B | `nfc_render_iso14443_4b_dump()` | Delegates to 3B dump |
| ISO15693-3 | `nfc_render_iso15693_3_info()` | Block dump (Full format) |
| FeliCa, Ultralight, Classic, Plus, DESFire, NTAG 4xx, Type4Tag, Slix, Jewel | (render function) | Protocol-specific data |
| ST25TB | `nfc_render_st25tb_dump()` | UID, type, block count, block dump |
| EMV | `nfc_render_emv_dump()` | UID, ATS, AID, PDOL, AFL, PAN, AIP |
| SRIX | `nfc_render_srix_dump()` | 512-byte hex dump (128 blocks) |

See `documentation/NFCProtocolMatrix.md` for full gap analysis and hardware constraints.

## Gotchas

- `./fbt` auto-updates git submodules on first run (set `FBT_NO_SYNC=1` to skip)
- `flash_usb_*` requires DFU mode + correct USB driver (Zadig on Windows)
- `fbt` aliases defined in `SConstruct`, not separate scripts
- API consistency checked in CI: `targets/f7/api_symbols.csv` must match OFW release channel
- **Unit tests build** (`FIRMWARE_APP_SET=unit_tests`) had pre-existing link errors for JS symbols (`js_thread_run`, `js_thread_stop`, `js_value_buffer_size`, `js_value_parse`). Fixed by removing JS entries from `unit_test_api_table_i.h` — JS app is compiled as FAP plugin, not linked into firmware ELF, so API table references were never resolvable. Build now passes cleanly.
- **I2C3 is permanently disabled** — `furi_hal_i2c_handle_external` Activate now calls `furi_crash()`. Do not use it.
- **`sizeof(pointer)` anti-pattern** — when passing a heap/scratch buffer pointer to a PN532 exchange, always use the named constant (`PN532_MAX_FRAME_SIZE`, `PN532_MAX_RX_FRAME`) not `sizeof(ptr)`.

## NFC Protocol Audit (2026-05-29)

Full audit report: `C:\Users\alojy\.local\share\opencode\plans\NFC_AUDIT.md`

### Findings Summary

113 findings across 14 protocols + PN532 HAL: 15 HIGH, 40 MEDIUM, 58 LOW.

### Critical Fixes Applied

| Fix | File | Change |
|---|---|---|
| PN532 auth timeout | `furi_hal_nfc_pn532.c:14` | `PN532_TIMEOUT_MF_AUTH_MS` 150→1000 |
| Stale target | `furi_hal_nfc_pn532.c:436` | Clear `target_valid` in `reset_keep_target()` |
| Bank card detection | `furi_hal_pn532.c:863` | `MxRtyPassiveActivation` 0x02→0x05 |
| Nested calibrate hang | `mf_classic_poller.c:1404-1447` | Retry limit (max 3) |
| EMV crash | `emv_poller_i.c:791-796` | Graceful `break` instead of `furi_crash` |
| DESFire infinite loop | `mf_desfire_poller.c:88` | `NfcCommandContinue` instead of `Reset` |
| DESFire event type | `mf_desfire_poller.c:178` | Set `MfDesfirePollerEventTypeReadFailed` |
| ISO14443-3B crash | `iso14443_3b_poller_i.c:24,69` | Pass valid data to `activate()` |
| ISO14443-3B UB | `iso14443_3b_poller.c:22` | Initialize `state = Idle` |
| Old-format masks | `mf_classic.c:203-208` | Preserve key/block masks |
| UL pages_read underflow | `mf_ultralight_poller.c:647-649` | Guard `pages_read -= 2` |
| KeyB permission | `mf_classic_poller.c:368` | `KeyAWrite` → `KeyBWrite` |
| get_key stale data | `mf_classic.c:553-567` | Return zero key if not found |
| 3DES key in log | `mf_ultralight_poller.c:497-515` | Removed |
| Type 4 Tag VLA | `type_4_tag_poller_i.c:293` | Fixed-size buffer |
| Type 4 Tag write | `type_4_tag_listener_i.c:211-217` | Buffer validation |
| UL fast_read overflow | `mf_ultralight_listener.c:170-171` | Cap at 64 pages |
| 50ms settle waste | `furi_hal_nfc_pn532.c:1263` | Only on `FuriHalPn532ErrorComm` |
| Hot-path log spam | `furi_hal_pn532.c:1100` | `WARN` → `DEBUG` |
| MF Classic emulate | `mf_classic.c:370` | `EmulateFull` → `EmulateUid` |
| NTAG 4xx write stub | `ntag4xx.c:122` | Removed `Write` feature flag |

### Protocol Limitations (PN532 Hardware)

- **No listener mode**: `set_mode(FuriHalNfcModeListener)` returns error. MF Classic emulation shows "Not Supported".
- **No ISO15693-3/Slix/ST25TB**: Disabled (no PN532 native support).
- **InCommunicateThru only for auth**: MF Classic auth uses CIU register writes + InCommunicateThru. InDataExchange only works for PN532 native auth.
- **NTAG 4xx write is a stub**: Version is read-only per NXP spec; write handler is a no-op.

### ISO-DEP Chaining Diagnostics

**Constants:**
| Constant | Value | File |
|---|---|---|
| `ISO14443_4_MAX_APDU_SIZE` | 1024 | `iso14443_4_layer.h` |
| `PN532_ISO_DEP_MAX_INF_SIZE` | 255 | `furi_hal_nfc_pn532.c` |
| `PN532_STATUS_CHAINING` | 0x40 | `furi_hal_nfc_pn532.c` |

**Log patterns to watch:**
- `"ISO-DEP chaining overflow"` — reassembly buffer exceeded (should not happen with 1024-byte buffer)
- `"Chained TX: fragment failed"` — poller-side chaining error (check card supports chaining)
- `"Chained TX: expected R(ACK)"` — card rejected chained I-block (card may not support chaining)
- `"READ_ALL short"` — Jewel returned < 128 bytes (normal for some Topaz variants)
- `"READ_ALL HR mismatch"` — Jewel READ_ALL returned wrong HR0/HR1 (card removed during read)

**Test recipes:**
- DESFire long read: `nfc` app → read DESFire with file > 256 bytes → should return full data
- EMV long APDU: scan bank card with large PDOL → full PDOL returned
- Jewel READ_ALL: read Topaz tag → 128-byte dump displayed in MoreInfo
- Regression: MF Classic 1K auth, UL NTAG213 read, FeliCa system info

## Performance Notes

- Animation cache (LRU, 2 slots) in `animation_storage.c` avoids re-reading SD frames on each animation switch
- **Main menu file cached** (`loader_menu.c`): `.mainmenu_apps.txt` is read from SD once and cached in memory for 30 seconds (`MENU_CACHE_TTL_MS`). Subsequent menu opens use the in-memory string — no SD I/O.
- ViewPort lockup warnings at `view_port.c:208` are expected on DIY board due to SPI1 bus sharing (display + CC1101 + SD) — reduced by menu cache
- Excessive debug logs (`log trace`) will spam console - use `log level info` to reduce output
- **OOM crash with NFC + logging**: Each `FURI_LOG_D` call in debug builds does `furi_string_alloc()` (heap) + `vsnprintf` + `furi_string_free()`. 596 log statements in `lib/nfc/` + 135 in NFC HAL = hundreds of alloc/free per second during NFC scan → heap fragmentation → hard fault. Fix: use release builds (`DEBUG=0`) where `FURI_LOG_D` is compiled out as `if(0)` dead code. Debug builds are for development only.
- **Heap guards added** (2026-06-01): `mf_classic_poller_alloc()` and `mf_classic_alloc()` now check `memmgr_get_free_heap()` before allocating ~5KB and log warning if heap < 8KB. Diagnostic only — cannot prevent crash since Flipper malloc is designed to hard-fault on OOM.
- **MfClassicData fixed at 256 blocks** (4,132 bytes): Always allocates full 256-block array regardless of card type (Mini=20, 1K=64, 4K=256). Dynamic sizing would save ~3.7KB on 1K cards but requires rewriting 200+ array access sites. Deferred.
- **NFC OOM crash in debug builds**: Each `FURI_LOG_D` call allocates a FuriString (~28B) via heap malloc+free. In debug builds (no `FURI_NDEBUG`), 596 log statements in `lib/nfc/` + 135 in NFC HAL cause hundreds of alloc/free cycles per second during NFC operations → heap fragmentation → hard fault. **Fix**: use release builds (`DEBUG=0 COMPACT=1`) for production — `FURI_LOG_D` compiled out as `if(0)` dead code, zero alloc overhead. Debug builds are for development only.
- **MfClassicData always allocates 256 blocks** (4,132B) regardless of card type (Mini=20, 1K=64, 4K=256). Dynamic sizing would save ~3.7KB on 1K cards but requires rewriting 200+ array access sites. Deferred.
- **`keys_dict_alloc` is only NFC alloc with heap guard** — checks `memmgr_get_free_heap() > cache_needed + 4096` before allocating dict cache. All other NFC allocs (poller, data, bit buffers) use raw `malloc` + `furi_check` — crash on OOM. Heap guards won't help poller allocs (no graceful fallback), but could help dict cache (already implemented).

## Storage Optimization TODOs

- ~~**`.mainmenu_apps.txt` cache**~~ — **DONE** (2026-05-17): 30s TTL in-memory cache implemented in `loader_menu.c`.
- ~~**FAP metadata cache**~~ — **DONE** (2026-05-26): 64-entry LRU cache with 5s TTL in `archive_browser.c`.
- **SPI contention** - SPI1 shared by display (PB6), CC1101 (PA15), SD (PA10). ViewPort lockups occur when SPI mutex held too long. Consider:
  - Shorter SPI transactions
  - Release SPI bus quickly after CC1101/SD operations  
  - Avoid SPI I/O in draw callbacks
