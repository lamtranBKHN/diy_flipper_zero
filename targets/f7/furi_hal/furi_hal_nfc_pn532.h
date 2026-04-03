#pragma once

#include "furi_hal_nfc_i.h"

#ifdef __cplusplus
extern "C" {
#endif

bool furi_hal_nfc_pn532_backend_init(void);
bool furi_hal_nfc_pn532_is_active(void);
void furi_hal_nfc_pn532_reset(void);

FuriHalNfcError furi_hal_nfc_pn532_set_mode(FuriHalNfcMode mode, FuriHalNfcTech tech);
FuriHalNfcError furi_hal_nfc_pn532_low_power_mode_start(void);
FuriHalNfcError furi_hal_nfc_pn532_low_power_mode_stop(void);
FuriHalNfcError furi_hal_nfc_pn532_field_detect_start(void);
FuriHalNfcError furi_hal_nfc_pn532_field_detect_stop(void);
bool furi_hal_nfc_pn532_field_is_present(void);
FuriHalNfcError furi_hal_nfc_pn532_poller_field_on(void);
FuriHalNfcEvent furi_hal_nfc_pn532_wait_event(uint32_t timeout_ms);
FuriHalNfcError furi_hal_nfc_pn532_trx_reset(void);
FuriHalNfcError furi_hal_nfc_pn532_trx_short_frame(FuriHalNfcaShortFrame frame);
FuriHalNfcError furi_hal_nfc_pn532_tx(const uint8_t* tx_data, size_t tx_bits);
FuriHalNfcError furi_hal_nfc_pn532_tx_custom_parity(const uint8_t* tx_data, size_t tx_bits);
FuriHalNfcError furi_hal_nfc_pn532_rx(uint8_t* rx_data, size_t rx_data_size, size_t* rx_bits);

#ifdef __cplusplus
}
#endif
