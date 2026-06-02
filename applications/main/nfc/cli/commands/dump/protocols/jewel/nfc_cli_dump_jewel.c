#include "nfc_cli_dump_jewel.h"
#include <nfc/protocols/jewel/jewel_poller.h>

NfcCommand nfc_cli_dump_poller_callback_jewel(NfcGenericEvent event, void* context) {
    furi_assert(event.protocol == NfcProtocolJewel);

    NfcCliDumpContext* instance = context;
    const JewelPollerEvent* jewel_event = event.event_data;

    NfcCommand command = NfcCommandContinue;
    if(jewel_event->type == JewelPollerEventTypeReady) {
        nfc_device_set_data(
            instance->nfc_device, NfcProtocolJewel, nfc_poller_get_data(instance->poller));
        command = NfcCommandStop;
    } else if(jewel_event->type == JewelPollerEventTypeError) {
        command = NfcCommandStop;
        instance->result = NfcCliDumpErrorFailedToRead;
    }

    if(command == NfcCommandStop) {
        furi_semaphore_release(instance->sem_done);
    }

    return command;
}
