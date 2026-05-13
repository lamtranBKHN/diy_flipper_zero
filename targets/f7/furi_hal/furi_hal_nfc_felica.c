#include "furi_hal_nfc_i.h"
#include "furi_hal_nfc_pn532.h"
#include "furi_hal_nfc_tech_i.h"

#define FURI_HAL_NFC_FELICA_LISTENER_FDT_COMP_FC (0)

#define FURI_HAL_FELICA_COMMUNICATION_PERFORMANCE (0x0083U)
#define FURI_HAL_FELICA_RESPONSE_CODE             (0x01)
#define FURI_HAL_FELICA_IDM_PMM_LENGTH            (8)

#pragma pack(push, 1)
typedef struct {
    uint16_t system_code;
    uint8_t response_code;
    uint8_t Idm[FURI_HAL_FELICA_IDM_PMM_LENGTH];
    uint8_t Pmm[FURI_HAL_FELICA_IDM_PMM_LENGTH];
    uint16_t communication_performance;
} FuriHalFelicaPtMemory;
#pragma pack(pop)

static FuriHalNfcError furi_hal_nfc_felica_common_init(const FuriHalSpiBusHandle* handle) {
    UNUSED(handle);
    return FuriHalNfcErrorNone;
}

static FuriHalNfcError furi_hal_nfc_felica_poller_init(const FuriHalSpiBusHandle* handle) {
    if(furi_hal_nfc_pn532_is_active()) {
        UNUSED(handle);
        return FuriHalNfcErrorNone;
    }

    return furi_hal_nfc_felica_common_init(handle);
}

static FuriHalNfcError furi_hal_nfc_felica_poller_deinit(const FuriHalSpiBusHandle* handle) {
    UNUSED(handle);

    return FuriHalNfcErrorNone;
}

static FuriHalNfcError furi_hal_nfc_felica_listener_init(const FuriHalSpiBusHandle* handle) {
    if(furi_hal_nfc_pn532_is_active()) {
        UNUSED(handle);
        return FuriHalNfcErrorCommunication;
    }

    return FuriHalNfcErrorCommunication;
}

static FuriHalNfcError furi_hal_nfc_felica_listener_deinit(const FuriHalSpiBusHandle* handle) {
    UNUSED(handle);
    return FuriHalNfcErrorNone;
}

static FuriHalNfcEvent furi_hal_nfc_felica_listener_wait_event(uint32_t timeout_ms) {
    UNUSED(timeout_ms);
    FuriHalNfcEvent event = furi_hal_nfc_wait_event_common(timeout_ms);

    return event;
}

FuriHalNfcError furi_hal_nfc_felica_listener_tx(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* tx_data,
    size_t tx_bits) {
    furi_hal_nfc_common_fifo_tx(handle, tx_data, tx_bits);
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_felica_listener_sleep(const FuriHalSpiBusHandle* handle) {
    UNUSED(handle);
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_felica_listener_idle(const FuriHalSpiBusHandle* handle) {
    UNUSED(handle);
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_felica_listener_set_sensf_res_data(
    const uint8_t* idm,
    const uint8_t idm_len,
    const uint8_t* pmm,
    const uint8_t pmm_len,
    const uint16_t sys_code) {
    furi_check(idm);
    furi_check(pmm);
    furi_check(idm_len == FURI_HAL_FELICA_IDM_PMM_LENGTH);
    furi_check(pmm_len == FURI_HAL_FELICA_IDM_PMM_LENGTH);

    UNUSED(idm);
    UNUSED(idm_len);
    UNUSED(pmm);
    UNUSED(pmm_len);
    UNUSED(sys_code);
    return FuriHalNfcErrorCommunication;
}

const FuriHalNfcTechBase furi_hal_nfc_felica = {
    .poller =
        {
            .compensation =
                {
                    .fdt = FURI_HAL_NFC_POLLER_FDT_COMP_FC,
                    .fwt = FURI_HAL_NFC_POLLER_FWT_COMP_FC,
                },
            .init = furi_hal_nfc_felica_poller_init,
            .deinit = furi_hal_nfc_felica_poller_deinit,
            .wait_event = furi_hal_nfc_wait_event_common,
            .tx = furi_hal_nfc_poller_tx_common,
            .rx = furi_hal_nfc_common_fifo_rx,
        },

    .listener =
        {
            .compensation =
                {
                    .fdt = FURI_HAL_NFC_FELICA_LISTENER_FDT_COMP_FC,
                },
            .init = furi_hal_nfc_felica_listener_init,
            .deinit = furi_hal_nfc_felica_listener_deinit,
            .wait_event = furi_hal_nfc_felica_listener_wait_event,
            .tx = furi_hal_nfc_felica_listener_tx,
            .rx = furi_hal_nfc_common_fifo_rx,
            .sleep = furi_hal_nfc_felica_listener_sleep,
            .idle = furi_hal_nfc_felica_listener_idle,
        },
};
