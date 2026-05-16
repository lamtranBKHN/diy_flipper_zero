#pragma once

#include "srix.h"
#include <lib/nfc/nfc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SrixPoller SrixPoller;

typedef enum {
    SrixPollerEventTypeReady,
    SrixPollerEventTypeFailure,
    SrixPollerEventTypeSuccess,
} SrixPollerEventType;

typedef union {
    SrixError error;
} SrixPollerEventData;

typedef struct {
    SrixPollerEventType type;
    SrixPollerEventData* data;
} SrixPollerEvent;

SrixError srix_poller_detect_tag(SrixPoller* instance, uint8_t* chip_id);

SrixError srix_poller_select_tag(SrixPoller* instance, uint8_t chip_id);

SrixError srix_poller_read_uid(SrixPoller* instance, uint8_t* uid);

SrixError srix_poller_read_block(SrixPoller* instance, uint8_t* block_data, uint8_t block_number);

SrixError
    srix_poller_write_block(SrixPoller* instance, const uint8_t* block_data, uint8_t block_number);

#ifdef __cplusplus
}
#endif
