#include "input.h"

#include "input_settings.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <furi.h>
#include <furi_hal_gpio.h>
#include <furi_hal_pcf8574.h>
#include <furi_hal_vibro.h>
#include <toolbox/cli/cli_command.h>
#include <cli/cli_main_commands.h>
#include <toolbox/pipe.h>

#define INPUT_DEBOUNCE_TICKS_HALF (INPUT_DEBOUNCE_TICKS / 2)
#define INPUT_PRESS_TICKS         200
#define INPUT_LONG_PRESS_COUNTS   5
#define INPUT_THREAD_FLAG_ISR     0x00000001
#define INPUT_IDLE_POLL_TICKS     20

#define TAG "Input"

/** Input pin state */
typedef struct {
    const InputPin* pin;
    // State
    volatile bool state;
    volatile uint8_t debounce;
    FuriTimer* press_timer;
    FuriPubSub* event_pubsub;
    volatile uint8_t press_counter;
    volatile uint32_t counter;
} InputPinState;

// #define INPUT_DEBUG

// PCF8574 input cache
static volatile uint8_t g_pcf_state = 0xFF;

// Default mapping: map input indices to PCF8574 P0..P7.
// Wiring mapping provided by user: PCF8574 pins 0..5 correspond to keys:
// pin0 -> key 3 (Back)
// pin1 -> key 4 (OK)
// pin2 -> key 1 (Down)
// pin3 -> key 0 (Up)
// pin4 -> key 5 (Left)
// pin5 -> key 2 (Right)
// We invert this to get per-input-index -> PCF pin mapping:
// index 0 (Up)   -> PCF pin 0
// index 1 (Down) -> PCF pin 1
// index 2 (Right)-> PCF pin 3
// index 3 (Left) -> PCF pin 2
// index 4 (OK)   -> PCF pin 4
// index 5 (Back) -> PCF pin 5
static const uint8_t pcf_pin_map_default[] = {
    0, // input_pins[0] (Up)    -> PCF pin 0
    1, // input_pins[1] (Down)  -> PCF pin 1
    3, // input_pins[2] (Right) -> PCF pin 3
    2, // input_pins[3] (Left)  -> PCF pin 2
    4, // input_pins[4] (OK)    -> PCF pin 4
    5, // input_pins[5] (Back)  -> PCF pin 5
};

static uint8_t input_pcf_mask_for_index(size_t idx) {
    size_t cnt = sizeof(pcf_pin_map_default) / sizeof(pcf_pin_map_default[0]);
    if(idx < cnt) {
        uint8_t bit = pcf_pin_map_default[idx];
        return (uint8_t)(1u << bit);
    }
    // fallback: no mapping
    return 0;
}
#define GPIO_Read_PCF_BY_IDX(idx) (((g_pcf_state & input_pcf_mask_for_index(idx)) != 0) ^ (input_pins[idx].inverted))

void input_press_timer_callback(void* arg) {
    if(!arg) return;
    InputPinState* input_pin = arg;
    if(!input_pin->pin) return;

    InputEvent event;
    event.sequence_source = INPUT_SEQUENCE_SOURCE_HARDWARE;
    event.sequence_counter = input_pin->counter;
    event.key = input_pin->pin->key;
    input_pin->press_counter++;
    if(input_pin->press_counter == INPUT_LONG_PRESS_COUNTS) {
        event.type = InputTypeLong;
        if(input_pin->event_pubsub) {
            furi_pubsub_publish(input_pin->event_pubsub, &event);
        }
    } else if(input_pin->press_counter > INPUT_LONG_PRESS_COUNTS) {
        input_pin->press_counter--;
        event.type = InputTypeRepeat;
        if(input_pin->event_pubsub) {
            furi_pubsub_publish(input_pin->event_pubsub, &event);
        }
    }
}

void input_isr(void* _ctx) {
    FuriThreadId thread_id = (FuriThreadId)_ctx;
    furi_thread_flags_set(thread_id, INPUT_THREAD_FLAG_ISR);
}

