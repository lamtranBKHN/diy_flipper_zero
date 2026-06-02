#include "nfc_poller.h"

#include <nfc/protocols/nfc_poller_defs.h>

#include <furi.h>
#include <furi_hal_nfc.h>

#define TAG "NfcPoller" // <--- Added serial debug TAG

typedef enum {
    NfcPollerSessionStateIdle,
    NfcPollerSessionStateActive,
    NfcPollerSessionStateStopRequest,
} NfcPollerSessionState;

typedef struct NfcPollerListElement {
    NfcProtocol protocol;
    NfcGenericInstance* poller;
    const NfcPollerBase* poller_api;
    struct NfcPollerListElement* child;
} NfcPollerListElement;

typedef struct {
    NfcPollerListElement* head;
    NfcPollerListElement* tail;
} NfcPollerList;

struct NfcPoller {
    NfcProtocol protocol;
    Nfc* nfc;
    NfcPollerList list;
    NfcPollerSessionState session_state;
    bool protocol_detected;

    NfcGenericCallbackEx callback;
    void* context;

    FuriSemaphore* detect_sem;
};

static void nfc_poller_list_alloc(NfcPoller* instance) {
    FURI_LOG_D(TAG, "Allocating poller list for protocol: %d", instance->protocol);
    instance->list.head = malloc(sizeof(NfcPollerListElement));
    furi_check(instance->list.head);
    instance->list.head->protocol = instance->protocol;
    furi_check(nfc_pollers_api[instance->protocol]);
    instance->list.head->poller_api = nfc_pollers_api[instance->protocol];
    instance->list.head->child = NULL;
    instance->list.tail = instance->list.head;
    FURI_LOG_D(TAG, "Head protocol: %d", instance->list.head->protocol);

    do {
        NfcProtocol parent_protocol = nfc_protocol_get_parent(instance->list.head->protocol);
        if(parent_protocol == NfcProtocolInvalid) break;

        NfcPollerListElement* parent = malloc(sizeof(NfcPollerListElement));
        furi_check(parent);
        parent->protocol = parent_protocol;
        parent->poller_api = nfc_pollers_api[parent_protocol];
        parent->child = instance->list.head;
        instance->list.head = parent;
        FURI_LOG_D(TAG, "Added parent protocol: %d", parent_protocol);
    } while(true);

    NfcPollerListElement* iter = instance->list.head;
    iter->poller = iter->poller_api->alloc(instance->nfc);
    FURI_LOG_D(TAG, "Allocated poller for protocol: %d", iter->protocol);

    do {
        if(iter->child == NULL) break;
        iter->child->poller = iter->child->poller_api->alloc(iter->poller);
        iter->poller_api->set_callback(
            iter->poller, iter->child->poller_api->run, iter->child->poller);
        FURI_LOG_D(
            TAG,
            "Allocated poller for protocol: %d, set callback in parent: %d",
            iter->child->protocol,
            iter->protocol);

        iter = iter->child;
    } while(true);
    FURI_LOG_D(TAG, "Finished allocating poller list");
}

static void nfc_poller_list_free(NfcPoller* instance) {
    furi_check(instance);
    if(!instance->list.head) {
        FURI_LOG_W(TAG, "Freeing already-freed poller list, skipping");
        return;
    }
    FURI_LOG_D(TAG, "Freeing poller list");
    do {
        NfcProtocol current_protocol = instance->list.head->protocol;
        instance->list.head->poller_api->free(instance->list.head->poller);
        NfcPollerListElement* child = instance->list.head->child;
        free(instance->list.head);
        FURI_LOG_D(TAG, "Freed poller and element for protocol: %d", current_protocol);
        if(child == NULL) break;
        instance->list.head = child;
    } while(true);
    instance->list.head = NULL;
    FURI_LOG_D(TAG, "Finished freeing poller list");
}

NfcPoller* nfc_poller_alloc(Nfc* nfc, NfcProtocol protocol) {
    furi_check(nfc);
    furi_check(protocol < NfcProtocolNum);

    NfcPoller* instance = malloc(sizeof(NfcPoller));
    furi_check(instance);
    instance->session_state = NfcPollerSessionStateIdle;
    instance->nfc = nfc;
    instance->protocol = protocol;
    nfc_poller_list_alloc(instance);

    instance->detect_sem = furi_semaphore_alloc(1, 0);

    FURI_LOG_D(TAG, "Poller allocated for protocol: %d", protocol);
    return instance;
}

void nfc_poller_free(NfcPoller* instance) {
    furi_check(instance);

    FURI_LOG_D(TAG, "Poller freeing for protocol: %d", instance->protocol);
    furi_semaphore_free(instance->detect_sem);
    nfc_poller_list_free(instance);
    furi_hal_nfc_release_active_target();
    free(instance);
    FURI_LOG_D(TAG, "Poller freed");
}

