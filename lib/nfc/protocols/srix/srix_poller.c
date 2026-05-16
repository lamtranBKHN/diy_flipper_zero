#include "srix_poller.h"
#include "srix_poller_i.h"

#include <nfc/protocols/nfc_poller_base.h>

#define TAG "SrixPoller"

typedef NfcCommand (*SrixPollerStateHandler)(SrixPoller* instance);

const SrixData* srix_poller_get_data(SrixPoller* instance) {
    furi_assert(instance);
    furi_assert(instance->data);

    return instance->data;
}

static SrixPoller* srix_poller_alloc(Nfc* nfc) {
    furi_assert(nfc);

    SrixPoller* instance = malloc(sizeof(SrixPoller));
    instance->nfc = nfc;
    instance->state = SrixPollerStateIdle;
    instance->data = srix_alloc();

    nfc_config(instance->nfc, NfcModePoller, NfcTechIso14443b);

    instance->srix_event.data = &instance->srix_event_data;
    instance->general_event.protocol = NfcProtocolSrix;
    instance->general_event.event_data = &instance->srix_event;
    instance->general_event.instance = instance;

    return instance;
}

static void srix_poller_free(SrixPoller* instance) {
    furi_assert(instance);
    furi_assert(instance->data);

    srix_free(instance->data);
    free(instance);
}

static void
    srix_poller_set_callback(SrixPoller* instance, NfcGenericCallback callback, void* context) {
    furi_assert(instance);
    furi_assert(callback);

    instance->callback = callback;
    instance->context = context;
}

static NfcCommand srix_poller_select_handler(SrixPoller* instance) {
    NfcCommand command = NfcCommandContinue;

    do {
        uint8_t chip_id = 0;
        SrixError error = srix_poller_detect_tag(instance, &chip_id);
        if(error != SrixErrorNone) {
            instance->state = SrixPollerStateFailure;
            instance->srix_event_data.error = error;
            break;
        }

        error = srix_poller_select_tag(instance, chip_id);
        if(error != SrixErrorNone) {
            instance->state = SrixPollerStateFailure;
            instance->srix_event_data.error = error;
            break;
        }

        error = srix_poller_read_uid(instance, instance->data->uid);
        if(error != SrixErrorNone) {
            instance->state = SrixPollerStateFailure;
            instance->srix_event_data.error = error;
            break;
        }

        instance->srix_event.type = SrixPollerEventTypeReady;
        command = instance->callback(instance->general_event, instance->context);
        instance->state = SrixPollerStateRead;
        instance->poller_ctx.read.current_block = 0;
    } while(false);

    return command;
}

static NfcCommand srix_poller_read_handler(SrixPoller* instance) {
    SrixError error = SrixErrorNone;

    do {
        uint8_t* current_block = &instance->poller_ctx.read.current_block;
        if(*current_block >= SRIX_BLOCKS_TOTAL) {
            instance->state = SrixPollerStateSuccess;
            break;
        }

        uint8_t block_data[SRIX_BLOCK_SIZE];
        error = srix_poller_read_block(instance, block_data, *current_block);
        if(error != SrixErrorNone) {
            FURI_LOG_E(TAG, "Failed to read block %d", *current_block);
            instance->state = SrixPollerStateFailure;
            instance->srix_event_data.error = error;
            break;
        }

        memcpy(
            &instance->data->data[*current_block * SRIX_BLOCK_SIZE], block_data, SRIX_BLOCK_SIZE);

        *current_block += 1;
    } while(false);

    return NfcCommandContinue;
}

static NfcCommand srix_poller_success_handler(SrixPoller* instance) {
    NfcCommand command = NfcCommandContinue;
    instance->srix_event.type = SrixPollerEventTypeSuccess;
    command = instance->callback(instance->general_event, instance->context);

    return command;
}

static NfcCommand srix_poller_failure_handler(SrixPoller* instance) {
    NfcCommand command = NfcCommandContinue;
    instance->srix_event.type = SrixPollerEventTypeFailure;
    command = instance->callback(instance->general_event, instance->context);

    return command;
}

static SrixPollerStateHandler srix_poller_state_handlers[SrixPollerStateNum] = {
    [SrixPollerStateIdle] = srix_poller_select_handler,
    [SrixPollerStateSelect] = srix_poller_select_handler,
    [SrixPollerStateRead] = srix_poller_read_handler,
    [SrixPollerStateSuccess] = srix_poller_success_handler,
    [SrixPollerStateFailure] = srix_poller_failure_handler,
};

static NfcCommand srix_poller_run(NfcGenericEvent event, void* context) {
    furi_assert(context);
    furi_assert(event.protocol == NfcProtocolInvalid);
    furi_assert(event.event_data);

    SrixPoller* instance = context;
    NfcEvent* nfc_event = event.event_data;
    NfcCommand command = NfcCommandContinue;

    furi_assert(instance->state < SrixPollerStateNum);

    if(nfc_event->type == NfcEventTypePollerReady) {
        command = srix_poller_state_handlers[instance->state](instance);
    }

    return command;
}

static bool srix_poller_detect(NfcGenericEvent event, void* context) {
    furi_assert(context);
    furi_assert(event.event_data);
    furi_assert(event.instance);
    furi_assert(event.protocol == NfcProtocolInvalid);

    bool protocol_detected = false;
    SrixPoller* instance = context;
    NfcEvent* nfc_event = event.event_data;

    if(nfc_event->type == NfcEventTypePollerReady) {
        uint8_t chip_id = 0;
        SrixError error = srix_poller_detect_tag(instance, &chip_id);
        protocol_detected = (error == SrixErrorNone);
    }

    return protocol_detected;
}

const NfcPollerBase nfc_poller_srix = {
    .alloc = (NfcPollerAlloc)srix_poller_alloc,
    .free = (NfcPollerFree)srix_poller_free,
    .set_callback = (NfcPollerSetCallback)srix_poller_set_callback,
    .run = (NfcPollerRun)srix_poller_run,
    .detect = (NfcPollerDetect)srix_poller_detect,
    .get_data = (NfcPollerGetData)srix_poller_get_data,
};
