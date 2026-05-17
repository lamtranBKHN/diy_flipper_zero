#include "furi_hal_pcf8574.h"
#include "furi_hal_i2c.h"
#include <furi.h>
#include <core/common_defines.h>

#define TAG "FuriHalPCF8574"

static uint8_t pcf8574_state = 0xFF;
static bool pcf8574_ready = false;
static uint8_t pcf8574_addr = PCF8574_I2C_ADDR;
static uint32_t pcf8574_last_error_tick = 0;
#define PCF8574_REINIT_COOLDOWN_MS 500U /* Minimum ms between full I2C address scans */
#define PCF8574_I2C_TIMEOUT_MS 50
static GpioExtiCallback pcf8574_int_cb = NULL;
static void* pcf8574_int_ctx = NULL;
static const uint8_t pcf8574_output_mask = (1u << PCF8574_PIN_VIBRO) | (1u << PCF8574_PIN_BUZZER);
static uint8_t pcf8574_output_state = 0x00;

bool furi_hal_pcf8574_init(void) {
    uint8_t probe = 0xFF;
    bool ok = false;
    uint8_t detected_addr = PCF8574_I2C_ADDR;

    // Try all PCF8574 address variants:
    // 7-bit range 0x20..0x27 and their shifted forms 0x40..0x4E.
    uint8_t candidates[16];
    size_t cidx = 0;
    for(uint8_t a = 0x20; a <= 0x27; a++) {
        candidates[cidx++] = a;
    }
    for(uint8_t a = 0x20; a <= 0x27; a++) {
        candidates[cidx++] = (uint8_t)(a << 1);
    }

    for(size_t i = 0; i < cidx; i++) {
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, candidates[i], &probe, 1, PCF8574_I2C_TIMEOUT_MS);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        if(ok) {
            detected_addr = candidates[i];
            break;
        }
    }

    if(!ok) {
        pcf8574_ready = false;
        uint32_t now = furi_get_tick();
        /* Always update the tick so cooldown resets from the most recent failure */
        pcf8574_last_error_tick = now;
        FURI_LOG_E(TAG, "PCF8574 not detected on I2C (tried 0x20..0x27 and shifted forms)");
        return false;
    }

    pcf8574_addr = detected_addr;
    pcf8574_state = probe;
    // Keep input lines released (high) and restore current output latch.
    uint8_t frame =
        (uint8_t)((~pcf8574_output_mask) | (pcf8574_output_state & pcf8574_output_mask));
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool wr_ok = furi_hal_i2c_tx(&furi_hal_i2c_handle_power, pcf8574_addr, &frame, 1, PCF8574_I2C_TIMEOUT_MS);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);
    if(wr_ok) {
        pcf8574_state = frame;
    }
    pcf8574_ready = true;
    FURI_LOG_I(TAG, "PCF8574 ready at 0x%02X, state=0x%02X", pcf8574_addr, pcf8574_state);
    return true;
}

bool furi_hal_pcf8574_read(uint8_t* data) {
    if(!data) return false;
    if(!pcf8574_ready) {
        /* RISK-2 mitigation: throttle full 16-address reinit scan to once every
         * PCF8574_REINIT_COOLDOWN_MS ms. Without this guard a single I2C glitch
         * triggers up to 16 x 50ms probes = 800ms of bus time on every poll. */
        uint32_t now = furi_get_tick();
        if((now - pcf8574_last_error_tick) < PCF8574_REINIT_COOLDOWN_MS) {
            *data = pcf8574_state; /* return last known state during cooldown */
            return false;
        }
        if(!furi_hal_pcf8574_init()) return false;
    }

    uint8_t value = 0xFF;
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pcf8574_addr, &value, 1, PCF8574_I2C_TIMEOUT_MS);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);

    if(ok) pcf8574_state = value;
    *data = pcf8574_state;
    return ok;
}

bool furi_hal_pcf8574_write(uint8_t data) {
    if(!pcf8574_ready) {
        uint32_t now = furi_get_tick();
        if((now - pcf8574_last_error_tick) < PCF8574_REINIT_COOLDOWN_MS) return false;
        if(!furi_hal_pcf8574_init()) return false;
    }

    // Never drive input buttons (pins 0..5). Preserve only output pins in latch.
    pcf8574_output_state = data & pcf8574_output_mask;
    uint8_t frame = (uint8_t)((~pcf8574_output_mask) | pcf8574_output_state);

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_tx(&furi_hal_i2c_handle_power, pcf8574_addr, &frame, 1, PCF8574_I2C_TIMEOUT_MS);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);

    if(ok) pcf8574_state = frame;
    return ok;
}

bool furi_hal_pcf8574_write_pin(uint8_t pin, bool value) {
    if(pin > 7) return false;
    uint8_t next = pcf8574_output_state;
    if(value) {
        next |= (1u << pin);
    } else {
        next &= (uint8_t) ~(1u << pin);
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
