#pragma once

#include "srix_poller.h"

#include <nfc/nfc_poller.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SrixPollerStateIdle,
    SrixPollerStateSelect,
    SrixPollerStateRead,
    SrixPollerStateWrite,
    SrixPollerStateSuccess,
    SrixPollerStateFailure,

    SrixPollerStateNum,
} SrixPollerState;

typedef struct {
    uint8_t current_block;
} SrixPollerReadContext;

typedef struct {
    uint8_t current_block;
    const SrixData* write_data;
} SrixPollerWriteContext;

typedef union {
    SrixPollerReadContext read;
    SrixPollerWriteContext write;
} SrixPollerContext;

struct SrixPoller {
    Nfc* nfc;
    SrixPollerState state;
    SrixData* data;
    SrixError error;
    SrixPollerMode mode;
    const SrixData* write_data;

    SrixPollerContext poller_ctx;

    NfcGenericEvent general_event;
    SrixPollerEvent srix_event;
    SrixPollerEventData srix_event_data;
    NfcGenericCallback callback;
    void* context;
};

const SrixData* srix_poller_get_data(SrixPoller* instance);

#ifdef __cplusplus
}
#endif
