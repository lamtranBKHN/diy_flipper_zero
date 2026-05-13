#include "furi_hal_nfc_i.h"
#include "furi_hal_nfc_pn532.h"
#include "furi_hal_nfc_tech_i.h"

#include <digital_signal/presets/nfc/iso15693_signal.h>
#include <signal_reader/parsers/iso15693/iso15693_parser.h>

#include <furi_hal_resources.h>

#define FURI_HAL_NFC_ISO15693_MAX_FRAME_SIZE         (1024U)
#define FURI_HAL_NFC_ISO15693_POLLER_MAX_BUFFER_SIZE (64)

#define FURI_HAL_NFC_ISO15693_RESP_SOF_SIZE    (5)
#define FURI_HAL_NFC_ISO15693_RESP_EOF_SIZE    (5)
#define FURI_HAL_NFC_ISO15693_RESP_SOF_MASK    (0x1FU)
#define FURI_HAL_NFC_ISO15693_RESP_SOF_PATTERN (0x17U)
#define FURI_HAL_NFC_ISO15693_RESP_EOF_PATTERN (0x1DU)

#define FURI_HAL_NFC_ISO15693_RESP_PATTERN_MASK (0x03U)
#define FURI_HAL_NFC_ISO15693_RESP_PATTERN_0    (0x01U)
#define FURI_HAL_NFC_ISO15693_RESP_PATTERN_1    (0x02U)

// Derived experimentally
#define FURI_HAL_NFC_ISO15693_POLLER_FWT_COMP_FC   (-1300)
#define FURI_HAL_NFC_ISO15693_LISTENER_FDT_COMP_FC (2850)

#define BITS_IN_BYTE (8U)

#define TAG "FuriHalIso15693"

static FuriHalNfcError furi_hal_nfc_iso15693_poller_init(const FuriHalSpiBusHandle* handle) {
    UNUSED(handle);
    return FuriHalNfcErrorCommunication;
}

static FuriHalNfcError furi_hal_nfc_iso15693_poller_deinit(const FuriHalSpiBusHandle* handle) {
    UNUSED(handle);
    return FuriHalNfcErrorCommunication;
}

static FuriHalNfcError furi_hal_nfc_iso15693_poller_tx(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* tx_data,
    size_t tx_bits) {
    UNUSED(handle);
    UNUSED(tx_data);
    UNUSED(tx_bits);
    return FuriHalNfcErrorCommunication;
}

static FuriHalNfcError furi_hal_nfc_iso15693_poller_rx(
    const FuriHalSpiBusHandle* handle,
    uint8_t* rx_data,
    size_t rx_data_size,
    size_t* rx_bits) {
    UNUSED(handle);
    UNUSED(rx_data);
    UNUSED(rx_data_size);
    UNUSED(rx_bits);
    return FuriHalNfcErrorCommunication;
}

static FuriHalNfcEvent furi_hal_nfc_iso15693_wait_event(uint32_t timeout_ms) {
    UNUSED(timeout_ms);
    return 0;
}

static FuriHalNfcError furi_hal_nfc_iso15693_listener_init(const FuriHalSpiBusHandle* handle) {
    UNUSED(handle);
    return FuriHalNfcErrorCommunication;
}

static FuriHalNfcError furi_hal_nfc_iso15693_listener_deinit(const FuriHalSpiBusHandle* handle) {
    UNUSED(handle);
    return FuriHalNfcErrorCommunication;
}

static FuriHalNfcError furi_hal_nfc_iso15693_listener_tx(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* tx_data,
    size_t tx_bits) {
    UNUSED(handle);
    UNUSED(tx_data);
    UNUSED(tx_bits);
    return FuriHalNfcErrorCommunication;
}

static FuriHalNfcError furi_hal_nfc_iso15693_listener_rx(
    const FuriHalSpiBusHandle* handle,
    uint8_t* rx_data,
    size_t rx_data_size,
    size_t* rx_bits) {
    UNUSED(handle);
    UNUSED(rx_data);
    UNUSED(rx_data_size);
    UNUSED(rx_bits);
    return FuriHalNfcErrorCommunication;
}

FuriHalNfcError furi_hal_nfc_iso15693_listener_sleep(const FuriHalSpiBusHandle* handle) {
    UNUSED(handle);
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_iso15693_listener_tx_sof(void) {
    return FuriHalNfcErrorCommunication;
}

const FuriHalNfcTechBase furi_hal_nfc_iso15693 = {
    .poller =
        {
            .compensation =
                {
                    .fdt = FURI_HAL_NFC_POLLER_FDT_COMP_FC,
                    .fwt = FURI_HAL_NFC_ISO15693_POLLER_FWT_COMP_FC,
                },
            .init = furi_hal_nfc_iso15693_poller_init,
            .deinit = furi_hal_nfc_iso15693_poller_deinit,
            .wait_event = furi_hal_nfc_wait_event_common,
            .tx = furi_hal_nfc_iso15693_poller_tx,
            .rx = furi_hal_nfc_iso15693_poller_rx,
        },

    .listener =
        {
            .compensation =
                {
                    .fdt = FURI_HAL_NFC_ISO15693_LISTENER_FDT_COMP_FC,
                },
            .init = furi_hal_nfc_iso15693_listener_init,
            .deinit = furi_hal_nfc_iso15693_listener_deinit,
            .wait_event = furi_hal_nfc_iso15693_wait_event,
            .tx = furi_hal_nfc_iso15693_listener_tx,
            .rx = furi_hal_nfc_iso15693_listener_rx,
            .sleep = furi_hal_nfc_iso15693_listener_sleep,
            .idle = furi_hal_nfc_iso15693_listener_sleep,
        },
};
