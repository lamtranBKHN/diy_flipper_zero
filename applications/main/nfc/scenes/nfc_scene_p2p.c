#include "../nfc_app_i.h"

#include <lib/nfc/p2p/p2p.h>

#define TAG "NfcSceneP2p"

enum {
    NfcP2pSubmenuSendUrl,
    NfcP2pSubmenuSendText,
    NfcP2pSubmenuReceive,
};

static void nfc_scene_p2p_submenu_callback(void* context, uint32_t index) {
    NfcApp* nfc = context;
    view_dispatcher_send_custom_event(nfc->view_dispatcher, index);
}

void nfc_scene_p2p_on_enter(void* context) {
    NfcApp* nfc = context;
    Submenu* submenu = nfc->submenu;

    submenu_add_item(
        submenu, "Send URL", NfcP2pSubmenuSendUrl, nfc_scene_p2p_submenu_callback, nfc);
    submenu_add_item(
        submenu, "Send Text", NfcP2pSubmenuSendText, nfc_scene_p2p_submenu_callback, nfc);
    submenu_add_item(
        submenu, "Receive", NfcP2pSubmenuReceive, nfc_scene_p2p_submenu_callback, nfc);

    view_dispatcher_switch_to_view(nfc->view_dispatcher, NfcViewMenu);
}

bool nfc_scene_p2p_on_event(void* context, SceneManagerEvent event) {
    NfcApp* nfc = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == NfcP2pSubmenuSendUrl) {
            nfc_text_store_set(nfc, "https://");
            scene_manager_set_scene_state(nfc->scene_manager, NfcSceneP2pSend, 0);
            scene_manager_next_scene(nfc->scene_manager, NfcSceneP2pSend);
            consumed = true;
        } else if(event.event == NfcP2pSubmenuSendText) {
            nfc_text_store_set(nfc, "");
            scene_manager_set_scene_state(nfc->scene_manager, NfcSceneP2pSend, 1);
            scene_manager_next_scene(nfc->scene_manager, NfcSceneP2pSend);
            consumed = true;
        } else if(event.event == NfcP2pSubmenuReceive) {
            scene_manager_next_scene(nfc->scene_manager, NfcSceneP2pReceive);
            consumed = true;
        }
    }

    return consumed;
}

void nfc_scene_p2p_on_exit(void* context) {
    NfcApp* nfc = context;
    submenu_reset(nfc->submenu);
}

// --- P2P Send scene ---

static void nfc_scene_p2p_send_text_input_callback(void* context) {
    NfcApp* nfc = context;
    view_dispatcher_send_custom_event(nfc->view_dispatcher, NfcCustomEventTextInputDone);
}

void nfc_scene_p2p_send_on_enter(void* context) {
    NfcApp* nfc = context;
    bool is_url = (scene_manager_get_scene_state(nfc->scene_manager, NfcSceneP2pSend) == 0);

    text_input_set_header_text(nfc->text_input, is_url ? "Enter URL" : "Enter Text");
    text_input_set_result_callback(
        nfc->text_input,
        nfc_scene_p2p_send_text_input_callback,
        nfc,
        nfc->text_store,
        NFC_TEXT_STORE_SIZE,
        false);

    view_dispatcher_switch_to_view(nfc->view_dispatcher, NfcViewTextInput);
}

bool nfc_scene_p2p_send_on_event(void* context, SceneManagerEvent event) {
    NfcApp* nfc = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == NfcCustomEventTextInputDone) {
            nfc_blink_emulate_start(nfc);
            view_dispatcher_switch_to_view(nfc->view_dispatcher, NfcViewPopup);
            popup_set_header(nfc->popup, "Sending...", 64, 32, AlignCenter, AlignCenter);
            view_dispatcher_send_custom_event(nfc->view_dispatcher, NfcCustomEventPollerSuccess);
            consumed = true;
        } else if(event.event == NfcCustomEventPollerSuccess) {
            bool is_url =
                (scene_manager_get_scene_state(nfc->scene_manager, NfcSceneP2pSend) == 0);
            bool success = false;

            P2pSession* session = p2p_session_alloc();
            if(session) {
                if(is_url) {
                    success = p2p_send_url(session, nfc->text_store, LLCP_DEFAULT_TIMEOUT);
                } else {
                    success = p2p_send_text(session, nfc->text_store, LLCP_DEFAULT_TIMEOUT);
                }
                p2p_session_free(session);
            }

            nfc_blink_stop(nfc);

            if(success) {
                popup_set_header(nfc->popup, "Sent!", 64, 32, AlignCenter, AlignCenter);
            } else {
                popup_set_header(nfc->popup, "Failed", 64, 32, AlignCenter, AlignCenter);
                popup_set_text(
                    nfc->popup,
                    "No peer detected\nor connection failed",
                    64,
                    48,
                    AlignCenter,
                    AlignCenter);
            }

            consumed = true;
        }
    }

    return consumed;
}

void nfc_scene_p2p_send_on_exit(void* context) {
    NfcApp* nfc = context;
    popup_reset(nfc->popup);
    text_input_reset(nfc->text_input);
}

// --- P2P Receive scene ---

void nfc_scene_p2p_receive_on_enter(void* context) {
    NfcApp* nfc = context;

    nfc_blink_emulate_start(nfc);
    view_dispatcher_switch_to_view(nfc->view_dispatcher, NfcViewPopup);
    popup_set_header(nfc->popup, "Waiting...", 64, 32, AlignCenter, AlignCenter);
    popup_set_text(nfc->popup, "Tap phone to\nFlipper Zero", 64, 48, AlignCenter, AlignCenter);

    view_dispatcher_send_custom_event(nfc->view_dispatcher, NfcCustomEventPollerSuccess);
}

bool nfc_scene_p2p_receive_on_event(void* context, SceneManagerEvent event) {
    NfcApp* nfc = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == NfcCustomEventPollerSuccess) {
            char result[256];
            P2pSession* session = p2p_session_alloc();
            bool success = false;
            if(session) {
                success = p2p_receive(session, result, sizeof(result), LLCP_DEFAULT_TIMEOUT);
                p2p_session_free(session);
            }

            nfc_blink_stop(nfc);

            if(success) {
                furi_string_set(nfc->text_box_store, result);
                widget_reset(nfc->widget);
                widget_add_string_multiline_element(
                    nfc->widget, 64, 32, AlignCenter, AlignCenter, FontPrimary, result);
                view_dispatcher_switch_to_view(nfc->view_dispatcher, NfcViewWidget);
            } else {
                popup_set_header(nfc->popup, "Failed", 64, 32, AlignCenter, AlignCenter);
                popup_set_text(
                    nfc->popup,
                    "No peer detected\nor receive failed",
                    64,
                    48,
                    AlignCenter,
                    AlignCenter);
            }
            consumed = true;
        }
    }

    return consumed;
}

void nfc_scene_p2p_receive_on_exit(void* context) {
    NfcApp* nfc = context;
    nfc_blink_stop(nfc);
    popup_reset(nfc->popup);
    widget_reset(nfc->widget);
}
