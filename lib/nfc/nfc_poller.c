#include "nfc_poller.h"

#include <nfc/protocols/nfc_poller_defs.h>

#include <furi.h>

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
};

static void nfc_poller_list_alloc(NfcPoller* instance) {
    FURI_LOG_I(TAG, "Allocating poller list for protocol: %d", instance->protocol); // <--- Added Log
    instance->list.head = malloc(sizeof(NfcPollerListElement));
    instance->list.head->protocol = instance->protocol;
    instance->list.head->poller_api = nfc_pollers_api[instance->protocol];
    instance->list.head->child = NULL;
    instance->list.tail = instance->list.head;
    FURI_LOG_I(TAG, "Head protocol: %d", instance->list.head->protocol); // <--- Added Log

    do {
        NfcProtocol parent_protocol = nfc_protocol_get_parent(instance->list.head->protocol);
        if(parent_protocol == NfcProtocolInvalid) break;

        NfcPollerListElement* parent = malloc(sizeof(NfcPollerListElement));
        parent->protocol = parent_protocol;
        parent->poller_api = nfc_pollers_api[parent_protocol];
        parent->child = instance->list.head;
        instance->list.head = parent;
        FURI_LOG_I(TAG, "Added parent protocol: %d", parent_protocol); // <--- Added Log
    } while(true);

    NfcPollerListElement* iter = instance->list.head;
    iter->poller = iter->poller_api->alloc(instance->nfc);
    FURI_LOG_I(TAG, "Allocated poller for protocol: %d", iter->protocol); // <--- Added Log

    do {
        if(iter->child == NULL) break;
        iter->child->poller = iter->child->poller_api->alloc(iter->poller);
        iter->poller_api->set_callback(
            iter->poller, iter->child->poller_api->run, iter->child->poller);
        FURI_LOG_I(
            TAG, "Allocated poller for protocol: %d, set callback in parent: %d",
            iter->child->protocol,
            iter->protocol); // <--- Added Log

        iter = iter->child;
    } while(true);
    FURI_LOG_I(TAG, "Finished allocating poller list"); // <--- Added Log
}

static void nfc_poller_list_free(NfcPoller* instance) {
    FURI_LOG_I(TAG, "Freeing poller list"); // <--- Added Log
    do {
        NfcProtocol current_protocol = instance->list.head->protocol;
        instance->list.head->poller_api->free(instance->list.head->poller);
        NfcPollerListElement* child = instance->list.head->child;
        free(instance->list.head);
        FURI_LOG_I(TAG, "Freed poller and element for protocol: %d", current_protocol); // <--- Added Log
        if(child == NULL) break;
        instance->list.head = child;
    } while(true);
    FURI_LOG_I(TAG, "Finished freeing poller list"); // <--- Added Log
}

NfcPoller* nfc_poller_alloc(Nfc* nfc, NfcProtocol protocol) {
    furi_check(nfc);
    furi_check(protocol < NfcProtocolNum);

    NfcPoller* instance = malloc(sizeof(NfcPoller));
    instance->session_state = NfcPollerSessionStateIdle;
    instance->nfc = nfc;
    instance->protocol = protocol;
    nfc_poller_list_alloc(instance);

    FURI_LOG_I(TAG, "Poller allocated for protocol: %d", protocol); // <--- Added Log
    return instance;
}

void nfc_poller_free(NfcPoller* instance) {
    furi_check(instance);

    FURI_LOG_I(TAG, "Poller freeing for protocol: %d", instance->protocol); // <--- Added Log
    nfc_poller_list_free(instance);
    free(instance);
    FURI_LOG_I(TAG, "Poller freed"); // <--- Added Log
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

    FURI_LOG_I(TAG, "Start callback received event type: %d", event.type); // <--- Added Log

  //  if(event.type == NfcEventTypePollerReady) {
        NfcPollerListElement* head_poller = instance->list.head;
        command = head_poller->poller_api->run(poller_event, head_poller->poller);
        FURI_LOG_I(TAG, "PollerReady: starting run on head protocol: %d, command: %d", head_poller->protocol, command); // <--- Added Log
  //  }

    if(instance->session_state == NfcPollerSessionStateStopRequest) {
        command = NfcCommandStop;
        FURI_LOG_I(TAG, "Stop requested, returning NfcCommandStop"); // <--- Added Log
    }

    return command;
}

