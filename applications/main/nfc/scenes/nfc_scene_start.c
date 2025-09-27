#include "../nfc_app_i.h"
#include <dolphin/dolphin.h>

enum SubmenuIndex {
    SubmenuIndexRead,
    SubmenuIndexDetectReader,
    SubmenuIndexSaved,
    SubmenuIndexExtraAction,
    SubmenuIndexAddManually,
    SubmenuIndexDebug,
};

void nfc_scene_start_submenu_callback(void* context, uint32_t index) {
    NfcApp* nfc = context;

    view_dispatcher_send_custom_event(nfc->view_dispatcher, index);
}

void nfc_scene_start_on_enter(void* context) {
    
     FURI_LOG_E("NFC_SCENE_START", "On Enter event for Start Scene has fired!");
    NfcApp* nfc = context;

    // ====================================================================
    // --- NEW TEST CODE: Try to show a simple popup ---
    // This will tell us if the view_dispatcher can draw ANYTHING.
    // ====================================================================

    popup_set_header(nfc->popup, "Debug Test", 64, 12, AlignCenter, AlignBottom);
    popup_set_text(nfc->popup, "Can you see me?", 64, 24, AlignCenter, AlignTop);
    view_dispatcher_switch_to_view(nfc->view_dispatcher, NfcViewPopup);


    // ====================================================================
    // --- OLD CODE: Comment this out for the test ---
    // ====================================================================
    /*
    Submenu* submenu = nfc->submenu;

    // Clear file name and device contents
    furi_string_reset(nfc->file_name);
    nfc_device_clear(nfc->nfc_device);
    iso14443_3a_reset(nfc->iso14443_3a_edit_data);
    // Reset detected protocols list
    nfc_detected_protocols_reset(nfc->detected_protocols);

    submenu_add_item(submenu, "Read", SubmenuIndexRead, nfc_scene_start_submenu_callback, nfc);
    submenu_add_item(
        submenu,
        "Extract MFC Keys",
        SubmenuIndexDetectReader,
        nfc_scene_start_submenu_callback,
        nfc);
    submenu_add_item(submenu, "Saved", SubmenuIndexSaved, nfc_scene_start_submenu_callback, nfc);
    submenu_add_item(
        submenu, "Extra Actions", SubmenuIndexExtraAction, nfc_scene_start_submenu_callback, nfc);
    submenu_add_item(
        submenu, "Add Manually", SubmenuIndexAddManually, nfc_scene_start_submenu_callback, nfc);

    submenu_add_lockable_item(
        submenu,
        "Debug",
        SubmenuIndexDebug,
        nfc_scene_start_submenu_callback,
        nfc,
        !furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug),
        "Enable\n"
        "Settings >\n"
        "System >\n"
        "Debug");

    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(nfc->scene_manager, NfcSceneStart));

    view_dispatcher_switch_to_view(nfc->view_dispatcher, NfcViewMenu);
    */
}

bool nfc_scene_start_on_event(void* context, SceneManagerEvent event) {
    NfcApp* nfc = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        consumed = true;
        if(event.event == SubmenuIndexRead) {
            scene_manager_next_scene(nfc->scene_manager, NfcSceneDetect);
            dolphin_deed(DolphinDeedNfcRead);
        } else if(event.event == SubmenuIndexDetectReader) {
            scene_manager_next_scene(nfc->scene_manager, NfcSceneMfClassicDetectReader);
        } else if(event.event == SubmenuIndexSaved) {
            scene_manager_next_scene(nfc->scene_manager, NfcSceneFileSelect);
        } else if(event.event == SubmenuIndexExtraAction) {
            scene_manager_next_scene(nfc->scene_manager, NfcSceneExtraActions);
        } else if(event.event == SubmenuIndexAddManually) {
            scene_manager_next_scene(nfc->scene_manager, NfcSceneSetType);
        } else if(event.event == SubmenuIndexDebug) {
            scene_manager_next_scene(nfc->scene_manager, NfcSceneDebug);
        } else {
            consumed = false;
        }
        if(consumed) {
            scene_manager_set_scene_state(nfc->scene_manager, NfcSceneStart, event.event);
        }
    }
    return consumed;
}

void nfc_scene_start_on_exit(void* context) {
    NfcApp* nfc = context;

    submenu_reset(nfc->submenu);
}
