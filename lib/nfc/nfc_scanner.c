#include "nfc_scanner.h"
#include "nfc_poller.h"

#include <nfc/protocols/nfc_poller_defs.h>
#include <nfc/nfc_device.h>
#include <furi_hal_nfc_pn532.h>

#include <furi/furi.h>

#define TAG                                            "NfcScanner"
#define NFC_SCANNER_IDLE_DELAY_MS                      300
/* DIY board shares I2C1 with display + PCF8574; raise the rapid-scan delay
 * from 50ms to 100ms to cut bus contention by ~50% during empty rounds.
 * On reference boards (ST25R3916/SPI) this is a non-issue. */
#define NFC_SCANNER_IDLE_DELAY_REDUCED_MS              100
#define NFC_SCANNER_IDLE_REDUCE_THRESHOLD              3
/* Progressive backoff thresholds for extended idle scanning.
 * After NFC_SCANNER_IDLE_REDUCE_THRESHOLD empty rounds, quick_poll activates.
 * Delay then scales up progressively to reduce I2C bus stress on PN532:
 *   rounds 3-4:   300ms
 *   rounds 5-14:  500ms
 *   rounds 15-29: 1000ms
 *   round 30+:    2000ms (cap)
 * This prevents PN532 I2C timeout cascade after ~28s of continuous polling. */
#define NFC_SCANNER_IDLE_DELAY_PROGRESSIVE_THRESHOLD_1 5
#define NFC_SCANNER_IDLE_DELAY_PROGRESSIVE_THRESHOLD_2 15
#define NFC_SCANNER_IDLE_DELAY_PROGRESSIVE_THRESHOLD_3 30
#define NFC_SCANNER_IDLE_DELAY_STEP1_MS                500
#define NFC_SCANNER_IDLE_DELAY_STEP2_MS                1000
#define NFC_SCANNER_IDLE_DELAY_MAX_MS                  2000
/* Quick-poll false-positive debounce: number of consecutive InListPassiveTarget
 * hits required before treating a detected target as genuine (not RF noise).
 * RF noise bursts rarely produce consistent hits across multiple polls separated
 * by escalating delays.  Each miss resets the count.
 * Set to 5 (was 3): field testing showed 84% false-positive rate at threshold=3
 * on the unshielded DIY board.  5 consecutive hits at 50ms intervals adds ~250ms
 * latency on genuine card presentation but eliminates noise-driven full probe rounds. */
#define NFC_SCANNER_QP_CONFIRM_THRESHOLD               5

/* Base delay (ms) for progressive backoff between quick_poll confirmations.
 * Each successive hit multiplies this: hit#1 = 50ms, hit#2 = 100ms, hit#3+ = 200ms.
 * This reduces I2C bus stress on the PN532 during spurious RF noise events. */
#define NFC_SCANNER_QP_CONFIRM_DELAY_BASE 50

/* Maximum consecutive empty rounds before transitioning to complete state.
 * Prevents indefinite PN532 probing (PN532 I2C timeout cascade at ~28 rounds).
 * Each round ≈ 1s; 30 rounds ≈ 60s gives user time to present card while
 * leaving margin before PN532 hardware crash. */
#define NFC_SCANNER_MAX_EMPTY_ROUNDS 30

/* Maximum number of confirmed quick-poll cycles that resulted in empty
 * full-probe rounds before transitioning to complete state.
 *
 * Persistent RF noise false positives cause an infinite oscillation:
 *   3 skip-path quick_polls → confirm → 3 full-probe rounds (empty) → repeat
 * consecute_empty_scans oscillates 0→3→0→3 and never hits MAX_EMPTY_ROUNDS.
 * This cap breaks the loop: after N confirmed-then-empty cycles, give up.
 * 6 cycles × ~3s/cycle ≈ 18s before cap fires. */
#define NFC_SCANNER_QP_MAX_EMPTY_CONFIRMS 6

/* Yield between protocol probes so the display/UI thread can refresh.
 * 10ms is sufficient to let the GUI thread process a frame and input events. */
#define NFC_SCANNER_INTER_PROBE_YIELD_MS 50
/* Delay chunk size for interruptible waits: split long delays into
 * smaller chunks, checking session_state between each so nfc_scanner_stop()
 * doesn't block for the full delay duration. */
#define NFC_SCANNER_DELAY_CHUNK_MS       10

typedef enum {
    NfcScannerStateIdle,
    NfcScannerStateTryBasePollers,
    NfcScannerStateFindChildrenProtocols,
    NfcScannerStateDetectChildrenProtocols,
    NfcScannerStateComplete,

    NfcScannerStateNum,
} NfcScannerState;

