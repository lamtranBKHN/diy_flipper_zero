#include "furi_hal_mcp23017.h"
#include "furi_hal_i2c.h"
#include "furi_hal_gpio.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <furi.h>

// MCP23017 registers
#define MCP_IODIRA 0x00
#define MCP_IODIRB 0x01
#define MCP_GPIOA  0x12
#define MCP_GPIOB  0x13
#define MCP_GPINTENA 0x04
#define MCP_GPINTENB 0x05
#define MCP_DEFVALA 0x06
#define MCP_DEFVALB 0x07
#define MCP_INTCONA 0x08
#define MCP_INTCONB 0x09
#define MCP_IOCON 0x0A

#define TAG "FuriHalMCP23017"

static uint8_t mcp_addr = 0x20; // default 7-bit
// If true, the driver should use the 8-bit address form (addr<<1) when talking
// to the device. Some environments/devices require the 8-bit form for low-level
// register ops. This is discovered during probe and then cached.
static bool mcp_use_8bit_addr = false;
static GpioExtiCallback exti_cb = NULL;
static void* exti_ctx = NULL;

static bool mcp_write_reg(uint8_t reg, uint8_t val);
static bool mcp_read_reg(uint8_t reg, uint8_t* val);
static bool mcp_write_reg_locked(uint8_t reg, uint8_t val);
static bool mcp_write_reg_locked_addr(uint8_t addr, uint8_t reg, uint8_t val);

// Internal implementation that accepts explicit I2C address
bool furi_hal_mcp23017_init_ex(uint8_t i2c_addr) {
    // Accept explicit I2C address and store as normalized 7-bit form
    mcp_addr = i2c_addr;
    FURI_LOG_I(TAG, "Initializing MCP23017 at I2C address 0x%02X", mcp_addr);
    // Acquire bus and probe device similarly to INA219 driver
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_external);
    bool detected = false;
    // Quick device ready probe using 7-bit address
    if(furi_hal_i2c_is_device_ready(&furi_hal_i2c_handle_external, mcp_addr, 100)) {
        detected = true;
    } else {
        // Try direct register read probes using both 7-bit and 8-bit address forms
        uint8_t probe = 0;
        if(furi_hal_i2c_read_reg_8(&furi_hal_i2c_handle_external, mcp_addr, MCP_IOCON, &probe, 200)) {
            detected = true;
        } else {
            uint8_t probe8 = (uint8_t)(mcp_addr << 1);
            if(furi_hal_i2c_read_reg_8(&furi_hal_i2c_handle_external, probe8, MCP_IOCON, &probe, 200)) {
                detected = true;
            }
        }
    }

    if(!detected) {
        furi_hal_i2c_release(&furi_hal_i2c_handle_external);
        FURI_LOG_E(TAG, "MCP23017 not detected at 0x%02X", mcp_addr);
        return false;
    }

    // set IOCON: Mirror interrupts, active-low open-drain (common config)
    bool io_ok = false;
    // Try a few times with 7-bit address then 8-bit address form
    for(int attempt = 0; attempt < 3 && !io_ok; ++attempt) {
        if(mcp_write_reg_locked(MCP_IOCON, 0x44)) {
            io_ok = true;
            break;
        }
        // try 8-bit address form
        uint8_t addr8 = (uint8_t)(mcp_addr << 1);
        if(mcp_write_reg_locked_addr(addr8, MCP_IOCON, 0x44)) {
            io_ok = true;
            // remember that the device expects 8-bit address form
            mcp_use_8bit_addr = true;
            break;
        }
        furi_delay_ms(10);
    }

    if(!io_ok) {
        FURI_LOG_E(TAG, "Failed to write IOCON");
        furi_hal_i2c_release(&furi_hal_i2c_handle_external);
        return false; // MIRROR=1, INTPOL=0, ODR=1
    }
    furi_hal_i2c_release(&furi_hal_i2c_handle_external);
    FURI_LOG_I(TAG, "MCP23017 initialized");
    return true;
}

// Public no-argument wrapper kept for API compatibility. Calls the
// explicit-address implementation with the default address 0x27.
bool furi_hal_mcp23017_init(void) {
    return furi_hal_mcp23017_init_ex(mcp_addr);
}

static bool mcp_write_reg(uint8_t reg, uint8_t val) {
    bool ret = false;
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_external);
    if(mcp_use_8bit_addr) {
        uint8_t addr8 = (uint8_t)(mcp_addr << 1);
        ret = furi_hal_i2c_write_reg_8(&furi_hal_i2c_handle_external, addr8, reg, val, 200);
    } else {
        ret = furi_hal_i2c_write_reg_8(&furi_hal_i2c_handle_external, mcp_addr, reg, val, 200);
    }
    furi_hal_i2c_release(&furi_hal_i2c_handle_external);
    return ret;
}

static bool mcp_read_reg(uint8_t reg, uint8_t* val) {
    bool ret = false;
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_external);
    if(mcp_use_8bit_addr) {
        uint8_t addr8 = (uint8_t)(mcp_addr << 1);
        ret = furi_hal_i2c_read_reg_8(&furi_hal_i2c_handle_external, addr8, reg, val, 200);
    } else {
        ret = furi_hal_i2c_read_reg_8(&furi_hal_i2c_handle_external, mcp_addr, reg, val, 200);
    }
    furi_hal_i2c_release(&furi_hal_i2c_handle_external);
    return ret;
}

