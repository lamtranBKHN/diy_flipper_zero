#include "ntag4xx_poller_i.h"

#include <nfc/protocols/nfc_poller_base.h>

#include <furi.h>

#define TAG "Ntag4xxPoller"

#define NTAG4XX_BUF_SIZE        (64U)
#define NTAG4XX_RESULT_BUF_SIZE (512U)

typedef NfcCommand (*Ntag4xxPollerReadHandler)(Ntag4xxPoller* instance);

static const Ntag4xxData* ntag4xx_poller_get_data(Ntag4xxPoller* instance) {
    furi_assert(instance);

    return instance->data;
}

static Ntag4xxPoller* ntag4xx_poller_alloc(Iso14443_4aPoller* iso14443_4a_poller) {
    Ntag4xxPoller* instance = malloc(sizeof(Ntag4xxPoller));
    furi_check(instance);
    instance->iso14443_4a_poller = iso14443_4a_poller;
    instance->data = ntag4xx_alloc();
    furi_check(instance->data);
    instance->tx_buffer = bit_buffer_alloc(NTAG4XX_BUF_SIZE);
    furi_check(instance->tx_buffer);
    instance->rx_buffer = bit_buffer_alloc(NTAG4XX_BUF_SIZE);
    furi_check(instance->rx_buffer);
    instance->input_buffer = bit_buffer_alloc(NTAG4XX_BUF_SIZE);
    furi_check(instance->input_buffer);
    instance->result_buffer = bit_buffer_alloc(NTAG4XX_RESULT_BUF_SIZE);
    furi_check(instance->result_buffer);

    instance->state = Ntag4xxPollerStateIdle;
    instance->mode = Ntag4xxPollerModeRead;
    instance->write_data = NULL;
    instance->ntag4xx_event.data = &instance->ntag4xx_event_data;

    instance->general_event.protocol = NfcProtocolNtag4xx;
    instance->general_event.event_data = &instance->ntag4xx_event;
    instance->general_event.instance = instance;

    return instance;
}

static void ntag4xx_poller_free(Ntag4xxPoller* instance) {
    furi_assert(instance);

    ntag4xx_free(instance->data);
    bit_buffer_free(instance->tx_buffer);
    bit_buffer_free(instance->rx_buffer);
    bit_buffer_free(instance->input_buffer);
    bit_buffer_free(instance->result_buffer);
    free(instance);
}

static NfcCommand ntag4xx_poller_handler_idle(Ntag4xxPoller* instance) {
    bit_buffer_reset(instance->input_buffer);
    bit_buffer_reset(instance->result_buffer);
    bit_buffer_reset(instance->tx_buffer);
    bit_buffer_reset(instance->rx_buffer);

    iso14443_4a_copy(
        instance->data->iso14443_4a_data,
        iso14443_4a_poller_get_data(instance->iso14443_4a_poller));

    instance->state = Ntag4xxPollerStateRequestMode;
    return NfcCommandContinue;
}

static NfcCommand ntag4xx_poller_handler_request_mode(Ntag4xxPoller* instance) {
    instance->ntag4xx_event.type = Ntag4xxPollerEventTypeRequestMode;
    instance->ntag4xx_event.data->mode_request.mode = Ntag4xxPollerModeRead;
    instance->ntag4xx_event.data->mode_request.write_data = NULL;

    NfcCommand command = instance->callback(instance->general_event, instance->context);
    if(command != NfcCommandContinue) {
        return command;
    }

    if(instance->mode == Ntag4xxPollerModeWrite) {
        instance->state = Ntag4xxPollerStateWriteVersion;
    } else {
        instance->state = Ntag4xxPollerStateReadVersion;
    }

    return NfcCommandContinue;
}

static NfcCommand ntag4xx_poller_handler_read_version(Ntag4xxPoller* instance) {
    instance->error = ntag4xx_poller_read_version(instance, &instance->data->version);
    if(instance->error == Ntag4xxErrorNone) {
        FURI_LOG_D(TAG, "Read version success");
        instance->state = Ntag4xxPollerStateReadSuccess;
    } else {
        FURI_LOG_E(TAG, "Failed to read version");
        iso14443_4a_poller_halt(instance->iso14443_4a_poller);
        instance->state = Ntag4xxPollerStateReadFailed;
    }

    return NfcCommandContinue;
}

static NfcCommand ntag4xx_poller_handler_write_version(Ntag4xxPoller* instance) {
    FURI_LOG_W(TAG, "Write version unsupported (NTAG4xx DNA version is read-only)");
    iso14443_4a_poller_halt(instance->iso14443_4a_poller);
    instance->state = Ntag4xxPollerStateWriteFailed;
    return NfcCommandContinue;
}

