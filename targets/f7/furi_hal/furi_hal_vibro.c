#include <furi_hal_vibro.h>
#include <furi_hal_gpio.h>
#include <furi_hal_mcp23017.h>

#define TAG "FuriHalVibro"

void furi_hal_vibro_init(void) {
   // Use MCP23017 pin B0 (pin index 8) for the vibro motor
   const uint8_t vibro_pin = 8; // B0
   if(!furi_hal_mcp23017_init()) {
      FURI_LOG_E(TAG, "MCP23017 init failed");
   } else {
      // Configure pin as output and clear it
      if(!furi_hal_mcp23017_set_pin_direction(vibro_pin, false)) {
         FURI_LOG_E(TAG, "Failed to set vibro pin direction");
      }
      if(!furi_hal_mcp23017_write_pin(vibro_pin, false)) {
         FURI_LOG_E(TAG, "Failed to clear vibro pin");
      }
      FURI_LOG_I(TAG, "Vibro bound to MCP23017 pin B0 (8)");
   }
}

void furi_hal_vibro_on(bool value) {
      const uint8_t vibro_pin = 8; // B0
      if(!furi_hal_mcp23017_write_pin(vibro_pin, value)) {
         FURI_LOG_E(TAG, "Failed to set vibro pin");
      }
}
