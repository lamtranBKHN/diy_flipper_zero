#include <furi_hal_vibro.h>
#include <furi_hal_gpio.h>
#include <furi_hal_pcf8574.h>

#define TAG "FuriHalVibro"

void furi_hal_vibro_init(void) {
    if(!furi_hal_pcf8574_init()) {
        FURI_LOG_E(TAG, "PCF8574 init failed");
    } else {
        if(!furi_hal_pcf8574_write_pin(PCF8574_PIN_VIBRO, false)) {
            FURI_LOG_E(TAG, "Failed to clear vibro pin");
        }
        FURI_LOG_I(TAG, "Vibro bound to PCF8574 pin %u", PCF8574_PIN_VIBRO);
    }
}

void furi_hal_vibro_on(bool value) {
    if(!furi_hal_pcf8574_write_pin(PCF8574_PIN_VIBRO, value)) {
        FURI_LOG_E(TAG, "Failed to set vibro pin");
    }
}
