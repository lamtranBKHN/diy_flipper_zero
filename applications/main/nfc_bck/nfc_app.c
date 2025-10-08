#include "nfc_app_i.h"
#include "api/nfc_app_api_interface.h"
#include "helpers/protocol_support/nfc_protocol_support.h"

#include <dolphin/dolphin.h>
#include <loader/firmware_api/firmware_api.h>
#include <applications/main/archive/helpers/archive_helpers_ext.h>
#include <furi.h>


// A TAG for logging, helps filter messages for this specific file
#define TAG "NfcApp"

bool nfc_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    NfcApp* nfc = context;
    FURI_LOG_D(TAG, "Custom event callback, event ID: %lu", event);
    return scene_manager_handle_custom_event(nfc->scene_manager, event);
}

bool nfc_back_event_callback(void* context) {
    furi_assert(context);
    NfcApp* nfc = context;
    FURI_LOG_D(TAG, "Back event callback");
    return scene_manager_handle_back_event(nfc->scene_manager);
}

static void nfc_app_rpc_command_callback(const RpcAppSystemEvent* event, void* context) {
    furi_assert(context);
    NfcApp* nfc = (NfcApp*)context;
    FURI_LOG_I(TAG, "RPC command callback, event type: %d", event->type);

    furi_assert(nfc->rpc_ctx);

    if(event->type == RpcAppEventTypeSessionClose) {
        view_dispatcher_send_custom_event(nfc->view_dispatcher, NfcCustomEventRpcSessionClose);
        rpc_system_app_set_callback(nfc->rpc_ctx, NULL, NULL);
        nfc->rpc_ctx = NULL;
    } else if(event->type == RpcAppEventTypeAppExit) {
        view_dispatcher_send_custom_event(nfc->view_dispatcher, NfcCustomEventRpcExit);
    } else if(event->type == RpcAppEventTypeLoadFile) {
        furi_assert(event->data.type == RpcAppSystemEventDataTypeString);
        furi_string_set(nfc->file_path, event->data.string);
        view_dispatcher_send_custom_event(nfc->view_dispatcher, NfcCustomEventRpcLoadFile);
    } else {
        rpc_system_app_confirm(nfc->rpc_ctx, false);
    }
}

NfcApp* nfc_app_alloc(void) {
    FURI_LOG_I(TAG, "Allocating app...");
    NfcApp* instance = malloc(sizeof(NfcApp));
    FURI_LOG_D(TAG, "App structure allocated at %p", instance);

    FURI_LOG_D(TAG, "Allocating ViewDispatcher and SceneManager");
    instance->view_dispatcher = view_dispatcher_alloc();
    instance->scene_manager = scene_manager_alloc(&nfc_scene_handlers, instance);

    FURI_LOG_D(TAG, "Setting up dispatcher callbacks");
    view_dispatcher_set_event_callback_context(instance->view_dispatcher, instance);
    view_dispatcher_set_custom_event_callback(
        instance->view_dispatcher, nfc_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        instance->view_dispatcher, nfc_back_event_callback);

    FURI_LOG_D(TAG, "Allocating NFC HAL and helpers...");
    instance->nfc = nfc_alloc();
    instance->api_resolver = composite_api_resolver_alloc();
    composite_api_resolver_add(instance->api_resolver, firmware_api_interface);
    composite_api_resolver_add(instance->api_resolver, nfc_application_api_interface);
    instance->detected_protocols = nfc_detected_protocols_alloc();
    instance->felica_auth = felica_auth_alloc();
    instance->mf_ul_auth = mf_ultralight_auth_alloc();
    instance->slix_unlock = slix_unlock_alloc();
    instance->mfc_key_cache = mf_classic_key_cache_alloc();
    instance->nfc_supported_cards = nfc_supported_cards_alloc(instance->api_resolver);
    instance->nfc_device = nfc_device_alloc();
    nfc_device_set_loading_callback(instance->nfc_device, nfc_show_loading_popup, instance);

    FURI_LOG_D(TAG, "Opening OS records...");
    instance->gui = furi_record_open(RECORD_GUI);
    instance->notifications = furi_record_open(RECORD_NOTIFICATION);
    instance->storage = furi_record_open(RECORD_STORAGE);
    instance->dialogs = furi_record_open(RECORD_DIALOGS);
    FURI_LOG_D(TAG, "OS records opened.");

    FURI_LOG_D(TAG, "Allocating and registering views...");
    instance->submenu = submenu_alloc();
    view_dispatcher_add_view(
        instance->view_dispatcher, NfcViewMenu, submenu_get_view(instance->submenu));
    instance->dialog_ex = dialog_ex_alloc();
    view_dispatcher_add_view(
        instance->view_dispatcher, NfcViewDialogEx, dialog_ex_get_view(instance->dialog_ex));
    instance->popup = popup_alloc();
    view_dispatcher_add_view(
        instance->view_dispatcher, NfcViewPopup, popup_get_view(instance->popup));
    instance->loading = loading_alloc();
    view_dispatcher_add_view(
        instance->view_dispatcher, NfcViewLoading, loading_get_view(instance->loading));
    instance->text_input = text_input_alloc();
    view_dispatcher_add_view(
        instance->view_dispatcher, NfcViewTextInput, text_input_get_view(instance->text_input));
    instance->byte_input = byte_input_alloc();
    view_dispatcher_add_view(
        instance->view_dispatcher, NfcViewByteInput, byte_input_get_view(instance->byte_input));
    instance->text_box = text_box_alloc();
    view_dispatcher_add_view(
        instance->view_dispatcher, NfcViewTextBox, text_box_get_view(instance->text_box));
    instance->text_box_store = furi_string_alloc();
    instance->widget = widget_alloc();
    view_dispatcher_add_view(
        instance->view_dispatcher, NfcViewWidget, widget_get_view(instance->widget));
    instance->dict_attack = dict_attack_alloc();
    view_dispatcher_add_view(
        instance->view_dispatcher, NfcViewDictAttack, dict_attack_get_view(instance->dict_attack));
    instance->detect_reader = detect_reader_alloc();
    view_dispatcher_add_view(
        instance->view_dispatcher,
        NfcViewDetectReader,
        detect_reader_get_view(instance->detect_reader));
    FURI_LOG_D(TAG, "All views allocated and registered.");

    FURI_LOG_D(TAG, "Allocating path and name strings");
    instance->iso14443_3a_edit_data = iso14443_3a_alloc();
    instance->file_path = furi_string_alloc_set(NFC_APP_FOLDER);
    instance->file_name = furi_string_alloc();

    FURI_LOG_I(TAG, "App allocation finished.");
    return instance;
}