typedef enum {
    NfcScannerSessionStateIdle,
    NfcScannerSessionStateActive,
    NfcScannerSessionStateStopRequest,
} NfcScannerSessionState;

struct NfcScanner {
    Nfc* nfc;
    NfcScannerState state;
    NfcScannerSessionState session_state;

    NfcScannerCallback callback;
    void* context;

    NfcEvent nfc_event;

    NfcProtocol first_detected_protocol;

    size_t base_protocols_num;
    size_t base_protocols_idx;
    NfcProtocol base_protocols[NfcProtocolNum];

    size_t detected_base_protocols_num;
    NfcProtocol detected_base_protocols[NfcProtocolNum];

    size_t children_protocols_num;
    size_t children_protocols_idx;
    NfcProtocol children_protocols[NfcProtocolNum];

    size_t detected_protocols_num;
    NfcProtocol detected_protocols[NfcProtocolNum];

    NfcProtocol current_protocol;

    uint8_t consecutive_empty_scans;
    bool type_a_no_target;

    /* Cached SAK from detection phase (PN532 only).
     * The target may become invalid between detection and children enumeration
     * (after InRelease in set_mode), so we cache the SAK here while the target
     * is still valid.  Used by find_children_protocols() to determine ISO-DEP-only
     * status even when furi_hal_nfc_pn532_target_is_valid() returns false. */
    uint8_t cached_sak;
    bool cached_sak_valid;

    /* Quick-poll false-positive tracking (PN532 only).
     * When quick_poll returns true but subsequent full protocol probes detect
     * nothing, we increment this counter.  Logged at regular intervals and
     * on scan completion for false-positive rate analysis. */
    uint32_t qp_hits;
    uint32_t qp_false_positives;

    /* Quick-poll consecutive hit counter for false-positive debounce.
     * quick_poll must return true QP_CONFIRM_THRESHOLD times in a row before
     * we treat the target as genuine.  Each miss (quick_poll returns false)
     * resets this to 0.  Progressive delay between confirmation polls reduces
     * I2C bus stress during RF noise events. */
    uint32_t qp_consecutive_hits;

    /* Recent confirm flag and empty-confirm total.
     * Set true in the confirmed-hit path.  Cleared in the bottom-of-round
     * block when a full round completes with no detection.  Increments
     * qp_empty_confirms on each such cycle.  When it exceeds
     * NFC_SCANNER_QP_MAX_EMPTY_CONFIRMS, transitions to Complete state
     * to break out of the false-positive false-positive-infinite loop. */
    bool qp_recent_confirm;
    uint32_t qp_empty_confirms;

    FuriThread* scan_worker;
};

static void nfc_scanner_reset(NfcScanner* instance) {
    instance->state = NfcScannerStateIdle;
    instance->base_protocols_idx = 0;
    instance->base_protocols_num = 0;

    instance->children_protocols_idx = 0;
    instance->children_protocols_num = 0;

    instance->detected_protocols_num = 0;
    instance->detected_base_protocols_num = 0;

    instance->current_protocol = 0;
    instance->consecutive_empty_scans = 0;
    instance->type_a_no_target = false;

    instance->cached_sak = 0;
    instance->cached_sak_valid = false;
    instance->qp_hits = 0;
    instance->qp_false_positives = 0;
    instance->qp_consecutive_hits = 0;
    instance->qp_recent_confirm = false;
    instance->qp_empty_confirms = 0;
}

/** Compute progressive backoff delay based on consecutive empty scan count.
 *
 *  After NFC_SCANNER_IDLE_REDUCE_THRESHOLD empty rounds, delay scales up
 *  progressively to NFC_SCANNER_IDLE_DELAY_MAX_MS, reducing I2C bus stress
 *  on the PN532 during extended idle scanning.
 */
static uint32_t nfc_scanner_compute_idle_delay(uint32_t consecutive_empty_scans) {
    if(consecutive_empty_scans < NFC_SCANNER_IDLE_REDUCE_THRESHOLD) {
        return NFC_SCANNER_IDLE_DELAY_REDUCED_MS;
    }
    if(consecutive_empty_scans < NFC_SCANNER_IDLE_DELAY_PROGRESSIVE_THRESHOLD_1) {
        return NFC_SCANNER_IDLE_DELAY_MS;
    }
    if(consecutive_empty_scans < NFC_SCANNER_IDLE_DELAY_PROGRESSIVE_THRESHOLD_2) {
        return NFC_SCANNER_IDLE_DELAY_STEP1_MS;
    }
    if(consecutive_empty_scans < NFC_SCANNER_IDLE_DELAY_PROGRESSIVE_THRESHOLD_3) {
        return NFC_SCANNER_IDLE_DELAY_STEP2_MS;
    }
    return NFC_SCANNER_IDLE_DELAY_MAX_MS;
}

