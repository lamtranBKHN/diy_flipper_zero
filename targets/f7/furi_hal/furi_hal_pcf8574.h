#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "furi_hal_gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCF8574_I2C_ADDR       0x20
#define PCF8574_PIN_BUZZER     7
#define PCF8574_PIN_VIBRO      6

bool furi_hal_pcf8574_init(void);
bool furi_hal_pcf8574_read(uint8_t* data);
bool furi_hal_pcf8574_write(uint8_t data);
bool furi_hal_pcf8574_write_pin(uint8_t pin, bool value);

void furi_hal_pcf8574_attach_int(GpioExtiCallback cb, void* ctx);
void furi_hal_pcf8574_handle_int(void);

static inline bool furi_hal_pcf8574_bit_pressed(uint8_t data, uint8_t bit) {
    return (bit < 8) ? ((data & (1u << bit)) == 0) : false;
}

#ifdef __cplusplus
}
#endif
