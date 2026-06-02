#include "furi_hal_nfc_i.h"
#include "furi_hal_nfc_pn532.h"
#include "furi_hal_nfc_tech_i.h"
#include "furi_hal_pn532.h"

#include <furi_hal_i2c.h>
#include <furi.h>

#define TAG "FuriHalNfc"

const FuriHalNfcTechBase* const furi_hal_nfc_tech[FuriHalNfcTechNum] = {
    [FuriHalNfcTechIso14443a] = &furi_hal_nfc_iso14443a,
    [FuriHalNfcTechIso14443b] = &furi_hal_nfc_iso14443b,
    [FuriHalNfcTechIso15693] = &furi_hal_nfc_iso15693,
    [FuriHalNfcTechFelica] = &furi_hal_nfc_felica,
    // Add new technologies here
};

FuriHalNfc furi_hal_nfc;

// This helper is now public (not static) so it can be called from the ISR file.
void furi_hal_nfc_log_irq(const char* action, uint32_t irq_mask) {
    UNUSED(action);
    UNUSED(irq_mask);
}

FuriHalNfcError furi_hal_nfc_is_hal_ready(void) {
    FURI_LOG_I(TAG, "Checking if HAL is ready [PN532-R2]");

    // Try PN532 backend first
    if(furi_hal_nfc_pn532_is_active()) {
        FURI_LOG_I(TAG, "HAL ready via PN532 backend (already active)");
        if(furi_hal_nfc_event == NULL) {
            furi_hal_nfc_event_init();
        }
        return FuriHalNfcErrorNone;
    }

    FURI_LOG_I(TAG, "PN532 backend init begin");
    if(furi_hal_nfc_pn532_backend_init()) {
        if(furi_hal_nfc.mutex == NULL) {
            furi_hal_nfc.mutex = furi_mutex_alloc(FuriMutexTypeNormal);
        }
        if(furi_hal_nfc_event == NULL) {
            furi_hal_nfc_event_init();
        }
        FURI_LOG_I(TAG, "HAL ready via PN532 backend");
        return FuriHalNfcErrorNone;
    }

    FURI_LOG_W(TAG, "PN532 backend init failed");
    FURI_LOG_E(TAG, "PN532 backend not available");
    return FuriHalNfcErrorCommunication;
}

FuriHalNfcError furi_hal_nfc_init(void) {
    if(furi_hal_nfc_pn532_backend_init()) {
        if(furi_hal_nfc.mutex == NULL) {
            furi_hal_nfc.mutex = furi_mutex_alloc(FuriMutexTypeNormal);
        }
        // Event system needed even in PN532 mode — nfc.c worker calls
        // furi_hal_nfc_event_start() which asserts furi_hal_nfc_event != NULL
        if(furi_hal_nfc_event == NULL) {
            furi_hal_nfc_event_init();
        }
        FURI_LOG_I(TAG, "Initializing Furi HAL NFC with PN532 backend");
        return FuriHalNfcErrorNone;
    }

    FURI_LOG_E(TAG, "PN532 backend init failed");
    return FuriHalNfcErrorCommunication;
}

static bool furi_hal_nfc_is_mine(void) {
    return furi_mutex_get_owner(furi_hal_nfc.mutex) == furi_thread_get_current_id();
}

FuriHalNfcError furi_hal_nfc_acquire(void) {
    if(!furi_hal_nfc.mutex) {
        FURI_LOG_W(TAG, "Cannot acquire NFC: mutex not initialized");
        return FuriHalNfcErrorBusy;
    }
    FURI_LOG_T(TAG, "Acquiring NFC");

    /* Diagnostic: identify mutex owner when acquire fails */
    FuriHalNfcError error = FuriHalNfcErrorNone;
    if(furi_mutex_acquire(furi_hal_nfc.mutex, 100) != FuriStatusOk) {
        FuriThreadId owner = furi_mutex_get_owner(furi_hal_nfc.mutex);
        FuriThreadId self = furi_thread_get_current_id();
        FURI_LOG_D(
            TAG,
            "Failed to acquire mutex, NFC busy (owner=0x%08lX, self=0x%08lX)",
            (uint32_t)(uintptr_t)owner,
            (uint32_t)(uintptr_t)self);
        error = FuriHalNfcErrorBusy;
    }

    return error;
}

