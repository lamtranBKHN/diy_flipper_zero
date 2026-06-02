# Bugfix Requirements Document

## Introduction

Three bugs identified from debug log analysis of firmware `copilot/worktree-2026-04-03T07-50-46 mntm-dev (3a9bf9f0-dirty built on 28-05-2026)`. The logs show the NFC app starting, loading supported-card plugins, and running the scanner for ~15 seconds before giving up with 0 protocols detected.

---

## Bug 1 — `quick_poll` timeout too short causes PN532 I2C timeout cascade

### Observed Symptom

```
29368 [W][FuriHalPN532] PN532 wait ready: timeout (ready poller path)
29368 [W][FuriHalPN532] pn532_wait_ready_ms: ACK failed (transient, keeping ready)
29368 [W][FuriHalPN532] pn532_exchange: response timeout (hardware)
29769 [I][FuriHalPN532] InListPassiveTarget nb_targets=1
29826 [W][FuriHalPN532] I2C TX retry 1/3
29916 [W][FuriHalPN532] PN532 wait ready: timeout (ready poller path)
...
```

### Root Cause

`furi_hal_nfc_quick_poll()` in `targets/f7/furi_hal/furi_hal_nfc_pn532.c` calls:

```c
const bool found = furi_hal_pn532_poll_iso14443a_timeout(&target, 50);
```

This passes `timeout_ms=50` to `pn532_exchange()`, which calls `pn532_wait_ready_ms(50)`. The ready poller fires every `PN532_READY_POLL_INTERVAL_MS = 25ms`. However, `InListPassiveTarget` (BrTy=0x00, ISO14443-3A) takes up to `PN532_TIMEOUT_POLL_MS = 350ms` to complete — the PN532 waits for a card to appear before signalling READY. With a 50ms timeout, `pn532_wait_ready_ms()` expires before the PN532 finishes the command, triggering the ACK-failure two-strikes logic and spurious I2C retry warnings.

The subsequent `InListPassiveTarget nb_targets=1` at t=29769 (401ms later) shows the PN532 did eventually respond — the command succeeded, but the timeout had already fired and the response was read on the retry path.

### Current Behavior (Defect)

1.1 WHEN `furi_hal_nfc_quick_poll()` is called AND no card is present, THEN `pn532_wait_ready_ms(50)` times out before `InListPassiveTarget` completes, generating spurious `PN532 wait ready: timeout` and `pn532_exchange: response timeout` warnings.

1.2 WHEN the 50ms timeout fires, THEN `pn532_handle_ack_failure()` is called, incrementing the ACK-failure strike counter and potentially triggering a spurious PN532 reinit if two failures occur within 200ms.

1.3 WHEN the PN532 eventually responds after the timeout, THEN the response is read on the I2C retry path (`I2C TX retry 1/3`), adding unnecessary I2C bus traffic and latency.

### Expected Behavior (Correct)

2.1 WHEN `furi_hal_nfc_quick_poll()` is called, THEN the timeout passed to `pn532_exchange()` SHALL be at least `PN532_TIMEOUT_POLL_MS` (350ms) so the PN532 has time to complete `InListPassiveTarget` before the wait expires.

2.2 WHEN no card is present AND `InListPassiveTarget` completes normally with `nb_targets=0`, THEN no timeout warnings SHALL be generated.

2.3 WHEN a card is present AND `InListPassiveTarget` returns `nb_targets=1`, THEN `quick_poll` SHALL return `true` without any retry or timeout path.

### Unchanged Behavior (Regression Prevention)

3.1 WHEN `quick_poll` is called AND the PN532 is genuinely unresponsive (hardware fault), THEN the existing two-strikes ACK-failure logic SHALL CONTINUE TO detect and handle the fault.

3.2 WHEN `quick_poll` returns `true`, THEN the scanner SHALL CONTINUE TO apply the 3-consecutive-hit debounce before treating the result as a genuine card.