static NfcCommand ntag4xx_poller_handler_read_failed(Ntag4xxPoller* instance) {
    FURI_LOG_D(TAG, "Read Failed");
    iso14443_4a_poller_halt(instance->iso14443_4a_poller);
    instance->ntag4xx_event.type = Ntag4xxPollerEventTypeReadFailed;
    instance->ntag4xx_event.data->error = instance->error;
    return instance->callback(instance->general_event, instance->context);
}

static NfcCommand ntag4xx_poller_handler_read_success(Ntag4xxPoller* instance) {
    FURI_LOG_D(TAG, "Read success");
    iso14443_4a_poller_halt(instance->iso14443_4a_poller);
    instance->ntag4xx_event.type = Ntag4xxPollerEventTypeReadSuccess;
    return instance->callback(instance->general_event, instance->context);
}

static NfcCommand ntag4xx_poller_handler_write_failed(Ntag4xxPoller* instance) {
    FURI_LOG_D(TAG, "Write Failed");
    iso14443_4a_poller_halt(instance->iso14443_4a_poller);
    instance->ntag4xx_event.type = Ntag4xxPollerEventTypeWriteFailed;
    instance->ntag4xx_event.data->error = instance->error;
    return instance->callback(instance->general_event, instance->context);
}

static NfcCommand ntag4xx_poller_handler_write_success(Ntag4xxPoller* instance) {
    FURI_LOG_D(TAG, "Write success");
    iso14443_4a_poller_halt(instance->iso14443_4a_poller);
    instance->ntag4xx_event.type = Ntag4xxPollerEventTypeWriteSuccess;
    return instance->callback(instance->general_event, instance->context);
}

static const Ntag4xxPollerReadHandler ntag4xx_poller_read_handler[Ntag4xxPollerStateNum] = {
    [Ntag4xxPollerStateIdle] = ntag4xx_poller_handler_idle,
    [Ntag4xxPollerStateRequestMode] = ntag4xx_poller_handler_request_mode,
    [Ntag4xxPollerStateReadVersion] = ntag4xx_poller_handler_read_version,
    [Ntag4xxPollerStateWriteVersion] = ntag4xx_poller_handler_write_version,
    [Ntag4xxPollerStateReadFailed] = ntag4xx_poller_handler_read_failed,
    [Ntag4xxPollerStateReadSuccess] = ntag4xx_poller_handler_read_success,
    [Ntag4xxPollerStateWriteFailed] = ntag4xx_poller_handler_write_failed,
    [Ntag4xxPollerStateWriteSuccess] = ntag4xx_poller_handler_write_success,
};

static void ntag4xx_poller_set_callback(
    Ntag4xxPoller* instance,
    NfcGenericCallback callback,
    void* context) {
    furi_assert(instance);
    furi_assert(callback);

    instance->callback = callback;
    instance->context = context;
}

static NfcCommand ntag4xx_poller_run(NfcGenericEvent event, void* context) {
    furi_assert(event.protocol == NfcProtocolIso14443_4a);

    Ntag4xxPoller* instance = context;
    furi_assert(instance);
    furi_assert(instance->callback);

    const Iso14443_4aPollerEvent* iso14443_4a_event = event.event_data;
    furi_assert(iso14443_4a_event);

    NfcCommand command = NfcCommandContinue;

    if(iso14443_4a_event->type == Iso14443_4aPollerEventTypeReady) {
        command = ntag4xx_poller_read_handler[instance->state](instance);
    } else if(iso14443_4a_event->type == Iso14443_4aPollerEventTypeError) {
        instance->ntag4xx_event.type = Ntag4xxPollerEventTypeReadFailed;
        instance->callback(instance->general_event, instance->context);
        command = NfcCommandStop;
    }

    return command;
}

static bool ntag4xx_poller_detect(NfcGenericEvent event, void* context) {
    furi_assert(event.protocol == NfcProtocolIso14443_4a);

    Ntag4xxPoller* instance = context;
    furi_assert(instance);

    const Iso14443_4aPollerEvent* iso14443_4a_event = event.event_data;
    furi_assert(iso14443_4a_event);

    bool protocol_detected = false;

    if(iso14443_4a_event->type == Iso14443_4aPollerEventTypeReady) {
        do {
            Ntag4xxError error = ntag4xx_poller_read_version(instance, &instance->data->version);
            if(error != Ntag4xxErrorNone) break;

            protocol_detected = true;
        } while(false);
    }

    return protocol_detected;
}

const NfcPollerBase ntag4xx_poller = {
    .alloc = (NfcPollerAlloc)ntag4xx_poller_alloc,
    .free = (NfcPollerFree)ntag4xx_poller_free,
    .set_callback = (NfcPollerSetCallback)ntag4xx_poller_set_callback,
    .run = (NfcPollerRun)ntag4xx_poller_run,
    .detect = (NfcPollerDetect)ntag4xx_poller_detect,
    .get_data = (NfcPollerGetData)ntag4xx_poller_get_data,
};
