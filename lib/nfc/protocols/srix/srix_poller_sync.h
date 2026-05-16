#pragma once

#include "srix.h"
#include <nfc/nfc.h>

#ifdef __cplusplus
extern "C" {
#endif

SrixError srix_poller_sync_read(Nfc* nfc, SrixData* data);

#ifdef __cplusplus
}
#endif
