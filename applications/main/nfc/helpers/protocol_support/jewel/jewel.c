#include "jewel.h"
#include "jewel_render.h"

#include <nfc/protocols/jewel/jewel_poller.h>

#include "nfc/nfc_app_i.h"

#include "../nfc_protocol_support_common.h"
#include "../nfc_protocol_support_gui_common.h"

#include <gui/icon_i.h>
#include <core/common_defines.h>

/*
 * 16x16 pixel diamond/gem icon for Jewel/Topaz cards.
 * Stored as an uncompressed Icon frame (1 byte header + 32 bytes XBM bitmap).
 * LSB-first: bit 0 = leftmost pixel.
 */
static const uint8_t jewel_logo_16x16_data[] = {
    0x00, // Uncompressed marker
    // Row  0                   Row  8
    0x80,
    0x01, //  ##                    ##
    0xC0,
    0x03, // ####                  ####
    0xE0,
    0x07, // ######              ######
    0xF0,
    0x0F, // ########          ########
    0xF8,
    0x1F, // ##########      ##########
    0xFC,
    0x3F, // ############  ############
    0xFE,
    0x7F, // ##############  ##########
    0xFF,
    0xFF, // ##########################
    0xFF,
    0xFF, // ##########################
    0xFE,
    0x7F, // ##############  ##########
    0xFC,
    0x3F, // ############  ############
    0xF8,
    0x1F, // ##########      ##########
    0xF0,
    0x0F, // ########          ########
    0xE0,
    0x07, // ######              ######
    0xC0,
    0x03, // ####                  ####
    0x80,
    0x01, // ##                      ##
};

static const uint8_t* const jewel_logo_16x16_frames[] = {
    jewel_logo_16x16_data,
};

static const Icon jewel_logo_16x16 = {
    .width = 16,
    .height = 16,
    .frame_count = 1,
    .frame_rate = 0,
    .frames = jewel_logo_16x16_frames,
};

static void nfc_scene_info_on_enter_jewel(NfcApp* instance) {
    const NfcDevice* device = instance->nfc_device;
    const JewelData* data = nfc_device_get_data(device, NfcProtocolJewel);

    FuriString* temp_str = furi_string_alloc();
    furi_string_cat_printf(
        temp_str, "\e#%s\n", nfc_device_get_name(device, NfcDeviceNameTypeFull));
    nfc_render_jewel_info(data, NfcProtocolFormatTypeFull, temp_str);
    if(data->dump_valid) {
        furi_string_cat_printf(temp_str, "\nMemory dump: OK (%d bytes)", JEWEL_DUMP_SIZE);
    }

    widget_add_icon_element(instance->widget, 0, 1, &jewel_logo_16x16);
    widget_add_text_scroll_element(
        instance->widget, 18, 0, 110, 64, furi_string_get_cstr(temp_str));

    furi_string_free(temp_str);
}

static NfcCommand nfc_scene_read_poller_callback_jewel(NfcGenericEvent event, void* context) {
    furi_assert(event.protocol == NfcProtocolJewel);

    NfcApp* instance = context;
    const JewelPollerEvent* jewel_event = event.event_data;

    if(jewel_event->type == JewelPollerEventTypeRequestMode) {
        /* Read mode — don't modify mode/write_data; stay in Read. */
        return NfcCommandContinue;
    }

    if(jewel_event->type == JewelPollerEventTypeReady) {
        nfc_device_set_data(
            instance->nfc_device, NfcProtocolJewel, nfc_poller_get_data(instance->poller));
        view_dispatcher_send_custom_event(instance->view_dispatcher, NfcCustomEventPollerSuccess);
        return NfcCommandStop;
    }

    return NfcCommandContinue;
}

static void nfc_scene_read_on_enter_jewel(NfcApp* instance) {
    nfc_poller_start(instance->poller, nfc_scene_read_poller_callback_jewel, instance);
}

