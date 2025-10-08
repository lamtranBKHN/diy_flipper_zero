   
#include "furi_hal_nfc_i.h"
#include <furi.h>

#include <lib/drivers/st25r3916.h>
#include <furi_hal_resources.h>

#define TAG "FuriHalNfcIsr"

static void furi_hal_nfc_int_callback(void* context) {
    UNUSED(context);
    FURI_LOG_T(TAG, "NFC GPIO IRQ callback triggered");
    furi_hal_nfc_event_set(FuriHalNfcEventInternalTypeIrq);
}

uint32_t furi_hal_nfc_get_irq(const FuriHalSpiBusHandle* handle) {
    FURI_LOG_T(TAG, "Reading IRQ register from ST25R3916");
    uint32_t irq = 0;
    while(furi_hal_gpio_read_port_pin(gpio_nfc_irq_rfid_pull.port, gpio_nfc_irq_rfid_pull.pin)) {
        irq |= st25r3916_get_irq(handle);
    }
    FURI_LOG_T(TAG, "IRQ register value: 0x%08lX", irq);
    return irq;
}

void furi_hal_nfc_init_gpio_isr(void) {
    FURI_LOG_D(TAG, "Initializing NFC GPIO ISR");
    furi_hal_gpio_init(
        &gpio_nfc_irq_rfid_pull, GpioModeInterruptRiseFall, GpioPullUp, GpioSpeedVeryHigh);
    furi_hal_gpio_add_int_callback(&gpio_nfc_irq_rfid_pull, furi_hal_nfc_int_callback, NULL);
    furi_hal_gpio_enable_int_callback(&gpio_nfc_irq_rfid_pull);
    FURI_LOG_D(TAG, "NFC GPIO ISR Initialized");
}

void furi_hal_nfc_deinit_gpio_isr(void) {
    FURI_LOG_D(TAG, "Deinitializing NFC GPIO ISR");
    furi_hal_gpio_remove_int_callback(&gpio_nfc_irq_rfid_pull);
    furi_hal_gpio_init(&gpio_nfc_irq_rfid_pull, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    FURI_LOG_D(TAG, "NFC GPIO ISR Deinitialized");
}

  

