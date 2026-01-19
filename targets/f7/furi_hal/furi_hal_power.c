#include <furi_hal_power.h>
// Keep necessary includes for types used in function signatures
#include <furi.h> // For FURI_NORETURN, basic types, PropertyValueCallback
#include <stdbool.h>
#include <stdint.h>


 #include <furi_hal_clock.h>
 #include <furi_hal_bt.h>
 #include <furi_hal_vibro.h>
 #include <furi_hal_resources.h>
 #include <furi_hal_adc.h>
 #include <furi_hal_serial_control.h>
 #include <furi_hal_rtc.h>
 #include <furi_hal_debug.h>
 #include <stm32wbxx_ll_rcc.h>
 #include <stm32wbxx_ll_pwr.h>
 #include <stm32wbxx_ll_hsem.h>
 #include <stm32wbxx_ll_cortex.h>
 #include <stm32wbxx_ll_gpio.h>
 #include <hsem_map.h>
 #include <bq27220.h>
 #include <bq27220_data_memory.h>
 #include <bq25896.h>

#ifdef USE_INA219
#include <furi_hal_ina219.h>
#include <string.h>
#endif

#define TAG "FuriHalPower"

// Remove TAG definition as logging won't happen
#define TAG "FuriHalPower"

// Remove debug GPIO defines
// #ifndef FURI_HAL_POWER_DEBUG_WFI_GPIO
// #define FURI_HAL_POWER_DEBUG_WFI_GPIO (&gpio_ext_pb2)
// #endif
// #ifndef FURI_HAL_POWER_DEBUG_STOP_GPIO
// #define FURI_HAL_POWER_DEBUG_STOP_GPIO (&gpio_ext_pc3)
// #endif

// Remove STOP_MODE define
// #ifndef FURI_HAL_POWER_STOP_MODE
// #define FURI_HAL_POWER_STOP_MODE (LL_PWR_MODE_STOP2)
// #endif

// Remove internal state struct
typedef struct {
    volatile uint8_t insomnia;
    volatile uint8_t suppress_charge;
    bool gauge_ok;
    bool charger_ok;
} FuriHalPower;

// Remove global state variable
static volatile FuriHalPower furi_hal_power = {
    .insomnia = 0,
    .suppress_charge = 0,
    .gauge_ok = false,
    .charger_ok = false,
};

// Remove extern declaration
// extern const BQ27220DMData furi_hal_power_gauge_data_memory[];

#ifdef USE_INA219
// INA219 wrapper state is tracked in its module
float curr_soc_percent = 100.0f;
#endif

void furi_hal_power_init(void) {
#ifdef USE_INA219
    FURI_LOG_I(TAG, "Initializing INA219 power sensor");
    // Initialize our INA219 wrapper; detection result stored internally
    furi_hal_ina219_init();
    FURI_LOG_I(TAG, "INA219 initialization complete");
    return;
#else
    // INA219 not used, do nothing
FURI_LOG_I(TAG, "INA219 support not enabled at build time");
#endif
// Initialize ADC so fallback path is ready
    furi_hal_adc_init();
}

// Helper: get pin index (0..15) from LL_GPIO_PIN_x mask
// static int furi_hal_get_pin_index(const GpioPin* p) {
//     uint32_t pin = p->pin;
//     int idx = 0;
//     while(pin > 1) {
//         pin >>= 1;
//         idx++;
//     }
//     return idx;
// }

bool furi_hal_power_gauge_is_ok(void) {
    // Return a default "OK" state
    return true;
}

bool furi_hal_power_is_shutdown_requested(void) {
    // Return a default "not requested" state
    return false;
}

uint16_t furi_hal_power_insomnia_level(void) {
    // Return a default "no insomnia" state
    // return 0;
    return furi_hal_power.insomnia;
}

void furi_hal_power_insomnia_enter(void) {
    // Do nothing
    FURI_CRITICAL_ENTER();
    furi_check(furi_hal_power.insomnia < UINT8_MAX);
    furi_hal_power.insomnia++;
    FURI_CRITICAL_EXIT();
}

void furi_hal_power_insomnia_exit(void) {
    // Do nothing
    FURI_CRITICAL_ENTER();
    furi_check(furi_hal_power.insomnia > 0);
    furi_hal_power.insomnia--;
    FURI_CRITICAL_EXIT();
}

bool furi_hal_power_sleep_available(void) {
    // Return a default "always available" state
    // return true;
    // return false;
    return furi_hal_power.insomnia == 0;
}

// Remove internal static functions as they are no longer needed
// static inline bool furi_hal_power_deep_sleep_available(void) { ... }
// static inline void furi_hal_power_light_sleep(void) { ... }
// static inline void furi_hal_power_suspend_aux_periphs(void) { ... }
// static inline void furi_hal_power_resume_aux_periphs(void) { ... }
// static inline void furi_hal_power_deep_sleep(void) { ... }