static bool mcp_write_reg_locked(uint8_t reg, uint8_t val) {
    // Caller must hold the I2C bus
    if(mcp_use_8bit_addr) {
        uint8_t addr8 = (uint8_t)(mcp_addr << 1);
        return furi_hal_i2c_write_reg_8(&furi_hal_i2c_handle_external, addr8, reg, val, 200);
    }
    return furi_hal_i2c_write_reg_8(&furi_hal_i2c_handle_external, mcp_addr, reg, val, 200);
}

static bool mcp_write_reg_locked_addr(uint8_t addr, uint8_t reg, uint8_t val) {
    return furi_hal_i2c_write_reg_8(&furi_hal_i2c_handle_external, addr, reg, val, 200);
}

// mcp_read_reg_locked removed (not needed). Use mcp_read_reg which acquires/releases bus.

bool furi_hal_mcp23017_read_gpio(uint16_t* gpio_state) {
    uint8_t a,b;
    if(!mcp_read_reg(MCP_GPIOA, &a)) return false;
    if(!mcp_read_reg(MCP_GPIOB, &b)) return false;
    *gpio_state = (uint16_t)a | ((uint16_t)b << 8);
    return true;
}

bool furi_hal_mcp23017_configure_interrupts(uint16_t gpios_to_input_mask) {
    FURI_LOG_I(TAG, "Configuring MCP23017 interrupts with mask 0x%04X", gpios_to_input_mask);
    // Configure direction: 1=input, 0=output
    uint8_t mask_a = (uint8_t)(gpios_to_input_mask & 0xFF);
    uint8_t mask_b = (uint8_t)((gpios_to_input_mask >> 8) & 0xFF);
    if(!mcp_write_reg(MCP_IODIRA, mask_a)) {
        FURI_LOG_I(TAG, "Failed to write IODIRA");
        return false;
    }
    if(!mcp_write_reg(MCP_IODIRB, mask_b)) {
        FURI_LOG_I(TAG, "Failed to write IODIRB");
        return false;
    }
    // enable interrupt-on-change for those pins
    if(!mcp_write_reg(MCP_GPINTENA, mask_a)) {
        FURI_LOG_I(TAG, "Failed to write GPINTENA");
        return false;
    }
    if(!mcp_write_reg(MCP_GPINTENB, mask_b)) {
        FURI_LOG_I(TAG, "Failed to write GPINTENB");
        return false;
    }

    // Enable internal pull-ups for the input pins so buttons wired to ground
    // see defined levels (typical active-low wiring). Write GPPUA/GPPUB.
    // GPPUA = 0x0C, GPPUB = 0x0D
    if(!mcp_write_reg(0x0C, mask_a)) {
        FURI_LOG_I(TAG, "Failed to write GPPUA (pull-ups)");
        return false;
    }
    if(!mcp_write_reg(0x0D, mask_b)) {
        FURI_LOG_I(TAG, "Failed to write GPPUB (pull-ups)");
        return false;
    }

    // set interrupt control to compare to previous (0)
    if(!mcp_write_reg(MCP_INTCONA, 0x00)) {
        FURI_LOG_I(TAG, "Failed to write INTCONA");
        return false;
    }
    if(!mcp_write_reg(MCP_INTCONB, 0x00)) {
        FURI_LOG_I(TAG, "Failed to write INTCONB");
        // Dump registers for diagnostics
        uint8_t ra, rb, ga, gb, iocon;
        mcp_read_reg(MCP_IODIRA, &ra);
        mcp_read_reg(MCP_IODIRB, &rb);
        mcp_read_reg(MCP_GPINTENA, &ga);
        mcp_read_reg(MCP_GPINTENB, &gb);
        mcp_read_reg(MCP_IOCON, &iocon);
        FURI_LOG_E(TAG, "RegDump IODIR A:0x%02X B:0x%02X GPINTENA:0x%02X GPINTENB:0x%02X IOCON:0x%02X", ra, rb, ga, gb, iocon);
        return false;
    }

    // Read back a few registers for diagnostics
    uint8_t ra, rb, ga, gb, pu_a, pu_b, iocon;
    mcp_read_reg(MCP_IODIRA, &ra);
    mcp_read_reg(MCP_IODIRB, &rb);
    mcp_read_reg(MCP_GPINTENA, &ga);
    mcp_read_reg(MCP_GPINTENB, &gb);
    mcp_read_reg(0x0C, &pu_a);
    mcp_read_reg(0x0D, &pu_b);
    mcp_read_reg(MCP_IOCON, &iocon);
    FURI_LOG_I(TAG, "RegDump IODIR A:0x%02X B:0x%02X GPINTENA:0x%02X GPINTENB:0x%02X GPPUA:0x%02X GPPUB:0x%02X IOCON:0x%02X", ra, rb, ga, gb, pu_a, pu_b, iocon);

    FURI_LOG_I(TAG, "MCP23017 interrupts configured");
    return true;
}

void furi_hal_mcp23017_attach_int(GpioExtiCallback cb, void* ctx) {
    exti_cb = cb;
    exti_ctx = ctx;
}

// This function should be called by the board-specific EXTI ISR when the INT pin fires
void furi_hal_mcp23017_handle_int(void) {
    if(exti_cb) exti_cb(exti_ctx);
}
