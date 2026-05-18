#include "../nfc_app_i.h"

#include <furi_hal_nfc.h>
#include <furi_hal_pn532.h>

#define POLLER_TEST_TIMEOUT_MS 500

void nfc_scene_poller_test_on_enter(void* context) {
    NfcApp* nfc = context;
    Popup* popup = nfc->popup;

    furi_hal_nfc_low_power_mode_stop();

    FuriHalPn532Target target = {};
    uint32_t start = furi_get_tick();
    bool found = furi_hal_pn532_poll_iso14443a_timeout(&target, POLLER_TEST_TIMEOUT_MS);
    uint32_t elapsed = furi_get_tick() - start;

    FuriString* info = furi_string_alloc();
    if(found) {
        furi_string_printf(
            info,
            "ATQA:%02X%02X SAK:%02X\nUID:%02X%02X%02X%02X\nTime:%lums",
            target.atqa[0],
            target.atqa[1],
            target.sak,
            target.uid[0],
            target.uid[1],
            target.uid[2],
            target.uid[3],
            (unsigned long)elapsed);
    } else {
        furi_string_printf(info, "No tag found\nTime:%lums", (unsigned long)elapsed);
    }

    popup_set_header(popup, "Poller Test", 64, 11, AlignCenter, AlignTop);
    popup_set_text(popup, furi_string_get_cstr(info), 64, 32, AlignCenter, AlignCenter);
    view_dispatcher_switch_to_view(nfc->view_dispatcher, NfcViewPopup);

    furi_string_free(info);
}

bool nfc_scene_poller_test_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void nfc_scene_poller_test_on_exit(void* context) {
    NfcApp* nfc = context;
    furi_hal_nfc_low_power_mode_start();
    popup_reset(nfc->popup);
}