void nfc_poller_start(NfcPoller* instance, NfcGenericCallback callback, void* context) {
    furi_check(instance);
    furi_check(callback);
    furi_check(instance->session_state == NfcPollerSessionStateIdle);

    FURI_LOG_I(TAG, "Starting poller for protocol: %d (simple mode)", instance->protocol); // <--- Added Log

    NfcPollerListElement* tail_poller = instance->list.tail;
    tail_poller->poller_api->set_callback(tail_poller->poller, callback, context);

    instance->session_state = NfcPollerSessionStateActive;
    nfc_start(instance->nfc, nfc_poller_start_callback, instance);
    FURI_LOG_I(TAG, "nfc_start called"); // <--- Added Log
}

static NfcCommand nfc_poller_start_ex_tail_callback(NfcGenericEvent event, void* context) {
    furi_assert(context);
    furi_assert(event.protocol != NfcProtocolInvalid);

    NfcPoller* instance = context;
    NfcCommand command = NfcCommandContinue;

    FURI_LOG_I(TAG, "Start Ex tail callback received event protocol: %d", event.protocol); // <--- Added Log

    NfcGenericEventEx poller_event = {
        .poller = instance->list.tail->poller,
        .parent_event_data = event.event_data,
    };

    command = instance->callback(poller_event, instance->context);

    FURI_LOG_I(TAG, "Start Ex tail callback custom callback command: %d", command); // <--- Added Log

    return command;
}

static NfcCommand nfc_poller_start_ex_head_callback(NfcEvent event, void* context) {
    furi_assert(context);

    NfcCommand command = NfcCommandContinue;
    NfcPoller* instance = context;

    FURI_LOG_I(TAG, "Start Ex head callback received event type: %d", event.type); // <--- Added Log

    NfcProtocol parent_protocol = nfc_protocol_get_parent(instance->protocol);

    if(parent_protocol == NfcProtocolInvalid) {
        NfcGenericEventEx poller_event = {
            .poller = instance->list.tail->poller,
            .parent_event_data = &event,
        };

        command = instance->callback(poller_event, instance->context);
        FURI_LOG_I(TAG, "Start Ex: no parent, calling custom callback, command: %d", command); // <--- Added Log
    } else {
        NfcGenericEvent poller_event = {
            .protocol = NfcProtocolInvalid,
            .instance = instance->nfc,
            .event_data = &event,
        };
        NfcPollerListElement* head_poller = instance->list.head;
        command = head_poller->poller_api->run(poller_event, head_poller->poller);
        FURI_LOG_I(TAG, "Start Ex: has parent, starting run on head protocol: %d, command: %d", head_poller->protocol, command); // <--- Added Log
    }

    if(instance->session_state == NfcPollerSessionStateStopRequest) {
        command = NfcCommandStop;
        FURI_LOG_I(TAG, "Stop requested, returning NfcCommandStop"); // <--- Added Log
    }

    return command;
}

void nfc_poller_start_ex(NfcPoller* instance, NfcGenericCallbackEx callback, void* context) {
    furi_check(instance);
    furi_check(callback);
    furi_check(instance->session_state == NfcPollerSessionStateIdle);

    FURI_LOG_I(TAG, "Starting poller for protocol: %d (extended mode)", instance->protocol); // <--- Added Log

    instance->callback = callback;
    instance->context = context;

    NfcProtocol parent_protocol = nfc_protocol_get_parent(instance->protocol);
    if(parent_protocol != NfcProtocolInvalid) {
        NfcPollerListElement* iter = instance->list.head;
        while(iter->protocol != parent_protocol)
            iter = iter->child;

        iter->poller_api->set_callback(iter->poller, nfc_poller_start_ex_tail_callback, instance);
        FURI_LOG_I(TAG, "Start Ex: set tail callback on parent protocol: %d", parent_protocol); // <--- Added Log
    }

    instance->session_state = NfcPollerSessionStateActive;
    nfc_start(instance->nfc, nfc_poller_start_ex_head_callback, instance);
    FURI_LOG_I(TAG, "nfc_start called"); // <--- Added Log
}

