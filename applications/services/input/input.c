#include "input.h"
#include "input_settings.h"

#include <stdbool.h>
#include <stdint.h>
#include <furi.h>
#include <furi_hal_gpio.h>
#include <furi_hal_resources.h>

#define INPUT_PRESS_TICKS 500
#define INPUT_LONG_PRESS_COUNTS 2
#define INPUT_THREAD_FLAG_ISR 0x00000001
#define TAG "InputSrv"

typedef struct {
    const InputPin* pin;
    volatile bool state;
    FuriTimer* press_timer;
    FuriPubSub* event_pubsub;
    volatile uint8_t press_counter;
    volatile uint32_t counter;
} InputPinState;

void input_press_timer_callback(void* arg) {
    InputPinState* input_pin = arg;
    InputEvent event = {
        .sequence_source = INPUT_SEQUENCE_SOURCE_HARDWARE,
        .sequence_counter = input_pin->counter,
        .key = input_pin->pin->key};

    input_pin->press_counter++;
    if(input_pin->press_counter == INPUT_LONG_PRESS_COUNTS) {
        event.type = InputTypeLong;
        FURI_LOG_D(TAG, "Timer -> KeyID: %d, Type: Long", event.key);
        furi_pubsub_publish(input_pin->event_pubsub, &event);
    } else if(input_pin->press_counter > INPUT_LONG_PRESS_COUNTS) {
        event.type = InputTypeRepeat;
        FURI_LOG_D(TAG, "Timer -> KeyID: %d, Type: Repeat", event.key);
        furi_pubsub_publish(input_pin->event_pubsub, &event);
    }
}

void input_isr(void* _ctx) {
    FuriThreadId thread_id = (FuriThreadId)_ctx;
    furi_thread_flags_set(thread_id, INPUT_THREAD_FLAG_ISR);
}

const char* input_get_key_name(InputKey key) {
    switch(key) {
    case InputKeyOk: return "InputKeyOk";
    case InputKeyBack: return "InputKeyBack";
    case InputKeyUp: return "InputKeyUp";
    case InputKeyDown: return "InputKeyDown";
    case InputKeyRight: return "InputKeyRight";
    case InputKeyLeft: return "InputKeyLeft";
    default: return "Unknown";
    }
}

const char* input_get_type_name(InputType type) {
    switch(type) {
    case InputTypePress: return "Press";
    case InputTypeRelease: return "Release";
    case InputTypeShort: return "Short";
    case InputTypeLong: return "Long";
    case InputTypeRepeat: return "Repeat";
    default: return "Unknown";
    }
}

int32_t input_srv(void* p) {
    UNUSED(p);
    const FuriThreadId thread_id = furi_thread_get_current_id();
    FuriPubSub* event_pubsub = furi_pubsub_alloc();
    furi_record_create(RECORD_INPUT_EVENTS, event_pubsub);
    FuriPubSub* ascii_pubsub = furi_pubsub_alloc();
    furi_record_create(RECORD_ASCII_EVENTS, ascii_pubsub);
    InputSettings* settings = malloc(sizeof(InputSettings));
    input_settings_load(settings);
    furi_record_create(RECORD_INPUT_SETTINGS, settings);

    InputPinState pin_states[InputKeyMAX] = {0};
    for(size_t i = 0; i < InputKeyMAX; i++) {
        pin_states[i].pin = NULL;
        for(size_t j = 0; j < input_pins_count; j++) {
            if(input_pins[j].key == (InputKey)i) {
                pin_states[i].pin = &input_pins[j];
                break;
            }
        }
        pin_states[i].press_timer =
            furi_timer_alloc(input_press_timer_callback, FuriTimerTypePeriodic, &pin_states[i]);

        pin_states[i].event_pubsub = event_pubsub;
        pin_states[i].press_counter = 0;
        pin_states[i].state = false;
    }

    // Setup GPIO interrupts for all buttons
    // for(size_t i = 0; i < input_pins_count; i++) {
    //     furi_hal_gpio_add_int_callback(input_pins[i].gpio, input_isr, (void*)thread_id);
    // }

    FURI_LOG_I(TAG, "Input Service Started - Direct GPIO Mode");

    while(1) {
        bool is_changing = false;
        uint32_t flags = furi_thread_flags_wait(INPUT_THREAD_FLAG_ISR, FuriFlagWaitAny, 25);

        // Always scan buttons (both on interrupt and on timeout for polling)
        if((flags & INPUT_THREAD_FLAG_ISR) || (flags == FuriFlagErrorTimeout)) {
            // Debounce: wait and check if state is stable
            if(flags & INPUT_THREAD_FLAG_ISR) {
                furi_delay_ms(4);
            }
        }

        // Read all button states
        for(size_t i = 0; i < input_pins_count; i++) {
            bool pin_state = furi_hal_gpio_read(input_pins[i].gpio);
            if(input_pins[i].inverted) {
                pin_state = !pin_state;
            }
            InputKey key = input_pins[i].key;

            if(pin_state) { // Button pressed
                if(pin_states[key].state == false) {
                    // Debounce filter
                    if(pin_states[key].counter++ > INPUT_DEBOUNCE_TICKS) {
                        pin_states[key].counter = 0;
                        pin_states[key].state = true;
                        InputEvent event;
                        event.sequence_source = INPUT_SEQUENCE_SOURCE_HARDWARE;
                        event.sequence_counter = pin_states[key].counter;
                        event.key = key;
                        event.type = InputTypePress;
                        furi_pubsub_publish(event_pubsub, &event);
                        furi_timer_start(pin_states[key].press_timer, INPUT_PRESS_TICKS);
                    } else {
                        is_changing = true;
                    }
                }
            } else { // Button released
                if(pin_states[key].state == true) {
                    // Debounce filter
                    if(pin_states[key].counter++ > INPUT_DEBOUNCE_TICKS) {
                        pin_states[key].counter = 0;
                        pin_states[key].state = false;
                        InputEvent event;
                        event.sequence_source = INPUT_SEQUENCE_SOURCE_HARDWARE;
                        event.sequence_counter = pin_states[key].counter;
                        event.key = key;
                        if(pin_states[key].press_counter < INPUT_LONG_PRESS_COUNTS) {
                            event.type = InputTypeShort;
                        } else {
                            event.type = InputTypeLong;
                        }
                        furi_pubsub_publish(event_pubsub, &event);
                        event.type = InputTypeRelease;
                        furi_pubsub_publish(event_pubsub, &event);
                        furi_timer_stop(pin_states[key].press_timer);
                        pin_states[key].press_counter = 0;
                    } else {
                        is_changing = true;
                    }
                } else {
                    pin_states[key].counter = 0;
                }
            }
        }

        if(is_changing) {
            furi_thread_flags_set(thread_id, INPUT_THREAD_FLAG_ISR);
        }
    }

    // Cleanup
    for(size_t i = 0; i < input_pins_count; i++) {
        furi_hal_gpio_remove_int_callback(input_pins[i].gpio);
    }
    for(size_t i = 0; i < InputKeyMAX; i++) {
        if(pin_states[i].press_timer != NULL) {
            furi_timer_stop(pin_states[i].press_timer);
            furi_timer_free(pin_states[i].press_timer);
        }
    }
    furi_pubsub_free(event_pubsub);
    furi_pubsub_free(ascii_pubsub);
    free(settings);

    return 0;
}