void nfc_app_free(NfcApp* instance) {
    furi_assert(instance);
    FURI_LOG_I(TAG, "Freeing app...");

    if(instance->rpc_ctx) {
        FURI_LOG_D(TAG, "Closing RPC session");
        rpc_system_app_send_exited(instance->rpc_ctx);
        rpc_system_app_set_callback(instance->rpc_ctx, NULL, NULL);
    }

    FURI_LOG_D(TAG, "Freeing NFC HAL and helpers");
    nfc_free(instance->nfc);
    nfc_detected_protocols_free(instance->detected_protocols);
    felica_auth_free(instance->felica_auth);
    mf_ultralight_auth_free(instance->mf_ul_auth);
    slix_unlock_free(instance->slix_unlock);
    mf_classic_key_cache_free(instance->mfc_key_cache);
    nfc_supported_cards_free(instance->nfc_supported_cards);
    if(instance->protocol_support) {
        nfc_protocol_support_free(instance);
    }
    nfc_device_free(instance->nfc_device);

    FURI_LOG_D(TAG, "Removing views and freeing UI components");
    view_dispatcher_remove_view(instance->view_dispatcher, NfcViewMenu);
    submenu_free(instance->submenu);
    view_dispatcher_remove_view(instance->view_dispatcher, NfcViewDialogEx);
    dialog_ex_free(instance->dialog_ex);
    view_dispatcher_remove_view(instance->view_dispatcher, NfcViewPopup);
    popup_free(instance->popup);
    view_dispatcher_remove_view(instance->view_dispatcher, NfcViewLoading);
    loading_free(instance->loading);
    view_dispatcher_remove_view(instance->view_dispatcher, NfcViewTextInput);
    text_input_free(instance->text_input);
    view_dispatcher_remove_view(instance->view_dispatcher, NfcViewByteInput);
    byte_input_free(instance->byte_input);
    view_dispatcher_remove_view(instance->view_dispatcher, NfcViewTextBox);
    text_box_free(instance->text_box);
    furi_string_free(instance->text_box_store);
    view_dispatcher_remove_view(instance->view_dispatcher, NfcViewWidget);
    widget_free(instance->widget);
    view_dispatcher_remove_view(instance->view_dispatcher, NfcViewDictAttack);
    dict_attack_free(instance->dict_attack);
    view_dispatcher_remove_view(instance->view_dispatcher, NfcViewDetectReader);
    detect_reader_free(instance->detect_reader);

    FURI_LOG_D(TAG, "Freeing ViewDispatcher and SceneManager");
    view_dispatcher_free(instance->view_dispatcher);
    scene_manager_free(instance->scene_manager);

    FURI_LOG_D(TAG, "Closing OS records");
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    instance->gui = NULL;
    instance->notifications = NULL;

    FURI_LOG_D(TAG, "Freeing remaining data");
    iso14443_3a_free(instance->iso14443_3a_edit_data);
    furi_string_free(instance->file_path);
    furi_string_free(instance->file_name);

    free(instance);
    FURI_LOG_I(TAG, "App freeing finished.");
}

