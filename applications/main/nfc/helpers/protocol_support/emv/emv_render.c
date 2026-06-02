#include "emv_render.h"

#include "../iso14443_4a/iso14443_4a_render.h"
#include <bit_lib.h>
#include "nfc/nfc_app_i.h"

void nfc_render_emv_info(const EmvData* data, NfcProtocolFormatType format_type, FuriString* str) {
    nfc_render_emv_header(str);
    nfc_render_emv_uid(
        data->iso14443_4a_data->iso14443_3a_data->uid,
        data->iso14443_4a_data->iso14443_3a_data->uid_len,
        str);

    if(format_type == NfcProtocolFormatTypeFull) nfc_render_emv_extra(data, str);
}

void nfc_render_emv_header(FuriString* str) {
    furi_string_cat_printf(str, "\e#%s\n", "EMV");
}

void nfc_render_emv_uid(const uint8_t* uid, const uint8_t uid_len, FuriString* str) {
    if(uid_len == 0) return;

    furi_string_cat_printf(str, "UID: ");

    for(uint8_t i = 0; i < uid_len; i++) {
        furi_string_cat_printf(str, "%02X ", uid[i]);
    }

    furi_string_cat_printf(str, "\n");
}

void nfc_render_emv_name(const char* data, FuriString* str) {
    if(!data || data[0] == '\0') return;
    furi_string_cat_printf(str, "Name: %s\n", data);
}

void nfc_render_emv_data(const EmvData* data, FuriString* str) {
    nfc_render_emv_pan(data->emv_application.pan, data->emv_application.pan_len, str);
    nfc_render_emv_name(data->emv_application.application_name, str);
}

void nfc_render_emv_pan(const uint8_t* data, const uint8_t len, FuriString* str) {
    if(len == 0) return;

    FuriString* card_number = furi_string_alloc();
    for(uint8_t i = 0; i < len; i++) {
        if((i % 2 == 0) && (i != 0)) furi_string_cat_printf(card_number, " ");
        furi_string_cat_printf(card_number, "%02X", data[i]);
    }

    // Cut padding 'F' from card number
    furi_string_trim(card_number, "F");
    furi_string_cat(str, card_number);
    furi_string_free(card_number);

    furi_string_cat_printf(str, "\n");
}

void nfc_render_emv_currency(uint16_t cur_code, FuriString* str) {
    if(!cur_code) return;

    furi_string_cat_printf(str, "Currency code: %04X\n", cur_code);
}

void nfc_render_emv_country(uint16_t country_code, FuriString* str) {
    if(!country_code) return;

    furi_string_cat_printf(str, "Country code: %04X\n", country_code);
}

void nfc_render_emv_application(const EmvApplication* apl, FuriString* str) {
    const uint8_t len = apl->aid_len;

    furi_string_cat_printf(str, "AID: ");
    for(uint8_t i = 0; i < len; i++)
        furi_string_cat_printf(str, "%02X", apl->aid[i]);
    furi_string_cat_printf(str, "\n");
}

void nfc_render_emv_application_interchange_profile(const EmvApplication* apl, FuriString* str) {
    uint16_t data = bit_lib_bytes_to_num_be(apl->application_interchange_profile, 2);

    if(!data) {
        furi_string_cat_printf(str, "No Interchange profile found\n");
        return;
    }

    furi_string_cat_printf(str, "Interchange profile: ");
    for(uint8_t i = 0; i < 2; i++)
        furi_string_cat_printf(str, "%02X", apl->application_interchange_profile[i]);
    furi_string_cat_printf(str, "\n");
}