const char* input_get_key_name(InputKey key) {
    for(size_t i = 0; i < input_pins_count; i++) {
        if(input_pins[i].key == key) {
            return input_pins[i].name;
        }
    }
    return "Unknown";
}

const char* input_get_type_name(InputType type) {
    switch(type) {
    case InputTypePress:
        return "Press";
    case InputTypeRelease:
        return "Release";
    case InputTypeShort:
        return "Short";
    case InputTypeLong:
        return "Long";
    case InputTypeRepeat:
        return "Repeat";
    default:
        return "Unknown";
    }
}

int32_t input_srv(void* p) {
    UNUSED(p);

    const FuriThreadId thread_id = furi_thread_get_current_id();
    FuriPubSub* event_pubsub = furi_pubsub_alloc();
    FuriPubSub* ascii_pubsub = furi_pubsub_alloc();
    uint32_t counter = 1;
    furi_record_create(RECORD_INPUT_EVENTS, event_pubsub);
    furi_record_create(RECORD_ASCII_EVENTS, ascii_pubsub);

    //define object input_settings, take memory load (or init) settings and create record for access to settings structure from outside
    InputSettings* settings = malloc(sizeof(InputSettings));
    input_settings_load(settings);
    furi_record_create(RECORD_INPUT_SETTINGS, settings);

#ifdef INPUT_DEBUG
    furi_hal_gpio_init_simple(&gpio_ext_pa4, GpioModeOutputPushPull);
#endif

    InputPinState* pin_states = malloc(sizeof(InputPinState) * input_pins_count);
    if(!pin_states) {
        FURI_LOG_E(TAG, "Failed to allocate pin states");
        return 0;
    }
    for(size_t i = 0; i < input_pins_count; i++) {
        // Using expander backend only: register a single callback later, don't attach per-pin
        (void)input_pins;
        pin_states[i].pin = &input_pins[i];
        pin_states[i].state = false;
        pin_states[i].debounce = INPUT_DEBOUNCE_TICKS_HALF;
        pin_states[i].press_timer = furi_timer_alloc(
            input_press_timer_callback, FuriTimerTypePeriodic, &pin_states[i]);
        if(!pin_states[i].press_timer) {
            FURI_LOG_W(TAG, "Timer alloc failed for pin %u", (unsigned)i);
        }
        pin_states[i].event_pubsub = event_pubsub;
        pin_states[i].press_counter = 0;
    }

    if(furi_hal_pcf8574_init()) {
        FURI_LOG_I(TAG, "PCF8574 initialized");
        for(size_t i = 0; i < input_pins_count; i++) {
            (void)input_pcf_mask_for_index(i);
        }

        // Read current PCF8574 state and seed cache so we don't generate spurious
        // events from the initial snapshot. Also mark debounces as stable.
        uint8_t tmp_state = 0xFF;
        if(furi_hal_pcf8574_read(&tmp_state)) {
            g_pcf_state = tmp_state;
            for(size_t j = 0; j < input_pins_count; j++) {
                pin_states[j].state = GPIO_Read_PCF_BY_IDX(j);
                pin_states[j].debounce = INPUT_DEBOUNCE_TICKS; // stable
            }
        }

        // Attach INT callback: when PCF reports interrupt, call input_isr
        furi_hal_pcf8574_attach_int(input_isr, (void*)thread_id);
        // Ensure the MCU INT pin is configured as an interrupt input with pull-up
        furi_hal_gpio_init(&gpio_pcf8574_int, GpioModeInterruptRiseFall, GpioPullUp, GpioSpeedLow);
        // Attach the expander INT line to the MCU EXTI pin
        furi_hal_gpio_add_int_callback(
            &gpio_pcf8574_int, (GpioExtiCallback)furi_hal_pcf8574_handle_int, NULL);
        furi_hal_gpio_enable_int_callback(&gpio_pcf8574_int);
    } else {
        FURI_LOG_E(TAG, "PCF8574 input expander not detected");
    }

    while(1) {
        bool is_changing = false;
        uint8_t new_state = g_pcf_state;
        if(furi_hal_pcf8574_read(&new_state)) {
            g_pcf_state = new_state;
        } else {
            // I2C read failed — assume all buttons released (0xFF = all high = none pressed)
            // to prevent phantom stuck keys from stale cached state
            g_pcf_state = 0xFF;
        }

        for(size_t i = 0; i < input_pins_count; i++) {
            bool state;
            state = GPIO_Read_PCF_BY_IDX(i);
            if(state) {
                if(pin_states[i].debounce < INPUT_DEBOUNCE_TICKS) pin_states[i].debounce += 1;
            } else {
                if(pin_states[i].debounce > 0) pin_states[i].debounce -= 1;
            }

            if(pin_states[i].debounce > 0 && pin_states[i].debounce < INPUT_DEBOUNCE_TICKS) {
                is_changing = true;
            } else if(pin_states[i].state != state) {
                pin_states[i].state = state;

                // Common state info
                InputEvent event;
                event.sequence_source = INPUT_SEQUENCE_SOURCE_HARDWARE;
                event.key = pin_states[i].pin->key;

                // Short / Long / Repeat timer routine
                if(state) {
                    pin_states[i].counter = counter++;
                    event.sequence_counter = pin_states[i].counter;
                    furi_timer_start(pin_states[i].press_timer, INPUT_PRESS_TICKS);
                } else {
                    event.sequence_counter = pin_states[i].counter;
                    if(pin_states[i].press_timer) {
                        furi_timer_stop(pin_states[i].press_timer);
                    }
                    if(pin_states[i].press_counter < INPUT_LONG_PRESS_COUNTS) {
                        event.type = InputTypeShort;
                        // FURI_LOG_I(TAG, "Publish Short: key=%d seq=%u", event.key, event.sequence_counter);
                        furi_pubsub_publish(event_pubsub, &event);
                    }
                    pin_states[i].press_counter = 0;
                }

                // Send Press/Release event
                event.type = pin_states[i].state ? InputTypePress : InputTypeRelease;
                // FURI_LOG_I(TAG, "Publish %s: key=%d seq=%u", pin_states[i].state ? "Press" : "Release", event.key, event.sequence_counter);
                furi_pubsub_publish(event_pubsub, &event);

#ifdef INPUT_MCP_IMMEDIATE_PUBLISH
                // Also publish an immediate short event for quicker UI testing
                InputEvent dbg = event;
                dbg.type = InputTypeShort;
                // FURI_LOG_I(TAG, "Immediate publish debug: key=%d", dbg.key);
                furi_pubsub_publish(event_pubsub, &dbg);
#endif
                // vibro signal if user setup vibro touch level in Settings-Input.
                if(settings->vibro_touch_level &&
                   ((1 << event.type) & settings->vibro_touch_trigger_mask)) {
                    //delay 1 ticks for compatibility with rgb_backlight_mod
                    furi_delay_tick(1);
                    furi_hal_vibro_on(true);
                    furi_delay_tick(settings->vibro_touch_level * 10);
                    furi_hal_vibro_on(false);
                }
            }
        }

        if(is_changing) {
#ifdef INPUT_DEBUG
            // furi_hal_gpio_write(&gpio_ext_pa4, 1);
#endif
            furi_delay_tick(1);
        } else {
#ifdef INPUT_DEBUG
            // furi_hal_gpio_write(&gpio_ext_pa4, 0);
#endif
            // Polling fallback: do not block forever waiting for INT.
            // Some board revisions have missing/unstable PCF8574 INT wiring.
            (void)furi_thread_flags_wait(
                INPUT_THREAD_FLAG_ISR, FuriFlagWaitAny, INPUT_IDLE_POLL_TICKS);
        }
    }

    return 0;
}
