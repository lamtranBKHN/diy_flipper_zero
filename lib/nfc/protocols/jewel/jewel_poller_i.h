#pragma once

#include "jewel_poller.h"

#include <nfc/protocols/nfc_generic_event.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JewelPollerStateIdle,
    JewelPollerStateReadBlock0,
    JewelPollerStateReadAllBlocks,
    JewelPollerStateWriteBlocks, /**< Write blocks 1-15 to card (write mode). */
    JewelPollerStateReadSuccess,
    JewelPollerStateReadFailed,
    JewelPollerStateWriteSuccess, /**< All blocks written successfully. */
    JewelPollerStateWriteFailed, /**< Write operation failed. */

    JewelPollerStateNum,
} JewelPollerState;

struct JewelPoller {
    Nfc* nfc;
    JewelPollerState state;

    JewelData* data;

    JewelPollerMode mode; /**< Read or write mode (set via RequestMode event). */
    const JewelData* write_data; /**< Data to write (set via RequestMode event). */

    NfcGenericEvent general_event;
    JewelPollerEvent jewel_event;
    JewelPollerEventData jewel_event_data;
    NfcGenericCallback callback;
    void* context;
};

const JewelData* jewel_poller_get_data(JewelPoller* instance);
JewelError jewel_poller_read_all(JewelPoller* instance);

/** Write an 8-byte block to the Jewel/Topaz card using WRITE-E.
 *
 * Sends WRITE-E (0x53 | block<<1) + 8 bytes via InCommunicateThru.
 * Must be called with an active Jewel target selected on the PN532.
 *
 * @param[in,out] instance  poller instance (for assertion only).
 * @param[in]     block_num block number (0-15).  Block 0 is OTP and cannot
 *                          be rewritten.
 * @param[in]     data      8 bytes of data to write.
 * @return JewelErrorNone on success, error code on failure.
 */
JewelError jewel_poller_write_block(JewelPoller* instance, uint8_t block_num, const uint8_t* data);

#ifdef __cplusplus
}
#endif
