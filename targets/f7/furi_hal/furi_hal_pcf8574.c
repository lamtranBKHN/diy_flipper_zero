#include "furi_hal_pcf8574.h"
#include "furi_hal_i2c.h"
#include <furi.h>
#include <core/common_defines.h>

#define TAG "FuriHalPCF8574"

static uint8_t pcf8574_state = 0xFF;
static bool pcf8574_ready = false;
static uint8_t pcf8574_addr = PCF8574_I2C_ADDR;
static uint32_t pcf8574_last_error_tick = 0;
static GpioExtiCallback pcf8574_int_cb = NULL;
static void* pcf8574_int_ctx = NULL;

bool furi_hal_pcf8574_init(void) {
    uint8_t probe = 0xFF;
    bool ok = false;
    uint8_t detected_addr = PCF8574_I2C_ADDR;

    // Try both 7-bit (0x20) and 8-bit shifted (0x40) address forms.
    const uint8_t candidates[] = {PCF8574_I2C_ADDR, (uint8_t)(PCF8574_I2C_ADDR << 1)};

    for(size_t i = 0; i < COUNT_OF(candidates); i++) {
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, candidates[i], &probe, 1, 50);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        if(ok) {
            detected_addr = candidates[i];
            break;
        }
    }

    if(!ok) {
        pcf8574_ready = false;
        uint32_t now = furi_get_tick();
        if((now - pcf8574_last_error_tick) > 1000) {
            pcf8574_last_error_tick = now;
            FURI_LOG_E(TAG, "PCF8574 not detected (tried 0x%02X and 0x%02X)", PCF8574_I2C_ADDR, (uint8_t)(PCF8574_I2C_ADDR << 1));
        }
        return false;
    }

    pcf8574_addr = detected_addr;
    pcf8574_state = probe;
    pcf8574_ready = true;
    FURI_LOG_I(TAG, "PCF8574 ready at 0x%02X, state=0x%02X", pcf8574_addr, pcf8574_state);
    return true;
}

bool furi_hal_pcf8574_read(uint8_t* data) {
    if(!data) return false;
    if(!pcf8574_ready && !furi_hal_pcf8574_init()) return false;

    uint8_t value = 0xFF;
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pcf8574_addr, &value, 1, 50);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);

    if(ok) pcf8574_state = value;
    *data = pcf8574_state;
    return ok;
}

bool furi_hal_pcf8574_write(uint8_t data) {
    if(!pcf8574_ready && !furi_hal_pcf8574_init()) return false;

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_tx(&furi_hal_i2c_handle_power, pcf8574_addr, &data, 1, 50);
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