FuriHalNfcError furi_hal_nfc_release(void) {
    if(!furi_hal_nfc.mutex) {
        FURI_LOG_W(TAG, "Cannot release NFC: mutex not initialized");
        return FuriHalNfcErrorBusy;
    }
    if(!furi_hal_nfc_is_mine()) {
        FURI_LOG_W(TAG, "Cannot release NFC: not the mutex owner");
        return FuriHalNfcErrorBusy;
    }
    FURI_LOG_T(TAG, "Releasing NFC");
    if(furi_mutex_release(furi_hal_nfc.mutex) != FuriStatusOk) {
        FURI_LOG_W(TAG, "Failed to release NFC mutex");
        return FuriHalNfcErrorBusy;
    }

    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_low_power_mode_start(void) {
    if(furi_hal_nfc_acquire() != FuriHalNfcErrorNone) {
        return FuriHalNfcErrorBusy;
    }

    FuriHalNfcError error = FuriHalNfcErrorCommunication;
    if(furi_hal_nfc_pn532_is_active()) {
        error = furi_hal_nfc_pn532_low_power_mode_start();
    }

    furi_hal_nfc_release();
    return error;
}

FuriHalNfcError furi_hal_nfc_low_power_mode_stop(void) {
    if(furi_hal_nfc_acquire() != FuriHalNfcErrorNone) {
        return FuriHalNfcErrorBusy;
    }

    FuriHalNfcError error = FuriHalNfcErrorCommunication;
    if(furi_hal_nfc_pn532_is_active()) {
        error = furi_hal_nfc_pn532_low_power_mode_stop();
    }

    furi_hal_nfc_release();
    return error;
}

FuriHalNfcError furi_hal_nfc_set_mode(FuriHalNfcMode mode, FuriHalNfcTech tech) {
    furi_check(mode < FuriHalNfcModeNum);
    furi_check(tech < FuriHalNfcTechNum);
    FURI_LOG_I(TAG, "Setting mode: %d, tech: %d", mode, tech);

    if(furi_hal_nfc_acquire() != FuriHalNfcErrorNone) {
        return FuriHalNfcErrorBusy;
    }

    FuriHalNfcError error = FuriHalNfcErrorCommunication;
    if(furi_hal_nfc_pn532_is_active()) {
        error = furi_hal_nfc_pn532_set_mode(mode, tech);
        if(error == FuriHalNfcErrorNone) {
            furi_hal_nfc.mode = mode;
            furi_hal_nfc.tech = tech;
        }
        goto release;
    }

    furi_hal_nfc_pn532_backend_init();
    if(furi_hal_nfc_pn532_is_active()) {
        error = furi_hal_nfc_pn532_set_mode(mode, tech);
        if(error == FuriHalNfcErrorNone) {
            furi_hal_nfc.mode = mode;
            furi_hal_nfc.tech = tech;
        }
        goto release;
    }

release:
    furi_hal_nfc_release();
    return error;
}

FuriHalNfcError furi_hal_nfc_reset_mode(void) {
    if(furi_hal_nfc_acquire() != FuriHalNfcErrorNone) {
        return FuriHalNfcErrorBusy;
    }

    FuriHalNfcError error = FuriHalNfcErrorCommunication;
    if(furi_hal_nfc_pn532_is_active()) {
        /* Lightweight reset: clear mode/tech/volatile state but preserve
         * target activation cache. Full reset (InRelease + cache wipe)
         * happens in set_mode() on tech change or low_power_mode_start()
         * when RF turns off. */
        furi_hal_nfc_pn532_reset_keep_target();
        furi_hal_nfc_event_stop();
        error = FuriHalNfcErrorNone;
    }

    furi_hal_nfc_release();
    return error;
}

FuriHalNfcError furi_hal_nfc_field_detect_start(void) {
    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_field_detect_start();
    }
    return FuriHalNfcErrorCommunication;
}

FuriHalNfcError furi_hal_nfc_field_detect_stop(void) {
    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_field_detect_stop();
    }
    return FuriHalNfcErrorCommunication;
}

