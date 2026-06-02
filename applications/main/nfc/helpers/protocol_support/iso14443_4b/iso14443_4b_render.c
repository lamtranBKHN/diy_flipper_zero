#include "iso14443_4b_render.h"

#include "../iso14443_3b/iso14443_3b_render.h"

void nfc_render_iso14443_4b_info(
    const Iso14443_4bData* data,
    NfcProtocolFormatType format_type,
    FuriString* str) {
    nfc_render_iso14443_3b_info(iso14443_4b_get_base_data(data), format_type, str);
}

void nfc_render_iso14443_4b_dump(const Iso14443_4bData* data, FuriString* str) {
    furi_assert(data);
    furi_assert(str);

    furi_string_cat_printf(str, "ISO 14443-4B (NFC-B)\n");
    furi_string_cat_printf(str, "Based on ISO 14443-3B:\n");
    nfc_render_iso14443_3b_dump(iso14443_4b_get_base_data(data), str);
}
