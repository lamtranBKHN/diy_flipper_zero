// Parser for Jewel/Topaz (Innovation) NFC tags.
// Identifies card type based on HR0/HR1 header bytes from the RID response.

#include "nfc_supported_card_plugin.h"

#include <flipper_application/flipper_application.h>
#include <nfc/protocols/jewel/jewel.h>
#include <nfc/protocols/jewel/jewel_poller_sync.h>

#define TAG "JewelParser"

static const char* jewel_get_card_type(uint8_t hr0, uint8_t hr1) {
    if(hr0 == 0x78) {
        switch(hr1) {
        case 0x00:
            return "Topaz 512B";
        case 0x01:
            return "Jewel 128B";
        case 0x02:
            return "Topaz 32B";
        default:
            return "Innovation (Topaz/Jewel)";
        }
    }
    return "Unknown Jewel family";
}

static bool jewel_parser_verify(Nfc* nfc) {
    furi_assert(nfc);

    bool verified = false;
    JewelData* data = jewel_alloc();

    do {
        JewelError error = jewel_poller_sync_read(nfc, data);
        if(error != JewelErrorNone) {
            FURI_LOG_D(TAG, "Verify failed: %d", error);
            break;
        }
        if(data->hr0 != 0x78) {
            FURI_LOG_D(TAG, "Verify failed: HR0=0x%02X, expected 0x78", data->hr0);
            break;
        }
        verified = true;
    } while(false);

    jewel_free(data);
    return verified;
}

static bool jewel_parser_parse(const NfcDevice* device, FuriString* parsed_data) {
    furi_assert(device);
    furi_assert(parsed_data);

    const JewelData* data = nfc_device_get_data(device, NfcProtocolJewel);

    bool parsed = false;

    do {
        const char* card_type = jewel_get_card_type(data->hr0, data->hr1);

        furi_string_printf(
            parsed_data,
            "\e#Jewel / Topaz Tag\n"
            "Card Type: %s\n"
            "HR0: 0x%02X\n"
            "HR1: 0x%02X\n"
            "UID: %02X %02X %02X %02X %02X %02X\n"
            "ATQA: 0x%02X%02X",
            card_type,
            data->hr0,
            data->hr1,
            data->hr0,
            data->hr1,
            data->uid[0],
            data->uid[1],
            data->uid[2],
            data->uid[3],
            data->atqa[0],
            data->atqa[1]);

        parsed = true;
    } while(false);

    return parsed;
}

/* Actual implementation of app<>plugin interface */
static const NfcSupportedCardsPlugin jewel_parser_plugin = {
    .protocol = NfcProtocolJewel,
    .verify = jewel_parser_verify,
    .read = NULL,
    .parse = jewel_parser_parse,
};

/* Plugin descriptor to comply with basic plugin specification */
static const FlipperAppPluginDescriptor jewel_parser_plugin_descriptor = {
    .appid = NFC_SUPPORTED_CARD_PLUGIN_APP_ID,
    .ep_api_version = NFC_SUPPORTED_CARD_PLUGIN_API_VERSION,
    .entry_point = &jewel_parser_plugin,
};

/* Plugin entry point - must return a pointer to const descriptor  */
const FlipperAppPluginDescriptor* jewel_parser_plugin_ep(void) {
    return &jewel_parser_plugin_descriptor;
}