bool furi_hal_nfc_field_is_present(void) {
    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_field_is_present();
    }
    return false;
}

FuriHalNfcError furi_hal_nfc_poller_field_on(void) {
    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_poller_field_on();
    }
    return FuriHalNfcErrorCommunication;
}

FuriHalNfcError furi_hal_nfc_poller_tx_common(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* tx_data,
    size_t tx_bits) {
    if(furi_hal_nfc_pn532_is_active()) {
        UNUSED(handle);
        UNUSED(tx_data);
        UNUSED(tx_bits);
        return FuriHalNfcErrorCommunication;
    }
    UNUSED(handle);
    UNUSED(tx_data);
    UNUSED(tx_bits);
    return FuriHalNfcErrorCommunication;
}

FuriHalNfcError furi_hal_nfc_common_fifo_tx(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* tx_data,
    size_t tx_bits) {
    UNUSED(handle);
    UNUSED(tx_data);
    UNUSED(tx_bits);
    return FuriHalNfcErrorCommunication;
}

FuriHalNfcError furi_hal_nfc_poller_tx(const uint8_t* tx_data, size_t tx_bits) {
    furi_check(furi_hal_nfc.mode == FuriHalNfcModePoller);
    furi_check(furi_hal_nfc.tech < FuriHalNfcTechNum);
    FURI_LOG_T(TAG, "Poller TX for tech %d", furi_hal_nfc.tech);
    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_tx(tx_data, tx_bits);
    }
    return furi_hal_nfc_tech[furi_hal_nfc.tech]->poller.tx(
        &furi_hal_spi_bus_handle_nfc, tx_data, tx_bits);
}

FuriHalNfcError furi_hal_nfc_poller_rx(uint8_t* rx_data, size_t rx_data_size, size_t* rx_bits) {
    furi_check(furi_hal_nfc.mode == FuriHalNfcModePoller);
    furi_check(furi_hal_nfc.tech < FuriHalNfcTechNum);
    FURI_LOG_T(TAG, "Poller RX for tech %d", furi_hal_nfc.tech);
    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_rx(rx_data, rx_data_size, rx_bits);
    }
    return furi_hal_nfc_tech[furi_hal_nfc.tech]->poller.rx(
        &furi_hal_spi_bus_handle_nfc, rx_data, rx_data_size, rx_bits);
}

FuriHalNfcEvent furi_hal_nfc_poller_wait_event(uint32_t timeout_ms) {
    furi_check(furi_hal_nfc.mode == FuriHalNfcModePoller);
    furi_check(furi_hal_nfc.tech < FuriHalNfcTechNum);
    FURI_LOG_T(TAG, "Poller wait event for tech %d", furi_hal_nfc.tech);

    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_wait_event(timeout_ms);
    }

    return furi_hal_nfc_tech[furi_hal_nfc.tech]->poller.wait_event(timeout_ms);
}

FuriHalNfcEvent furi_hal_nfc_listener_wait_event(uint32_t timeout_ms) {
    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_listener_wait_event(timeout_ms);
    }
    furi_check(furi_hal_nfc.mode == FuriHalNfcModeListener);
    furi_check(furi_hal_nfc.tech < FuriHalNfcTechNum);
    FURI_LOG_T(TAG, "Listener wait event for tech %d", furi_hal_nfc.tech);

    return furi_hal_nfc_tech[furi_hal_nfc.tech]->listener.wait_event(timeout_ms);
}

FuriHalNfcError furi_hal_nfc_listener_tx(const uint8_t* tx_data, size_t tx_bits) {
    furi_check(tx_data);

    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_listener_tx(tx_data, tx_bits);
    }

    furi_check(furi_hal_nfc.mode == FuriHalNfcModeListener);
    furi_check(furi_hal_nfc.tech < FuriHalNfcTechNum);
    FURI_LOG_T(TAG, "Listener TX for tech %d", furi_hal_nfc.tech);

    return furi_hal_nfc_tech[furi_hal_nfc.tech]->listener.tx(
        &furi_hal_spi_bus_handle_nfc, tx_data, tx_bits);
}