typedef void (*NfcScannerStateHandler)(NfcScanner* instance);

static bool nfc_scanner_is_iso_dep_only_card(const NfcScanner* instance) {
    if(!furi_hal_nfc_pn532_is_active()) return false;

    uint8_t sak = 0;
    bool have_sak = false;

    if(instance->cached_sak_valid) {
        sak = instance->cached_sak;
        have_sak = true;
    } else if(furi_hal_nfc_pn532_target_is_valid()) {
        sak = furi_hal_nfc_pn532_get_sak();
        have_sak = true;
    }

    return have_sak && ((sak & 0x20U) != 0U) && ((sak & 0x18U) == 0U);
}

void nfc_scanner_state_handler_idle(NfcScanner* instance) {
    for(size_t i = 0; i < NfcProtocolNum; i++) {
        /* Skip protocols with no registered poller (NULL on PN532-only builds). */
        if(nfc_pollers_api[i] == NULL) {
            continue;
        }
        NfcProtocol parent_protocol = nfc_protocol_get_parent(i);
        if(parent_protocol == NfcProtocolInvalid) {
            instance->base_protocols[instance->base_protocols_num] = i;
            instance->base_protocols_num++;
        }
    }

    if(furi_hal_nfc_pn532_is_active()) {
        static const NfcProtocol pn532_probe_order[] = {
            NfcProtocolIso14443_3a,
            NfcProtocolIso14443_3b,
            NfcProtocolFelica,
            NfcProtocolJewel,
            NfcProtocolSrix, // SRIX uses InCommunicateThru — proven working
            // ISO15693-3, Slix, ST25TB omitted — no PN532 native support,
            // always timeout wasting ~400ms per round.
        };

        size_t write_idx = 0;
        for(size_t i = 0; i < COUNT_OF(pn532_probe_order); i++) {
            const NfcProtocol ordered_protocol = pn532_probe_order[i];
            for(size_t j = 0; j < instance->base_protocols_num; j++) {
                if(instance->base_protocols[j] == ordered_protocol) {
                    instance->base_protocols[write_idx++] = ordered_protocol;
                    break;
                }
            }
        }
        instance->base_protocols_num = write_idx;
        FURI_LOG_W(TAG, "PN532 active - best-effort probe order enabled");
    }

    FURI_LOG_D(TAG, "Found %zu base protocols", instance->base_protocols_num);

    instance->first_detected_protocol = NfcProtocolInvalid;
    instance->state = NfcScannerStateTryBasePollers;
}

