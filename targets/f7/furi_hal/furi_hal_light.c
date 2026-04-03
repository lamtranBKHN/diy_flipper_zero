#include <core/common_defines.h>
#include <furi_hal_resources.h>
#include <furi_hal_light.h>
#include <stdint.h>
#include <momentum/momentum.h>
#include <rgb_backlight.h>

#define LED_CURRENT_RED   (50u)
#define LED_CURRENT_GREEN (50u)
#define LED_CURRENT_BLUE  (50u)
#define LED_CURRENT_WHITE (150u)

void furi_hal_light_init(void) {
    // No discrete RGB expander in this board revision.
}

void furi_hal_light_set(Light light, uint8_t value) {
    if(light & LightBacklight) {
        if(momentum_settings.rgb_backlight) {
            rgb_backlight_update(value, false);
        }
    }
}

void furi_hal_light_blink_start(Light light, uint8_t brightness, uint16_t on_time, uint16_t period) {
    // No dedicated RGB expander on this board, so just force on.
    UNUSED(brightness);
    UNUSED(on_time);
    UNUSED(period);
    furi_hal_light_set(light, 0xFF);
}

void furi_hal_light_blink_stop(void) {
    // RGB expander is not present in this board revision.
}

void furi_hal_light_blink_set_color(Light light) {
    UNUSED(light);
}

void furi_hal_light_sequence(const char* sequence) {
    do {
        switch(*sequence) {
        case 'R':
            furi_hal_light_set(LightRed, 0xFF);
            break;
        case 'r':
            furi_hal_light_set(LightRed, 0x00);
            break;
        case 'G':
            furi_hal_light_set(LightGreen, 0xFF);
            break;
        case 'g':
            furi_hal_light_set(LightGreen, 0x00);
            break;
        case 'B':
            furi_hal_light_set(LightBlue, 0xFF);
            break;
        case 'b':
            furi_hal_light_set(LightBlue, 0x00);
            break;
        case 'W':
            furi_hal_light_set(LightBacklight, 0xFF);
            break;
        case 'w':
            furi_hal_light_set(LightBacklight, 0x00);
            break;
        case '.':
            furi_delay_ms(250);
            break;
        case '-':
            furi_delay_ms(500);
            break;
        default:
            break;
        }
        sequence++;
    } while(*sequence != 0);
}
