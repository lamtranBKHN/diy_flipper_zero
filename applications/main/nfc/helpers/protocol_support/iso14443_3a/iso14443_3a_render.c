#include "iso14443_3a_render.h"

void nfc_render_iso14443_3a_format_bytes(FuriString* str, const uint8_t* data, size_t size) {
    for(size_t i = 0; i < size; i++) {
        furi_string_cat_printf(str, " %02X", data[i]);
    }
}

void nfc_render_iso14443_tech_type(const Iso14443_3aData* data, FuriString* str) {
    const char iso_type = iso14443_3a_supports_iso14443_4(data) ? '4' : '3';
    furi_string_cat_printf(str, "Tech: ISO 14443-%c (NFC-A)\n", iso_type);
}

void nfc_render_iso14443_3a_info(
    const Iso14443_3aData* data,
    NfcProtocolFormatType format_type,
    FuriString* str) {
    if(format_type == NfcProtocolFormatTypeFull) {
        nfc_render_iso14443_tech_type(data, str);
    }

    nfc_render_iso14443_3a_brief(data, str);

    if(format_type == NfcProtocolFormatTypeFull) {
        nfc_render_iso14443_3a_extra(data, str);
    }
}

void nfc_render_iso14443_3a_brief(const Iso14443_3aData* data, FuriString* str) {
    furi_string_cat_printf(str, "UID:");

    nfc_render_iso14443_3a_format_bytes(str, data->uid, data->uid_len);
}

void nfc_render_iso14443_3a_extra(const Iso14443_3aData* data, FuriString* str) {
    furi_string_cat_printf(str, "\nATQA: %02X %02X  ", data->atqa[1], data->atqa[0]);
    furi_string_cat_printf(str, "\nSAK: %02X", data->sak);
}

void nfc_render_iso14443_3a_dump(const Iso14443_3aData* data, FuriString* str) {
    furi_assert(data);
    furi_assert(str);

    furi_string_cat_printf(str, "Raw data (%zu bytes):\n", sizeof(Iso14443_3aData));

    // UID
    furi_string_cat_printf(str, "UID: ");
    for(size_t i = 0; i < data->uid_len && i < ISO14443_3A_MAX_UID_SIZE; i++) {
        furi_string_cat_printf(str, "%02X ", data->uid[i]);
    }
    furi_string_cat_printf(str, "\n");

    // ATQA
    furi_string_cat_printf(str, "ATQA: %02X %02X\n", data->atqa[1], data->atqa[0]);

    // SAK
    furi_string_cat_printf(str, "SAK: %02X\n", data->sak);

    // UID length
    furi_string_cat_printf(str, "UID len: %u\n", data->uid_len);

    // Full hex dump in 8-byte rows
    furi_string_cat_printf(str, "\n--- Raw Hex Dump ---\n");
    const uint8_t* all_bytes = (const uint8_t*)data;
    const size_t total = sizeof(Iso14443_3aData);
    for(size_t i = 0; i < total; i += 8) {
        furi_string_cat_printf(str, "%04zX:", i);
        for(size_t j = 0; j < 8 && (i + j) < total; j++) {
            furi_string_cat_printf(str, " %02X", all_bytes[i + j]);
        }
        furi_string_cat_printf(str, "\n");
    }
}
