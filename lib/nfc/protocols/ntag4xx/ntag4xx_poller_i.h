#pragma once

#include "ntag4xx_poller.h"

#include <lib/nfc/protocols/iso14443_4a/iso14443_4a_poller_i.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    Ntag4xxPollerStateIdle,
    Ntag4xxPollerStateRequestMode,
    Ntag4xxPollerStateReadVersion,
    Ntag4xxPollerStateWriteVersion,
    Ntag4xxPollerStateReadFailed,
    Ntag4xxPollerStateReadSuccess,
    Ntag4xxPollerStateWriteFailed,
    Ntag4xxPollerStateWriteSuccess,

    Ntag4xxPollerStateNum,
} Ntag4xxPollerState;

struct Ntag4xxPoller {
    Iso14443_4aPoller* iso14443_4a_poller;
    Ntag4xxPollerState state;
    Ntag4xxPollerMode mode;
    const Ntag4xxData* write_data;
    Ntag4xxError error;
    Ntag4xxData* data;
    BitBuffer* tx_buffer;
    BitBuffer* rx_buffer;
    BitBuffer* input_buffer;
    BitBuffer* result_buffer;
    Ntag4xxPollerEventData ntag4xx_event_data;
    Ntag4xxPollerEvent ntag4xx_event;
    NfcGenericEvent general_event;
    NfcGenericCallback callback;
    void* context;
};

Ntag4xxError ntag4xx_poller_read_version(Ntag4xxPoller* instance, Ntag4xxVersion* data);

#ifdef __cplusplus
}
#endif