void nfc_poller_stop(NfcPoller* instance) {
    furi_check(instance);
    furi_check(instance->nfc);

    FURI_LOG_I(TAG, "Stopping poller for protocol: %d", instance->protocol); // <--- Added Log
    instance->session_state = NfcPollerSessionStateStopRequest;
    nfc_stop(instance->nfc);
    instance->session_state = NfcPollerSessionStateIdle;
    FURI_LOG_I(TAG, "Poller stopped"); // <--- Added Log
}

static NfcCommand nfc_poller_detect_tail_callback(NfcGenericEvent event, void* context) {
    furi_assert(context);

    NfcPoller* instance = context;
    NfcPollerListElement* tail_poller = instance->list.tail;
    instance->protocol_detected = tail_poller->poller_api->detect(event, tail_poller->poller);

    FURI_LOG_I(TAG, "Detect tail callback: protocol %d detected: %d", tail_poller->protocol, instance->protocol_detected); // <--- Added Log

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

    FURI_LOG_I(TAG, "Detect head callback received event type: %d", event.type); // <--- Added Log

    if(event.type == NfcEventTypePollerReady) {
        if(tail_poller == head_poller) {
            instance->protocol_detected =
                tail_poller->poller_api->detect(poller_event, tail_poller->poller);
            command = NfcCommandStop;
            FURI_LOG_I(TAG, "Detect: head == tail, detected: %d, command: %d", instance->protocol_detected, command); // <--- Added Log
        } else {
            command = head_poller->poller_api->run(poller_event, head_poller->poller);
            FURI_LOG_I(TAG, "Detect: starting run on head protocol: %d, command: %d", head_poller->protocol, command); // <--- Added Log
        }
    }

    return command;
}

bool nfc_poller_detect(NfcPoller* instance) {
    furi_check(instance);
    furi_check(instance->session_state == NfcPollerSessionStateIdle);

    FURI_LOG_I(TAG, "Starting protocol detection for: %d", instance->protocol); // <--- Added Log

    instance->session_state = NfcPollerSessionStateActive;
    NfcPollerListElement* tail_poller = instance->list.tail;
    NfcPollerListElement* iter = instance->list.head;

    if(tail_poller != instance->list.head) {
        while(iter->child != tail_poller)
            iter = iter->child;
        iter->poller_api->set_callback(iter->poller, nfc_poller_detect_tail_callback, instance);
        FURI_LOG_I(TAG, "Detect: set tail callback on protocol: %d", iter->protocol); // <--- Added Log
    }

    nfc_start(instance->nfc, nfc_poller_detect_head_callback, instance);
    FuriStatus sem_status = nfc_wait_for_poller_ready(instance->nfc, 3000);
    if(sem_status != FuriStatusOk) {
        FURI_LOG_W(TAG, "Detect: semaphore acquire timeout, forcing stop");
    }
    nfc_stop(instance->nfc);

    if(tail_poller != instance->list.head) {
        iter->poller_api->set_callback(
            iter->poller, tail_poller->poller_api->run, tail_poller->poller);
        FURI_LOG_I(TAG, "Detect: restored callback on protocol: %d", iter->protocol); // <--- Added Log
    }

    instance->session_state = NfcPollerSessionStateIdle;
    FURI_LOG_I(TAG, "Detection finished, result: %d", instance->protocol_detected); // <--- Added Log

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