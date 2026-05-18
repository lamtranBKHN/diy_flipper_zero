#include "srix_poller_sync.h"
#include "srix_poller_i.h"

#define SRIX_POLLER_FLAG_COMMAND_COMPLETE (1UL << 0)

typedef enum {
    SrixPollerCmdTypeRead,

    SrixPollerCmdTypeNum,
} SrixPollerCmdType;

typedef struct {
    SrixData* data;
} SrixPollerCmdReadData;

typedef union {
    SrixPollerCmdReadData read;
} SrixPollerCmdData;

typedef struct {
    FuriThreadId thread_id;
    SrixError error;
    SrixPollerCmdType cmd_type;
    SrixPollerCmdData cmd_data;
} SrixPollerSyncContext;

typedef SrixError (*SrixPollerCmdHandler)(SrixPoller* poller, SrixPollerCmdData* data);

static SrixError srix_poller_read_handler(SrixPoller* poller, SrixPollerCmdData* data) {
    SrixError error;

    do {
        uint8_t chip_id = 0;
        error = srix_poller_detect_tag(poller, &chip_id);
        if(error != SrixErrorNone) break;

        error = srix_poller_select_tag(poller, chip_id);
        if(error != SrixErrorNone) break;

        error = srix_poller_read_uid(poller, data->read.data->uid);
        if(error != SrixErrorNone) break;

        for(size_t i = 0; i < SRIX_BLOCKS_TOTAL; i++) {
            error = srix_poller_read_block(poller, &data->read.data->data[i * SRIX_BLOCK_SIZE], i);
            if(error != SrixErrorNone) break;
        }
    } while(false);

    return error;
}

static SrixPollerCmdHandler srix_poller_cmd_handlers[SrixPollerCmdTypeNum] = {
    [SrixPollerCmdTypeRead] = srix_poller_read_handler,
};

static NfcCommand srix_poller_cmd_callback(NfcGenericEvent event, void* context) {
    furi_assert(context);
    furi_assert(event.event_data);
    furi_assert(event.instance);
    furi_assert(event.protocol == NfcProtocolSrix);

    SrixPollerSyncContext* poller_context = context;
    SrixPoller* srix_poller = event.instance;
    SrixPollerEvent* srix_event = event.event_data;

    if(srix_event->type == SrixPollerEventTypeReady) {
        poller_context->error = srix_poller_cmd_handlers[poller_context->cmd_type](
            srix_poller, &poller_context->cmd_data);
    } else {
        poller_context->error = srix_event->data->error;
    }

    furi_thread_flags_set(poller_context->thread_id, SRIX_POLLER_FLAG_COMMAND_COMPLETE);

    return NfcCommandStop;
}

static SrixError srix_poller_cmd_execute_raw(Nfc* nfc, SrixPollerSyncContext* poller_ctx) {
    furi_assert(nfc);
    furi_assert(poller_ctx->cmd_type < SrixPollerCmdTypeNum);
    poller_ctx->thread_id = furi_thread_get_current_id();

    NfcPoller* poller = nfc_poller_alloc(nfc, NfcProtocolSrix);
    nfc_poller_start(poller, srix_poller_cmd_callback, poller_ctx);
    uint32_t flags =
        furi_thread_flags_wait(SRIX_POLLER_FLAG_COMMAND_COMPLETE, FuriFlagWaitAny, 5000);
    if(!(flags & SRIX_POLLER_FLAG_COMMAND_COMPLETE)) {
        nfc_poller_stop(poller);
        nfc_poller_free(poller);
        return SrixErrorTimeout;
    }
    furi_thread_flags_clear(SRIX_POLLER_FLAG_COMMAND_COMPLETE);

    nfc_poller_stop(poller);
    nfc_poller_free(poller);

    return poller_ctx->error;
}

SrixError srix_poller_sync_read(Nfc* nfc, SrixData* data) {
    furi_check(nfc);
    furi_check(data);

    SrixPollerSyncContext poller_context = {
        .cmd_type = SrixPollerCmdTypeRead,
        .cmd_data =
            {
                .read =
                    {
                        .data = data,
                    },
            },
    };
    return srix_poller_cmd_execute_raw(nfc, &poller_context);
}
