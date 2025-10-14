#include <furi.h>

#include <gui/gui.h>
#include <gui/view_holder.h>
#include <gui/modules/empty_screen.h>

#include <dialogs/dialogs.h>
#include <assets_icons.h>

#include <furi_hal_version.h>
#include <furi_hal_region.h>
#include <furi_hal_bt.h>
#include <furi_hal_info.h>

typedef DialogMessageButton (*AboutDialogScreen)(DialogsApp* dialogs, DialogMessage* message);

static DialogMessageButton about_screen_product(DialogsApp* dialogs, DialogMessage* message) {
    DialogMessageButton result;

    FuriString* screen_header = furi_string_alloc_printf(
        "Product: %s\n"
        "Model: %s",
        furi_hal_version_get_model_name(),
        furi_hal_version_get_model_code());

    FuriString* screen_text = furi_string_alloc_printf("got it");

    dialog_message_set_header(
        message, furi_string_get_cstr(screen_header), 0, 0, AlignLeft, AlignTop);
    dialog_message_set_text(
        message, furi_string_get_cstr(screen_text), 0, 26, AlignLeft, AlignTop);
    result = dialog_message_show(dialogs, message);

    furi_string_free(screen_header);
    furi_string_free(screen_text);

    return result;
}

static DialogMessageButton about_screen_hw_version(DialogsApp* dialogs, DialogMessage* message) {
    DialogMessageButton result;
    FuriString* buffer;
    buffer = furi_string_alloc();
    const char* my_name = furi_hal_version_get_name_ptr();

    furi_string_cat_printf(
        buffer,
        "%d.F%dB%dC%d %s:%s %s\n",
        furi_hal_version_get_hw_version(),
        furi_hal_version_get_hw_target(),
        furi_hal_version_get_hw_body(),
        furi_hal_version_get_hw_connect(),
        furi_hal_version_get_hw_region_name(),
        furi_hal_region_get_name(),
        my_name ? my_name : "Unknown");

    furi_string_cat_printf(buffer, "Serial Number:\n");
    const uint8_t* uid = furi_hal_version_uid();
    for(size_t i = 0; i < furi_hal_version_uid_size(); i++) {
        furi_string_cat_printf(buffer, "%02X", uid[i]);
    }

    dialog_message_set_header(message, "Hardware Info:", 0, 0, AlignLeft, AlignTop);
    dialog_message_set_text(message, furi_string_get_cstr(buffer), 0, 13, AlignLeft, AlignTop);
    result = dialog_message_show(dialogs, message);
    furi_string_free(buffer);

    return result;
}

static DialogMessageButton about_screen_fw_version(DialogsApp* dialogs, DialogMessage* message) {
    DialogMessageButton result;
    FuriString* buffer;
    buffer = furi_string_alloc();
    const Version* ver = furi_hal_version_get_firmware_version();
    const BleGlueC2Info* c2_ver = NULL;
#if defined(SRV_BT) || defined(FAP_VERSION)
    c2_ver = ble_glue_get_c2_info();
#endif

    if(!ver) { //-V1051
        furi_string_cat_printf(buffer, "No info\n");
    } else {
        uint16_t api_major, api_minor;
        furi_hal_info_get_api_version(&api_major, &api_minor);
        furi_string_cat_printf(
            buffer,
            "%s [%s]\n%s%s [%d.%d] %s\n[%d] ",
            version_get_version(ver),
            version_get_builddate(ver),
            version_get_dirty_flag(ver) ? "[!] " : "",
            version_get_githash(ver),
            api_major,
            api_minor,
            c2_ver ? c2_ver->StackTypeString : "<none>",
            version_get_target(ver));
        if(!strcmp(version_get_version(ver), "mntm-dev") &&
           strcmp(version_get_gitbranch(ver), "dev")) {
            // Not a tag but not dev branch, show custom branch
            furi_string_cat(buffer, version_get_gitbranch(ver));
        } else {
            furi_string_cat(buffer, "momentum-fw.dev");
        }
    }

    dialog_message_set_header(message, "Firmware Info:", 0, 0, AlignLeft, AlignTop);
    dialog_message_set_text(message, furi_string_get_cstr(buffer), 0, 13, AlignLeft, AlignTop);
    result = dialog_message_show(dialogs, message);
    furi_string_free(buffer);

    return result;
}

const AboutDialogScreen about_screens[] = {
    about_screen_product,
    about_screen_hw_version,
    about_screen_fw_version,

};

int32_t about_settings_app(void* p) {
    bool about_battery = p && !strncmp(p, "about_battery", strlen("about_battery"));
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogMessage* message = dialog_message_alloc();

    Gui* gui = furi_record_open(RECORD_GUI);
    ViewHolder* view_holder = view_holder_alloc();
    EmptyScreen* empty_screen = empty_screen_alloc();

    size_t screen_index = 0;
    DialogMessageButton screen_result;

    // draw empty screen to prevent menu flickering
    view_holder_attach_to_gui(view_holder, gui);
    view_holder_set_view(view_holder, empty_screen_get_view(empty_screen));

    int32_t ret = 0;
    while(1) {
        if(screen_index >= COUNT_OF(about_screens) - 1) {
            dialog_message_set_buttons(message, "Prev.", NULL, NULL);
        } else if(screen_index == 0 && !about_battery) {
            dialog_message_set_buttons(message, NULL, NULL, "Next");
        } else {
            dialog_message_set_buttons(message, "Prev.", NULL, "Next");
        }

        screen_result = about_screens[screen_index](dialogs, message);

        dialog_message_set_icon(message, NULL, 0, 0);
        dialog_message_set_header(message, NULL, 0, 0, AlignLeft, AlignTop);
        dialog_message_set_text(message, NULL, 0, 0, AlignLeft, AlignTop);

        if(screen_result == DialogMessageButtonLeft) {
            if(screen_index <= 0) {
                ret = 1;
                break;
            } else {
                screen_index--;
            }
        } else if(screen_result == DialogMessageButtonRight) {
            if(screen_index < COUNT_OF(about_screens) - 1) {
                screen_index++;
            }
        } else if(screen_result == DialogMessageButtonBack) {
            break;
        }
    }

    dialog_message_free(message);
    furi_record_close(RECORD_DIALOGS);

    view_holder_set_view(view_holder, NULL);
    view_holder_free(view_holder);
    empty_screen_free(empty_screen);
    furi_record_close(RECORD_GUI);

    return ret;
}
