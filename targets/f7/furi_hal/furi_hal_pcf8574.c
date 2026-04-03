#include "furi_hal_pcf8574.h"
#include "furi_hal_i2c.h"
#include <furi.h>

#define TAG "FuriHalPCF8574"

static uint8_t pcf8574_state = 0xFF;
static bool pcf8574_ready = false;
static GpioExtiCallback pcf8574_int_cb = NULL;
static void* pcf8574_int_ctx = NULL;

bool furi_hal_pcf8574_init(void) {
    uint8_t probe = 0xFF;

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, PCF8574_I2C_ADDR, &probe, 1, 50);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);

    if(!ok) {
        pcf8574_ready = false;
        FURI_LOG_E(TAG, "PCF8574 not detected at 0x%02X", PCF8574_I2C_ADDR);
        return false;
    }

    pcf8574_state = probe;
    pcf8574_ready = true;
    FURI_LOG_I(TAG, "PCF8574 ready, state=0x%02X", pcf8574_state);
    return true;
}

bool furi_hal_pcf8574_read(uint8_t* data) {
    if(!data) return false;
    if(!pcf8574_ready && !furi_hal_pcf8574_init()) return false;

    uint8_t value = 0xFF;
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, PCF8574_I2C_ADDR, &value, 1, 50);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);

    if(ok) pcf8574_state = value;
    *data = pcf8574_state;
    return ok;
}

bool furi_hal_pcf8574_write(uint8_t data) {
    if(!pcf8574_ready && !furi_hal_pcf8574_init()) return false;

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_tx(&furi_hal_i2c_handle_power, PCF8574_I2C_ADDR, &data, 1, 50);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);

    if(ok) pcf8574_state = data;
    return ok;
}

bool furi_hal_pcf8574_write_pin(uint8_t pin, bool value) {
    if(pin > 7) return false;
    uint8_t next = pcf8574_state;
    if(value) {
        next |= (1u << pin);
    } else {
        next &= (uint8_t)~(1u << pin);
    }
    return furi_hal_pcf8574_write(next);
}

void furi_hal_pcf8574_attach_int(GpioExtiCallback cb, void* ctx) {
    pcf8574_int_cb = cb;
    pcf8574_int_ctx = ctx;
}

void furi_hal_pcf8574_handle_int(void) {
    if(pcf8574_int_cb) pcf8574_int_cb(pcf8574_int_ctx);
}
