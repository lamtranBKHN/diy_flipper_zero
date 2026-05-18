#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PN532_I2C_ADDR_7BIT 0x48 // 8-bit addr 0x90 (7-bit 0x48 shifted for STM32 I2C HAL)

typedef enum {
    FuriHalPn532ErrorNone = 0,
    FuriHalPn532ErrorTimeout,
    FuriHalPn532ErrorComm,
    FuriHalPn532ErrorInvalidAck,
    FuriHalPn532ErrorInvalidFrame,
    FuriHalPn532ErrorBufferOverflow,
    FuriHalPn532ErrorUnsupported,
} FuriHalPn532Error;

typedef struct {
    uint8_t target_number;
    uint8_t atqa[2];
    uint8_t sak;
    uint8_t uid[10];
    size_t uid_len;
    uint8_t app_data[4]; /**< Type B Application Data (from ATQB) */
    uint8_t proto_info[3]; /**< Type B Protocol Info (from ATQB) */
    uint8_t pmm[8]; /**< FeliCa PMm */
    uint8_t ats[20]; /**< ATS bytes (ISO14443-4A, present when SAK bit 0x20 set) */
    size_t ats_len;
    bool iso_dep_active; /**< SAK bit 0x20 indicates ISO-DEP capability */
} FuriHalPn532Target;

bool furi_hal_pn532_init(void);
bool furi_hal_pn532_is_ready(void);
bool furi_hal_pn532_read_status(void);

/** Start the software-IRQ I2C polling thread.
 *  Called automatically by furi_hal_pn532_init() after a successful init.
 *  Safe to call again if already running (no-op). */
void furi_hal_pn532_irq_start(void);

/** Stop the software-IRQ I2C polling thread.
 *  Must be called during NFC session teardown / HAL deinit.
 *  Safe to call even if the thread was never started (no-op). */
void furi_hal_pn532_irq_stop(void);

bool furi_hal_pn532_poll_iso14443a(FuriHalPn532Target* target);
bool furi_hal_pn532_poll_iso14443a_timeout(FuriHalPn532Target* target, uint32_t timeout_ms);
const char* furi_hal_pn532_strerror(uint8_t status_code);
bool furi_hal_pn532_srix_detect(uint8_t* chip_id);
bool furi_hal_pn532_srix_select(uint8_t chip_id);
bool furi_hal_pn532_srix_get_uid(uint8_t* uid, size_t* uid_len);
bool furi_hal_pn532_srix_read_block(uint8_t block_num, uint8_t* data);
bool furi_hal_pn532_srix_write_block(uint8_t block_num, const uint8_t* data);

FuriHalPn532Error furi_hal_pn532_in_data_exchange(
    uint8_t target_number,
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len);
FuriHalPn532Error furi_hal_pn532_in_communicate_thru(
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len);

FuriHalPn532Error furi_hal_pn532_in_communicate_thru_timeout(
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len,
    uint32_t timeout_ms);
FuriHalPn532Error furi_hal_pn532_send_command(const uint8_t* cmd, size_t cmd_len);

FuriHalPn532Error furi_hal_pn532_read_response(
    uint8_t* data,
    size_t data_size,
    size_t* data_len,
    uint32_t timeout_ms);

bool furi_hal_pn532_poll_felica(FuriHalPn532Target* target);

bool furi_hal_pn532_poll_iso14443b(FuriHalPn532Target* target);

FuriHalPn532Error furi_hal_pn532_mf_auth(
    uint8_t target_number,
    uint8_t block_num,
    const uint8_t* key,
    uint8_t key_type,
    const uint8_t* uid,
    uint8_t uid_len);

bool furi_hal_pn532_mf_backdoor_auth(
    uint8_t block_num,
    uint8_t key_type,
    const uint8_t* key,
    uint8_t backdoor_type);

bool furi_hal_pn532_mf_backdoor_write_block0(uint8_t block_num, const uint8_t* block_data);

// Target / Listener mode functions
FuriHalPn532Error
    furi_hal_pn532_tg_init_as_target(const uint8_t* params, size_t params_len, uint32_t timeout_ms);

FuriHalPn532Error
    furi_hal_pn532_tg_get_data(uint8_t* buf, size_t buf_size, size_t* out_len, uint32_t timeout_ms);

FuriHalPn532Error furi_hal_pn532_tg_set_data(const uint8_t* data, size_t data_len);

#ifdef __cplusplus
}
#endif
