#include "jewel.h"

#include <furi.h>
#include <nfc/nfc_common.h>

#define JEWEL_PROTOCOL_NAME "Jewel"
#define JEWEL_DEVICE_NAME   "Jewel/Topaz"

const NfcDeviceBase nfc_device_jewel = {
    .protocol_name = JEWEL_PROTOCOL_NAME,
    .alloc = (NfcDeviceAlloc)jewel_alloc,
    .free = (NfcDeviceFree)jewel_free,
    .reset = (NfcDeviceReset)jewel_reset,
    .copy = (NfcDeviceCopy)jewel_copy,
    .verify = (NfcDeviceVerify)jewel_verify,
    .load = (NfcDeviceLoad)jewel_load,
    .save = (NfcDeviceSave)jewel_save,
    .is_equal = (NfcDeviceEqual)jewel_is_equal,
    .get_name = (NfcDeviceGetName)jewel_get_device_name,
    .get_uid = (NfcDeviceGetUid)jewel_get_uid,
    .set_uid = (NfcDeviceSetUid)jewel_set_uid,
    .get_base_data = (NfcDeviceGetBaseData)jewel_get_base_data,
};

JewelData* jewel_alloc(void) {
    JewelData* data = malloc(sizeof(JewelData));
    furi_check(data);
    return data;
}

void jewel_free(JewelData* data) {
    furi_check(data);
    free(data);
}

void jewel_reset(JewelData* data) {
    furi_check(data);
    memset(data, 0, sizeof(JewelData));
}

void jewel_copy(JewelData* data, const JewelData* other) {
    furi_check(data);
    furi_check(other);
    *data = *other;
}

bool jewel_verify(JewelData* data, const FuriString* device_type) {
    UNUSED(device_type);
    furi_check(data);
    return data->atqa[0] == 0x0C;
}

bool jewel_load(JewelData* data, FlipperFormat* ff, uint32_t version) {
    furi_check(data);
    furi_check(ff);

    bool parsed = false;
    do {
        if(version < NFC_UNIFIED_FORMAT_VERSION) break;
        uint8_t uid_buf[JEWEL_UID_SIZE] = {0};
        if(!flipper_format_read_hex(ff, "UID", uid_buf, JEWEL_UID_SIZE)) break;
        data->hr0 = uid_buf[0];
        data->hr1 = uid_buf[1];
        memcpy(data->uid, &uid_buf[2], 4);

        /* Try to load full dump — optional field */
        data->dump_valid = flipper_format_read_hex(ff, "Memory", data->dump, JEWEL_DUMP_SIZE);
        if(data->dump_valid) {
            FURI_LOG_D("Jewel", "Loaded full memory dump (%d bytes)", JEWEL_DUMP_SIZE);
        }

        parsed = true;
    } while(false);

    return parsed;
}

bool jewel_save(const JewelData* data, FlipperFormat* ff) {
    furi_check(data);
    furi_check(ff);

    bool saved = false;
    do {
        if(!flipper_format_write_comment_cstr(ff, JEWEL_PROTOCOL_NAME " specific data")) break;
        uint8_t uid_buf[JEWEL_UID_SIZE];
        uid_buf[0] = data->hr0;
        uid_buf[1] = data->hr1;
        memcpy(&uid_buf[2], data->uid, 4);
        if(!flipper_format_write_hex(ff, "UID", uid_buf, JEWEL_UID_SIZE)) break;

        /* Save full dump if available */
        if(data->dump_valid) {
            if(!flipper_format_write_comment_cstr(ff, "Full memory dump")) break;
            if(!flipper_format_write_hex(ff, "Memory", data->dump, JEWEL_DUMP_SIZE)) break;
        }

        saved = true;
    } while(false);

    return saved;
}

bool jewel_is_equal(const JewelData* data, const JewelData* other) {
    furi_check(data);
    furi_check(other);
    return data->hr0 == other->hr0 && data->hr1 == other->hr1 &&
           memcmp(data->uid, other->uid, sizeof(data->uid)) == 0;
}

const char* jewel_get_device_name(const JewelData* data, NfcDeviceNameType name_type) {
    UNUSED(data);
    UNUSED(name_type);
    return JEWEL_DEVICE_NAME;
}

const uint8_t* jewel_get_uid(const JewelData* data, size_t* uid_len) {
    furi_check(data);
    if(uid_len) {
        *uid_len = JEWEL_UID_SIZE;
    }
    return &data->hr0;
}

bool jewel_set_uid(JewelData* data, const uint8_t* uid, size_t uid_len) {
    furi_check(data);
    const bool uid_valid = uid_len == JEWEL_UID_SIZE;
    if(uid_valid) {
        data->hr0 = uid[0];
        data->hr1 = uid[1];
        memcpy(data->uid, &uid[2], 4);
        data->dump_valid = false;
    }
    return uid_valid;
}

JewelData* jewel_get_base_data(const JewelData* data) {
    UNUSED(data);
    furi_crash("No base data");
}
