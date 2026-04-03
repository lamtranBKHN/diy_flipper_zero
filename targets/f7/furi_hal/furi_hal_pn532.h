#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PN532_I2C_ADDR_7BIT 0x24

typedef enum {
    FuriHalPn532ErrorNone = 0,
    FuriHalPn532ErrorTimeout,
    FuriHalPn532ErrorComm,
    FuriHalPn532ErrorInvalidAck,
    FuriHalPn532ErrorInvalidFrame,
} FuriHalPn532Error;

bool furi_hal_pn532_init(void);
bool furi_hal_pn532_is_ready(void);

#ifdef __cplusplus
}
#endif