FuriHalNfcError furi_hal_nfc_common_fifo_rx(
    const FuriHalSpiBusHandle* handle,
    uint8_t* rx_data,
    size_t rx_data_size,
    size_t* rx_bits) {
    if(furi_hal_nfc_pn532_is_active()) {
        UNUSED(handle);
        UNUSED(rx_data);
        UNUSED(rx_data_size);
        UNUSED(rx_bits);
        return FuriHalNfcErrorCommunication;
    }
    UNUSED(handle);
    UNUSED(rx_data);
    UNUSED(rx_data_size);
    UNUSED(rx_bits);
    return FuriHalNfcErrorCommunication;
}

FuriHalNfcError furi_hal_nfc_listener_rx(uint8_t* rx_data, size_t rx_data_size, size_t* rx_bits) {
    furi_check(rx_data);
    furi_check(rx_bits);

    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_listener_rx(rx_data, rx_data_size, rx_bits);
    }

    furi_check(furi_hal_nfc.mode == FuriHalNfcModeListener);
    furi_check(furi_hal_nfc.tech < FuriHalNfcTechNum);
    FURI_LOG_T(TAG, "Listener RX for tech %d", furi_hal_nfc.tech);

    return furi_hal_nfc_tech[furi_hal_nfc.tech]->listener.rx(
        &furi_hal_spi_bus_handle_nfc, rx_data, rx_data_size, rx_bits);
}

FuriHalNfcError furi_hal_nfc_trx_reset(void) {
    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_trx_reset();
    }
    return FuriHalNfcErrorCommunication;
}

FuriHalNfcError furi_hal_nfc_listener_sleep(void) {
    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_listener_sleep();
    }
    furi_check(furi_hal_nfc.mode == FuriHalNfcModeListener);
    furi_check(furi_hal_nfc.tech < FuriHalNfcTechNum);
    FURI_LOG_I(TAG, "Listener entering sleep state for tech %d", furi_hal_nfc.tech);
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;
    return furi_hal_nfc_tech[furi_hal_nfc.tech]->listener.sleep(handle);
}

FuriHalNfcError furi_hal_nfc_listener_idle(void) {
    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_listener_idle();
    }
    furi_check(furi_hal_nfc.mode == FuriHalNfcModeListener);
    furi_check(furi_hal_nfc.tech < FuriHalNfcTechNum);
    FURI_LOG_I(TAG, "Listener entering idle state for tech %d", furi_hal_nfc.tech);
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;
    return furi_hal_nfc_tech[furi_hal_nfc.tech]->listener.idle(handle);
}

FuriHalNfcError furi_hal_nfc_listener_enable_rx(void) {
    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_listener_enable_rx();
    }
    return FuriHalNfcErrorCommunication;
}

void furi_hal_nfc_mf_auth_key_store(const uint8_t* key, uint8_t key_type) {
    if(furi_hal_nfc_pn532_is_active()) {
        furi_hal_nfc_pn532_mf_key_store(key, key_type);
    }
}

void furi_hal_nfc_emu_set_ndef(const uint8_t* msg, size_t len) {
    if(furi_hal_nfc_pn532_is_active()) {
        furi_hal_nfc_pn532_emu_set_ndef(msg, len);
    }
}

void furi_hal_nfc_set_exchange_deadline(uint32_t deadline_tick) {
    furi_hal_pn532_set_exchange_deadline(deadline_tick);
}

void furi_hal_nfc_clear_exchange_deadline(void) {
    furi_hal_pn532_clear_exchange_deadline();
}

void furi_hal_nfc_release_active_target(void) {
    furi_hal_nfc_pn532_release_if_listed();
}

FuriHalNfcError furi_hal_nfc_common_listener_rx_start(const FuriHalSpiBusHandle* handle) {
    /* Stub: ST25R3916-specific listener RX start.
     * On the UBYTE/PN532 board the ST25R3916 chip is absent; listener mode
     * is not yet implemented for PN532.  Return success so the SDK API
     * symbol resolves without pulling in missing ST25R3916 register calls.
     */
    UNUSED(handle);
    return FuriHalNfcErrorNone;
}