static NfcCommand nfc_poller_start_callback(NfcEvent event, void* context) {
    furi_assert(context);

    NfcPoller* instance = context;

    NfcCommand command = NfcCommandContinue;
    NfcGenericEvent poller_event = {
        .protocol = NfcProtocolInvalid,
        .instance = instance->nfc,
        .event_data = &event,
    };

    FURI_LOG_D(TAG, "Start callback received event type: %d", event.type);

    if(instance->session_state == NfcPollerSessionStateStopRequest) {
        FURI_LOG_D(TAG, "Stop requested before run, returning NfcCommandStop");
        return NfcCommandStop;
    }

    if(event.type == NfcEventTypePollerReady) {
        NfcPollerListElement* head_poller = instance->list.head;
        command = head_poller->poller_api->run(poller_event, head_poller->poller);
        FURI_LOG_D(
            TAG,
            "PollerReady: starting run on head protocol: %d, command: %d",
            head_poller->protocol,
            command);
    }

    if(instance->session_state == NfcPollerSessionStateStopRequest) {
        command = NfcCommandStop;
        FURI_LOG_D(TAG, "Stop requested, returning NfcCommandStop");
    }

    return command;
}

void nfc_poller_start(NfcPoller* instance, NfcGenericCallback callback, void* context) {
    furi_check(instance);
    furi_check(callback);
    furi_check(instance->session_state == NfcPollerSessionStateIdle);

    FURI_LOG_D(TAG, "Starting poller for protocol: %d (simple mode)", instance->protocol);

    NfcPollerListElement* tail_poller = instance->list.tail;
    tail_poller->poller_api->set_callback(tail_poller->poller, callback, context);

    instance->session_state = NfcPollerSessionStateActive;
    nfc_start(instance->nfc, nfc_poller_start_callback, instance);
    FURI_LOG_D(TAG, "nfc_start called");
}

static NfcCommand nfc_poller_start_ex_tail_callback(NfcGenericEvent event, void* context) {
    furi_assert(context);
    furi_assert(event.protocol != NfcProtocolInvalid);

    NfcPoller* instance = context;
    NfcCommand command = NfcCommandContinue;

    FURI_LOG_D(TAG, "Start Ex tail callback received event protocol: %d", event.protocol);

    NfcGenericEventEx poller_event = {
        .poller = instance->list.tail->poller,
        .parent_event_data = event.event_data,
    };

    command = instance->callback(poller_event, instance->context);

    FURI_LOG_D(TAG, "Start Ex tail callback custom callback command: %d", command);

    return command;
}

static NfcCommand nfc_poller_start_ex_head_callback(NfcEvent event, void* context) {
    furi_assert(context);

    NfcCommand command = NfcCommandContinue;
    NfcPoller* instance = context;

    FURI_LOG_D(TAG, "Start Ex head callback received event type: %d", event.type);

    NfcProtocol parent_protocol = nfc_protocol_get_parent(instance->protocol);

    if(parent_protocol == NfcProtocolInvalid) {
        NfcGenericEventEx poller_event = {
            .poller = instance->list.tail->poller,
            .parent_event_data = &event,
        };

        command = instance->callback(poller_event, instance->context);
        FURI_LOG_D(TAG, "Start Ex: no parent, calling custom callback, command: %d", command);
    } else {
        NfcGenericEvent poller_event = {
            .protocol = NfcProtocolInvalid,
            .instance = instance->nfc,
            .event_data = &event,
        };
        NfcPollerListElement* head_poller = instance->list.head;
        command = head_poller->poller_api->run(poller_event, head_poller->poller);
        FURI_LOG_D(
            TAG,
            "Start Ex: has parent, starting run on head protocol: %d, command: %d",
            head_poller->protocol,
            command);
    }

    if(instance->session_state == NfcPollerSessionStateStopRequest) {
        command = NfcCommandStop;
        FURI_LOG_D(TAG, "Stop requested, returning NfcCommandStop");
    }

    return command;
}

void nfc_poller_start_ex(NfcPoller* instance, NfcGenericCallbackEx callback, void* context) {
    furi_check(instance);
    furi_check(callback);
    furi_check(instance->session_state == NfcPollerSessionStateIdle);

    FURI_LOG_D(TAG, "Starting poller for protocol: %d (extended mode)", instance->protocol);

    instance->callback = callback;
    instance->context = context;

    NfcProtocol parent_protocol = nfc_protocol_get_parent(instance->protocol);
    if(parent_protocol != NfcProtocolInvalid) {
        NfcPollerListElement* iter = instance->list.head;
        while(iter && iter->protocol != parent_protocol)
            iter = iter->child;

        iter->poller_api->set_callback(iter->poller, nfc_poller_start_ex_tail_callback, instance);
        FURI_LOG_D(TAG, "Start Ex: set tail callback on parent protocol: %d", parent_protocol);
    }

    instance->session_state = NfcPollerSessionStateActive;
    nfc_start(instance->nfc, nfc_poller_start_ex_head_callback, instance);
    FURI_LOG_D(TAG, "nfc_start called");
}