void nfc_scanner_state_handler_try_base_pollers(NfcScanner* instance) {
    if(instance->base_protocols_num == 0) {
        instance->state = NfcScannerStateComplete;
        return;
    }

    instance->current_protocol = instance->base_protocols[instance->base_protocols_idx];

    if(instance->first_detected_protocol == instance->current_protocol) {
        instance->state = NfcScannerStateFindChildrenProtocols;
        return;
    }

    /* Quick presence check — at the start of each new round (idx == 0),
     * enter energy-saving skip mode after NFC_SCANNER_IDLE_REDUCE_THRESHOLD
     * consecutive empty rounds.  Instead of individually polling all 4 base
     * protocols (~1s/round), do a single fast 50ms InListPassiveTarget poll.
     *
     * IMPORTANT: Do NOT gate this on !furi_hal_nfc_quick_poll().  On PN532
     * the InListPassiveTarget can return false-positive targets from RF noise,
     * making the skip condition always false and causing the scanner to cycle
     * full protocol probes indefinitely until the PN532 dies from I2C stress
     * (~28 rounds ≈ 28s).  Instead, call quick_poll INSIDE the skip path and
     * use its result only to reset the idle counter when a genuine card appears. */
    if(furi_hal_nfc_pn532_is_active() && instance->base_protocols_idx == 0 &&
       instance->consecutive_empty_scans >= NFC_SCANNER_IDLE_REDUCE_THRESHOLD &&
       instance->detected_base_protocols_num == 0) {
        instance->type_a_no_target = false;

        /* Check if a real card appeared, with false-positive debounce.
         * quick_poll must return true QP_CONFIRM_THRESHOLD times in a row
         * before we reset the idle counter.  Between confirmations, apply
         * a progressive delay (50ms, 100ms, 200ms) instead of immediately
         * triggering a full round of expensive protocol probes (~1s).
         * This prevents RF noise false-positives from stressing the PN532
         * with repeated full-probe rounds. */
        if(furi_hal_nfc_quick_poll()) {
            instance->qp_hits++;
            instance->qp_consecutive_hits++;

            if(instance->qp_consecutive_hits >= NFC_SCANNER_QP_CONFIRM_THRESHOLD) {
                FURI_LOG_D(
                    TAG,
                    "Quick poll confirmed [hit #%lu, %u consecutive], resetting idle counter",
                    (unsigned long)instance->qp_hits,
                    (unsigned)instance->qp_consecutive_hits);
                instance->consecutive_empty_scans = 0;
                instance->qp_consecutive_hits = 0;
                instance->qp_recent_confirm = true;
                return;
            }

            /* Not yet confirmed — apply progressive backoff delay.
             * Each successive hit increases delay: 50ms, 100ms, 200ms...
             * This reduces I2C bus stress on PN532 during spurious RF noise
             * and gives the debounce time to self-correct. */
            uint32_t delay_ms =
                (uint32_t)instance->qp_consecutive_hits * NFC_SCANNER_QP_CONFIRM_DELAY_BASE;
            FURI_LOG_D(
                TAG,
                "Quick poll tentative [hit #%lu, %u/%u], backoff %lums",
                (unsigned long)instance->qp_hits,
                (unsigned)instance->qp_consecutive_hits,
                (unsigned)NFC_SCANNER_QP_CONFIRM_THRESHOLD,
                (unsigned long)delay_ms);
            while(delay_ms > 0 && instance->session_state == NfcScannerSessionStateActive) {
                uint32_t chunk = MIN(delay_ms, (uint32_t)NFC_SCANNER_DELAY_CHUNK_MS);
                furi_delay_ms(chunk);
                delay_ms -= chunk;
            }
            return;
        }

        /* Quick poll missed (no RF target) — reset consecutive hit counter */
        instance->qp_consecutive_hits = 0;

        /* No card — cap check: transition after MAX_EMPTY_ROUNDS */
        instance->consecutive_empty_scans++;
        if(instance->consecutive_empty_scans >= NFC_SCANNER_MAX_EMPTY_ROUNDS) {
            FURI_LOG_I(
                TAG,
                "Max empty rounds (%u) reached in skip path, stopping scan (QP stats: %lu hits, %lu FP)",
                (unsigned)NFC_SCANNER_MAX_EMPTY_ROUNDS,
                (unsigned long)instance->qp_hits,
                (unsigned long)instance->qp_false_positives);
            instance->state = NfcScannerStateComplete;
            return;
        }

        /* No card — delay with progressive backoff to reduce I2C bus stress */
        uint32_t delay_ms = nfc_scanner_compute_idle_delay(instance->consecutive_empty_scans);
        while(delay_ms > 0 && instance->session_state == NfcScannerSessionStateActive) {
            uint32_t chunk = MIN(delay_ms, (uint32_t)NFC_SCANNER_DELAY_CHUNK_MS);
            furi_delay_ms(chunk);
            delay_ms -= chunk;
        }
        return;
    }

    /* Skip B/FeliCa when Type A already failed this round, no card detected,
     * and we're still in the first 2 rapid-scan rounds.  After 2 empty rounds,
     * always probe B/FeliCa at least once to detect rare Type B/FeliCa-only cards.
     * Tradeoff: rounds 1-2 skip B/FeliCa (~400ms saved per round), round 3 does
     * a full scan, catching B/FeliCa within ~1.3s worst case. */
    bool skip_poll = false;
    if(instance->type_a_no_target && instance->detected_base_protocols_num == 0 &&
       instance->consecutive_empty_scans < 2 && instance->current_protocol == NfcProtocolFelica) {
        /* Skip FeliCa only on rapid-scan rounds — FeliCa is rare (~400ms/poll).
         * ISO14443B is always probed to detect ATM/bank cards on first tap.
         * After 2 empty rounds, FeliCa is also probed. */
        skip_poll = true;
        FURI_LOG_D(
            TAG,
            "Skipping FeliCa (Type A failed, fast scan round %d)",
            instance->consecutive_empty_scans);
    }

    if(!skip_poll) {
        /* Reset Type A tracking when starting a fresh Type A poll */
        if(instance->current_protocol == NfcProtocolIso14443_3a) {
            instance->type_a_no_target = false;
        }

        NfcPoller* poller = nfc_poller_alloc(instance->nfc, instance->current_protocol);
        bool protocol_detected = nfc_poller_detect(poller);
        nfc_poller_free(poller);

        /* PN532 absent check: if the PN532 was active when we entered this
         * round but is no longer active after the probe (TX failure + failed
         * reinit), terminate the scan immediately.  Without this check the
         * scanner burns through all MAX_EMPTY_ROUNDS (~12s) with every
         * nfc_config() call failing, producing a flood of
         * "Detect: nfc_config failed" errors and wasting time. */
        if(!furi_hal_nfc_pn532_is_active() && instance->base_protocols_num > 0) {
            FURI_LOG_W(
                TAG,
                "PN532 became absent during scan (protocol %u), stopping scan",
                instance->current_protocol);
            instance->state = NfcScannerStateComplete;
            return;
        }

        /* Yield between probes so the display/UI thread can refresh.
         * On the DIY board this prevents I2C1 starvation of the OLED. */
        if(furi_hal_nfc_pn532_is_active()) {
            furi_delay_ms(NFC_SCANNER_INTER_PROBE_YIELD_MS);
        }

        if(protocol_detected) {
            instance->detected_protocols[instance->detected_protocols_num] =
                instance->current_protocol;
            instance->detected_protocols_num++;

            instance->detected_base_protocols[instance->detected_base_protocols_num] =
                instance->current_protocol;
            instance->detected_base_protocols_num++;

            /* Cache the SAK while the target is still valid (PN532 only).
             * After InRelease in set_mode, target_is_valid() may return false,
             * but we need the SAK in find_children_protocols() to determine
             * ISO-DEP-only status.  Cache it now while we can. */
            if(furi_hal_nfc_pn532_is_active() && furi_hal_nfc_pn532_target_is_valid()) {
                instance->cached_sak = furi_hal_nfc_pn532_get_sak();
                instance->cached_sak_valid = true;
                FURI_LOG_D(TAG, "Cached SAK=0x%02X from detection phase", instance->cached_sak);
            }

            if(instance->first_detected_protocol == NfcProtocolInvalid) {
                instance->first_detected_protocol = instance->current_protocol;
                instance->current_protocol = NfcProtocolInvalid;
            }

            /* PN532 fast path: once any base protocol is detected, skip probing
             * the remaining base protocols (B, FeliCa).  The PN532 can only
             * activate one card at a time, so probing extras just wastes ~400ms
             * per protocol with guaranteed timeouts. */
            if(furi_hal_nfc_pn532_is_active()) {
                instance->base_protocols_idx = 0; /* reset so next line sets state correctly */
                instance->state = NfcScannerStateFindChildrenProtocols;
                return;
            }
        } else if(instance->current_protocol == NfcProtocolIso14443_3a) {
            instance->type_a_no_target = true;
        } else if(furi_hal_nfc_pn532_is_active()) {
            FURI_LOG_D(
                TAG,
                "PN532 probe protocol %u: %s",
                instance->current_protocol,
                furi_hal_nfc_pn532_last_result_str());
        }
    }

    instance->base_protocols_idx =
        (instance->base_protocols_idx + 1) % instance->base_protocols_num;

    if(instance->base_protocols_idx == 0 && instance->detected_base_protocols_num == 0) {
        /* Reset per-round flag for next cycle */
        instance->type_a_no_target = false;

        /* Track quick-poll false positives: if we completed a full round
         * with zero detections and had a quick_poll hit, that hit was a
         * false positive (no genuine card present). */
        if(instance->qp_hits > instance->qp_false_positives) {
            instance->qp_false_positives++;
            FURI_LOG_D(
                TAG,
                "QP false-positive #%lu (hits=%lu FP=%lu rate=%.0f%%)",
                (unsigned long)instance->qp_false_positives,
                (unsigned long)instance->qp_hits,
                (unsigned long)instance->qp_false_positives,
                (double)((float)instance->qp_false_positives * 100.0f / (float)instance->qp_hits));
        }

        /* Track confirmed-then-empty cycles: if a recent quick_poll confirm
         * was followed by a full round with zero detections, increment the
         * empty-confirms counter.  Persistent false positives cause an
         * infinite oscillation (consecutive_empty_scans never reaches cap)
         * — this secondary cap breaks the loop. */
        if(instance->qp_recent_confirm) {
            instance->qp_recent_confirm = false;
            instance->qp_empty_confirms++;
            if(instance->qp_empty_confirms >= NFC_SCANNER_QP_MAX_EMPTY_CONFIRMS) {
                FURI_LOG_I(
                    TAG,
                    "QP max empty confirms (%u) reached, resetting debounce counters "
                    "(hits=%lu FP=%lu empty_confirms=%lu)",
                    (unsigned)NFC_SCANNER_QP_MAX_EMPTY_CONFIRMS,
                    (unsigned long)instance->qp_hits,
                    (unsigned long)instance->qp_false_positives,
                    (unsigned long)instance->qp_empty_confirms);
                /* Reset debounce state — do NOT terminate. The user may still be
                 * trying to present a card. The MAX_EMPTY_ROUNDS cap will terminate
                 * the scan if no card is ever detected. */
                instance->qp_empty_confirms = 0;
                instance->qp_consecutive_hits = 0;
                instance->qp_recent_confirm = false;
            }
        }

        /* Dynamic timing: progressive backoff with reduced initial delay,
          * scaling up over extended idle periods to reduce I2C bus stress. */
        uint32_t delay_ms = nfc_scanner_compute_idle_delay(instance->consecutive_empty_scans);
        instance->consecutive_empty_scans++;
        while(delay_ms > 0 && instance->session_state == NfcScannerSessionStateActive) {
            uint32_t chunk = MIN(delay_ms, (uint32_t)NFC_SCANNER_DELAY_CHUNK_MS);
            furi_delay_ms(chunk);
            delay_ms -= chunk;
        }
    } else if(instance->detected_base_protocols_num > 0) {
        /* Reset counter when any protocol is detected */
        instance->consecutive_empty_scans = 0;
    } else {
        /* Cap: transition to complete after MAX_EMPTY_ROUNDS consecutive
         * empty rounds.  Prevents indefinite PN532 I2C hammering that
         * leads to response timeout cascade after ~28 rounds (~28s). */
        if(instance->consecutive_empty_scans >= NFC_SCANNER_MAX_EMPTY_ROUNDS) {
            FURI_LOG_I(
                TAG,
                "Max empty rounds (%u) reached, stopping scan (QP stats: %lu hits, %lu FP)",
                (unsigned)NFC_SCANNER_MAX_EMPTY_ROUNDS,
                (unsigned long)instance->qp_hits,
                (unsigned long)instance->qp_false_positives);
            instance->state = NfcScannerStateComplete;
        }
    }
}

