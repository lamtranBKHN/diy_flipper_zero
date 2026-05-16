#include "srix_render.h"

void nfc_render_srix_info(
    const SrixData* data,
    NfcProtocolFormatType format_type,
    FuriString* str) {
    furi_string_cat_printf(str, "UID:");
    for(size_t i = 0; i < SRIX_UID_SIZE; i++) {
        furi_string_cat_printf(str, " %02X", data->uid[i]);
    }

    if(format_type != NfcProtocolFormatTypeFull) return;

    furi_string_cat_printf(str, "\nType: %s", srix_get_device_name(data, NfcDeviceNameTypeFull));
    furi_string_cat_printf(str, "\n::::::::::::::[Blocks]::::::::::::::");
    for(size_t i = 0; i < SRIX_BLOCKS_TOTAL; i++) {
        const uint8_t* block = &data->data[i * SRIX_BLOCK_SIZE];
        furi_string_cat_printf(
            str, "\n%02u: %02X %02X %02X %02X", i, block[0], block[1], block[2], block[3]);
    }
}
