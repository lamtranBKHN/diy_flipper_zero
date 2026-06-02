#include "jewel_render.h"
#include <furi.h>

static const char* jewel_get_card_type(uint8_t hr0, uint8_t hr1) {
    // Jewel/Topaz identification based on HR0/HR1
    // HR0 = 0x78 indicates Jewel/Topaz protocol family
    if(hr0 == 0x78) {
        if(hr1 == 0x00) {
            return "Topaz (512 byte)";
        } else if(hr1 == 0x01) {
            return "Jewel (128 byte)";
        } else if(hr1 == 0x10) {
            return "Topaz (32 byte)";
        } else if(hr1 == 0x11) {
            return "Jewel Topaz (96 byte)";
        } else if(hr1 == 0x20) {
            return "Topaz (128 block)";
        } else if(hr1 == 0x30) {
            return "Jewel (8 block)";
        } else if(hr1 >= 0x40 && hr1 <= 0x4F) {
            return "Jewel/Topaz (NFC Tag)";
        } else {
            return "Jewel/Topaz";
        }
    } else if(hr0 == 0x00 && hr1 == 0x00) {
        return "Uninitialized";
    }
    return "Unknown Jewel family";
}

void nfc_render_jewel_info(
    const JewelData* data,
    NfcProtocolFormatType format_type,
    FuriString* str) {
    furi_assert(data);
    furi_assert(str);

    switch(format_type) {
    case NfcProtocolFormatTypeFull:
        furi_string_cat_printf(str, "Card type: %s\n", jewel_get_card_type(data->hr0, data->hr1));
        furi_string_cat_printf(str, "HR0: %02X  HR1: %02X\n", data->hr0, data->hr1);
        furi_string_cat_printf(
            str,
            "UID: %02X %02X %02X %02X %02X %02X\n",
            data->hr0,
            data->hr1,
            data->uid[0],
            data->uid[1],
            data->uid[2],
            data->uid[3]);
        furi_string_cat_printf(str, "ATQA: %02X%02X\n", data->atqa[0], data->atqa[1]);
        break;
    case NfcProtocolFormatTypeShort:
        furi_string_cat_printf(str, "%s\n", jewel_get_card_type(data->hr0, data->hr1));
        furi_string_cat_printf(
            str,
            "UID: %02X %02X %02X %02X %02X %02X",
            data->hr0,
            data->hr1,
            data->uid[0],
            data->uid[1],
            data->uid[2],
            data->uid[3]);
        break;
    }
}

void nfc_render_jewel_dump(const JewelData* data, FuriString* str) {
    furi_assert(data);
    furi_assert(str);

    if(!data->dump_valid) {
        furi_string_cat_printf(str, "No dump data available\n");
        return;
    }

    for(uint8_t block = 0; block < JEWEL_BLOCKS_TOTAL; block++) {
        furi_string_cat_printf(str, "Block %02X:", block);
        for(uint8_t off = 0; off < JEWEL_BLOCK_SIZE; off += 2) {
            furi_string_cat_printf(
                str,
                " %02X%02X",
                data->dump[block * JEWEL_BLOCK_SIZE + off],
                data->dump[block * JEWEL_BLOCK_SIZE + off + 1]);
        }
        furi_string_cat_printf(str, "\n");
    }
}
