#include "jewel_poller_i.h"

#include <nfc/protocols/nfc_poller_base.h>

#include <furi.h>

#define TAG "JewelPoller"

typedef NfcCommand (*JewelPollerReadHandler)(JewelPoller* instance);

static JewelPoller* jewel_poller_alloc(Nfc* nfc) {
    furi_assert(nfc);

    JewelPoller* instance = malloc(sizeof(JewelPoller));
    furi_check(instance);
    instance->nfc = nfc;
    instance->data = jewel_alloc();
    furi_check(instance->data);

    nfc_config(instance->nfc, NfcModePoller, NfcTechIso14443a);

    instance->state = JewelPollerStateIdle;
    instance->mode = JewelPollerModeRead;
    instance->write_data = NULL;
    instance->jewel_event.data = &instance->jewel_event_data;
    instance->general_event.protocol = NfcProtocolJewel;
    instance->general_event.event_data = &instance->jewel_event;
    instance->general_event.instance = instance;

    return instance;
}

static void jewel_poller_free(JewelPoller* instance) {
    furi_assert(instance);
    furi_assert(instance->data);
    jewel_free(instance->data);
    free(instance);
}

static void
    jewel_poller_set_callback(JewelPoller* instance, NfcGenericCallback callback, void* context) {
    furi_assert(instance);
    furi_assert(callback);

    instance->callback = callback;
    instance->context = context;
}

NfcCommand jewel_poller_state_handler_idle(JewelPoller* instance) {
    FURI_LOG_D(TAG, "Idle");
    jewel_reset(instance->data);
    instance->state = JewelPollerStateReadBlock0;
    return NfcCommandContinue;
}

NfcCommand jewel_poller_state_handler_read_block0(JewelPoller* instance) {
    FURI_LOG_D(TAG, "ReadBlock0");

    JewelError error = jewel_poller_activate(instance, instance->data);
    if(error == JewelErrorNone) {
        instance->state = JewelPollerStateReadAllBlocks;
    } else {
        instance->jewel_event.type = JewelPollerEventTypeError;
        instance->jewel_event_data.error = error;
        instance->state = JewelPollerStateReadFailed;
    }
    return NfcCommandContinue;
}

NfcCommand jewel_poller_state_handler_read_all_blocks(JewelPoller* instance) {
    FURI_LOG_D(TAG, "ReadAllBlocks");

    JewelError error = jewel_poller_read_all(instance);
    if(error == JewelErrorNone) {
        /* Fire RequestMode event so the callback can request a write operation. */
        instance->jewel_event.type = JewelPollerEventTypeRequestMode;
        instance->jewel_event_data.mode_request.mode = JewelPollerModeRead;
        instance->jewel_event_data.mode_request.write_data = NULL;

        NfcCommand cmd = instance->callback(instance->general_event, instance->context);
        if(cmd == NfcCommandStop) {
            instance->state = JewelPollerStateReadFailed;
            return NfcCommandStop;
        }

        if(instance->mode == JewelPollerModeWrite && instance->write_data != NULL) {
            instance->state = JewelPollerStateWriteBlocks;
        } else {
            instance->state = JewelPollerStateReadSuccess;
        }
    } else {
        instance->jewel_event.type = JewelPollerEventTypeError;
        instance->jewel_event_data.error = error;
        instance->state = JewelPollerStateReadFailed;
    }
    return NfcCommandContinue;
}

NfcCommand jewel_poller_state_handler_read_success(JewelPoller* instance) {
    FURI_LOG_D(TAG, "Read Success");

    instance->jewel_event.type = JewelPollerEventTypeReady;
    instance->jewel_event_data.error = JewelErrorNone;
    return instance->callback(instance->general_event, instance->context);
}

NfcCommand jewel_poller_state_handler_write_blocks(JewelPoller* instance) {
    FURI_LOG_D(TAG, "WriteBlocks");

    furi_assert(instance->write_data);
    furi_assert(instance->write_data->dump_valid);

    JewelError error = JewelErrorNone;
    bool wrote_any = false;

    /* Write user blocks 1-15.  Block 0 (HR0/HR1/UID) is OTP and cannot be
     * rewritten on a programmed card; skip it to avoid protocol errors. */
    for(uint8_t block = 1; block < JEWEL_BLOCKS_TOTAL; block++) {
        const uint8_t* src = &instance->write_data->dump[block * JEWEL_BLOCK_SIZE];
        const uint8_t* cur = &instance->data->dump[block * JEWEL_BLOCK_SIZE];

        /* Skip blocks that already contain the target data */
        if(memcmp(src, cur, JEWEL_BLOCK_SIZE) == 0) continue;

        error = jewel_poller_write_block(instance, block, src);
        if(error != JewelErrorNone) {
            FURI_LOG_W(TAG, "Write failed at block %u", block);
            break;
        }
        wrote_any = true;
    }

    if(error == JewelErrorNone) {
        /* Update poller's data copy to reflect what was written */
        memcpy(instance->data->dump, instance->write_data->dump, JEWEL_DUMP_SIZE);
        instance->data->dump_valid = true;
        instance->state = JewelPollerStateWriteSuccess;
        FURI_LOG_D(TAG, "Write complete (wrote %sblocks)", wrote_any ? "" : "0 ");
    } else {
        instance->jewel_event.type = JewelPollerEventTypeError;
        instance->jewel_event_data.error = error;
        instance->state = JewelPollerStateWriteFailed;
    }
    return NfcCommandContinue;
}

