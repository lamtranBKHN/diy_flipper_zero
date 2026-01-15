#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "furi_hal_gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*Mcp23017IntCallback)(void* ctx);

// Initialize MCP23017 at given I2C address (7-bit)
// Note: public no-arg API maintained for compatibility.
bool furi_hal_mcp23017_init(void);
// Internal: initialize with explicit address (not exported)
bool furi_hal_mcp23017_init_ex(uint8_t i2c_addr);

// Read GPIOA and GPIOB registers (16 bits)
bool furi_hal_mcp23017_read_gpio(uint16_t* gpio_state);

// Configure IOCON / interrupt registers as minimal convenience
// gpios_to_input_mask: lower 8 bits = GPIOA mask, upper 8 bits = GPIOB mask
bool furi_hal_mcp23017_configure_interrupts(uint16_t gpios_to_input_mask);

// Attach callback for INT pin (STM32 pin) - the code will call this when INT line triggers
void furi_hal_mcp23017_attach_int(GpioExtiCallback cb, void* ctx);

// Called by board EXTI handler to propagate interrupt from MCP23017
void furi_hal_mcp23017_handle_int(void);

#ifdef __cplusplus
}
#endif
