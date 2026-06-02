#pragma once

#include "jewel.h"
#include <nfc/nfc.h>

#ifdef __cplusplus
extern "C" {
#endif

JewelError jewel_poller_sync_read(Nfc* nfc, JewelData* data);

/** Perform a blocking Jewel/Topaz write operation.
 *
 * Reads the card, then writes blocks 1-15 from the provided data's
 * dump buffer.  Returns when the operation completes or fails.
 *
 * @param[in,out] nfc   NFC interface instance.
 * @param[in]     data  data to write (must have dump_valid=true).
 * @return JewelErrorNone on success, error code on failure.
 */
JewelError jewel_poller_sync_write(Nfc* nfc, const JewelData* data);

#ifdef __cplusplus
}
#endif