void nfc_render_emv_transactions(const EmvApplication* apl, FuriString* str) {
    if(apl->transaction_counter)
        furi_string_cat_printf(str, "Transactions count: %d\n", apl->transaction_counter);
    if(apl->last_online_atc)
        furi_string_cat_printf(str, "Last Online ATC: %d\n", apl->last_online_atc);

    const uint8_t len = apl->active_tr;
    if(!len) {
        furi_string_cat_printf(str, "No transactions info\n");
        return;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* tmp = furi_string_alloc();

    furi_string_cat_printf(str, "Transactions:\n");
    for(int i = 0; i < len; i++) {
        // If no date and amount - skip
        if((!apl->trans[i].date) && (!apl->trans[i].amount)) continue;
        // transaction counter
        furi_string_cat_printf(str, "\e#%d: ", apl->trans[i].atc);

        // Print transaction amount
        if(!apl->trans[i].amount) {
            furi_string_cat_printf(str, "???");
        } else {
            FURI_LOG_D("EMV Render", "Amount: %llX\n", apl->trans[i].amount);
            uint8_t amount_bytes[6];
            bit_lib_num_to_bytes_le(apl->trans[i].amount, 6, amount_bytes);

            bool junk = false;
            uint64_t amount = bit_lib_bytes_to_num_bcd(amount_bytes, 6, &junk);
            uint8_t amount_cents = amount % 100;

            furi_string_cat_printf(str, "%llu.%02u", amount / 100, amount_cents);
        }

        if(apl->trans[i].currency) {
            furi_string_set_str(tmp, "UNK");
            nfc_emv_parser_get_currency_name(storage, apl->trans[i].currency, tmp);
            furi_string_cat_printf(str, " %s\n", furi_string_get_cstr(tmp));
        }

        if(apl->trans[i].country) {
            furi_string_set_str(tmp, "UNK");
            nfc_emv_parser_get_country_name(storage, apl->trans[i].country, tmp);
            furi_string_cat_printf(str, "Country: %s\n", furi_string_get_cstr(tmp));
        }

        if(apl->trans[i].date)
            furi_string_cat_printf(
                str,
                "%02lx.%02lx.%02lx  ",
                apl->trans[i].date >> 16,
                (apl->trans[i].date >> 8) & 0xff,
                apl->trans[i].date & 0xff);

        if(apl->trans[i].time)
            furi_string_cat_printf(
                str,
                "%02lx:%02lx:%02lx",
                apl->trans[i].time & 0xff,
                (apl->trans[i].time >> 8) & 0xff,
                apl->trans[i].time >> 16);

        // Line break
        furi_string_cat_printf(str, "\n");
    }

    furi_string_free(tmp);
    furi_record_close(RECORD_STORAGE);
}

void nfc_render_emv_extra(const EmvData* data, FuriString* str) {
    nfc_render_emv_application(&data->emv_application, str);
    nfc_render_emv_application_interchange_profile(&data->emv_application, str);

    nfc_render_emv_currency(data->emv_application.currency_code, str);
    nfc_render_emv_country(data->emv_application.country_code, str);
}

void nfc_render_emv_dump(const EmvData* data, FuriString* str) {
    furi_assert(data);
    furi_assert(str);

    furi_string_cat_printf(str, "\e#EMV Raw Data\n");

    // UID from ISO14443-3A
    const Iso14443_3aData* iso3a = data->iso14443_4a_data->iso14443_3a_data;
    furi_string_cat_printf(str, "UID (%u): ", iso3a->uid_len);
    for(uint8_t i = 0; i < iso3a->uid_len; i++) {
        furi_string_cat_printf(str, "%02X ", iso3a->uid[i]);
    }
    furi_string_cat_printf(str, "\n");
    furi_string_cat_printf(str, "ATQA: %02X%02X\n", iso3a->atqa[1], iso3a->atqa[0]);
    furi_string_cat_printf(str, "SAK: %02X\n", iso3a->sak);

    // ATS from ISO14443-4A
    const Iso14443_4aAtsData* ats = &data->iso14443_4a_data->ats_data;
    if(ats->tl > 0) {
        furi_string_cat_printf(str, "\nATS (TL=%u):\n", ats->tl);
        furi_string_cat_printf(str, "  T0=%02X", ats->t0);
        if(ats->t0 & (1U << 4)) furi_string_cat_printf(str, " TA1=%02X", ats->ta_1);
        if(ats->t0 & (1U << 5)) furi_string_cat_printf(str, " TB1=%02X", ats->tb_1);
        if(ats->t0 & (1U << 6)) furi_string_cat_printf(str, " TC1=%02X", ats->tc_1);
        furi_string_cat_printf(str, "\n");
        uint32_t hist_count = 0;
        const uint8_t* hist =
            iso14443_4a_get_historical_bytes(data->iso14443_4a_data, &hist_count);
        if(hist_count > 0) {
            furi_string_cat_printf(str, "  T1-TK (%lu): ", hist_count);
            for(uint32_t i = 0; i < hist_count; i++) {
                furi_string_cat_printf(str, "%02X ", hist[i]);
            }
            furi_string_cat_printf(str, "\n");
        }
    }

    // Application ID (AID)
    const EmvApplication* apl = &data->emv_application;
    if(apl->aid_len > 0) {
        furi_string_cat_printf(str, "\nAID (%u): ", apl->aid_len);
        for(uint8_t i = 0; i < apl->aid_len; i++) {
            furi_string_cat_printf(str, "%02X", apl->aid[i]);
        }
        furi_string_cat_printf(str, "\n");
    }

    // PDOL
    if(apl->pdol.size > 0) {
        furi_string_cat_printf(str, "PDOL (%u): ", apl->pdol.size);
        for(uint8_t i = 0; i < apl->pdol.size; i++) {
            furi_string_cat_printf(str, "%02X ", apl->pdol.data[i]);
        }
        furi_string_cat_printf(str, "\n");
    }

    // AFL
    if(apl->afl.size > 0) {
        furi_string_cat_printf(str, "AFL (%u): ", apl->afl.size);
        for(uint8_t i = 0; i < apl->afl.size; i++) {
            furi_string_cat_printf(str, "%02X ", apl->afl.data[i]);
        }
        furi_string_cat_printf(str, "\n");
    }

    // PAN
    if(apl->pan_len > 0) {
        furi_string_cat_printf(str, "PAN (%u): ", apl->pan_len);
        for(uint8_t i = 0; i < apl->pan_len; i++) {
            furi_string_cat_printf(str, "%02X", apl->pan[i]);
        }
        furi_string_cat_printf(str, "\n");
    }

    // Application Interchange Profile
    furi_string_cat_printf(
        str,
        "AIP: %02X %02X\n",
        apl->application_interchange_profile[0],
        apl->application_interchange_profile[1]);
}
