#pragma once

#include "jewel.h"
#include <lib/nfc/nfc.h>

#include <nfc/nfc_poller.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JewelPoller JewelPoller;

typedef enum {
    JewelPollerModeRead, /**< Read card data (default). */
    JewelPollerModeWrite, /**< Write card data (set via RequestMode event). */
} JewelPollerMode;

typedef enum {
    JewelPollerEventTypeError,
    JewelPollerEventTypeReady,
    JewelPollerEventTypeRequestMode, /**< Poller asks callback for mode + write data. */
} JewelPollerEventType;

/** Data for the RequestMode event.
 *  The callback sets mode to JewelPollerModeWrite and provides write_data
 *  to initiate a write operation.  Leaving mode at JewelPollerModeRead
 *  skips writing and returns the read result.
 */
typedef struct {
    JewelPollerMode mode; /**< Callback sets this to Write to initiate writing. */
    const JewelData* write_data; /**< Callback sets this to the data to write. */
} JewelPollerModeRequest;

typedef union {
    JewelError error;
    JewelPollerModeRequest mode_request;
} JewelPollerEventData;

typedef struct {
    JewelPollerEventType type;
    JewelPollerEventData* data;
} JewelPollerEvent;

JewelError jewel_poller_activate(JewelPoller* instance, JewelData* data);

#ifdef __cplusplus
}
#endif