void nfc_scanner_state_handler_find_children_protocols(NfcScanner* instance) {
    /* Check if detected card supports ISO-DEP (ISO14443-4).
     * SAK bit 5 = 0x20 indicates ISO-DEP capability.
     * Non-ISO-DEP cards (e.g. NTAG, MIFARE Classic) cannot activate
     * ISO14443-4A children (MfDesfire, MfPlus, NTAG4xx, Type4Tag, EMV),
     * so skip probing them to save ~275ms. */
    bool sak_has_iso_dep = false;
    /* ISO-DEP-only flag: SAK=0x20 with no MIFARE bits (0x08, 0x10).
     * Bank/ATM cards typically have SAK=0x20.  On such cards the PN532
     * has already entered ISO-DEP mode; sending MIFARE/UL/NTAG-type
     * frames (which are raw ISO14443-3A exchanges) will always timeout
     * because the card is not listening on that layer anymore.  Skip
     * all non-4A children to save ~900ms of guaranteed timeouts.
     */
    bool sak_is_iso_dep_only = false;

    /* Use cached SAK from detection phase if available (PN532 only).
     * The target may become invalid between detection and children enumeration
     * (after InRelease in set_mode), so furi_hal_nfc_pn532_target_is_valid()
     * may return false here.  The cached SAK was captured while the target
     * was still valid during try_base_pollers(). */
    if(furi_hal_nfc_pn532_is_active()) {
        uint8_t sak = 0;
        bool have_sak = false;

        if(instance->cached_sak_valid) {
            sak = instance->cached_sak;
            have_sak = true;
            FURI_LOG_D(TAG, "Using cached SAK=0x%02X for children filtering", sak);
        } else if(furi_hal_nfc_pn532_target_is_valid()) {
            sak = furi_hal_nfc_pn532_get_sak();
            have_sak = true;
            FURI_LOG_D(TAG, "Using live SAK=0x%02X for children filtering", sak);
        }

        if(have_sak) {
            sak_has_iso_dep = (sak & 0x20) != 0;
            /* SAK bit 6 set (0x20) but no MIFARE Classic bits (bit 4=0x08 or bit 5=0x10):
             * this is a pure ISO-DEP card (bank card, SAK=0x20). */
            if((sak & 0x20) && !(sak & 0x18)) {
                sak_is_iso_dep_only = true;
            }
            if(sak_is_iso_dep_only) {
                FURI_LOG_D(
                    TAG,
                    "SAK=0x%02X: ISO-DEP only card (e.g. bank card), skipping non-4A children",
                    sak);
            }
        }
    }

    for(size_t i = 0; i < NfcProtocolNum; i++) {
        /* Skip protocols with no registered poller (e.g. EMV/SRIX/ST25TB
         * are NULL on PN532-only builds — nfc_poller_alloc would crash). */
        if(nfc_pollers_api[i] == NULL) {
            continue;
        }
        for(size_t j = 0; j < instance->detected_base_protocols_num; j++) {
            if(!nfc_protocol_has_parent(i, instance->detected_base_protocols[j])) {
                continue;
            }
            /* Skip 4A children on non-ISO-DEP cards (SAK bit 5 = 0) */
            if(!sak_has_iso_dep && nfc_protocol_has_parent(i, NfcProtocolIso14443_4a)) {
                continue;
            }
            /* Skip non-4A children (MfClassic, MfUltralight, NTAG, etc.) on
             * ISO-DEP-only cards: these require raw 14443-3A MIFARE frames
             * that an ISO-DEP-mode card (bank card) will never respond to. */
            if(sak_is_iso_dep_only && !nfc_protocol_has_parent(i, NfcProtocolIso14443_4a)) {
                continue;
            }
            /* Belt-and-suspenders: explicitly exclude MfClassic on ISO-DEP-only cards
             * regardless of parent chain analysis.  This guards against edge cases where
             * nfc_protocol_has_parent() might return unexpected results for MfClassic
             * (e.g. if MfClassic's parent chain is modified in the future).
             * Only applies on PN532 builds to avoid affecting ST25R3916. */
            if(sak_is_iso_dep_only && (i == NfcProtocolMfClassic)) {
                FURI_LOG_D(
                    TAG,
                    "Explicit MfClassic exclusion: SAK is ISO-DEP-only, skipping protocol %zu",
                    i);
                continue;
            }
            instance->children_protocols[instance->children_protocols_num] = i;
            instance->children_protocols_num++;
        }
    }

    /* Prefer EMV first on ISO-DEP-only cards.  On PN532 the other ISO14443-4A
     * children are expensive probes; a positive EMV detect should usually end
     * the scan immediately. */
    if(sak_is_iso_dep_only && instance->children_protocols_num > 1) {
        for(size_t i = 1; i < instance->children_protocols_num; i++) {
            if(instance->children_protocols[i] == NfcProtocolEmv) {
                NfcProtocol tmp = instance->children_protocols[0];
                instance->children_protocols[0] = NfcProtocolEmv;
                instance->children_protocols[i] = tmp;
                FURI_LOG_D(TAG, "SAK=0x20: EMV moved to front of children probe order");
                break;
            }
        }
    }

    if(instance->children_protocols_num > 0) {
        instance->state = NfcScannerStateDetectChildrenProtocols;
    } else {
        instance->state = NfcScannerStateComplete;
    }
    FURI_LOG_D(TAG, "Found %zu children", instance->children_protocols_num);
}