// ... [The other helper functions like nfc_text_store_set, nfc_save_file, etc. do not need logging for this purpose] ...
// ... [They will only be called in response to user actions, which you can already see in the scene logs] ...

// MODIFIED: 'static' keyword removed, now accepts NfcApp*
bool nfc_is_hal_ready(NfcApp* nfc) {
    FURI_LOG_D(TAG, "Checking NFC HAL readiness...");
    if(furi_hal_nfc_is_hal_ready() != FuriHalNfcErrorNone) {
        FURI_LOG_E(TAG, "NFC HAL check FAILED");
        DialogMessage* message = dialog_message_alloc();
        dialog_message_set_header(message, "Error: NFC Chip Failed", 64, 0, AlignCenter, AlignTop);
        dialog_message_set_text(
            message, "Send error photo via\nsupport.flipper.net", 0, 63, AlignLeft, AlignBottom);
        dialog_message_set_icon(message, &I_err_09, 128 - 25, 64 - 25);
        dialog_message_show(nfc->dialogs, message);
        dialog_message_free(message);
        return false;
    } else {
        FURI_LOG_D(TAG, "NFC HAL check PASSED");
        return true;
    }
}

static void nfc_show_initial_scene_for_device(NfcApp* nfc) {
    FURI_LOG_D(TAG, "Determining initial scene for loaded device");
    NfcProtocol prot = nfc_device_get_protocol(nfc->nfc_device);
    uint32_t scene = nfc_protocol_support_has_feature(
                         prot, nfc, NfcProtocolFeatureEmulateFull | NfcProtocolFeatureEmulateUid) ?
                         NfcSceneEmulate :
                         NfcSceneSavedMenu;
    FURI_LOG_D(TAG, "Initial scene will be: %lu", scene);
    if(scene == NfcSceneSavedMenu) {
        FURI_LOG_D(TAG, "Loading plugins for saved menu");
        nfc_show_loading_popup(nfc, true);
        nfc_supported_cards_load_cache(nfc->nfc_supported_cards);
        nfc_show_loading_popup(nfc, false);
    }
    scene_manager_next_scene(nfc->scene_manager, scene);
}

// ... [nfc_app_run_external is less critical for this debug] ...

int32_t nfc_app(void* p) {
    FURI_LOG_I(TAG, "NFC Application Entry Point");
    NfcApp* nfc = nfc_app_alloc();
    const char* args = p;
    FURI_LOG_D(TAG, "Arguments pointer: %p", args);

    if(args && strlen(args)) {
        FURI_LOG_I(TAG, "Arguments detected: \"%s\"", args);
        if(sscanf(args, "RPC %p", &nfc->rpc_ctx) == 1) {
            FURI_LOG_I(TAG, "Entering RPC mode");
            rpc_system_app_set_callback(nfc->rpc_ctx, nfc_app_rpc_command_callback, nfc);
            rpc_system_app_send_started(nfc->rpc_ctx);
            view_dispatcher_attach_to_gui(
                nfc->view_dispatcher, nfc->gui, ViewDispatcherTypeDesktop);
            scene_manager_next_scene(nfc->scene_manager, NfcSceneRpc);
        } else {
            FURI_LOG_I(TAG, "Entering File Load mode");
            view_dispatcher_attach_to_gui(
                nfc->view_dispatcher, nfc->gui, ViewDispatcherTypeFullscreen);
            furi_string_set(nfc->file_path, args);
            if(nfc_load_file(nfc, nfc->file_path, true)) {
                FURI_LOG_D(TAG, "File loaded successfully, showing initial scene");
                nfc_show_initial_scene_for_device(nfc);
            } else {
                FURI_LOG_W(TAG, "Failed to load file, stopping dispatcher");
                view_dispatcher_stop(nfc->view_dispatcher);
            }
        }
    } else {
        FURI_LOG_I(TAG, "No arguments, entering normal mode");
        view_dispatcher_attach_to_gui(
            nfc->view_dispatcher, nfc->gui, ViewDispatcherTypeFullscreen);
        FURI_LOG_D(TAG, "Requesting initial scene: NfcSceneStart");
        scene_manager_next_scene(nfc->scene_manager, NfcSceneStart);
    }

    FURI_LOG_I(TAG, "Starting main event loop (view_dispatcher_run)");
    view_dispatcher_run(nfc->view_dispatcher);
    FURI_LOG_I(TAG, "Main event loop finished. Exiting app.");

    nfc_app_free(nfc);
    return 0;
}