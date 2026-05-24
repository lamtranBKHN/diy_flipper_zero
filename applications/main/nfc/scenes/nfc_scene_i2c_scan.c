#include "../nfc_app_i.h"

#include <furi_hal_i2c.h>
#include <furi_hal_i2c_config.h>
#include <furi_hal_pn532.h>

#define I2C_SCAN_ADDR_MIN  0x08
#define I2C_SCAN_ADDR_MAX  0x77
#define I2C_SCAN_TIMEOUT   50

#define PN532_I2C_ADDR_7BIT 0x48

void nfc_scene_i2c_scan_on_enter(void* context) {
    NfcApp* nfc = context;
    TextBox* text_box = nfc->text_box;
    FuriString* text = furi_string_alloc();

    text_box_set_font(text_box, TextBoxFontText);
    furi_string_cat_printf(text, "I2C Scan (0x08-0x77)\n\n");

    uint8_t found_count = 0;
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    for(uint8_t addr = I2C_SCAN_ADDR_MIN; addr <= I2C_SCAN_ADDR_MAX; addr++) {
        if(furi_hal_i2c_is_device_ready(&furi_hal_i2c_handle_power, addr, I2C_SCAN_TIMEOUT)) {
            furi_string_cat_printf(text, "0x%02X", addr);
            if(addr == PN532_I2C_ADDR_7BIT) {
                furi_string_cat_printf(text, " PN532");
            }
            furi_string_cat_printf(text, "\n");
            found_count++;
        }
    }
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);

    if(found_count == 0) {
        furi_string_cat_printf(text, "No devices found");
    }

    furi_string_cat_printf(text, "\nPress Back");
    text_box_set_text(text_box, furi_string_get_cstr(text));
    furi_string_free(text);
    view_dispatcher_switch_to_view(nfc->view_dispatcher, NfcViewTextBox);
}

bool nfc_scene_i2c_scan_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void nfc_scene_i2c_scan_on_exit(void* context) {
    NfcApp* nfc = context;
    text_box_reset(nfc->text_box);
}
