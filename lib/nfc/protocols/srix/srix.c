#include "srix.h"

#include "flipper_format.h"
#include <furi.h>
#include <nfc/nfc_common.h>

#define SRIX_DATA_KEY "Data"

static const char* srix_type_names[] = {
    [SrixTypeUnknown] = "Unknown",
    [SrixType512] = "SRIX512",
    [SrixType4K] = "SRIX4K",
};

const NfcDeviceBase nfc_device_srix = {
    .protocol_name = SRIX_PROTOCOL_NAME,
    .alloc = (NfcDeviceAlloc)srix_alloc,
    .free = (NfcDeviceFree)srix_free,
    .reset = (NfcDeviceReset)srix_reset,
    .copy = (NfcDeviceCopy)srix_copy,
    .verify = (NfcDeviceVerify)srix_verify,
    .load = (NfcDeviceLoad)srix_load,
    .save = (NfcDeviceSave)srix_save,
    .is_equal = (NfcDeviceEqual)srix_is_equal,
    .get_name = (NfcDeviceGetName)srix_get_device_name,
    .get_uid = (NfcDeviceGetUid)srix_get_uid,
    .set_uid = (NfcDeviceSetUid)srix_set_uid,
    .get_base_data = (NfcDeviceGetBaseData)srix_get_base_data,
};

SrixData* srix_alloc(void) {
    SrixData* data = malloc(sizeof(SrixData));
    return data;
}

void srix_free(SrixData* data) {
    furi_check(data);

    free(data);
}

void srix_reset(SrixData* data) {
    furi_check(data);
    memset(data, 0, sizeof(SrixData));
}

void srix_copy(SrixData* data, const SrixData* other) {
    furi_check(data);
    furi_check(other);

    *data = *other;
}

bool srix_verify(SrixData* data, const FuriString* device_type) {
    furi_check(device_type);
    UNUSED(data);

    return furi_string_equal_str(device_type, SRIX_PROTOCOL_NAME);
}

bool srix_load(SrixData* data, FlipperFormat* ff, uint32_t version) {
    furi_check(data);
    furi_check(ff);

    bool parsed = false;

    FuriString* temp_str = furi_string_alloc();

    do {
        if(version < NFC_UNIFIED_FORMAT_VERSION) break;

        if(!flipper_format_read_hex(ff, SRIX_DATA_KEY, data->data, SRIX_DATA_SIZE)) break;

        parsed = true;
    } while(false);

    furi_string_free(temp_str);

    return parsed;
}

bool srix_save(const SrixData* data, FlipperFormat* ff) {
    furi_check(data);
    furi_check(ff);

    bool saved = false;

    do {
        if(!flipper_format_write_comment_cstr(ff, SRIX_PROTOCOL_NAME " specific data")) break;
        if(!flipper_format_write_hex(ff, SRIX_DATA_KEY, data->data, SRIX_DATA_SIZE)) break;

        saved = true;
    } while(false);

    return saved;
}

bool srix_is_equal(const SrixData* data, const SrixData* other) {
    furi_check(data);
    furi_check(other);

    return memcmp(data, other, sizeof(SrixData)) == 0;
}

const char* srix_get_device_name(const SrixData* data, NfcDeviceNameType name_type) {
    furi_check(data);
    UNUSED(name_type);

    return srix_type_names[data->type];
}

const uint8_t* srix_get_uid(const SrixData* data, size_t* uid_len) {
    furi_check(data);

    if(uid_len) {
        *uid_len = SRIX_UID_SIZE;
    }

    return data->uid;
}

bool srix_set_uid(SrixData* data, const uint8_t* uid, size_t uid_len) {
    furi_check(data);
    furi_check(uid);

    const bool uid_valid = (uid_len == SRIX_UID_SIZE);

    if(uid_valid) {
        memcpy(data->uid, uid, uid_len);
    }

    return uid_valid;
}

SrixData* srix_get_base_data(const SrixData* data) {
    UNUSED(data);
    furi_crash("No base data");
}
