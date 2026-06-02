#include "jewel_poller_i.h"

#include <furi_hal_nfc_pn532.h>
#include <furi_hal.h>

#define TAG "JewelPoller"

JewelError
    jewel_poller_write_block(JewelPoller* instance, uint8_t block_num, const uint8_t* data) {
    furi_assert(instance);
    furi_assert(data);
    furi_assert(block_num < JEWEL_BLOCKS_TOTAL);

    /* WRITE-E: command byte = 0x53 | (block_num << 1), followed by 8 bytes of data.
     * The block address is the block number left-shifted by 1 (each block is 2
     * bytes in the address space, but on Type 1 Tags each block is 8 bytes).
     * The card echoes the written 8 bytes on success. */
    uint8_t cmd[1 + JEWEL_BLOCK_SIZE];
    cmd[0] = 0x53 | (block_num << 1);
    memcpy(&cmd[1], data, JEWEL_BLOCK_SIZE);

    uint8_t response[16] = {0};
    size_t response_len = 0;

    FuriHalPn532Error error = furi_hal_pn532_in_communicate_thru(
        cmd, sizeof(cmd), response, sizeof(response), &response_len);

    if(error != FuriHalPn532ErrorNone) {
        FURI_LOG_D(TAG, "WRITE-E block %u failed: %s", block_num, furi_hal_pn532_error_str(error));
        return JewelErrorCommunication;
    }

    if(response_len != JEWEL_BLOCK_SIZE) {
        FURI_LOG_W(
            TAG,
            "WRITE-E block %u bad response len: %zu (expected %d)",
            block_num,
            response_len,
            JEWEL_BLOCK_SIZE);
        return JewelErrorProtocol;
    }

    /* Verify the card echoed the data we wrote */
    if(memcmp(response, data, JEWEL_BLOCK_SIZE) != 0) {
        FURI_LOG_W(TAG, "WRITE-E block %u data mismatch (write may have failed)", block_num);
        return JewelErrorProtocol;
    }

    FURI_LOG_D(
        TAG,
        "WRITE-E block %u OK: %02X%02X%02X%02X%02X%02X%02X%02X",
        block_num,
        data[0],
        data[1],
        data[2],
        data[3],
        data[4],
        data[5],
        data[6],
        data[7]);

    return JewelErrorNone;
}

JewelError jewel_poller_read_all(JewelPoller* instance) {
    furi_assert(instance);
    furi_assert(instance->data);

    /* Send READ ALL command (0x0A) via InCommunicateThru.
     * After InListPassiveTarget with BrTy=0x04 in jewel_poller_activate(),
     * the PN532 keeps the Jewel/Topaz target selected.  InCommunicateThru
     * sends the raw command on the same RF interface and returns the card's
     * full memory (128 bytes for standard Topaz, 48 bytes for smaller variants). */
    uint8_t cmd[] = {0x0A};
    uint8_t response[160] = {0};
    size_t response_len = 0;

    /* Retry once on communication error (transient bus glitch or clone module settle) */
    FuriHalPn532Error error;
    for(int retry = 0; retry < 2; retry++) {
        error = furi_hal_pn532_in_communicate_thru(
            cmd, sizeof(cmd), response, sizeof(response), &response_len);
        if(error == FuriHalPn532ErrorNone) break;
        FURI_LOG_D(
            TAG, "READ_ALL attempt %d failed: %s", retry + 1, furi_hal_pn532_error_str(error));
        furi_delay_ms(10);
    }

    if(error != FuriHalPn532ErrorNone) {
        FURI_LOG_D(TAG, "READ_ALL failed after retries: %s", furi_hal_pn532_error_str(error));
        return JewelErrorCommunication;
    }

    /* Accept responses from 48 bytes (minimum Topaz) to 128 bytes (standard).
     * Some Topaz variants (Topaz 512, Topaz 1024) may return fewer bytes. */
    if(response_len < 48 || response_len > JEWEL_DUMP_SIZE) {
        FURI_LOG_W(
            TAG, "READ_ALL unexpected length: %zu (expected 48-%d)", response_len, JEWEL_DUMP_SIZE);
        return JewelErrorProtocol;
    }

    /* Verify the dump starts with matching HR0/HR1 from the RID */
    if(response[0] != instance->data->hr0 || response[1] != instance->data->hr1) {
        FURI_LOG_W(
            TAG,
            "READ_ALL HR mismatch: got %02X%02X expected %02X%02X",
            response[0],
            response[1],
            instance->data->hr0,
            instance->data->hr1);
        return JewelErrorProtocol;
    }

    /* Copy dump (partial if shorter than expected) and zero-pad remainder */
    size_t copy_len = response_len;
    if(copy_len > JEWEL_DUMP_SIZE) copy_len = JEWEL_DUMP_SIZE;
    memcpy(instance->data->dump, response, copy_len);
    if(copy_len < JEWEL_DUMP_SIZE) {
        memset(&instance->data->dump[copy_len], 0, JEWEL_DUMP_SIZE - copy_len);
        FURI_LOG_W(TAG, "READ_ALL short: %zu/%d bytes, zero-padded", copy_len, JEWEL_DUMP_SIZE);
    }
    instance->data->dump_valid = true;

    FURI_LOG_D(
        TAG,
        "READ_ALL OK: %zu bytes, UID=%02X%02X%02X%02X",
        copy_len,
        instance->data->uid[0],
        instance->data->uid[1],
        instance->data->uid[2],
        instance->data->uid[3]);

    return JewelErrorNone;
}

JewelError jewel_poller_activate(JewelPoller* instance, JewelData* data) {
    furi_assert(instance);
    furi_assert(data);

    FuriHalPn532Target target = {0};
    if(!furi_hal_nfc_pn532_poll_jewel(&target)) {
        return JewelErrorNotPresent;
    }

    furi_assert(target.uid_len == JEWEL_UID_SIZE);
    if(target.uid_len != JEWEL_UID_SIZE) {
        FURI_LOG_W(
            TAG,
            "Activate: unexpected UID length %u (expected %u)",
            target.uid_len,
            JEWEL_UID_SIZE);
        return JewelErrorNotPresent;
    }
    data->hr0 = target.uid[0];
    data->hr1 = target.uid[1];
    memcpy(data->uid, &target.uid[2], 4);
    data->atqa[0] = target.atqa[0];
    data->atqa[1] = target.atqa[1];

    FURI_LOG_D(
        TAG,
        "Jewel detected: HR0=%02X HR1=%02X UID=%02X%02X%02X%02X",
        data->hr0,
        data->hr1,
        data->uid[0],
        data->uid[1],
        data->uid[2],
        data->uid[3]);

    return JewelErrorNone;
}

const JewelData* jewel_poller_get_data(JewelPoller* instance) {
    furi_assert(instance);
    furi_assert(instance->data);
    return instance->data;
}