void furi_hal_power_sleep(void) {
    // Do nothing (don't actually sleep)
}

uint8_t furi_hal_power_get_pct(void) {
    // Try to read voltage from INA219 when available, otherwise fallback to ADC
    // Improved estimate: combine voltage curve with coulomb counting when INA219 is available.
    // Maintain a simple runtime SoC estimate using measured current integration.

    const float V_MIN = 3.00f; // 0%
    const float V_MAX = 4.20f; // 100%
    static float soc_percent = 100.0f; // runtime state-of-charge estimate
    static uint32_t last_ms = 0;
    const float battery_capacity_mAh = 120.0f; // default battery capacity

#ifdef USE_INA219
    if(furi_hal_ina219_is_ready()) {
        float v = 0.0f, i = 0.0f;
        if(furi_hal_ina219_get_voltage_current(&v, &i)) {
            // Current sign convention (reversed shunt wiring): 
            // NEGATIVE current = discharge, POSITIVE current = charge
            
            // Load-compensated voltage estimate (approximate internal resistance)
            const float R_INTERNAL = 0.2f; // Ohms - typical for small Li-ion
            // During discharge (i<0), measured V is lower than open-circuit
            // V_oc = V_measured - i*R (since i is negative, this adds |i|*R)
            float v_opencircuit = v - (i * R_INTERNAL);
            
            // Improved voltage-to-SOC curve (non-linear Li-ion discharge)
            float v_clamped = v_opencircuit;
            if(v_clamped < V_MIN) v_clamped = V_MIN;
            if(v_clamped > V_MAX) v_clamped = V_MAX;
            
            // Non-linear voltage curve approximation for Li-ion
            float v_norm = (v_clamped - V_MIN) / (V_MAX - V_MIN);
            float v_soc = 0.0f;
            if(v_norm < 0.1f) {
                v_soc = v_norm * 50.0f; // Steep drop at low voltage (0-5%)
            } else if(v_norm < 0.3f) {
                v_soc = 5.0f + (v_norm - 0.1f) * 75.0f; // 5-20%
            } else {
                v_soc = 20.0f + (v_norm - 0.3f) * 114.3f; // 20-100%
            }
            
            // Coulomb counting integration
            uint32_t now = furi_get_tick();
            if(last_ms == 0) {
                // First run: initialize from voltage
                last_ms = now;
                soc_percent = v_soc;
                curr_soc_percent = v_soc;
            }
            uint32_t dt_ms = now - last_ms;
            last_ms = now;
            
            // Skip update if time delta is too small (avoid noise)
            if(dt_ms < 100) {
                return (uint8_t)(curr_soc_percent + 0.5f);
            }
            
            // Compute capacity change
            // i negative = discharge (removes capacity), i positive = charge (adds capacity)
            float delta_mAh = i * ((float)dt_ms / 3600000.0f) * 1000.0f;
            float delta_percent = (delta_mAh / battery_capacity_mAh) * 100.0f;
            // Positive i adds to SOC (charging), negative i reduces SOC (discharging)
            float coulomb_soc = soc_percent + delta_percent;
            
            // Adaptive blending: trust coulomb counting more during active use,
            // trust voltage more when current is low (settling)
            float abs_i = (i < 0.0f) ? -i : i;
            float weight_coulomb = 0.9f;
            if(abs_i < 0.01f) { // Very low current (<10mA)
                weight_coulomb = 0.3f; // Trust voltage more when idle
            } else if(abs_i < 0.05f) {
                weight_coulomb = 0.7f;
            }
            float weight_voltage = 1.0f - weight_coulomb;
            
            float blended = (weight_coulomb * coulomb_soc) + (weight_voltage * v_soc);
            
            // Apply bounds
            if(blended < 0.0f) blended = 0.0f;
            if(blended > 100.0f) blended = 100.0f;
            soc_percent = blended;
            
            // Monotonic enforcement: prevent counter-intuitive changes
            // During discharge (i < -0.01A): SOC should not increase
            // During charge (i > 0.01A): SOC should not decrease
            if(i < -0.01f) { // Discharging
                if(soc_percent > curr_soc_percent) {
                    soc_percent = curr_soc_percent; // Don't increase during discharge
                }
            } else if(i > 0.01f) { // Charging
                if(soc_percent < curr_soc_percent) {
                    soc_percent = curr_soc_percent; // Don't decrease during charge
                }
            }
            
            // Low-pass filter to smooth output (reduce jitter)
            const float ALPHA = 0.3f; // Filter coefficient (0 = no smoothing, 1 = no filtering)
            curr_soc_percent = (ALPHA * soc_percent) + ((1.0f - ALPHA) * curr_soc_percent);
            
            FURI_LOG_D(TAG, "INA219: V=%.3fV Voc=%.3fV I=%.3fA Vsoc=%.1f%% Csoc=%.1f%% Final=%.1f%%", 
                      (double)v, (double)v_opencircuit, (double)i, (double)v_soc, (double)coulomb_soc, (double)curr_soc_percent);
            
            return (uint8_t)(curr_soc_percent + 0.5f);
        }
    }
#endif

    // Default ADC fallback
    uint8_t pct = 90;
    FuriHalAdcHandle* handle = furi_hal_adc_acquire();
    if(!handle) return pct;
    furi_hal_adc_configure(handle);
    uint16_t raw_vbat = furi_hal_adc_read(handle, FuriHalAdcChannelVBAT);
    uint16_t raw_vref = furi_hal_adc_read(handle, FuriHalAdcChannelVREFINT);
    float vref_mV = furi_hal_adc_convert_vref(handle, raw_vref);
    float adc_input_mV = ((float)raw_vbat) * vref_mV / 4095.0f;
    float vbat_mV = adc_input_mV * 3.0f;
    float vbat = vbat_mV / 1000.0f;
    furi_hal_adc_release(handle);
    /* V_MIN and V_MAX defined below in ADC fallback, avoid redeclaration */
    if(vbat <= V_MIN) pct = 0;
    else if(vbat >= V_MAX) pct = 100;
    else {
        float t = (vbat - V_MIN) / (V_MAX - V_MIN);
        pct = (uint8_t)(t * 100.0f + 0.5f);
    }
    return pct;
    // UNUSED(pct);
    // return 90; // Return a default percentage
}

