#include <furi_hal_speaker.h>
#include <furi_hal_gpio.h>
#include <furi_hal_resources.h>
#include <furi_hal_power.h>
#include <furi_hal_pcf8574.h>

#include <furi_hal_cortex.h>

#define TAG "FuriHalSpeaker"

#define FURI_HAL_SPEAKER_DEFAULT_FREQUENCY (2000.0f)
#define FURI_HAL_SPEAKER_DEFAULT_HALF_PERIOD_US (250U)
#define FURI_HAL_SPEAKER_THREAD_STACK_SIZE (1024U)

typedef struct {
    FuriThread* thread;
    volatile bool terminate;
    volatile bool active;
    volatile uint32_t half_period_us;
} FuriHalSpeakerWorker;

static FuriMutex* furi_hal_speaker_mutex = NULL;
static FuriHalSpeakerWorker furi_hal_speaker_worker_state = {
    .thread = NULL,
    .terminate = false,
    .active = false,
    .half_period_us = FURI_HAL_SPEAKER_DEFAULT_HALF_PERIOD_US,
};

static bool furi_hal_speaker_pcf_ready = false;

static bool furi_hal_speaker_pcf_ensure_ready(void) {
    if(!furi_hal_speaker_pcf_ready) {
        furi_hal_speaker_pcf_ready = furi_hal_pcf8574_init();
        if(!furi_hal_speaker_pcf_ready) {
            return false;
        }
    }
    return true;
}

static void furi_hal_speaker_pin_start(void) {
    if(furi_hal_speaker_pcf_ensure_ready()) {
        furi_hal_pcf8574_write_pin(PCF8574_PIN_BUZZER, false);
    }
}

static void furi_hal_speaker_pin_stop(void) {
    // PB8 is intentionally unused on this board; keep it high-Z.
    furi_hal_gpio_init(&gpio_speaker, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    if(furi_hal_speaker_pcf_ensure_ready()) {
        furi_hal_pcf8574_write_pin(PCF8574_PIN_BUZZER, false);
    }
}

static uint32_t furi_hal_speaker_frequency_to_half_period(float frequency) {
    if(!(frequency > 0.0f)) {
        frequency = FURI_HAL_SPEAKER_DEFAULT_FREQUENCY;
    }

    uint32_t half_period_us = (uint32_t)(500000.0f / frequency);
    if(half_period_us == 0) {
        half_period_us = 1;
    }

    return half_period_us;
}

static int32_t furi_hal_speaker_worker(void* context) {
    UNUSED(context);

    bool pin_active = false;
    bool pin_state = false;

    while(!furi_hal_speaker_worker_state.terminate) {
        if(!furi_hal_speaker_worker_state.active) {
            if(pin_active) {
                furi_hal_speaker_pin_stop();
                pin_active = false;
                pin_state = false;
            }
            furi_delay_tick(1);
            continue;
        }

        if(!pin_active) {
            furi_hal_speaker_pin_start();
            pin_active = true;
            pin_state = false;
        }

        uint32_t half_period_us = furi_hal_speaker_worker_state.half_period_us;
        if(half_period_us == 0) {
            half_period_us = FURI_HAL_SPEAKER_DEFAULT_HALF_PERIOD_US;
        }

        pin_state = !pin_state;
        if(!furi_hal_speaker_pcf_ensure_ready() ||
           !furi_hal_pcf8574_write_pin(PCF8574_PIN_BUZZER, pin_state)) {
            furi_hal_speaker_pcf_ready = false;
            pin_active = false;
            pin_state = false;
            furi_delay_tick(1);
            continue;
        }

        FuriHalCortexTimer timer = furi_hal_cortex_timer_get(half_period_us);
        while(!furi_hal_cortex_timer_is_expired(timer)) {
            if(furi_hal_speaker_worker_state.terminate ||
               !furi_hal_speaker_worker_state.active ||
               (furi_hal_speaker_worker_state.half_period_us != half_period_us)) {
                break;
            }
        }
    }

    furi_hal_speaker_pin_stop();

    return 0;
}

void furi_hal_speaker_init(void) {
    furi_assert(furi_hal_speaker_mutex == NULL);

    furi_hal_speaker_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_hal_speaker_worker_state.terminate = false;
    furi_hal_speaker_worker_state.active = false;
    furi_hal_speaker_worker_state.half_period_us = FURI_HAL_SPEAKER_DEFAULT_HALF_PERIOD_US;
    furi_hal_speaker_pcf_ready = false;
    furi_hal_speaker_pin_stop();

    furi_hal_speaker_worker_state.thread = furi_thread_alloc();
    furi_thread_set_name(furi_hal_speaker_worker_state.thread, "SpeakerWorker");
    furi_thread_set_stack_size(
        furi_hal_speaker_worker_state.thread, FURI_HAL_SPEAKER_THREAD_STACK_SIZE);
    furi_thread_set_context(furi_hal_speaker_worker_state.thread, NULL);
    furi_thread_set_callback(furi_hal_speaker_worker_state.thread, furi_hal_speaker_worker);
    furi_thread_start(furi_hal_speaker_worker_state.thread);

    FURI_LOG_I(TAG, "Init OK");
}

void furi_hal_speaker_deinit(void) {
    furi_check(furi_hal_speaker_mutex != NULL);

    furi_hal_speaker_worker_state.active = false;
    furi_hal_speaker_worker_state.terminate = true;

    if(furi_hal_speaker_worker_state.thread) {
        furi_thread_join(furi_hal_speaker_worker_state.thread);
        furi_thread_free(furi_hal_speaker_worker_state.thread);
        furi_hal_speaker_worker_state.thread = NULL;
    }

    furi_hal_speaker_worker_state.half_period_us = FURI_HAL_SPEAKER_DEFAULT_HALF_PERIOD_US;
    furi_hal_speaker_pin_stop();
    furi_mutex_free(furi_hal_speaker_mutex);
    furi_hal_speaker_mutex = NULL;
}

bool furi_hal_speaker_acquire(uint32_t timeout) {
    furi_check(!FURI_IS_IRQ_MODE());

    if(furi_mutex_acquire(furi_hal_speaker_mutex, timeout) == FuriStatusOk) {
        furi_hal_power_insomnia_enter();
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

    if(volume <= 0) {
        furi_hal_speaker_stop();
        return;
    }

    furi_hal_speaker_worker_state.half_period_us =
        furi_hal_speaker_frequency_to_half_period(frequency);
    furi_hal_speaker_worker_state.active = true;
}

void furi_hal_speaker_set_volume(float volume) {
    furi_check(furi_hal_speaker_is_mine());

    furi_hal_speaker_worker_state.active = (volume > 0);
}

void furi_hal_speaker_stop(void) {
    furi_check(furi_hal_speaker_is_mine());

    furi_hal_speaker_worker_state.active = false;
}