void nfc_scanner_state_handler_detect_children_protocols(NfcScanner* instance) {
    furi_assert(instance->children_protocols_num);

    instance->current_protocol = instance->children_protocols[instance->children_protocols_idx];

    NfcPoller* poller = nfc_poller_alloc(instance->nfc, instance->current_protocol);
    bool protocol_detected = nfc_poller_detect(poller);
    nfc_poller_free(poller);

    /* PN532 MfUltralight/NTAG detection:
     * Removed retry logic for clone PN532 — re-poll after failed detection
     * sends more commands to a potentially corrupted PN532 state, causing
     * InDataExchange collision cascade (status=0x06) and ViewPort lockup.
     * Clone PN532 handles re-activation internally via InListPassiveTarget
     * with MxRtyPassiveActivation=0x02, which is sufficient. */

    if(protocol_detected) {
        instance->detected_protocols[instance->detected_protocols_num] =
            instance->current_protocol;
        instance->detected_protocols_num++;

        if(instance->current_protocol == NfcProtocolEmv &&
           nfc_scanner_is_iso_dep_only_card(instance)) {
            FURI_LOG_D(TAG, "EMV confirmed on ISO-DEP-only card, stopping child probe early");
            instance->state = NfcScannerStateComplete;
            return;
        }
    }

    /* Yield between child probes so I2C1 bus can settle. On DIY board,
     * PN532 and PCF8574 share I2C1 — no yield = bus contention. */
    if(furi_hal_nfc_pn532_is_active()) {
        furi_delay_ms(NFC_SCANNER_INTER_PROBE_YIELD_MS);
    }

    instance->children_protocols_idx++;
    if(instance->children_protocols_idx == instance->children_protocols_num) {
        instance->state = NfcScannerStateComplete;
    }
}