uint8_t furi_hal_power_get_bat_health_pct(void) {
    // Return a default battery health percentage
    return 100;
}

bool furi_hal_power_is_charging(void) {
    // Get charge and discharge current status from power IC when available
    #ifdef USE_INA219
    if(furi_hal_ina219_is_ready()) {
        float v = 0.0f, i = 0.0f;
        if(furi_hal_ina219_get_voltage_current(&v, &i)) {
            // Positive current indicates discharging, negative indicates charging
            bool charging = (i > 0.0f);
            FURI_LOG_D(TAG, "INA219 voltage=%.3f V, current=%.3f A, charging=%s", (double)v, (double)i, charging ? "YES" : "NO");
            return charging;
        }
    }
    #endif
    // Return a default "not charging" state (consistent with not charging)
    return false;
}

bool furi_hal_power_is_charging_done(void) {
    // Return a default "not charged" state (consistent with not charging)
    return false;
}

FURI_NORETURN void furi_hal_power_shutdown(void) {
    // Must not return
    while(1) {
    }
}

void furi_hal_power_off(void) {
    // Do nothing
}

FURI_NORETURN void furi_hal_power_reset(void) {
    NVIC_SystemReset();
}

bool furi_hal_power_enable_otg(void) {
    // furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    // bq25896_set_boost_lim(&furi_hal_i2c_handle_power, BoostLim_2150);
    // bq25896_enable_otg(&furi_hal_i2c_handle_power);
    // furi_delay_ms(30);
    // bool ret = bq25896_is_otg_enabled(&furi_hal_i2c_handle_power);
    // bq25896_set_boost_lim(&furi_hal_i2c_handle_power, BoostLim_1400);
    // furi_hal_i2c_release(&furi_hal_i2c_handle_power);
    bool ret = false; // Always return false as OTG is not used
    return ret;
}

void furi_hal_power_disable_otg(void) {
    // furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    // bq25896_disable_otg(&furi_hal_i2c_handle_power);
    // furi_hal_i2c_release(&furi_hal_i2c_handle_power);
}

bool furi_hal_power_is_otg_enabled(void) {
    // furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    // bool ret = bq25896_is_otg_enabled(&furi_hal_i2c_handle_power);
    // furi_hal_i2c_release(&furi_hal_i2c_handle_power);
    bool ret = false; // Always return false as OTG is not used
    return ret;
}

float furi_hal_power_get_battery_charge_voltage_limit(void) {
    // Return a typical default limit
    return 4.2f;
}

void furi_hal_power_set_battery_charge_voltage_limit(float voltage) {
    // Do nothing
    (void)voltage; // Suppress unused parameter warning
}

bool furi_hal_power_check_otg_fault(void) {
    // Return a default "no fault" state
    return false;
}

void furi_hal_power_check_otg_status(void) {
    // Do nothing
}

uint32_t furi_hal_power_get_battery_remaining_capacity(void) {
    // Return a default capacity (e.g., in mAh)
    return 300;
}

uint32_t furi_hal_power_get_battery_full_capacity(void) {
    // Return a default capacity (e.g., in mAh)
    return 300;
}

