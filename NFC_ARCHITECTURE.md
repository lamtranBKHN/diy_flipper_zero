# NFC Subsystem Architecture

> **DIY Flipper Zero** — STM32WB55CGU6 + PN532 (I2C) + PCF8574
> Last updated: 2026-05-26

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Protocol Hierarchy](#2-protocol-hierarchy)
3. [Core Library (`lib/nfc/`)](#3-core-library-libnfc)
4. [PN532 HAL Layer](#4-pn532-hal-layer)
5. [NFC Application (`applications/main/nfc/`)](#5-nfc-app)
6. [Protocol Implementations](#6-protocol-implementations)
7. [Device Detection Flow](#7-device-detection-flow)
8. [Authentication & Security](#8-authentication--security)
9. [Key Design Patterns](#9-key-design-patterns)
10. [Known Limitations](#10-known-limitations)

---

## 1. System Overview

```
┌──────────────────────────────────────────────────────────┐
│                    NFC Application                         │
│           (scenes, protocol_support, supported_cards)      │
├──────────────────────────────────────────────────────────┤
│                   NFC Core Library                         │
│    (nfc.c, scanner, poller, listener, device, protocol)    │
├──────────────────────────────────────────────────────────┤
│                    PN532 HAL Layer                         │
│    (furi_hal_nfc.c, furi_hal_nfc_pn532.c, furi_hal_pn532) │
├──────────────────────────────────────────────────────────┤
│                    I2C1 Bus                                │
│                    (PN532 @ 0x48)                          │
└──────────────────────────────────────────────────────────┘
```

The NFC subsystem has 3 layers:

1. **Application layer** — user-facing NFC app with scenes (menus), protocol support plugins, and supported-card plugin system
2. **Core library** — protocol-agnostic NFC framework managing scanner → poller → listener lifecycle, device data, and protocol registration
3. **HAL layer** — PN532-specific I2C driver (CIU register access, InDataExchange, InCommunicateThru, auth path)

Hardware: PN532 NFC controller on I2C1 at 0x48 (7-bit) / 0x90-0x91 (8-bit). No HW RST pin.

---

## 2. Protocol Hierarchy

```
NfcProtocol
├── NfcProtocolIso14443_3a      (base) — ISO14443-3A anticollision/SDD
│   ├── NfcProtocolIso14443_4a  (child) — ISO14443-4A frame chaining
│   │   ├── NfcProtocolMfDesfire(child) — DESFire
│   │   ├── NfcProtocolEmv      (child) — EMV payment
│   │   └── NfcProtocolType4Tag (child) — Type 4 Tag (NDEF)
│   ├── NfcProtocolMfClassic    (child) — MIFARE Classic
│   │   └── NfcProtocolNtag4xx  (child) — NTAG 4xx
│   ├── NfcProtocolMfUltralight (child) — MIFARE Ultralight
│   └── NfcProtocolMfPlus       (child) — MIFARE Plus
├── NfcProtocolIso14443_3b      (base) — ISO14443-3B
│   │                           (no children registered)
├── NfcProtocolIso15693_3       (base) — ISO15693 vicinity
│   └── NfcProtocolSlix         (child) — NXP SLIX
├── NfcProtocolFelica           (base) — FeliCa / NFC Type F
├── NfcProtocolSt25tb           (base) — ST25TB
├── NfcProtocolSrix             (base) — SRIX
└── NfcProtocolJewel            (base) — Jewel/Topaz
```

**Registration** is in `nfc_protocol.c` using static arrays:

```c
// nfc_protocol.c — tree node definitions
static const NfcProtocolNode nfc_protocol_nodes[] = {
    [NfcProtocolIso14443_3a]  = { .parent = NfcProtocolInvalid, .children = iso14443_3a_children },
    [NfcProtocolIso14443_3b]  = { .parent = NfcProtocolInvalid, .children = NULL },
    [NfcProtocolIso14443_4a]  = { .parent = NfcProtocolIso14443_3a, .children = iso14443_4a_children },
    [NfcProtocolIso14443_4b]  = { .parent = NfcProtocolIso14443_3b, .children = NULL },
    [NfcProtocolIso15693_3]   = { .parent = NfcProtocolInvalid, .children = iso15693_3_children },
    [NfcProtocolFelica]       = { .parent = NfcProtocolInvalid, .children = NULL },
    [NfcProtocolMfUltralight] = { .parent = NfcProtocolIso14443_3a, .children = NULL },
    [NfcProtocolMfClassic]    = { .parent = NfcProtocolIso14443_3a, .children = mf_classic_children },
    [NfcProtocolMfPlus]       = { .parent = NfcProtocolIso14443_3a, .children = NULL },
    [NfcProtocolMfDesfire]    = { .parent = NfcProtocolIso14443_4a, .children = NULL },
    [NfcProtocolSlix]         = { .parent = NfcProtocolIso15693_3, .children = NULL },
    [NfcProtocolSt25tb]       = { .parent = NfcProtocolInvalid, .children = NULL },
    [NfcProtocolNtag4xx]      = { .parent = NfcProtocolMfClassic, .children = NULL },
    [NfcProtocolType4Tag]     = { .parent = NfcProtocolIso14443_4a, .children = NULL },
    [NfcProtocolEmv]          = { .parent = NfcProtocolIso14443_4a, .children = NULL },
    [NfcProtocolSrix]         = { .parent = NfcProtocolInvalid, .children = NULL },
    [NfcProtocolJewel]        = { .parent = NfcProtocolInvalid, .children = NULL },
};
```

---

## 3. Core Library (`lib/nfc/`)

```
lib/nfc/
├── nfc.h / nfc.c                 — Top-level NFC context (worker thread, event loop)
├── nfc_scanner.h / nfc_scanner.c — Probe protocols, detect card type
├── nfc_poller.h / nfc_poller.c   — Poller lifecycle (activate, exchange, deactivate)
├── nfc_listener.h / nfc_listener.c — Listener (emulation) lifecycle
├── nfc_device.h / nfc_device.c   — Device data (UID, protocol, save/load)
├── nfc_common.h                  — Shared types (NfcCommand, error codes)
└── protocols/
    ├── nfc_protocol.h / .c       — Protocol enum, parent/child tree, protocol names
    ├── nfc_poller_defs.c         — Poller base → protocol dispatch table
    ├── nfc_listener_defs.c       — Listener base → protocol dispatch table
    ├── nfc_device_defs.c         — Device base → protocol dispatch table
    ├── nfc_poller_base.h         — NfcPollerBase interface (methods vtable)
    ├── nfc_listener_base.h       — NfcListenerBase interface (methods vtable)
    ├── nfc_device_base.h         — NfcDeviceBase interface (methods vtable)
    └── nfc_generic_event.h       — Generic event type for inter-layer messaging
```

### 3.1 `nfc.c` — Top-level context

The central coordinator managing one session at a time. Key components:

```c
struct Nfc {
    NfcWorkerThread* worker_thread;       // Dedicated RTOS thread
    NfcMode mode;                         // POLL, LISTEN, or IDLE
    FuriMessageQueue* event_queue;         // Event queue for user interaction
    NfcScanner* scanner;                   // Scanner instance
    NfcPoller* poller;                     // Poller instance
    NfcListener* listener;                 // Listener instance
    NfcDeviceCommonData* common_data;      // Shared device data
};
```

**Worker thread** runs in a loop:
```
nfc_worker() →
  └─ nfc_worker_poll()     (if mode == POLL)
      ├─ nfc_scanner_run()           # detect card
      ├─ nfc_poller_run()            # read card data
      └─ nfc_worker_poller_ready_handler()  # yield to GUI
  └─ nfc_worker_listen()   (if mode == LISTEN)
```

The worker yields to GUI via `furi_thread_yield()` on `NfcCommandContinue` to prevent GUI thread starvation (a known watchdog bug fixed 2026-05-26).

### 3.2 `nfc_scanner.c` — Protocol detection

The scanner iterates through registered protocols in a probe order and detects which protocol(s) a card supports.

**Array initialization:**
```c
#define NFC_SCANNER_PROTOCOL_MAX NfcProtocolNum  // 17 protocols

const NfcProtocol nfc_default_probe_order[] = {
    // Base protocols first
    NfcProtocolIso14443_3a,
    NfcProtocolIso14443_3b,
    NfcProtocolIso15693_3,
    NfcProtocolFelica,
    NfcProtocolSt25tb,
    NfcProtocolSrix,
    NfcProtocolJewel,  // added 2026-05-26
};

// On PN532 build, scanner auto-detects ISO14443-3A and branches
// to a PN532-specific probe order for child protocols:
const NfcProtocol pn532_probe_order[] = {
    NfcProtocolIso14443_3b,
    NfcProtocolFelica,
    NfcProtocolJewel,
};
```

**Detection algorithm:**
```
nfc_scanner_run():
  1. Detect base protocol via furi_hal_nfc_scanner_detect()
     → Returns NfcProtocolIso14443_3a (most common)
  2. If ISO14443-3A: detect SAK → identify child protocol
  3. If no base detected → try next base protocol
  4. Returns NfcProtocolInvalid if nothing found
```

### 3.3 `nfc_poller.c` — Reading card data

After scanner identifies the protocol, poller actuates the full poller lifecycle per protocol:

```c
struct NfcPoller {
    NfcProtocol protocol;
    NfcPollerBase* poller_base;     // Protocol-specific impl
    PollerMode mode;                  // SINGLE, LISTEN
    NfcPollerCallback callback;      // Event callback
    void* callback_context;
};
```

The poller mode determines whether it returns card data (SINGLE) or continues listening for multiple transactions (LISTEN).

### 3.4 `nfc_listener.c` — Card emulation

Listener mode emulates a card. On PN532 build, listener is disabled (`FURI_HAL_NFC_PN532_ONLY` flag) because PN532 cannot be both initiator and target on the same I2C bus.

```c
struct NfcListener {
    NfcProtocol protocol;
    NfcListenerBase* listener_base;
};
```

### 3.5 `nfc_device.c` — Data persistence

Device data contains UID, protocol, and protocol-specific data in a union/pointer:

```c
struct NfcDevice {
    NfcProtocol protocol;
    uint8_t* uid;
    size_t uid_len;
    NfcDeviceCommonData* common_data;    // Shared (future use)
    NfcDeviceData protocol_data;          // Tagged union per protocol
};
```

Supports save/load to `.nfc` files (Flipper File Format) via `nfc_device_save()` and `nfc_device_load()`.

### 3.6 Interface vtable pattern

Each protocol implements 3 interfaces declared as vtable structs:

```c
typedef struct {
    bool (*all)(...);   // all: alloc, free, reset, copy, save, load, get_uid, get_name, is_equal, verify
    // Base-specific methods
} NfcDeviceBase;

typedef struct {
    NfcGenericEvent (*run)(NfcPollerBase* poller, NfcGenericEvent event);
    // Poller-specific methods
} NfcPollerBase;

typedef struct {
    NfcGenericEvent (*run)(NfcListenerBase* listener, NfcGenericEvent event);
    // Listener-specific methods
} NfcListenerBase;
```

Dispatch from generic to protocol-specific via array lookup:

```c
// nfc_poller_defs.c
const NfcPollerBase nfc_poller_protocols[NfcProtocolNum] = {
    [NfcProtocolIso14443_3a]  = iso14443_3a_poller,
    [NfcProtocolMfClassic]    = mf_classic_poller,
    // ... one entry per protocol
};
```

---

## 4. PN532 HAL Layer

```
targets/f7/furi_hal/
├── furi_hal_nfc.h / .c             — High-level NFC HAL (scanner, poller, listener wrappers)
├── furi_hal_nfc_i.h                — Internal HAL state (PN532 context, CRC buffers)
├── furi_hal_nfc_pn532.c            — PN532 exchange_internal(), auth path, poll/listen primitives
├── furi_hal_pn532.h / .c           — Low-level PN532 I2C: write/read registers, frames, ACK
└── furi_hal_resources.h            — Pin macros (PN532_IRQ, etc.)
```

### 4.1 Layer architecture

```
furi_hal_nfc (high-level API)
    │
    ├─ nfc_scanner_detect()         → furi_hal_nfc_pn532_poll_*()
    ├─ nfc_poller_start/stop()
    ├─ nfc_listener_start/stop()
    │
    └→ furi_hal_nfc_pn532 (mid-level)
           │
           ├─ exchange_internal()       ← central dispatch: MIFARE auth vs InDataExchange vs InCommunicateThru
           ├─ poller_read()/write()
           ├─ listener_read()/write()
           │
           └→ furi_hal_pn532 (low-level)
                  │
                  ├─ pn532_write_frame()       ← I2C transaction
                  ├─ pn532_read_ack()           ← ACK polling
                  ├─ pn532_read_response()      ← response read
                  ├─ furi_hal_pn532_in_data_exchange()
                  ├─ furi_hal_pn532_in_communicate_thru()
                  └─ pn532_read_register() / pn532_write_register()   ← CIU reg access
```

### 4.2 `furi_hal_nfc_pn532.c` — Exchange internal

`exchange_internal()` is the central function handling all NFC data exchange:

```
exchange_internal(tx_bytes, tx_len, rx_payload, rx_size, rx_len, add_parity_to_rx, strip_crc_from_tx, timeout_ms)

IF (MIFARE Classic auth — tx is [0x60|0x61, block]):
    1. PN532_TIMEOUT_MF_AUTH_MS = 80ms
    2. CIU config: CIU_RxMode.set(0x00) + CIU_ManualRCV.set(0x10)  ← disable CRC check for auth
    3. furi_delay_us(1000)  ← CIU settle delay (fix added 2026-05-26)
    4. InCommunicateThru with timeout
    5. CIU config restore (re-enable CRC)
    → Returns raw 4-byte nonce or error

ELIF (use_comm_thru — Crypto1-authed MIFARE):
    1. furi_delay_us(1000)  ← mode-transition settle delay (fix added 2026-05-26)
    2. InCommunicateThru (raw frame, no CRC handling)

ELSE (normal InDataExchange):
    1. Add CRC if !strip_crc_from_tx
    2. InDataExchange (PN532 handles CRC + parity internally)
    3. Strip CRC from response if add_parity_to_rx
```

### 4.3 `furi_hal_pn532.c` — Low-level I2C

Key low-level functions:

| Function | Purpose | Timeout |
|---|---|---|
| `pn532_write_frame()` | Write PN532 command frame via I2C | 20ms |
| `pn532_read_ack()` | Read ACK (6 bytes: `00 00 FF 00 FF 00`) | 20ms |
| `pn532_read_response()` | Read response payload | variable |
| `pn532_write_register()` | Write a single CIU register | 20ms |
| `pn532_read_register()` | Read a single CIU register | 20ms |
| `furi_hal_pn532_send_command()` | Full cmd cycle: write + ACK + response | per cmd |
| `furi_hal_pn532_in_data_exchange()` | InDataExchange via PN532 | PN532_TIMEOUT_EXCHANGE_MS (1000ms) |
| `furi_hal_pn532_in_communicate_thru()` | InCommunicateThru raw frame | PN532_TIMEOUT_CMD_MS (250ms) |
| `furi_hal_pn532_in_communicate_thru_timeout()` | InCommunicateThru with custom timeout | caller-specified |
| `furi_hal_pn532_mf_auth_configure_ciu()` | Toggle CIU RxCRC + parity for auth | N/A (register-only) |

### 4.4 CIU registers for MIFARE Classic auth

PN532 internal CIU (Contactless Interface Unit) registers that must be modified for MIFARE Classic auth:

```
CIU_RxMode (0x05):
  - Bit 3 (RxCRC): 1 = hardware CRC check (normal), 0 = no CRC check (auth)
  - Set to 0x00 before auth, restore after

CIU_ManualRCV (0x12):
  - Bit 4: 1 = manual reception (auth), 0 = normal (restore)
  - Set to 0x10 before auth, restore after
```

The CIU register writes are the source of the **logging Heisenbug** (fixed 2026-05-26): without `furi_delay_us(1000)` between CIU config and InCommunicateThru, the CIU state machine was mid-switch when the auth frame went out → timeout 0x01 on all cards.

### 4.5 Timeout constants

```c
#define PN532_TIMEOUT_MS            275     // General command
#define PN532_TIMEOUT_CMD_MS        250     // InCommunicateThru (non-auth)
#define PN532_TIMEOUT_EXCHANGE_MS   1000    // InDataExchange (general)
#define PN532_TIMEOUT_MF_AUTH_MS    80      // MIFARE Classic auth via InCommunicateThru
#define PN532_TIMEOUT_POLL_MS       300     // InListPassiveTarget polling
#define PN532_TIMEOUT_ACK_MS        20      // ACK response
#define PN532_TIMEOUT_WRITE_MS      20      // Register write
```

### 4.6 PN532 vs ST25R3916 paths

The codebase supports two NFC chips via compile-time flag:

| Feature | ST25R3916 (OFW target) | PN532 (DIY build) |
|---|---|---|
| `FURI_HAL_NFC_PN532_ONLY` | not defined | defined |
| Listener (emulation) | full support | error return |
| RF field control | GPIO-managed | no-op (PN532 internal) |
| Field detection | hardware detect | always `true` |
| ISO15693-3 | full | removed PN532 build |
| Slix | full | removed PN532 build |
| ST25TB | full | disabled PN532 build |
| EMV | N/A | enabled PN532 build (2026-05-26) |
| Jewel/Topaz | N/A | added PN532 build (2026-05-26) |

---

## 5. NFC Application (`applications/main/nfc/`)

```
applications/main/nfc/
├── application.fam                — App manifest (entry points, dependencies)
├── nfc_app.h / .c / nfc_app_i.h  — App context, scene lifecycle
├── scenes/
│   ├── nfc_scene.h                — Scene enum
│   ├── nfc_scene_config.h         — Scene handlers array
│   ├── nfc_scene.c                — Scene manager glue
│   ├── nfc_scene_start.c          — Main menu
│   ├── nfc_scene_detect.c         — Card detection display
│   ├── nfc_scene_read.c           — Read card data
│   ├── nfc_scene_read_card_detect.c  — Card type detection during read
│   ├── nfc_scene_mf_classic_dict_attack.c — MIFARE Classic dict attack
│   ├── nfc_scene_mf_ultralight_read_auth.c — MIFARE Ultralight auth
│   ├── nfc_scene_supported_card.c — Plugin-detected card display
│   └── ... (40+ scenes)
└── helpers/
    ├── nfc_supported_cards.h / .c        — Plugin loading + verify cache
    ├── protocol_support/
    │   ├── nfc_protocol_support.h / .c   — Protocol-specific scene dispatch
    │   ├── nfc_protocol_support_gui_common.h — Shared GUI helpers
    │   ├── mf_classic/
    │   ├── mf_ultralight/
    │   ├── iso14443_3a/
    │   ├── iso14443_4a/
    │   ├── felica/
    │   ├── emv/
    │   ├── jewel/                         — Added 2026-05-26
    │   └── ... (one dir per protocol)
```

### 5.1 Scene system

The NFC app uses a scene-based state machine. Each scene has:
- `on_enter()` — setup UI + start work
- `on_event()` — handle events (user input, NFC events)
- `on_exit()` — cleanup

Scenes are defined in `nfc_scene_config.h` as an array of `{on_enter, on_event, on_exit}` handlers indexed by `NfcScene` enum.

**Scene flow for reading a card:**
```
NfcSceneStart → NfcSceneRead → NfcSceneDetect → NfcSceneReadCardDetect
    → [ProtocolSceneRead] → NfcSceneReadMenu → NfcSceneCardInfo
```

For MIFARE Classic specifically:
```
NfcSceneReadCardDetect → NfcSceneMfClassicDictAttack → NfcSceneMfClassicData
```

### 5.2 `nfc_supported_cards.c` — Plugin system

The supported cards plugin system loads FAP plugins to detect card-specific apps (e.g., "BIP", "T-Mobile", etc.):

```c
struct NfcSupportedCards {
    CompositeApiResolver* api_resolver;
    NfcSupportedCardsPluginCache_t plugins_cache_arr;  // BT/LL cache
    NfcSupportedCardsLoadState load_state;
    NfcSupportedCardsLoadContext* load_context;
    size_t consecutive_fails;
};

bool nfc_supported_cards_read(NfcSupportedCards* instance, NfcDevice* device, Nfc* nfc);
```

**Plugin verify flow:**
```
nfc_supported_cards_read():
  1. Load plugin list (from SD/apps/NFC/)
  2. Call each plugin's verify() with the detected protocol data
  3. Plugin sends auth commands → MIFARE Classic auth → timeout = wrong card
  4. After 3 consecutive verify failures → bail out (configurable via NFC_SUPPORTED_CARDS_MAX_VERIFY_FAILS=3)
  5. If plugin verifies → call its read() for full data
```

**Protections:**
- `SUPPORTED_CARD_PLUGIN_TIMEOUT_MS` = 2000ms per plugin
- `SUPPORTED_CARD_TOTAL_TIMEOUT_MS` = 5000ms total for all plugins
- `consecutive_fails` counter resets on successful verify
- `nfc_supported_cards_reset()` clears fail counter for new sessions

**Plugin cache** (LRU, 64 entries, 5s TTL, added 2026-05-26) caches plugin metadata (name, icon) to avoid re-opening `.fap` files on every archive listing.

### 5.3 `protocol_support` — Protocol-specific scene dispatch

Each protocol has subdirectory with protocol-specific scene handlers:

```c
// nfc_protocol_support.h
typedef struct {
    NfcProtocol protocol;
    bool (*read)(NfcProtocolSupportSceneRead, ...);
    bool (*emulate)(NfcProtocolSupportSceneEmulate, ...);
    // ... per scene hook
} NfcProtocolSupportBase;
```

Dispatch by protocol:
```c
const NfcProtocolSupportBase nfc_protocol_support_bases[NfcProtocolNum] = {
    [NfcProtocolIso14443_3a]  = iso14443_3a_support,
    [NfcProtocolMfClassic]    = mf_classic_support,
    [NfcProtocolMfUltralight] = mf_ultralight_support,
    [NfcProtocolFelica]       = felica_support,
    [NfcProtocolJewel]        = jewel_support,
    // ...
};
```

### 5.4 Application manifest (`application.fam`)

Defines app ID, entry points, plugin FAPs, and dependencies:

```python
App(
    appid="nfc",
    apptype=FlipperAppType.MAIN,
    entry_point="nfc_app",
    targets=["f7"],
    requires=["gui", "storage", "notification"],
    fap_plugins=[
        "nfc_plugin_supported_card_bip",
        # ... 40+ plugin FAPs
    ],
)

# Per-protocol plugins (fal_embedded=True for protocol support)
App(
    appid="nfc_jewel",
    targets=["f7"],
    apptype=FlipperAppType.PLUGIN,
    entry_point="nfc_jewel_ep",
    requires=["nfc"],
    sources=["helpers/protocol_support/jewel/*.c"],
    fal_embedded=True,
)
```

---

## 6. Protocol Implementations

### 6.1 ISO14443-3A (`lib/nfc/protocols/iso14443_3a/`)

**Role:** Base protocol — anticollision, SDD (Select Device Descriptor), SAK (Select Acknowledge)

**Detection:** PN532 `InListPassiveTarget` with BrTy=0x00 returns:
- 4/7-byte UID
- SAK byte (determines child protocol)
- ATQA bytes

**SAK → Protocol mapping:**
| SAK | Protocol |
|---|---|
| 0x00 | ISO14443-4A (Type 4 Tag) |
| 0x08 | MIFARE Classic 1K |
| 0x09 | MIFARE Classic Mini |
| 0x18 | MIFARE Classic 4K |
| 0x10 | MIFARE Plus |
| 0x20 | ISO14443-4A (→ EMV or T4T) |
| 0x38 | MIFARE Ultralight |
| 0x44 | NTAG 4xx (under MfClassic) |

**Files:**
```
iso14443_3a.c           — Core protocol data (UID, SAK, ATQA, manufacturer byte)
iso14443_3a.h
iso14443_3a_poller.c    — Poll FSM: Idle → Collision → Activate → Activated
iso14443_3a_poller_i.h  — Internal poller state
iso14443_3a_listener.c  — Emulation (disabled on PN532)
```

### 6.2 ISO14443-4A (`lib/nfc/protocols/iso14443_4a/`)

**Role:** Child protocol — frame chaining, R-block, WTX, DESELECT handling

**Activation:** After ISO14443-3A select, sends RATS (Request for Answer to Select):
- RATS → ATS (Answer to Select)
- ATS contains: FSCI (Frame Size for CID), TA(1), TB(1), TC(1)
- PPS (Protocol and Parameter Selection) negotiates baud rate and frame size

**Full chaining** (implemented 2026-05-16, enhanced 2026-05-30): supports I-block chaining, R-block (NAK/ACK), WTX (Waiting Time eXtension).

**Chaining details:**
- **Card-side (receive):** R(ACK) loop in `furi_hal_nfc_pn532.c` assembles fragmented I-block responses into a single buffer. Max reassembly size: 1024 bytes (`ISO14443_4_MAX_APDU_SIZE`).
- **Poller-side (transmit):** When outgoing INF > 255 bytes, `furi_hal_nfc_pn532_tx_chained()` splits into multiple I-blocks with CHAIN=1. Card responds with R(ACK) between fragments. Final fragment has CHAIN=0.
- **Max APDU size:** 1024 bytes (defined in `iso14443_4_layer.h` as `ISO14443_4_MAX_APDU_SIZE`).
- **PN532 buffers:** `scratch[]` and `rx_buffer[]` in `furi_hal_nfc_pn532.c` are sized to `ISO14443_4_MAX_APDU_SIZE` (was `PN532_MAX_FRAME_SIZE` = 192).

**Files:**
```
iso14443_4a.c               — Data types
iso14443_4a.h
iso14443_4a_poller.c        — FSM: Idle → RATS → WaitATS → PPS → Ready
iso14443_4a_poller_i.h
iso14443_4a_listener.c      — Emulation (disabled on PN532)
```

### 6.3 MIFARE Classic (`lib/nfc/protocols/mf_classic/`)

**Role:** Child of ISO14443-3A — most complex protocol

**Features:**
- Crypto1 authentication (7-byte UID, 2-key sectors of 4 blocks each)
- Dict attack (tries ~10000 keys via plugins)
- NTAG 4xx as child protocol (SAK=0x44)

**Files:**
```
mf_classic.c                     — Data types (keys, sectors, blocks, auth state)
mf_classic.h
mf_classic_poller.c              — Poller FSM (37 states!)
mf_classic_poller.h
mf_classic_poller_i.c / .h       — Internal poller state (auth, sector read)
mf_classic_listener.c            — Emulation (disabled on PN532)
mf_classic_sync.h / .c           — Sync API for FAPs (blocking read)
```

**Key functions on PN532:**
- Auth uses InCommunicateThru with CIU register toggle (see §4.2, §4.4)
- Backdoor auth via `furi_hal_pn532_mf_backdoor_auth()` — standalone `send_command(0x42)`
- Nonces use partial CRC check on PN532 (disabled RxCRC during auth)
- `fail_retry_count` (5x limit, added 2026-05-26) prevents infinite Fall→DetectType loop

### 6.4 MIFARE Ultralight (`lib/nfc/protocols/mf_ultralight/`)

**Role:** Child of ISO14443-3A — simple memory read/write

**Features:**
- 16-byte pages, simple READ/WRITE commands
- Optional 3DES authentication (pass-through to InCommunicateThru)
- Unlock for NTAG 4xx via sector 0xF0

**Detection:** Sends READ (0x30, block=4). If timeout → not Ultralight.

**Files:**
```
mf_ultralight.c              — Data types (pages, pages_locked, auth_state)
mf_ultralight.h
mf_ultralight_poller.c       — FSM: Idle → ReadPages → ...
mf_ultralight_listener.c     — Emulation (disabled on PN532)
```

### 6.5 MIFARE Plus & DESFire

- **MIFARE Plus** (`mf_plus/`): Child of ISO14443-3A, SAK=0x10. Security Level 1/2/3.
- **MIFARE DESFire** (`mf_desfire/`): Child of ISO14443-4A. Native ISO14443-4 with ISO-DEP frames.

### 6.6 FeliCa (`lib/nfc/protocols/felica/`)

**Role:** Base protocol — NFC Type F

**Detection:** PN532 `InListPassiveTarget` with BrTy=0x01 returns:
- 8-byte IDm (Manufacturer ID)
- 8-byte PMm (Manufacturer Parameter)

**Polling:** Uses FeliCa polling command (0x00FF) to detect cards.

**Files:**
```
felica.c              — Data types
felica.h
felica_poller.c       — FSM: Idle → Poll → ReadSuccess/ReadFailed
felica_listener.c     — Emulation (disabled on PN532)
```

### 6.7 Jewel/Topaz (`lib/nfc/protocols/jewel/`) — Added 2026-05-26

**Role:** Base protocol — simple 128-byte memory

**Detection:** PN532 `InListPassiveTarget` with BrTy=0x04 returns:
- HR0, HR1 (Header bytes)
- 4-byte UID
- ATQA=0x000C

**Data:** RID read (HR0, HR1, 4-byte UID) + READ_ALL (full memory dump via command `0x0A`). READ_ALL accepts 48-128 byte responses (Topaz 512/1024 compatibility). Short dumps are zero-padded.

**Files:**
```
jewel.c              — Data types (HR0, HR1, uid, ATQA, dump[128])
jewel.h
jewel_poller.c       — FSM: Idle → ReadBlock0 → ReadAllBlocks → WriteBlocks → Success/Failed
jewel_poller_i.c     — READ_ALL implementation (command 0x0A, retry, partial read handling)
jewel_sync.h / .c    — Sync API for FAPs
```

No listener (PN532 cannot emulate Jewel).

### 6.8 EMV (`lib/nfc/protocols/emv/`)

**Role:** Child of ISO14443-4A — payment card detection

**Detection:** SAK=0x20 → ISO14443-4A → EMV poller detects via PPSE SELECT (2PAY.SYS.DDF01).

**Files:**
```
emv.c              — Data types (AID, transaction data)
emv.h
emv_poller.c       — FSM: Idle → PPSE → SelectAID → ReadRecords
```

### 6.9 NTAG 4xx (`lib/nfc/protocols/ntag4xx/`)

**Role:** Child of MIFARE Classic — special unlock/pwd handling

**Features:**
- SAK=0x44 detection
- Password auth for NTAG 4xx series
- Special sector 0xF0 unlock mechanism

**Files:**
```
ntag4xx.c              — Data types
ntag4xxc.h
ntag4xx_poller.c       — FSM extends MfClassic poller
```

### 6.10 ISO15693-3 & SLIX

**Role:** Vicinity cards (REMOVED from PN532 build 2026-05-26 — no PN532 native support, always times out)

**Files:**
```
iso15693_3/            — Base: poller FSM + listener
slix/                  — Child: SLIX-specific features
```

### 6.11 ST25TB & SRIX

- **ST25TB** (`st25tb/`): Base protocol, disabled in PN532 build
- **SRIX** (`srix/`): Base protocol, uses InCommunicateThru for 0x06/0x0B/0x0E commands. Standalone calls — no CIU reg writes.

---

## 7. Device Detection Flow

### 7.1 Full read flow (typical — MIFARE Classic card)

```
User: NFC App → "Read NFC" button
     ↓
nfc_app.c: Enter NfcSceneRead scene
     ↓
nfc_scene_read.c: Set mode to POLL, start Nfc worker thread
     ↓
nfc.c: nfc_worker_poll()
     ↓
nfc_scanner_run():
  1. Call furi_hal_nfc_scanner_detect()
     → furi_hal_nfc_pn532.c: InListPassiveTarget(BrTy=0x00: ISO14443-3A)
     → Returns: UID=xxx, SAK=0x08, ATQA=0x0004
  2. Parse SAK=0x08 → MIFARE Classic 1K
  3. Return NfcProtocolMfClassic
     ↓
nfc.c: Set device protocol to MfClassic
     ↓
nfc_poller_run(NfcProtocolMfClassic):
  1. nfc_poller_detect() → ISO14443-3A anticollision
  2. mf_classic_poller_run() → Start poller FSM
     FSM: Idle → Collision → Activate → Auth → ReadSectors → Completed
     ↓
  MfClassic auth (via furi_hal_nfc_pn532 exchange_internal):
    1. CIU config: RxCRC off, parity off
    2. furi_delay_us(1000)
    3. InCommunicateThru([0x60, block], timeout=80ms)
    4. CIU restore
    5. Parse 4-byte nonce → Crypto1 authenticate
    6. Read sector blocks, repeat for all 16 sectors
     ↓
nfc.c: Device data populated → scene transition
     ↓
nfc_supported_cards_read(): Try plugins (BIP, etc.)
     → 3 verify fails → plugin bail-out
     ↓
nfc_scene_mf_classic_dict_attack.c: Try known keys from dictionary
     → If success → read key A/B for all sectors
     ↓
nfc_scene_mf_classic_data.c: Display card data
```

### 7.2 Scanner probe order (PN532 build)

```
furi_hal_nfc_pn532_poll():
  1. Detect with BrTy=0x00 (ISO14443-3A)
     → If found: return NfcProtocolIso14443_3a + UID
  2. If not found: try BrTy=0x01 (FeliCa)
     → If found: return NfcProtocolFelica
  3. If not found: try BrTy=0x04 (Jewel/Topaz, added 2026-05-26)
     → If found: return NfcProtocolJewel
  4. If nothing found: return NfcProtocolInvalid
```

Once base protocol determined, child protocol detection:
```
For ISO14443-3A:
  → SAK byte maps to specific child:
     SAK=0x08 → MfClassic 1K
     SAK=0x09 → MfClassic Mini
     SAK=0x10 → MfPlus
     SAK=0x18 → MfClassic 4K
     SAK=0x20 → ISO14443-4A (→ EMV or Type4Tag)
     SAK=0x38 → MfUltralight
     SAK=0x44 → NTAG 4xx (→ MfClassic)
  
  Stack detection:
    1. Try MfUltralight detection → READ(0x30) timeout? → skip
    2. Try MfClassic → auth? → SAK=0x08 → classic
    3. Try NTAG 4xx → SAK=0x44 → try auth/unlock
    4. Try ISO14443-4A → SAK=0x20 → RATS/ATS → EMV or T4T
```

---

## 8. Authentication & Security

### 8.1 MIFARE Classic Crypto1

The PN532 does NOT have internal Crypto1 engine (unlike NXP reader ICs). All Crypto1 auth is done **client-side** in firmware:

```
1. CIU config: disable RxCRC, manual receive
2. furi_delay_us(1000) ← CIU settle
3. InCommunicateThru([0x60|0x61, block]) → PN532 sends auth frame
4. Card responds with 4-byte nonce (nt)
5. Client firmware: run Crypto1 LFSR with nt + key → generate ks1, ks2
6. CIU restore: re-enable RxCRC, normal receive
7. Subsequent read/write: InCommunicateThru with Crypto1-encrypted frames
```

### 8.2 Dictionary attack

```
nfc_scene_mf_classic_dict_attack.c:
  1. Load dictionary from SD (user_keys.nfc + internal dict)
  2. For each key:
     a. Try auth via hal_pn532_mf_backdoor_auth()
     b. If success: split into sectors, read data
     c. Cache successful keys for faster sector reads
  3. Cache backdoor results (added 2026-05-26): once a key works for block 0,
     mark it as valid for all blocks in that sector — skip retry
```

### 8.3 UID & card verification

- `nfc_device_verify()`: verify loaded device matches data via protocol-specific comparison
- Plugin verify: send protocol-specific auth command, check response
- `nfc_supported_cards_reset()`: clear consecutive failure counter for retry

---

## 9. Key Design Patterns

### 9.1 Worker thread pattern

NFC uses dedicated RTOS thread for all I2C operations:

```c
// nfc.c
struct NfcWorkerThread {
    FuriThread* thread;
    FuriMessageQueue* queue;
    NfcWorkerState state;  // IDLE, READY, POLL, LISTEN
};
```

Worker thread owns the PN532 session. Application layer communicates via events.

### 9.2 Event-driven FSM

Each poller is a finite state machine driven by events:

```c
// Example: iso14443_3a_poller.c FSM
static Iso14443_3aPollerEvent iso14443_3a_poller_process(Iso14443_3aPoller* poller) {
    switch(poller->state) {
    case Iso14443_3aPollerStateIdle:
        // Send REQA → detect card
        poller->state = Iso14443_3aPollerStateCollision;
        return Iso14443_3aPollerEventDetected;
    case Iso14443_3aPollerStateCollision:
        // Anticollision loop → get UID
        poller->state = Iso14443_3aPollerStateActivate;
        return Iso14443_3aPollerEventUIDReady;
    case Iso14443_3aPollerStateActivate:
        // SELECT → get SAK
        poller->state = Iso14443_3aPollerStateActivated;
        return Iso14443_3aPollerEventActivated;
    default:
        return Iso14443_3aPollerEventError;
    }
}
```

### 9.3 Interface vtable dispatch

Each protocol implements three vtables:

```c
const NfcDeviceBase mf_classic_device = {
    .alloc = mf_classic_alloc,
    .free = mf_classic_free,
    .reset = mf_classic_reset,
    .copy = mf_classic_copy,
    .save = mf_classic_save,
    .load = mf_classic_load,
    .get_uid = mf_classic_get_uid,
    .set_uid = mf_classic_set_uid,
    .get_name = mf_classic_get_name,
    .is_equal = mf_classic_is_equal,
    .verify = mf_classic_verify,
};
```

Dispatch table arrays (in `*_defs.c` files) map protocol enum → vtable:

```c
const NfcDeviceBase nfc_device_protocols[NfcProtocolNum] = {
    [NfcProtocolIso14443_3a] = iso14443_3a_device,
    [NfcProtocolMfClassic]   = mf_classic_device,
    // ...
};
```

### 9.4 CRC handling

PN532 handles CRC differently based on exchange mode:

| Mode | Tx CRC | Rx CRC strip |
|---|---|---|
| InDataExchange | PN532 appends CRC | PN532 strips CRC (unless parity-only) |
| InCommunicateThru | Client must set `strip_crc_from_tx` | Client must set `add_parity_to_rx` |
| Auth (CIU mode) | CIU disabled = no CRC | CIU disabled = raw bytes |

**BUG FIX (2026-05-26):** NTAG READ via InCommunicateThru had double-CRC bug. `InCommunicateThru` returns card data WITH native CRC. `prepare_rx()` appended ANOTHER CRC → 18≠16 bytes → protocol error. Fixed by `!add_parity_to_rx && !use_comm_thru` guard in CRC append logic.

### 9.5 Cache patterns

| Cache | Location | Purpose | TTL |
|---|---|---|---|
| Menu apps cache | `loader_menu.c` | `.mainmenu_apps.txt` from SD | 30s |
| FAP metadata cache | `archive_browser.c` | `.fap` file name/icon | 5s (LRU, 64 entries) |
| Plugin verify cache | `nfc_supported_cards.c` | Plugin metadata | session |
| Dict key cache | `nfc_dict.c` | Dictionary keys LRU | session |

---

## 10. Known Limitations

### 10.1 PN532 hardware limitations

1. **No listener mode** — PN532 cannot be target on I2C bus. All `furi_hal_nfc_listener_*()` return error.
2. **No RF field control** — PN532 manages field internally. `poller_field_on/off()` are no-ops. `field_is_present` always returns `true`.
3. **No hardware RST pin** — I2C bus desync is fatal; retry logic used instead of bus reset.
4. **270-byte read on every I2C response** — Inefficient but minor perf impact (read unused trailing bytes).
5. **I2C bus share** with OLED display (SH1106/SSD1306) + PCF8574 I/O expander — contention causing occasional ViewPort lockup warnings.
6. **CIU settle timing** (fixed 2026-05-26) — `furi_delay_us(1000)` required after CIU register writes before InCommunicateThru. Without delay: timeout 0x01 on all MIFARE Classic + NTAG detections. Affects MF Classic auth and NTAG/MfUltralight READ via InCommunicateThru.
   - Audit result: 9 InCommunicateThru call sites examined. Only 2 needed settle delay (both fixed). 5 SRIX sites clean (no CIU reg writes). `in_communicate_thru_bits` writes `CIU_BitFraming` (serial-level, not CIU pipeline) with zero callers. `mf_backdoor_auth` uses raw `send_command(0x42)` path with no CIU reg writes.

### 10.2 Build limitations

1. **Protocols disabled on PN532 build:** ISO15693-3, Slix (no native PN532 vicinity support)
2. **Protocols absent:** U2F (removed from `main_apps` metapackage 2026-05-26, HAL driver stays ~2-4KB)
3. **ISO-DEP max APDU:** 1024 bytes (`ISO14443_4_MAX_APDU_SIZE`). Larger responses are truncated.

### 10.3 Firmware size (as of 2026-05-30)

| Component | Size |
|---|---|
| firmware.bin | 851,968 B (832.0 KB) |
| Free flash (860KB) | 28.0 KB |
| Jewel/Topaz protocol | ~1.2 KB |
| EMV protocol | ~7.8 KB |

---

## Appendix A: Key File Index

| File | Purpose |
|---|---|
| `lib/nfc/nfc.c` | Top-level NFC session manager, worker thread |
| `lib/nfc/nfc_scanner.c` | Protocol probe order and detection |
| `lib/nfc/nfc_poller.c` | Generic poller lifecycle |
| `lib/nfc/nfc_listener.c` | Generic listener lifecycle (disabled on PN532) |
| `lib/nfc/nfc_device.c` | Device data persistence (save/load) |
| `lib/nfc/protocols/nfc_protocol.c` | Protocol tree, parent/child relationships |
| `lib/nfc/protocols/nfc_poller_defs.c` | Poller dispatch table |
| `lib/nfc/protocols/nfc_listener_defs.c` | Listener dispatch table |
| `lib/nfc/protocols/nfc_device_defs.c` | Device dispatch table |
| `lib/nfc/protocols/nfc_poller_base.h` | Poller interface definition |
| `lib/nfc/protocols/nfc_listener_base.h` | Listener interface definition |
| `lib/nfc/protocols/nfc_device_base.h` | Device interface definition |
| `lib/nfc/protocols/nfc_generic_event.h` | Generic event type |
| `targets/f7/furi_hal/furi_hal_nfc.c` | High-level NFC HAL (wrapper) |
| `targets/f7/furi_hal/furi_hal_nfc_i.h` | Internal HAL state |
| `targets/f7/furi_hal/furi_hal_nfc_pn532.c` | PN532 exchange_internal, auth, poll/listen primitives |
| `targets/f7/furi_hal/furi_hal_pn532.c` | Low-level PN532 I2C driver |
| `targets/f7/furi_hal/furi_hal_pn532.h` | PN532 register definitions, error codes, timeout constants |
| `applications/main/nfc/nfc_app.c` | NFC app context, scene manager |
| `applications/main/nfc/nfc_app_i.h` | App internal state |
| `applications/main/nfc/helpers/nfc_supported_cards.c` | Plugin loading, verify + cache |
| `applications/main/nfc/helpers/protocol_support/nfc_protocol_support.c` | Protocol-specific scene dispatch |

## Appendix B: Key Constants

| Constant | Value | Context |
|---|---|---|
| `NfcProtocolNum` | 17 | Total supported protocols |
| `NFC_SCANNER_PROTOCOL_MAX` | NfcProtocolNum | Max probe targets |
| `NFC_SUPPORTED_CARDS_MAX_VERIFY_FAILS` | 3 | Plugin bail-out threshold |
| `SUPPORTED_CARD_PLUGIN_TIMEOUT_MS` | 2000 | Per-plugin timeout |
| `SUPPORTED_CARD_TOTAL_TIMEOUT_MS` | 5000 | Total plugin timeout |
| `PN532_TIMEOUT_MF_AUTH_MS` | 80 | MF Classic auth timeout |
| `PN532_TIMEOUT_EXCHANGE_MS` | 1000 | InDataExchange timeout |
| `PN532_TIMEOUT_CMD_MS` | 250 | InCommunicateThru timeout |
| `PN532_TIMEOUT_POLL_MS` | 300 | Card polling timeout |
| `PN532_MAX_FRAME_SIZE` | 192 | Max PN532 frame payload |
| `MENU_CACHE_TTL_MS` | 30000 | Menu file cache TTL |
| `fail_retry_count` | 5 | MF Classic poller retry limit |

---

*Document generated from source code audit — `lib/nfc/`, `targets/f7/furi_hal/`, `applications/main/nfc/` — as of 2026-05-26.*