static void nfc_scanner_filter_detected_protocols(NfcScanner* instance) {
    size_t filtered_protocols_num = 0;
    NfcProtocol filtered_protocols[NfcProtocolNum] = {};

    for(size_t i = 0; i < instance->detected_protocols_num; i++) {
        bool is_parent = false;
        for(size_t j = i + 1; j < instance->detected_protocols_num; j++) {
            is_parent = nfc_protocol_has_parent(
                instance->detected_protocols[j], instance->detected_protocols[i]);
            if(is_parent) break;
        }
        if(!is_parent) {
            filtered_protocols[filtered_protocols_num] = instance->detected_protocols[i];
            filtered_protocols_num++;
        }
    }

    instance->detected_protocols_num = filtered_protocols_num;
    memcpy(
        instance->detected_protocols,
        filtered_protocols,
        filtered_protocols_num * sizeof(NfcProtocol));
}

void nfc_scanner_state_handler_complete(NfcScanner* instance) {
    if(instance->detected_protocols_num > 1) {
        nfc_scanner_filter_detected_protocols(instance);
    }
    if(furi_hal_nfc_pn532_is_active() && instance->qp_hits > 0) {
        FURI_LOG_I(
            TAG,
            "QP false-positive rate: %lu/%lu (%.0f%%)",
            (unsigned long)instance->qp_false_positives,
            (unsigned long)instance->qp_hits,
            (double)((float)instance->qp_false_positives * 100.0f / (float)instance->qp_hits));
    }

    FURI_LOG_I(TAG, "Detected %zu protocols", instance->detected_protocols_num);
    for(size_t i = 0; i < instance->detected_protocols_num; i++) {
        FURI_LOG_I(
            TAG, "  [%zu] %s", i, nfc_device_get_protocol_name(instance->detected_protocols[i]));
    }

    NfcScannerEvent event = {
        .type = NfcScannerEventTypeDetected,
        .data =
            {
                .protocol_num = instance->detected_protocols_num,
                .protocols = instance->detected_protocols,
            },
    };

    instance->callback(event, instance->context);
    // Exit the scanner worker after firing the callback once.
    // Without this, Idle→TryBasePollers→detect would restart scanning
    // and fire the callback repeatedly until nfc_scanner_stop() is called.
    instance->session_state = NfcScannerSessionStateStopRequest;
}

