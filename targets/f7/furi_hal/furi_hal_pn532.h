#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PN532_I2C_ADDR_7BIT (0x24 << 1) // 7-bit addr 0x24, shifted for STM32 LL

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
bool furi_hal_pn532_poll_iso14443a(FuriHalPn532Target* target);
FuriHalPn532Error furi_hal_pn532_in_data_exchange(
    uint8_t target_number,
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len);


#ifdef __cplusplus
}
#endif
