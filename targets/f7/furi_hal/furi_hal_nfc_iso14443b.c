#include "furi_hal_nfc_i.h"
#include "furi_hal_nfc_pn532.h"
#include "furi_hal_nfc_tech_i.h"

static FuriHalNfcError furi_hal_nfc_iso14443b_poller_init(const FuriHalSpiBusHandle* handle) {
    if(furi_hal_nfc_pn532_is_active()) {
        UNUSED(handle);
        return FuriHalNfcErrorNone;
    }
    return FuriHalNfcErrorCommunication;
}

static FuriHalNfcError furi_hal_nfc_iso14443b_poller_deinit(const FuriHalSpiBusHandle* handle) {
    UNUSED(handle);
    return FuriHalNfcErrorNone;
}

const FuriHalNfcTechBase furi_hal_nfc_iso14443b = {
    .poller =
        {
            .compensation =
                {
                    .fdt = FURI_HAL_NFC_POLLER_FDT_COMP_FC,
                    .fwt = FURI_HAL_NFC_POLLER_FWT_COMP_FC,
                },
            .init = furi_hal_nfc_iso14443b_poller_init,
            .deinit = furi_hal_nfc_iso14443b_poller_deinit,
            .wait_event = furi_hal_nfc_wait_event_common,
            .tx = furi_hal_nfc_poller_tx_common,
            .rx = furi_hal_nfc_common_fifo_rx,
        },

    .listener = {},
};
