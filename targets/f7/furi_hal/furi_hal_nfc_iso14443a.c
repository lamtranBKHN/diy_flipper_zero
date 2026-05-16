#include "furi_hal_nfc_i.h"
#include "furi_hal_nfc_pn532.h"
#include "furi_hal_nfc_tech_i.h"

#include <furi.h>
#include <furi_hal_resources.h>

#include <digital_signal/presets/nfc/iso14443_3a_signal.h>

#define TAG "FuriHalIso14443a"

// Prevent FDT timer from starting
#define FURI_HAL_NFC_ISO14443A_LISTENER_FDT_COMP_FC (INT32_MAX)

static Iso14443_3aSignal* iso14443_3a_signal = NULL;

static FuriHalNfcError furi_hal_nfc_iso14443a_poller_init(const FuriHalSpiBusHandle* handle) {
    if(furi_hal_nfc_pn532_is_active()) {
        UNUSED(handle);
        return FuriHalNfcErrorNone;
    }
    return FuriHalNfcErrorNone;
}

static FuriHalNfcError furi_hal_nfc_iso14443a_poller_deinit(const FuriHalSpiBusHandle* handle) {
    if(furi_hal_nfc_pn532_is_active()) {
        UNUSED(handle);
        return FuriHalNfcErrorNone;
    }
    return FuriHalNfcErrorNone;
}

static FuriHalNfcError furi_hal_nfc_iso14443a_listener_init(const FuriHalSpiBusHandle* handle) {
    if(furi_hal_nfc_pn532_is_active()) {
        UNUSED(handle);
        return FuriHalNfcErrorNone;
    }
    furi_check(iso14443_3a_signal == NULL);
    iso14443_3a_signal = iso14443_3a_signal_alloc(&gpio_spi_mosi);
    UNUSED(handle);
    return FuriHalNfcErrorNone;
}

static FuriHalNfcError furi_hal_nfc_iso14443a_listener_deinit(const FuriHalSpiBusHandle* handle) {
    UNUSED(handle);

    if(iso14443_3a_signal) {
        iso14443_3a_signal_free(iso14443_3a_signal);
        iso14443_3a_signal = NULL;
    }

    return FuriHalNfcErrorNone;
}

static FuriHalNfcEvent furi_hal_nfc_iso14443_3a_listener_wait_event(uint32_t timeout_ms) {
    UNUSED(timeout_ms);
    return FuriHalNfcEventTimeout;
}

FuriHalNfcError furi_hal_nfc_iso14443a_poller_trx_short_frame(FuriHalNfcaShortFrame frame) {
    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_trx_short_frame(frame);
    }
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_iso14443a_tx_sdd_frame(const uint8_t* tx_data, size_t tx_bits) {
    FuriHalNfcError error = FuriHalNfcErrorNone;
    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_tx(tx_data, tx_bits);
    }
    // No anticollision is supported in this version of library
    error = furi_hal_nfc_poller_tx(tx_data, tx_bits);

    return error;
}

FuriHalNfcError
    furi_hal_nfc_iso14443a_rx_sdd_frame(uint8_t* rx_data, size_t rx_data_size, size_t* rx_bits) {
    FuriHalNfcError error = FuriHalNfcErrorNone;

    error = furi_hal_nfc_poller_rx(rx_data, rx_data_size, rx_bits);
    // No anticollision is supported in this version of library

    return error;
}

FuriHalNfcError
    furi_hal_nfc_iso14443a_poller_tx_custom_parity(const uint8_t* tx_data, size_t tx_bits) {
    furi_check(tx_data);

    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_tx_custom_parity(tx_data, tx_bits);
    }

    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_iso14443a_listener_set_col_res_data(
    uint8_t* uid,
    uint8_t uid_len,
    uint8_t* atqa,
    uint8_t sak) {
    if(furi_hal_nfc_pn532_is_active()) {
        UNUSED(uid);
        UNUSED(uid_len);
        UNUSED(atqa);
        UNUSED(sak);
        return FuriHalNfcErrorNone;
    }
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_iso14443a_listener_tx(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* tx_data,
    size_t tx_bits) {
    UNUSED(handle);
    UNUSED(tx_data);
    UNUSED(tx_bits);
    return FuriHalNfcErrorCommunication;
}

FuriHalNfcError furi_hal_nfc_iso14443a_listener_tx_custom_parity(
    const uint8_t* tx_data,
    const uint8_t* tx_parity,
    size_t tx_bits) {
    UNUSED(tx_parity);
    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_listener_tx(tx_data, tx_bits);
    }
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_iso14443_3a_listener_sleep(const FuriHalSpiBusHandle* handle) {
    UNUSED(handle);
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_iso14443_3a_listener_idle(const FuriHalSpiBusHandle* handle) {
    UNUSED(handle);
    return FuriHalNfcErrorNone;
}

const FuriHalNfcTechBase furi_hal_nfc_iso14443a = {
    .poller =
        {
            .compensation =
                {
                    .fdt = FURI_HAL_NFC_POLLER_FDT_COMP_FC,
                    .fwt = FURI_HAL_NFC_POLLER_FWT_COMP_FC,
                },
            .init = furi_hal_nfc_iso14443a_poller_init,
            .deinit = furi_hal_nfc_iso14443a_poller_deinit,
            .wait_event = furi_hal_nfc_wait_event_common,
            .tx = furi_hal_nfc_poller_tx_common,
            .rx = furi_hal_nfc_common_fifo_rx,
        },

    .listener =
        {
            .compensation =
                {
                    .fdt = FURI_HAL_NFC_ISO14443A_LISTENER_FDT_COMP_FC,
                },
            .init = furi_hal_nfc_iso14443a_listener_init,
            .deinit = furi_hal_nfc_iso14443a_listener_deinit,
            .wait_event = furi_hal_nfc_iso14443_3a_listener_wait_event,
            .tx = furi_hal_nfc_iso14443a_listener_tx,
            .rx = furi_hal_nfc_common_fifo_rx,
            .sleep = furi_hal_nfc_iso14443_3a_listener_sleep,
            .idle = furi_hal_nfc_iso14443_3a_listener_idle,
        },
};
