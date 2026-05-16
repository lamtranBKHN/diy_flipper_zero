#include "furi_hal_nfc_i.h"
#include <furi.h>

#define TAG "FuriHalNfcIsr"

uint32_t furi_hal_nfc_get_irq(const FuriHalSpiBusHandle* handle) {
    UNUSED(handle);
    return 0;
}

void furi_hal_nfc_init_gpio_isr(void) {
    FURI_LOG_D(TAG, "NFC GPIO ISR disabled (PN532 mode)");
}

void furi_hal_nfc_deinit_gpio_isr(void) {
    FURI_LOG_D(TAG, "NFC GPIO ISR deinit skipped (PN532 mode)");
}