---

## Bug 2 — `poll_iso14443a_timeout` logs at `[I]` level, flooding the log

### Observed Symptom

Every quick-poll call (which fires on every scanner idle round after 3 empty rounds) generates two `[I]` log lines:

```
29769 [I][FuriHalPN532] InListPassiveTarget nb_targets=1
30317 [I][FuriHalPN532] InListPassiveTarget nb_targets=1
30421 [I][FuriHalPN532] InListPassiveTarget nb_targets=1
30577 [I][FuriHalPN532] InListPassiveTarget nb_targets=1
...
```

And at function entry:
```c
FURI_LOG_I(TAG, "poll_iso14443a: ENTER timeout=%lu", timeout_ms);
FURI_LOG_I(TAG, "InListPassiveTarget nb_targets=%u", response[1]);
```

These are in `furi_hal_pn532_poll_iso14443a_timeout()` at lines 1052 and 1072 of `furi_hal_pn532.c`. With the scanner calling `quick_poll` every ~150ms during idle scanning, this generates ~100+ `[I]` log lines per minute, making the log unreadable and adding I2C bus overhead for UART output.

### Current Behavior (Defect)

4.1 WHEN `furi_hal_pn532_poll_iso14443a_timeout()` is called, THEN it logs `"poll_iso14443a: ENTER timeout=%lu"` at `[I]` level on every call.

4.2 WHEN `InListPassiveTarget` returns any result, THEN it logs `"InListPassiveTarget nb_targets=%u"` at `[I]` level on every call, including the common `nb_targets=0` (no card) case.

### Expected Behavior (Correct)

5.1 WHEN `furi_hal_pn532_poll_iso14443a_timeout()` is called, THEN the entry log SHALL use `FURI_LOG_D` (debug level) instead of `FURI_LOG_I`.

5.2 WHEN `InListPassiveTarget` returns `nb_targets=0` (no card), THEN no log SHALL be emitted (or at most `FURI_LOG_D`).

5.3 WHEN `InListPassiveTarget` returns `nb_targets >= 1` (card found), THEN the log SHALL use `FURI_LOG_D` instead of `FURI_LOG_I`.

### Unchanged Behavior (Regression Prevention)

6.1 WHEN `nb_targets > 1` (multiple cards), THEN the existing `FURI_LOG_W` warning SHALL CONTINUE TO be emitted.

6.2 WHEN `response_len < 7` (malformed response), THEN the existing early-return behavior SHALL CONTINUE TO operate unchanged.

---

## Bug 3 — Quick-poll false-positive rate 84% causes premature scan termination

### Observed Symptom

```
42650 [I][NfcScanner] QP max empty confirms (6) reached, stopping scan (hits=19 FP=16 empty_confirms=6)
42760 [I][NfcScanner] QP false-positive rate: 16/19 (84%)
42760 [I][NfcScanner] Detected 0 protocols
```

The scanner ran for ~15.4 seconds (t=27224 to t=42650) and terminated with 0 protocols detected. The `NFC_SCANNER_QP_MAX_EMPTY_CONFIRMS = 6` cap fired after 6 confirmed-then-empty cycles.

### Root Cause Analysis

The 84% false-positive rate is caused by RF noise on the unshielded DIY board causing the PN532 to return `nb_targets=1` from `InListPassiveTarget` without a real card present. This is a known hardware characteristic of the PN532 on I2C without RF shielding.

However, the current `NFC_SCANNER_QP_CONFIRM_THRESHOLD = 3` (3 consecutive hits required) is insufficient to filter this noise. The logs show the PN532 returning `nb_targets=1` on 3 consecutive quick-polls (t=29769, t=30317, t=30421, t=30577), triggering a confirmed hit and a full protocol probe round — which then detects nothing. This cycle repeats 6 times before the `QP_MAX_EMPTY_CONFIRMS` cap fires.

