#include "jewel_poller_sync.h"
#include "jewel_poller.h"
#include "jewel_poller_i.h"

#include <nfc/nfc_poller.h>

#include <furi/furi.h>

#define JEWEL_POLLER_FLAG_COMMAND_COMPLETE (1UL << 0)

typedef struct {
    FuriThreadId thread_id;
    JewelError error;
    JewelData* data;
    const JewelData* write_data; /**< Data to write (write mode only). */
} JewelPollerContext;

NfcCommand jewel_poller_read_callback(NfcGenericEvent event, void* context) {
    furi_assert(context);
    furi_assert(event.event_data);
    furi_assert(event.instance);
    furi_assert(event.protocol == NfcProtocolJewel);

    JewelPollerContext* poller_context = context;
    JewelPollerEvent* jewel_event = event.event_data;

    if(jewel_event->type == JewelPollerEventTypeRequestMode) {
        /* Read mode — don't modify mode/write_data; stay in Read. */
        return NfcCommandContinue;
    }

    if(jewel_event->type == JewelPollerEventTypeReady) {
        JewelPoller* jewel_poller = event.instance;
        jewel_copy(poller_context->data, jewel_poller->data);
    } else if(jewel_event->type == JewelPollerEventTypeError) {
        poller_context->error = jewel_event->data->error;
    } else {
        poller_context->error = JewelErrorProtocol;
    }

    furi_thread_flags_set(poller_context->thread_id, JEWEL_POLLER_FLAG_COMMAND_COMPLETE);
    return NfcCommandStop;
}

JewelError jewel_poller_sync_read(Nfc* nfc, JewelData* data) {
    furi_check(nfc);
    furi_check(data);

    JewelPollerContext poller_context = {
        .thread_id = furi_thread_get_current_id(),
        .error = JewelErrorNone,
        .data = jewel_alloc(),
    };

    NfcPoller* poller = nfc_poller_alloc(nfc, NfcProtocolJewel);
    nfc_poller_start(poller, jewel_poller_read_callback, &poller_context);
    furi_thread_flags_wait(JEWEL_POLLER_FLAG_COMMAND_COMPLETE, FuriFlagWaitAny, FuriWaitForever);
    furi_thread_flags_clear(JEWEL_POLLER_FLAG_COMMAND_COMPLETE);

    nfc_poller_stop(poller);
    nfc_poller_free(poller);

    if(poller_context.error == JewelErrorNone) {
        jewel_copy(data, poller_context.data);
    }

    jewel_free(poller_context.data);
    return poller_context.error;
}

static NfcCommand jewel_poller_write_callback(NfcGenericEvent event, void* context) {
    furi_assert(context);
    furi_assert(event.event_data);
    furi_assert(event.instance);
    furi_assert(event.protocol == NfcProtocolJewel);

    JewelPollerContext* poller_context = context;
    JewelPollerEvent* jewel_event = event.event_data;

    if(jewel_event->type == JewelPollerEventTypeRequestMode) {
        /* The poller asks for mode after reading the card.  Set write mode
         * and point to the caller-provided write data. */
        JewelPoller* jewel_poller = event.instance;
        jewel_poller->mode = JewelPollerModeWrite;
        jewel_poller->write_data = poller_context->write_data;
        return NfcCommandContinue;
    }

    if(jewel_event->type == JewelPollerEventTypeReady) {
        /* Read or write completed successfully */
        JewelPoller* jewel_poller = event.instance;
        jewel_copy(poller_context->data, jewel_poller->data);
    } else if(jewel_event->type == JewelPollerEventTypeError) {
        poller_context->error = jewel_event->data->error;
    } else {
        poller_context->error = JewelErrorProtocol;
    }

    furi_thread_flags_set(poller_context->thread_id, JEWEL_POLLER_FLAG_COMMAND_COMPLETE);
    return NfcCommandStop;
}

JewelError jewel_poller_sync_write(Nfc* nfc, const JewelData* data) {
    furi_check(nfc);
    furi_check(data);
    furi_check(data->dump_valid);

    JewelPollerContext poller_context = {
        .thread_id = furi_thread_get_current_id(),
        .error = JewelErrorNone,
        .data = jewel_alloc(),
        .write_data = data,
    };

    NfcPoller* poller = nfc_poller_alloc(nfc, NfcProtocolJewel);
    nfc_poller_start(poller, jewel_poller_write_callback, &poller_context);
    furi_thread_flags_wait(JEWEL_POLLER_FLAG_COMMAND_COMPLETE, FuriFlagWaitAny, FuriWaitForever);
    furi_thread_flags_clear(JEWEL_POLLER_FLAG_COMMAND_COMPLETE);

    nfc_poller_stop(poller);
    nfc_poller_free(poller);

    jewel_free(poller_context.data);
    return poller_context.error;
}