void nfc_poller_stop(NfcPoller* instance) {
    furi_check(instance);
    furi_check(instance->nfc);

    FURI_LOG_D(TAG, "Stopping poller for protocol: %d", instance->protocol);
    instance->session_state = NfcPollerSessionStateStopRequest;
    nfc_stop(instance->nfc);
    instance->session_state = NfcPollerSessionStateIdle;
    FURI_LOG_D(TAG, "Poller stopped");
}

static NfcCommand nfc_poller_detect_tail_callback(NfcGenericEvent event, void* context) {
    furi_assert(context);

    NfcPoller* instance = context;
    NfcPollerListElement* tail_poller = instance->list.tail;
    instance->protocol_detected = tail_poller->poller_api->detect(event, tail_poller->poller);
    furi_semaphore_release(instance->detect_sem);

    FURI_LOG_D(
        TAG,
        "Detect tail callback: protocol %d detected: %d",
        tail_poller->protocol,
        instance->protocol_detected);

    return NfcCommandStop;
}

static NfcCommand nfc_poller_detect_head_callback(NfcEvent event, void* context) {
    furi_assert(context);

    NfcPoller* instance = context;
    NfcPollerListElement* tail_poller = instance->list.tail;
    NfcPollerListElement* head_poller = instance->list.head;

    NfcCommand command = NfcCommandContinue;
    NfcGenericEvent poller_event = {
        .protocol = NfcProtocolInvalid,
        .instance = instance->nfc,
        .event_data = &event,
    };

    FURI_LOG_D(TAG, "Detect head callback received event type: %d", event.type);

    if(event.type == NfcEventTypePollerReady) {
        if(tail_poller == head_poller) {
            instance->protocol_detected =
                tail_poller->poller_api->detect(poller_event, tail_poller->poller);
            furi_semaphore_release(instance->detect_sem);
            command = NfcCommandStop;
            FURI_LOG_D(
                TAG,
                "Detect: head == tail, detected: %d, command: %d",
                instance->protocol_detected,
                command);
        } else {
            command = head_poller->poller_api->run(poller_event, head_poller->poller);
            FURI_LOG_D(
                TAG,
                "Detect: starting run on head protocol: %d, command: %d",
                head_poller->protocol,
                command);
        }
    }

    return command;
}

bool nfc_poller_detect(NfcPoller* instance) {
    furi_check(instance);
    furi_check(instance->session_state == NfcPollerSessionStateIdle);

    FURI_LOG_D(TAG, "Starting protocol detection for: %d", instance->protocol);

    instance->protocol_detected = false;
    instance->session_state = NfcPollerSessionStateActive;
    NfcPollerListElement* tail_poller = instance->list.tail;
    NfcPollerListElement* iter = instance->list.head;

    if(tail_poller != instance->list.head) {
        while(iter->child != tail_poller)
            iter = iter->child;
        iter->poller_api->set_callback(iter->poller, nfc_poller_detect_tail_callback, instance);
        FURI_LOG_D(TAG, "Detect: set tail callback on protocol: %d", iter->protocol);
    }

    if(!nfc_is_config_done(instance->nfc)) {
        FURI_LOG_W(
            TAG, "Detect: nfc_config NOT done for protocol %d, aborting", instance->protocol);
        if(tail_poller != instance->list.head) {
            iter->poller_api->set_callback(
                iter->poller, tail_poller->poller_api->run, tail_poller->poller);
        }
        instance->session_state = NfcPollerSessionStateIdle;
        return false;
    }

    /* A previous detect timeout can abort the worker after the caller has
     * already stopped waiting, leaving detect_sem with a stale count of 1.
     * If not drained here, the next detect returns immediately, calls
     * nfc_stop(), and injects an abort into the fresh worker's first PN532
     * exchange ("pn532_exchange: abort set, giving up"). */
    furi_semaphore_acquire(instance->detect_sem, 0);

    nfc_set_detect_sem(instance->nfc, instance->detect_sem);
    nfc_start(instance->nfc, nfc_poller_detect_head_callback, instance);
    FuriStatus sem_status = furi_semaphore_acquire(instance->detect_sem, 3000);
    if(sem_status != FuriStatusOk) {
        FURI_LOG_W(TAG, "Detect: semaphore acquire timeout, forcing stop");
    }
    nfc_stop(instance->nfc);
    nfc_set_detect_sem(instance->nfc, NULL);

    if(tail_poller != instance->list.head) {
        iter->poller_api->set_callback(
            iter->poller, tail_poller->poller_api->run, tail_poller->poller);
        FURI_LOG_D(TAG, "Detect: restored callback on protocol: %d", iter->protocol);
    }

    instance->session_state = NfcPollerSessionStateIdle;
    FURI_LOG_D(TAG, "Detection finished, result: %d", instance->protocol_detected);

    return instance->protocol_detected;
}

NfcProtocol nfc_poller_get_protocol(const NfcPoller* instance) {
    furi_check(instance);

    return instance->protocol;
}

const NfcDeviceData* nfc_poller_get_data(const NfcPoller* instance) {
    furi_check(instance);

    NfcPollerListElement* tail_poller = instance->list.tail;
    return tail_poller->poller_api->get_data(tail_poller->poller);
}
