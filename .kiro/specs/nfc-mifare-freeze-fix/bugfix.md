# Bugfix Requirements Document

## Introduction

Two related NFC bugs observed on a DIY Flipper Zero board (PN532 over I2C, `FURI_HAL_NFC_PN532_ONLY` active):

**Bug 1 — Device freeze when MIFARE Classic 1K/4K card is placed.**
After the NFC scanner detects a MIFARE Classic card (Protocol 7, ATQA=0004, SAK=08), the supported-cards plugin system runs up to 3 consecutive read attempts, each of which times out on PN532 (native auth fails, Crypto1 fallback also times out). After the bail-out threshold is reached, the system proceeds to load 2311 dictionary keys into RAM and then allocates a new MF Classic poller for the dictionary attack. The device freezes at or immediately after the `NfcPoller` allocation for protocol 7 — the log cuts off and the device becomes unresponsive.

**Bug 2 — ATM/multi-protocol card shows protocol selection menu instead of reading data.**
When a multi-protocol card (e.g. ATM/bank card with SAK=0x20, ISO-DEP capable) is placed, the scanner correctly detects ISO14443-3A as the base protocol and then identifies both ISO14443-4A and MfClassic as candidate children. Because both children are detected (or the ISO-DEP exchange fails and MfClassic is also listed), the scanner fires the callback with `protocol_num > 1`. The detect scene then routes to `NfcSceneSelectProtocol` instead of directly to `NfcSceneRead`, presenting the user with a "Read as MF Classic / Read as ISO14443-4A" choice without first attempting a raw data read.

---

## Bug Analysis

### Current Behavior (Defect)

1.1 WHEN a MIFARE Classic 1K or 4K card (ATQA=0004, SAK=08/18) is placed on the PN532 reader AND the key cache is not found THEN the system freezes/crashes after loading the 2311-key system dictionary and allocating the MF Classic poller for the dictionary attack, leaving the device unresponsive

1.2 WHEN the supported-cards plugin system reaches 3 consecutive read failures on a MIFARE Classic card via PN532 (each attempt: native auth fails err=4, Crypto1 fallback times out with `in_communicate_thru status error: 0x01`) THEN the system proceeds to the full dictionary attack without any PN532-specific guard, causing the PN532 I2C bus to be held for >100ms (SPI warning at 235ms) and subsequently freezing during poller allocation

1.3 WHEN a multi-protocol card with SAK=0x20 (ISO-DEP capable, e.g. ATM/bank card) is placed AND the ISO-DEP exchange fails with `in_data_exchange status error: 0x13` THEN the system marks the ISO14443-4 target as invalid but still detects MfClassic via ATQA/SAK, resulting in multiple protocols being reported to the detect scene

1.4 WHEN the NFC detect scene receives `protocol_num > 1` from the scanner callback THEN the system routes to `NfcSceneSelectProtocol` and presents a protocol choice menu to the user without performing any raw data read first

### Expected Behavior (Correct)

2.1 WHEN a MIFARE Classic 1K or 4K card is placed on the PN532 reader AND the key cache is not found THEN the system SHALL proceed to the dictionary attack scene without freezing, displaying the "MF Classic System Directory" progress UI and attempting key authentication

2.2 WHEN the supported-cards plugin bail-out threshold is reached on a PN532 build (3 consecutive read failures) THEN the system SHALL release the I2C bus cleanly and transition to the dictionary attack scene without holding the bus beyond the 100ms SPI limit

2.3 WHEN a multi-protocol card with SAK=0x20 is placed AND ISO-DEP exchange fails on PN532 THEN the system SHALL NOT present both MfClassic and ISO14443-4A as selectable protocols; instead it SHALL select the most appropriate single protocol (MfClassic, based on ATQA/SAK) and proceed directly to `NfcSceneRead`

2.4 WHEN the NFC detect scene receives exactly one viable protocol after PN532-aware filtering THEN the system SHALL route directly to `NfcSceneRead` without showing the protocol selection menu

### Unchanged Behavior (Regression Prevention)

3.1 WHEN a MIFARE Classic card is placed AND a valid key cache exists for its UID THEN the system SHALL CONTINUE TO load the key cache and read the card directly without entering the dictionary attack

3.2 WHEN a genuine multi-protocol card is placed on a non-PN532 (ST25R3916) build AND multiple protocols are successfully detected THEN the system SHALL CONTINUE TO present the protocol selection menu to the user

3.3 WHEN a MIFARE Ultralight, NTAG, or other ISO14443-3A card (non-MfClassic) is placed THEN the system SHALL CONTINUE TO detect and read it correctly without triggering the dictionary attack flow

3.4 WHEN a FeliCa or ISO14443-3B card is placed THEN the system SHALL CONTINUE TO be detected and offered for reading as before

3.5 WHEN the dictionary attack is running and the user presses Skip THEN the system SHALL CONTINUE TO transition to the next dictionary phase (user dict → system dict) or to the read success scene as appropriate

3.6 WHEN the NFC scanner bail-out fires after 3 consecutive plugin verify/read failures THEN the system SHALL CONTINUE TO skip remaining plugins and not attempt further plugin reads for that card presentation

---

## Bug Condition Pseudocode

### Bug 1 — MIFARE Classic Freeze

```pascal
FUNCTION isBugCondition_MfClassicFreeze(X)
  INPUT: X of type NfcCardPresentation
  OUTPUT: boolean

  RETURN X.protocol = MfClassic
     AND X.hardware = PN532_I2C
     AND X.key_cache_found = false
END FUNCTION

// Property: Fix Checking
FOR ALL X WHERE isBugCondition_MfClassicFreeze(X) DO
  result ← handleMfClassicDetection'(X)
  ASSERT device_responsive(result)
    AND dict_attack_scene_entered(result)
    AND i2c_bus_not_held_beyond_limit(result)
END FOR

// Property: Preservation Checking
FOR ALL X WHERE NOT isBugCondition_MfClassicFreeze(X) DO
  ASSERT handleMfClassicDetection(X) = handleMfClassicDetection'(X)
END FOR
```

### Bug 2 — Multi-Protocol Card Protocol Menu

```pascal
FUNCTION isBugCondition_MultiProtocolMenu(X)
  INPUT: X of type NfcCardPresentation
  OUTPUT: boolean

  RETURN X.hardware = PN532_I2C
     AND X.sak = 0x20          // ISO-DEP only, no MF Classic bits
     AND X.iso_dep_exchange_failed = true
END FUNCTION

// Property: Fix Checking
FOR ALL X WHERE isBugCondition_MultiProtocolMenu(X) DO
  result ← handleCardDetection'(X)
  ASSERT NOT protocol_selection_menu_shown(result)
    AND single_protocol_selected(result)
END FOR

// Property: Preservation Checking
FOR ALL X WHERE NOT isBugCondition_MultiProtocolMenu(X) DO
  ASSERT handleCardDetection(X) = handleCardDetection'(X)
END FOR
```
