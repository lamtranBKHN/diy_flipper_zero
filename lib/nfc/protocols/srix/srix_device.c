#include "srix_device.h"

#include <nfc/nfc_device.h>
#include <furi.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>

#define TAG "SrixDevice"

#define SRIX_DEVICE_NAME     "SRIX"
#define SRIX_FILE_TYPE       "Flipper NFC device"
#define SRIX_FILE_VERSION    (2U)
#define SRIX_KEY_DEVICE_TYPE "Device type"
#define SRIX_KEY_UID         "UID"
#define SRIX_KEY_DATA        "Data"

bool srix_device_save(const SrixData* data, const char* path) {
    furi_check(data);
    furi_check(path);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);

    bool saved = false;
    do {
        if(!flipper_format_file_open_always(file, path)) break;
        if(!flipper_format_write_header_cstr(file, SRIX_FILE_TYPE, SRIX_FILE_VERSION)) break;
        if(!flipper_format_write_comment_cstr(file, SRIX_DEVICE_NAME " data file")) break;
        if(!flipper_format_write_string_cstr(file, SRIX_KEY_DEVICE_TYPE, SRIX_DEVICE_NAME)) break;
        if(!flipper_format_write_hex(file, SRIX_KEY_UID, data->uid, SRIX_UID_SIZE)) break;
        if(!flipper_format_write_hex(file, SRIX_KEY_DATA, data->data, SRIX_DATA_SIZE)) break;

        saved = true;
    } while(false);

    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
    return saved;
}

bool srix_device_load(SrixData* data, const char* path) {
    furi_check(data);
    furi_check(path);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);

    bool loaded = false;
    do {
        if(!flipper_format_file_open_existing(file, path)) break;

        uint32_t version;
        if(!flipper_format_read_header(file, NULL, &version)) break;
        if(version != SRIX_FILE_VERSION) break;

        if(!flipper_format_read_hex(file, SRIX_KEY_UID, data->uid, SRIX_UID_SIZE)) break;

        if(!flipper_format_read_hex(file, SRIX_KEY_DATA, data->data, SRIX_DATA_SIZE)) break;

        loaded = true;
    } while(false);

    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
    return loaded;
}
