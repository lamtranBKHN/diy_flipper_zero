#pragma once

#include "furi_hal_nfc_i.h"
#include "furi_hal_pn532.h"

#ifdef __cplusplus
extern "C" {
#endif

bool furi_hal_nfc_pn532_backend_init(void);
bool furi_hal_nfc_pn532_is_active(void);
void furi_hal_nfc_pn532_reset(void);
void furi_hal_nfc_pn532_reset_keep_target(void);
void furi_hal_nfc_pn532_queue_reset(void);

typedef enum {
    FuriHalNfcPn532ResultDetected,
    FuriHalNfcPn532ResultNotPresent,
    FuriHalNfcPn532ResultUnsupportedByPn532,
    FuriHalNfcPn532ResultCommunicationError,
    FuriHalNfcPn532ResultParseError,
} FuriHalNfcPn532Result;

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

// Listener (target) mode functions
FuriHalNfcEvent furi_hal_nfc_pn532_listener_wait_event(uint32_t timeout_ms);
FuriHalNfcError furi_hal_nfc_pn532_listener_tx(const uint8_t* tx_data, size_t tx_bits);
FuriHalNfcError
    furi_hal_nfc_pn532_listener_rx(uint8_t* rx_data, size_t rx_data_size, size_t* rx_bits);
FuriHalNfcError furi_hal_nfc_pn532_listener_idle(void);
FuriHalNfcError furi_hal_nfc_pn532_listener_sleep(void);
FuriHalNfcError furi_hal_nfc_pn532_listener_enable_rx(void);
FuriHalNfcError furi_hal_nfc_pn532_listener_set_col_res_data(
    const uint8_t* uid,
    uint8_t uid_len,
    const uint8_t* atqa,
    uint8_t sak);

/** Configure the NFCID2t for FeliCa listener (emulation) mode.
 *
 * Must be called before furi_hal_nfc_pn532_listener_wait_event() to ensure
 * the emulated FeliCa card advertises the correct NFCID2t. Forces
 * re-initialisation of TgInitAsTarget on the next wait_event call.
 *
 * @param[in] nfcid2t  8-byte NFCID2t of the FeliCa card being emulated.
 */
void furi_hal_nfc_pn532_listener_set_felica_params(const uint8_t* nfcid2t);

void furi_hal_nfc_pn532_emu_set_ndef(const uint8_t* msg, size_t len);

// Diagnostic API
FuriHalPn532Error furi_hal_nfc_pn532_last_error_get(void);
const char* furi_hal_nfc_pn532_last_error_str(void);
FuriHalNfcPn532Result furi_hal_nfc_pn532_last_result_get(void);
const char* furi_hal_nfc_pn532_last_result_str(void);

// Store MIFARE Classic auth key for hardware auth interception
void furi_hal_nfc_pn532_mf_key_store(const uint8_t* key, uint8_t key_type);

// PN532 native MIFARE Classic auth (InDataExchange)
FuriHalNfcError furi_hal_nfc_pn532_mf_auth(
    uint8_t block_num,
    const uint8_t* key,
    uint8_t key_type,
    const uint8_t* uid,
    uint8_t uid_len);
bool furi_hal_nfc_pn532_mf_is_authed(void);
void furi_hal_nfc_pn532_mf_deauth(void);

/** Send InRelease(0x52, 0x00) if a target is in-listed, then clear state.
 *  Idempotent. Safe to call from any *_poller_free() / teardown path.
 *  Best-effort: errors are swallowed because InRelease is cleanup, not
 *  part of the application-facing protocol. */
void furi_hal_nfc_pn532_release_if_listed(void);
FuriHalNfcError
    furi_hal_nfc_pn532_mf_read_block(uint8_t block_num, uint8_t* data, size_t data_size);
FuriHalNfcError
    furi_hal_nfc_pn532_mf_write_block(uint8_t block_num, const uint8_t* data, size_t data_size);

// Access cached target data (used by scanner for SAK-based child optimization)
uint8_t furi_hal_nfc_pn532_get_sak(void);
bool furi_hal_nfc_pn532_target_is_valid(void);

/** Poll for a Jewel/Topaz (NFC Type 1 Tag) card using the PN532.
 *
 * This is a direct HAL-level poll that bypasses the FuriHalNfcTech dispatch
 * (Jewel is not in the FuriHalNfcTech enum).  Call from a FAP or custom
 * protocol handler.  On success, target->uid contains the 6-byte RID
 * (HR0, HR1, UID0..UID3) and target->atqa[0] == 0x0C (Jewel marker).
 *
 * @param[out] target  Pointer to target struct to fill, or NULL for presence check.
 * @returns true if a Jewel/Topaz card was detected, false otherwise.
 */
bool furi_hal_nfc_pn532_poll_jewel(FuriHalPn532Target* target);

// Test accessors — expose internal state for property-based testing
uint8_t furi_hal_nfc_pn532_get_target_number(void);
bool furi_hal_nfc_pn532_test_get_needs_relist(void);
uint32_t furi_hal_nfc_pn532_test_get_target_tick(void);
void furi_hal_nfc_pn532_test_set_state(bool mf_authed, bool needs_relist, uint32_t target_tick);

#ifdef __cplusplus
}
#endif