The core issue: the quick-poll debounce threshold of 3 is too low for this hardware environment. With RF noise producing sustained false positives, 3 consecutive hits is easily achieved by noise alone.

Additionally, the `NFC_SCANNER_QP_MAX_EMPTY_CONFIRMS = 6` cap terminates the scan after only ~18 seconds of idle scanning. If a user is trying to scan a card and the RF environment is noisy, the scanner gives up before the user can present the card.

### Current Behavior (Defect)

7.1 WHEN RF noise causes `InListPassiveTarget` to return `nb_targets=1` on 3 consecutive quick-polls, THEN the scanner treats this as a confirmed card presence and runs a full protocol probe round (~1s), even though no card is present.

7.2 WHEN 6 confirmed-then-empty cycles occur, THEN the scanner terminates with `Detected 0 protocols`, even if the user is still trying to present a card.

7.3 WHEN the scanner terminates due to `QP_MAX_EMPTY_CONFIRMS`, THEN the NFC detect scene routes back to `NfcSceneStart` with no card read, providing no feedback to the user about why scanning stopped.

### Expected Behavior (Correct)

8.1 WHEN RF noise causes `InListPassiveTarget` to return `nb_targets=1`, THEN the scanner SHALL require `NFC_SCANNER_QP_CONFIRM_THRESHOLD = 5` (increased from 3) consecutive hits before treating the result as a genuine card presence, reducing false-positive confirmation rate.

8.2 WHEN `NFC_SCANNER_QP_MAX_EMPTY_CONFIRMS` is reached, THEN the scanner SHALL NOT terminate immediately; instead it SHALL reset the `qp_empty_confirms` counter and `qp_consecutive_hits` counter and continue scanning with a longer inter-round delay (2000ms), giving the user more time to present a card.

8.3 WHEN the scanner has been running for more than `NFC_SCANNER_MAX_EMPTY_ROUNDS` rounds with no detection, THEN the scanner SHALL terminate as before (this cap is unchanged).

### Unchanged Behavior (Regression Prevention)

9.1 WHEN a genuine card is present AND `InListPassiveTarget` returns `nb_targets=1` consistently, THEN the scanner SHALL CONTINUE TO detect it within `NFC_SCANNER_QP_CONFIRM_THRESHOLD` consecutive hits.

9.2 WHEN `NFC_SCANNER_MAX_EMPTY_ROUNDS = 12` is reached, THEN the scanner SHALL CONTINUE TO terminate as before.

9.3 WHEN the scanner detects a protocol, THEN the existing detection flow SHALL CONTINUE TO operate unchanged.

---

## Bug Condition Pseudocode

### Bug 1 — Quick-poll timeout too short

```pascal
FUNCTION isBugCondition_QuickPollTimeout(X)
  INPUT: X of type PN532Exchange
  OUTPUT: boolean

  RETURN X.command = InListPassiveTarget
     AND X.timeout_ms = 50
     AND X.actual_response_time > 50
END FUNCTION

// Property: Fix Checking
FOR ALL X WHERE isBugCondition_QuickPollTimeout(X) DO
  result ← furi_hal_nfc_quick_poll'()
  ASSERT no_timeout_warning_generated(result)
    AND no_ack_failure_incremented(result)
END FOR
```

### Bug 3 — Quick-poll false-positive confirmation

```pascal
FUNCTION isBugCondition_FalsePositiveConfirm(X)
  INPUT: X of type ScannerRound
  OUTPUT: boolean

  RETURN X.qp_consecutive_hits >= 3
     AND X.actual_card_present = false
     AND X.full_probe_result = 0
END FUNCTION

// Property: Fix Checking
FOR ALL X WHERE isBugCondition_FalsePositiveConfirm(X) DO
  result ← scanner_quick_poll_path'(X)
  ASSERT NOT full_probe_triggered(result)
    OR qp_consecutive_hits_required >= 5
END FOR
```
