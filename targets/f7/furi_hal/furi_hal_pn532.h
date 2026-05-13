#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PN532_I2C_ADDR_7BIT (0x24 << 1) // 8-bit addr 0x48 (7-bit 0x24 shifted for STM32 I2C HAL)

typedef enum {
    FuriHalPn532ErrorNone = 0,
    FuriHalPn532ErrorTimeout,
    FuriHalPn532ErrorComm,
    FuriHalPn532ErrorInvalidAck,
    FuriHalPn532ErrorInvalidFrame,
} FuriHalPn532Error;

typedef struct {
    uint8_t target_number;
    uint8_t atqa[2];
    uint8_t sak;
    uint8_t uid[10];
    size_t uid_len;
} FuriHalPn532Target;

bool furi_hal_pn532_init(void);
bool furi_hal_pn532_is_ready(void);
bool furi_hal_pn532_read_status(void);
bool furi_hal_pn532_poll_iso14443a(FuriHalPn532Target* target);
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

#ifdef __cplusplus
}
#endif