NfcCommand jewel_poller_state_handler_write_success(JewelPoller* instance) {
    FURI_LOG_D(TAG, "Write Success");

    instance->jewel_event.type = JewelPollerEventTypeReady;
    instance->jewel_event_data.error = JewelErrorNone;
    return instance->callback(instance->general_event, instance->context);
}

NfcCommand jewel_poller_state_handler_write_failed(JewelPoller* instance) {
    FURI_LOG_D(TAG, "Write Fail");
    return instance->callback(instance->general_event, instance->context);
}

NfcCommand jewel_poller_state_handler_read_failed(JewelPoller* instance) {
    FURI_LOG_D(TAG, "Read Fail");
    return instance->callback(instance->general_event, instance->context);
}

static const JewelPollerReadHandler jewel_poller_handler[JewelPollerStateNum] = {
    [JewelPollerStateIdle] = jewel_poller_state_handler_idle,
    [JewelPollerStateReadBlock0] = jewel_poller_state_handler_read_block0,
    [JewelPollerStateReadAllBlocks] = jewel_poller_state_handler_read_all_blocks,
    [JewelPollerStateWriteBlocks] = jewel_poller_state_handler_write_blocks,
    [JewelPollerStateReadSuccess] = jewel_poller_state_handler_read_success,
    [JewelPollerStateReadFailed] = jewel_poller_state_handler_read_failed,
    [JewelPollerStateWriteSuccess] = jewel_poller_state_handler_write_success,
    [JewelPollerStateWriteFailed] = jewel_poller_state_handler_write_failed,
};

static NfcCommand jewel_poller_run(NfcGenericEvent event, void* context) {
    furi_assert(context);
    furi_assert(event.protocol == NfcProtocolInvalid);
    furi_assert(event.event_data);

    JewelPoller* instance = context;
    NfcEvent* nfc_event = event.event_data;
    NfcCommand command = NfcCommandContinue;

    if(nfc_event->type == NfcEventTypePollerReady) {
        command = jewel_poller_handler[instance->state](instance);
    }

    return command;
}

static bool jewel_poller_detect(NfcGenericEvent event, void* context) {
    furi_assert(context);
    furi_assert(event.event_data);
    furi_assert(event.instance);
    furi_assert(event.protocol == NfcProtocolInvalid);

    bool protocol_detected = false;
    JewelPoller* instance = context;
    NfcEvent* nfc_event = event.event_data;
    furi_assert(instance->state == JewelPollerStateIdle);

    if(nfc_event->type == NfcEventTypePollerReady) {
        JewelError error = jewel_poller_activate(instance, instance->data);
        protocol_detected = (error == JewelErrorNone);
        if(protocol_detected) {
            char uid_str[32] = {0};
            const size_t uid_len = sizeof(instance->data->uid);
            for(size_t i = 0; i < uid_len; i++) {
                char byte_buf[4];
                snprintf(byte_buf, sizeof(byte_buf), "%02X", instance->data->uid[i]);
                strcat(uid_str, byte_buf);
                if(i < uid_len - 1) strcat(uid_str, " ");
            }
            FURI_LOG_I(
                TAG,
                "Jewel/Topaz card detected: HR0=0x%02X, HR1=0x%02X, UID=%s, ATQA=%02X%02X",
                instance->data->hr0,
                instance->data->hr1,
                uid_str,
                instance->data->atqa[0],
                instance->data->atqa[1]);
        } else {
            FURI_LOG_D(TAG, "No Jewel/Topaz card detected");
        }
    }

    return protocol_detected;
}

const NfcPollerBase nfc_poller_jewel = {
    .alloc = (NfcPollerAlloc)jewel_poller_alloc,
    .free = (NfcPollerFree)jewel_poller_free,
    .set_callback = (NfcPollerSetCallback)jewel_poller_set_callback,
    .run = (NfcPollerRun)jewel_poller_run,
    .detect = (NfcPollerDetect)jewel_poller_detect,
    .get_data = (NfcPollerGetData)jewel_poller_get_data,
};
