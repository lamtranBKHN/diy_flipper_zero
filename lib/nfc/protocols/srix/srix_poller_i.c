#include "srix_poller_i.h"

#include <furi_hal_pn532.h>

#define TAG "SrixPoller"

SrixError srix_poller_detect_tag(SrixPoller* instance, uint8_t* chip_id) {
    furi_check(instance);

    SrixError error = SrixErrorNone;

    do {
        if(!furi_hal_pn532_srix_detect(chip_id)) {
            error = SrixErrorNotPresent;
            break;
        }
    } while(false);

    return error;
}

SrixError srix_poller_select_tag(SrixPoller* instance, uint8_t chip_id) {
    furi_check(instance);

    SrixError error = SrixErrorNone;

    do {
        if(!furi_hal_pn532_srix_select(chip_id)) {
            error = SrixErrorCommunication;
            break;
        }
    } while(false);

    return error;
}

SrixError srix_poller_read_uid(SrixPoller* instance, uint8_t* uid) {
    furi_check(instance);
    furi_check(uid);

    SrixError error = SrixErrorNone;

    do {
        size_t uid_len = 0;
        if(!furi_hal_pn532_srix_get_uid(uid, &uid_len)) {
            error = SrixErrorCommunication;
            break;
        }
        if(uid_len != SRIX_UID_SIZE) {
            error = SrixErrorCommunication;
            break;
        }
    } while(false);

    return error;
}

SrixError srix_poller_read_block(SrixPoller* instance, uint8_t* block_data, uint8_t block_number) {
    furi_check(instance);
    furi_check(block_data);
    furi_check(block_number < SRIX_BLOCKS_TOTAL);

    SrixError error = SrixErrorNone;

    do {
        if(!furi_hal_pn532_srix_read_block(block_number, block_data)) {
            error = SrixErrorCommunication;
            break;
        }
    } while(false);

    return error;
}

SrixError
    srix_poller_write_block(SrixPoller* instance, const uint8_t* block_data, uint8_t block_number) {
    furi_check(instance);
    furi_check(block_data);
    furi_check(block_number < SRIX_BLOCKS_TOTAL);

    SrixError error = SrixErrorNone;

    do {
        if(!furi_hal_pn532_srix_write_block(block_number, block_data)) {
            error = SrixErrorWriteFailed;
            break;
        }
    } while(false);

    return error;
}
