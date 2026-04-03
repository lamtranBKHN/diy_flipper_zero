#include <furi_hal_speaker.h>
#include <furi_hal_gpio.h>
#include <furi_hal_resources.h>
#include <furi_hal_power.h>
#include <furi_hal_bus.h>
#include <furi_hal_pcf8574.h>

#include <furi_hal_cortex.h>

#define TAG "FuriHalSpeaker"

static FuriMutex* furi_hal_speaker_mutex = NULL;

// #define FURI_HAL_SPEAKER_NEW_VOLUME

void furi_hal_speaker_init(void) {
    furi_assert(furi_hal_speaker_mutex == NULL);
    furi_hal_speaker_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    FURI_LOG_I(TAG, "Init OK");
}

void furi_hal_speaker_deinit(void) {
    furi_check(furi_hal_speaker_mutex != NULL);
    furi_mutex_free(furi_hal_speaker_mutex);
    furi_hal_speaker_mutex = NULL;
}

bool furi_hal_speaker_acquire(uint32_t timeout) {
    furi_check(!FURI_IS_IRQ_MODE());

    if(furi_mutex_acquire(furi_hal_speaker_mutex, timeout) == FuriStatusOk) {
        furi_hal_power_insomnia_enter();
        furi_hal_pcf8574_init();
        return true;
    } else {
        return false;
    }
}

void furi_hal_speaker_release(void) {
    furi_check(!FURI_IS_IRQ_MODE());
    furi_check(furi_hal_speaker_is_mine());

    furi_hal_speaker_stop();
    furi_hal_power_insomnia_exit();

    furi_check(furi_mutex_release(furi_hal_speaker_mutex) == FuriStatusOk);
}

bool furi_hal_speaker_is_mine(void) {
    return (FURI_IS_IRQ_MODE()) ||
           (furi_mutex_get_owner(furi_hal_speaker_mutex) == furi_thread_get_current_id());
}

void furi_hal_speaker_start(float frequency, float volume) {
    furi_check(furi_hal_speaker_is_mine());
    UNUSED(frequency);

    if(volume <= 0) {
        furi_hal_speaker_stop();
        return;
    }
    furi_hal_pcf8574_write_pin(PCF8574_PIN_BUZZER, true);
}

void furi_hal_speaker_set_volume(float volume) {
    furi_check(furi_hal_speaker_is_mine());
    if(volume <= 0) furi_hal_pcf8574_write_pin(PCF8574_PIN_BUZZER, false);
    else furi_hal_pcf8574_write_pin(PCF8574_PIN_BUZZER, true);
}

void furi_hal_speaker_stop(void) {
    furi_check(furi_hal_speaker_is_mine());
    furi_hal_pcf8574_write_pin(PCF8574_PIN_BUZZER, false);
}