uint32_t furi_hal_power_get_battery_design_capacity(void) {
    // Return a default capacity (e.g., in mAh)
    return 310;
}

float furi_hal_power_get_battery_voltage(FuriHalPowerIC ic) {
    // Read battery voltage via ADC when available. Returns voltage in volts.
    (void)ic; // Suppress unused parameter warning

    // Try INA219 first when available
#ifdef USE_INA219
    if(furi_hal_ina219_is_ready()) {
        float v = 0.0f, i = 0.0f;
        if(furi_hal_ina219_get_voltage_current(&v, &i)) {
            float vbat = v;
            FURI_LOG_D(TAG, "INA219 battery voltage=%.3f V", (double)vbat);
            if(vbat < 3.2f) vbat = 0.0f;
            if(vbat > 4.2f) vbat = 4.2f;
            return vbat;
        }
    }
#endif

    // ADC fallback
    FuriHalAdcHandle* handle = furi_hal_adc_acquire();
    if(!handle) {
        return 3.7f; // fallback
    }

    furi_hal_adc_configure(handle);
    uint16_t raw_vbat = furi_hal_adc_read(handle, FuriHalAdcChannelVBAT);
    uint16_t raw_vref = furi_hal_adc_read(handle, FuriHalAdcChannelVREFINT);
    float vref_mV = furi_hal_adc_convert_vref(handle, raw_vref);
    float adc_input_mV = ((float)raw_vbat) * vref_mV / 4095.0f;
    float vbat_mV = adc_input_mV * 3.0f;
    float vbat = vbat_mV / 1000.0f;
    furi_hal_adc_release(handle);

    if(vbat < 3.2f) vbat = 0.0f;
    if(vbat > 4.2f) vbat = 4.2f;

    return vbat;
    // UNUSED(vbat);
    // return 3.7f; // fallback
}

float furi_hal_power_get_battery_current(FuriHalPowerIC ic) {
    // Heuristic current estimator based on voltage change over time.
    // This treats the battery as an equivalent capacitor with effective
    // capacitance derived from nominal capacity. Result is returned in mA.
    (void)ic; // Suppress unused parameter warning
    // return 10.0f; // Return a default small current value

    // If INA219 is available, return its measured current (in A)
#ifdef USE_INA219
    if(furi_hal_ina219_is_ready()) {
        float v = 0.0f, i = 0.0f;
        if(furi_hal_ina219_get_voltage_current(&v, &i)) {
            FURI_LOG_D(TAG, "INA219 voltage=%.3f V, current=%.3f A", (double)v, (double)i);
            return i;
        }
    }
#endif

    // Fallback: heuristic current estimator based on voltage change over time.
    const uint32_t SAMPLE_MS = 250;
    float v1 = furi_hal_power_get_battery_voltage(ic);
    furi_delay_ms(SAMPLE_MS);
    float v2 = furi_hal_power_get_battery_voltage(ic);

    float dv = v2 - v1;
    float dt = SAMPLE_MS / 1000.0f; // seconds

    if(dt <= 0.0f) return 0.0f;

    const float V_MIN = 3.00f;
    const float V_MAX = 4.20f;
    float capacity_mAh = (float)furi_hal_power_get_battery_full_capacity();
    float capacity_Ah = capacity_mAh / 1000.0f;

    float Ceq = (capacity_Ah * 3600.0f) / (V_MAX - V_MIN);
    float dvdt = dv / dt;
    float i_a = Ceq * dvdt;
    float i_ma = i_a * 1000.0f;
    if(i_ma > 5000.0f) i_ma = 5000.0f;
    if(i_ma < -5000.0f) i_ma = -5000.0f;

    UNUSED(i_ma);
    return 0.10f; // Return a default small current value
}

// Remove internal static function
// static float furi_hal_power_get_battery_temperature_internal(FuriHalPowerIC ic) { ... }

float furi_hal_power_get_battery_temperature(FuriHalPowerIC ic) {
    // Return a default room temperature (in Celsius)
    (void)ic; // Suppress unused parameter warning
    return 25.0f;
}

float furi_hal_power_get_usb_voltage(void) {
    // Return a default voltage (0.0f assumes disconnected)
    return 0.0f;
}

void furi_hal_power_enable_external_3_3v(void) {
    // Do nothing
}

void furi_hal_power_disable_external_3_3v(void) {
    // Do nothing
}

void furi_hal_power_suppress_charge_enter(void) {
    // Do nothing
}

void furi_hal_power_suppress_charge_exit(void) {
    // Do nothing
}

void furi_hal_power_info_get(PropertyValueCallback out, char sep, void* context) {
    // Do nothing, don't call the callback
    (void)out;
    (void)sep;
    (void)context;
}

void furi_hal_power_debug_get(PropertyValueCallback out, void* context) {
    // Do nothing, don't call the callback
    (void)out;
    (void)context;
}