static NfcCommand nfc_scene_write_poller_callback_jewel(NfcGenericEvent event, void* context) {
    furi_assert(event.protocol == NfcProtocolJewel);

    NfcApp* instance = context;
    const JewelPollerEvent* jewel_event = event.event_data;

    if(jewel_event->type == JewelPollerEventTypeRequestMode) {
        /* Set write mode and provide the data to write from the loaded device. */
        const JewelData* write_data = nfc_device_get_data(instance->nfc_device, NfcProtocolJewel);
        jewel_event->data->mode_request.mode = JewelPollerModeWrite;
        jewel_event->data->mode_request.write_data = write_data;
        return NfcCommandContinue;
    }

    if(jewel_event->type == JewelPollerEventTypeReady) {
        nfc_device_set_data(
            instance->nfc_device, NfcProtocolJewel, nfc_poller_get_data(instance->poller));
        view_dispatcher_send_custom_event(instance->view_dispatcher, NfcCustomEventPollerSuccess);
        return NfcCommandStop;
    }

    return NfcCommandContinue;
}

static void nfc_scene_write_on_enter_jewel(NfcApp* instance) {
    instance->poller = nfc_poller_alloc(instance->nfc, NfcProtocolJewel);
    nfc_poller_start(instance->poller, nfc_scene_write_poller_callback_jewel, instance);
    furi_string_set(instance->text_box_store, "Apply the card\nwith the same\nUID");
}

static void nfc_scene_more_info_on_enter_jewel(NfcApp* instance) {
    const NfcDevice* device = instance->nfc_device;
    const JewelData* data = nfc_device_get_data(device, NfcProtocolJewel);

    furi_string_reset(instance->text_box_store);
    nfc_render_jewel_dump(data, instance->text_box_store);

    text_box_set_font(instance->text_box, TextBoxFontHex);
    text_box_set_text(instance->text_box, furi_string_get_cstr(instance->text_box_store));
    view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewTextBox);
}

static void nfc_scene_read_success_on_enter_jewel(NfcApp* instance) {
    const NfcDevice* device = instance->nfc_device;
    const JewelData* data = nfc_device_get_data(device, NfcProtocolJewel);

    FuriString* temp_str = furi_string_alloc();
    furi_string_cat_printf(
        temp_str, "\e#%s\n", nfc_device_get_name(device, NfcDeviceNameTypeFull));
    nfc_render_jewel_info(data, NfcProtocolFormatTypeShort, temp_str);
    if(data->dump_valid) {
        furi_string_cat_printf(temp_str, "\nDump: %d bytes", JEWEL_DUMP_SIZE);
    }

    widget_add_icon_element(instance->widget, 0, 1, &jewel_logo_16x16);
    widget_add_text_scroll_element(
        instance->widget, 18, 0, 110, 52, furi_string_get_cstr(temp_str));

    furi_string_free(temp_str);
}

const NfcProtocolSupportBase nfc_protocol_support_jewel = {
    .features = NfcProtocolFeatureEditUid | NfcProtocolFeatureMoreInfo | NfcProtocolFeatureWrite,

    .scene_info =
        {
            .on_enter = nfc_scene_info_on_enter_jewel,
            .on_event = nfc_protocol_support_common_on_event_empty,
        },
    .scene_more_info =
        {
            .on_enter = nfc_scene_more_info_on_enter_jewel,
            .on_event = nfc_protocol_support_common_on_event_empty,
        },
    .scene_read =
        {
            .on_enter = nfc_scene_read_on_enter_jewel,
            .on_event = nfc_protocol_support_common_on_event_empty,
        },
    .scene_read_menu =
        {
            .on_enter = nfc_protocol_support_common_on_enter_empty,
            .on_event = nfc_protocol_support_common_on_event_empty,
        },
    .scene_read_success =
        {
            .on_enter = nfc_scene_read_success_on_enter_jewel,
            .on_event = nfc_protocol_support_common_on_event_empty,
        },
    .scene_saved_menu =
        {
            .on_enter = nfc_protocol_support_common_on_enter_empty,
            .on_event = nfc_protocol_support_common_on_event_empty,
        },
    .scene_save_name =
        {
            .on_enter = nfc_protocol_support_common_on_enter_empty,
            .on_event = nfc_protocol_support_common_on_event_empty,
        },
    .scene_write =
        {
            .on_enter = nfc_scene_write_on_enter_jewel,
            .on_event = nfc_protocol_support_common_on_event_empty,
        },
};

NFC_PROTOCOL_SUPPORT_PLUGIN(jewel, NfcProtocolJewel);
