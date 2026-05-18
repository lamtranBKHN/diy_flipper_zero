#include "../nfc_app_i.h"

#include <furi_hal_pn532.h>
#include <furi_hal_nfc_pn532.h>

void nfc_scene_pn532_info_on_enter(void* context) {
    NfcApp* nfc = context;
    Popup* popup = nfc->popup;

    bool ready = furi_hal_pn532_is_ready();
    bool status = furi_hal_pn532_read_status();
    FuriHalPn532Error last_err = furi_hal_nfc_pn532_last_error_get();
    const char* last_err_str = furi_hal_nfc_pn532_last_error_str();
    const char* last_result_str = furi_hal_nfc_pn532_last_result_str();

    FuriString* info = furi_string_alloc_printf(
        "Ready:%s Status:%s\nErr:%s (%d)\nRes:%s",
        ready ? "OK" : "FAIL",
        status ? "OK" : "FAIL",
        last_err_str ? last_err_str : "none",
        last_err,
        last_result_str ? last_result_str : "none");

    popup_set_header(popup, "PN532 Info", 64, 11, AlignCenter, AlignTop);
    popup_set_text(popup, furi_string_get_cstr(info), 64, 32, AlignCenter, AlignCenter);
    view_dispatcher_switch_to_view(nfc->view_dispatcher, NfcViewPopup);

    furi_string_free(info);
}

bool nfc_scene_pn532_info_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void nfc_scene_pn532_info_on_exit(void* context) {
    NfcApp* nfc = context;
    popup_reset(nfc->popup);
}