static NfcScannerStateHandler nfc_scanner_state_handlers[NfcScannerStateNum] = {
    [NfcScannerStateIdle] = nfc_scanner_state_handler_idle,
    [NfcScannerStateTryBasePollers] = nfc_scanner_state_handler_try_base_pollers,
    [NfcScannerStateFindChildrenProtocols] = nfc_scanner_state_handler_find_children_protocols,
    [NfcScannerStateDetectChildrenProtocols] = nfc_scanner_state_handler_detect_children_protocols,
    [NfcScannerStateComplete] = nfc_scanner_state_handler_complete,
};

static int32_t nfc_scanner_worker(void* context) {
    furi_assert(context);

    NfcScanner* instance = context;
    while(instance->session_state == NfcScannerSessionStateActive) {
        nfc_scanner_state_handlers[instance->state](instance);
    }

    nfc_scanner_reset(instance);

    return 0;
}

NfcScanner* nfc_scanner_alloc(Nfc* nfc) {
    furi_check(nfc);

    NfcScanner* instance = malloc(sizeof(NfcScanner));
    furi_check(instance);
    instance->nfc = nfc;
    instance->state = NfcScannerStateIdle;
    instance->session_state = NfcScannerSessionStateIdle;

    return instance;
}

void nfc_scanner_free(NfcScanner* instance) {
    furi_check(instance);
    furi_check(instance->state == NfcScannerStateIdle);

    free(instance);
}

void nfc_scanner_start(NfcScanner* instance, NfcScannerCallback callback, void* context) {
    furi_check(instance);
    furi_check(callback);
    furi_check(instance->state == NfcScannerStateIdle);
    furi_check(instance->scan_worker == NULL);

    instance->callback = callback;
    instance->context = context;
    instance->session_state = NfcScannerSessionStateActive;
    instance->scan_worker = furi_thread_alloc();
    furi_thread_set_name(instance->scan_worker, "NfcScanWorker");
    furi_thread_set_context(instance->scan_worker, instance);
    /* Bumped from 8K to 12K: MFC dictionary attack and child-protocol probes
     * can recurse deeply into per-protocol state machines.  Field reports of
     * crashes after MIFARE detection trace back to stack-overflow in this
     * worker; the extra 4K is cheap insurance (single-threaded worker). */
    furi_thread_set_stack_size(instance->scan_worker, 12 * 1024);
    furi_thread_set_callback(instance->scan_worker, nfc_scanner_worker);
    furi_thread_start(instance->scan_worker);
}

void nfc_scanner_stop(NfcScanner* instance) {
    furi_check(instance);
    furi_check(instance->scan_worker);

    instance->session_state = NfcScannerSessionStateStopRequest;
    furi_hal_nfc_abort();
    furi_thread_join(instance->scan_worker);
    instance->session_state = NfcScannerSessionStateIdle;

    furi_thread_free(instance->scan_worker);
    instance->scan_worker = NULL;
    instance->callback = NULL;
    instance->context = NULL;
    instance->state = NfcScannerStateIdle;
}
