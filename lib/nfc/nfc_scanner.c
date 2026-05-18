#include "nfc_scanner.h"
#include "nfc_poller.h"

#include <nfc/protocols/nfc_poller_defs.h>
#include <furi_hal_nfc_pn532.h>

#include <furi/furi.h>

#define TAG                               "NfcScanner"
#define NFC_SCANNER_IDLE_DELAY_MS         300
#define NFC_SCANNER_IDLE_DELAY_REDUCED_MS 50
#define NFC_SCANNER_IDLE_REDUCE_THRESHOLD 3
#define NFC_SCANNER_MAX_EMPTY_ROUNDS      3

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
}

typedef void (*NfcScannerStateHandler)(NfcScanner* instance);

void nfc_scanner_state_handler_idle(NfcScanner* instance) {
    for(size_t i = 0; i < NfcProtocolNum; i++) {
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
            // SRIX and ST25TB omitted — PN532 has no native support,
            // they always fail with timeout, wasting ~400ms per round.
            // NfcProtocolIso15693_3 omitted for PN532 (unsupported, causes I2C deadlock)
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
     * do a fast 50ms InListPassiveTarget instead of individually polling
     * all 4 base protocols (~1200ms).  Once a card is detected, this
     * check passes and the normal per-protocol detection runs. */
    if(furi_hal_nfc_pn532_is_active() && instance->base_protocols_idx == 0 &&
       instance->consecutive_empty_scans > 0 && instance->consecutive_empty_scans < 2 &&
       instance->detected_base_protocols_num == 0 && !furi_hal_nfc_quick_poll()) {
        instance->type_a_no_target = false;
        uint32_t delay_ms;
        if(instance->consecutive_empty_scans < NFC_SCANNER_IDLE_REDUCE_THRESHOLD) {
            delay_ms = NFC_SCANNER_IDLE_DELAY_REDUCED_MS;
            instance->consecutive_empty_scans++;
        } else {
            delay_ms = NFC_SCANNER_IDLE_DELAY_MS;
        }
        furi_delay_ms(delay_ms);
        return;
    }

    /* Skip B/FeliCa when Type A already failed this round, no card detected,
     * and we're still in the first 2 rapid-scan rounds.  After 2 empty rounds,
     * always probe B/FeliCa at least once to detect rare Type B/FeliCa-only cards.
     * Tradeoff: rounds 1-2 skip B/FeliCa (~400ms saved per round), round 3 does
     * a full scan, catching B/FeliCa within ~1.3s worst case. */
    bool skip_poll = false;
    if(instance->type_a_no_target && instance->detected_base_protocols_num == 0 &&
       instance->consecutive_empty_scans < 2 &&
       (instance->current_protocol == NfcProtocolIso14443_3b ||
        instance->current_protocol == NfcProtocolFelica)) {
        skip_poll = true;
        FURI_LOG_D(
            TAG,
            "Skipping protocol %d (Type A failed, fast scan round %d)",
            instance->current_protocol,
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

        if(protocol_detected) {
            instance->detected_protocols[instance->detected_protocols_num] =
                instance->current_protocol;
            instance->detected_protocols_num++;

            instance->detected_base_protocols[instance->detected_base_protocols_num] =
                instance->current_protocol;
            instance->detected_base_protocols_num++;

            if(instance->first_detected_protocol == NfcProtocolInvalid) {
                instance->first_detected_protocol = instance->current_protocol;
                instance->current_protocol = NfcProtocolInvalid;
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

        /* Dynamic timing: start with reduced delay, escalate to full
          * delay only after several consecutive empty scan rounds.
          * This gives faster response when a tag is brought near. */
        uint32_t delay_ms;
        if(instance->consecutive_empty_scans < NFC_SCANNER_IDLE_REDUCE_THRESHOLD) {
            delay_ms = NFC_SCANNER_IDLE_DELAY_REDUCED_MS;
            instance->consecutive_empty_scans++;
        } else {
            delay_ms = NFC_SCANNER_IDLE_DELAY_MS;
        }
        furi_delay_ms(delay_ms);
    } else if(instance->detected_base_protocols_num > 0) {
        /* Reset counter when any protocol is detected */
        instance->consecutive_empty_scans = 0;
    }
}

void nfc_scanner_state_handler_find_children_protocols(NfcScanner* instance) {
    /* Check if detected card supports ISO-DEP (ISO14443-4).
     * SAK bit 5 = 0x20 indicates ISO-DEP capability.
     * Non-ISO-DEP cards (e.g. NTAG, MIFARE Classic) cannot activate
     * ISO14443-4A children (MfDesfire, MfPlus, NTAG4xx, Type4Tag, EMV),
     * so skip probing them to save ~275ms. */
    bool sak_has_iso_dep = false;
    if(furi_hal_nfc_pn532_is_active() && furi_hal_nfc_pn532_target_is_valid()) {
        sak_has_iso_dep = (furi_hal_nfc_pn532_get_sak() & 0x20) != 0;
    }

    for(size_t i = 0; i < NfcProtocolNum; i++) {
        for(size_t j = 0; j < instance->detected_base_protocols_num; j++) {
            if(!nfc_protocol_has_parent(i, instance->detected_base_protocols[j])) {
                continue;
            }
            /* Skip 4A children on non-ISO-DEP cards (SAK bit 5 = 0) */
            if(!sak_has_iso_dep && nfc_protocol_has_parent(i, NfcProtocolIso14443_4a)) {
                continue;
            }
            instance->children_protocols[instance->children_protocols_num] = i;
            instance->children_protocols_num++;
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

    if(protocol_detected) {
        instance->detected_protocols[instance->detected_protocols_num] =
            instance->current_protocol;
        instance->detected_protocols_num++;
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
        for(size_t j = i; j < instance->detected_protocols_num; j++) {
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
    FURI_LOG_I(TAG, "Detected %zu protocols", instance->detected_protocols_num);

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
    instance->nfc = nfc;

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
    furi_thread_set_stack_size(instance->scan_worker, 8 * 1024);
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
