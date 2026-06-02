#include "iso14443_4a_render.h"

#include "../iso14443_3a/iso14443_3a_render.h"

#include <nfc/protocols/iso14443_4a/iso14443_4a_i.h>

void nfc_render_iso14443_4a_info(
    const Iso14443_4aData* data,
    NfcProtocolFormatType format_type,
    FuriString* str) {
    nfc_render_iso14443_4a_brief(data, str);

    if(format_type != NfcProtocolFormatTypeFull) return;

    nfc_render_iso14443_4a_extra(data, str);
}

void nfc_render_iso14443_4a_brief(const Iso14443_4aData* data, FuriString* str) {
    nfc_render_iso14443_tech_type(iso14443_4a_get_base_data(data), str);
    nfc_render_iso14443_3a_brief(iso14443_4a_get_base_data(data), str);
}

void nfc_render_iso14443_4a_dump(const Iso14443_4aData* data, FuriString* str) {
    furi_assert(data);
    furi_assert(str);

    furi_string_cat_printf(str, "Raw ATS (%zu bytes):\n", sizeof(Iso14443_4aAtsData));

    // ATS TL
    furi_string_cat_printf(str, "TL: %02X\n", data->ats_data.tl);

    // ATS T0
    furi_string_cat_printf(str, "T0: %02X", data->ats_data.t0);
    if(data->ats_data.t0 & ISO14443_4A_ATS_T0_TA1) furi_string_cat(str, " (TA1 present)");
    if(data->ats_data.t0 & ISO14443_4A_ATS_T0_TB1) furi_string_cat(str, " (TB1 present)");
    if(data->ats_data.t0 & ISO14443_4A_ATS_T0_TC1) furi_string_cat(str, " (TC1 present)");
    furi_string_cat_printf(str, "\n");

    // TA1
    if(data->ats_data.t0 & ISO14443_4A_ATS_T0_TA1) {
        furi_string_cat_printf(str, "TA1: %02X\n", data->ats_data.ta_1);
    }

    // TB1
    if(data->ats_data.t0 & ISO14443_4A_ATS_T0_TB1) {
        furi_string_cat_printf(str, "TB1: %02X\n", data->ats_data.tb_1);
    }

    // TC1
    if(data->ats_data.t0 & ISO14443_4A_ATS_T0_TC1) {
        furi_string_cat_printf(str, "TC1: %02X", data->ats_data.tc_1);
        if(data->ats_data.tc_1 & ISO14443_4A_ATS_TC1_NAD) furi_string_cat(str, " (NAD)");
        if(data->ats_data.tc_1 & ISO14443_4A_ATS_TC1_CID) furi_string_cat(str, " (CID)");
        furi_string_cat_printf(str, "\n");
    }

    // Historical bytes (T1-TK)
    uint32_t hist_count = 0;
    const uint8_t* hist_bytes = iso14443_4a_get_historical_bytes(data, &hist_count);
    if(hist_count > 0) {
        furi_string_cat_printf(str, "Hist (%lu bytes):", (unsigned long)hist_count);
        for(uint32_t i = 0; i < hist_count; i++) {
            furi_string_cat_printf(str, " %02X", hist_bytes[i]);
        }
        furi_string_cat_printf(str, "\n");
    }

    // Full ATS struct hex dump
    furi_string_cat_printf(str, "\n--- ATS Hex Dump ---\n");
    const uint8_t* ats_bytes = (const uint8_t*)&data->ats_data;
    const size_t ats_total = sizeof(Iso14443_4aAtsData);
    for(size_t i = 0; i < ats_total; i += 8) {
        furi_string_cat_printf(str, "%04zX:", i);
        for(size_t j = 0; j < 8 && (i + j) < ats_total; j++) {
            furi_string_cat_printf(str, " %02X", ats_bytes[i + j]);
        }
        furi_string_cat_printf(str, "\n");
    }

    // Append the 3A dump for the base data
    furi_string_cat(str, "\n\e#--- ISO14443-3A Base ---\n");
    nfc_render_iso14443_3a_dump(iso14443_4a_get_base_data(data), str);
}

void nfc_render_iso14443_4a_extra(const Iso14443_4aData* data, FuriString* str) {
    furi_string_cat_printf(str, "\n::::::::::::::::[Protocol info]:::::::::::::::\n");

    if(iso14443_4a_supports_bit_rate(data, Iso14443_4aBitRateBoth106Kbit)) {
        furi_string_cat(str, "Bit rate PICC <-> PCD:\n  106 kBit/s supported\n");
    } else {
        furi_string_cat(str, "Bit rate PICC -> PCD:\n");
        if(iso14443_4a_supports_bit_rate(data, Iso14443_4aBitRatePiccToPcd212Kbit)) {
            furi_string_cat(str, "  212 kBit/s supported\n");
        }
        if(iso14443_4a_supports_bit_rate(data, Iso14443_4aBitRatePiccToPcd424Kbit)) {
            furi_string_cat(str, "  424 kBit/s supported\n");
        }
        if(iso14443_4a_supports_bit_rate(data, Iso14443_4aBitRatePiccToPcd848Kbit)) {
            furi_string_cat(str, "  848 kBit/s supported\n");
        }

        furi_string_cat(str, "Bit rate PICC <- PCD:\n");
        if(iso14443_4a_supports_bit_rate(data, Iso14443_4aBitRatePcdToPicc212Kbit)) {
            furi_string_cat(str, "  212 kBit/s supported\n");
        }
        if(iso14443_4a_supports_bit_rate(data, Iso14443_4aBitRatePcdToPicc424Kbit)) {
            furi_string_cat(str, "  424 kBit/s supported\n");
        }
        if(iso14443_4a_supports_bit_rate(data, Iso14443_4aBitRatePcdToPicc848Kbit)) {
            furi_string_cat(str, "  848 kBit/s supported\n");
        }
    }

    furi_string_cat(str, "Max frame size: ");

    const uint16_t max_frame_size = iso14443_4a_get_frame_size_max(data);
    if(max_frame_size != 0) {
        furi_string_cat_printf(str, "%u bytes\n", max_frame_size);
    } else {
        furi_string_cat(str, "? (RFU)\n");
    }

    const uint32_t fwt_fc = iso14443_4a_get_fwt_fc_max(data);
    if(fwt_fc != 0) {
        furi_string_cat_printf(str, "Max waiting time: %4.2g s\n", (double)(fwt_fc / 13.56e6));
    }

    const char* nad_support_str =
        iso14443_4a_supports_frame_option(data, Iso14443_4aFrameOptionNad) ? "" : "not ";
    furi_string_cat_printf(str, "NAD: %ssupported\n", nad_support_str);

    const char* cid_support_str =
        iso14443_4a_supports_frame_option(data, Iso14443_4aFrameOptionCid) ? "" : "not ";
    furi_string_cat_printf(str, "CID: %ssupported", cid_support_str);

    uint32_t hist_bytes_count;
    const uint8_t* hist_bytes = iso14443_4a_get_historical_bytes(data, &hist_bytes_count);

    if(hist_bytes_count > 0) {
        furi_string_cat_printf(str, "\n:::::::::::::[Historical bytes]:::::::::::::\nRaw:");

        for(size_t i = 0; i < hist_bytes_count; ++i) {
            furi_string_cat_printf(str, " %02X", hist_bytes[i]);
        }
    }

    furi_string_cat(str, "\n\e#ISO14443-3A data");
    nfc_render_iso14443_3a_extra(iso14443_4a_get_base_data(data), str);
}
