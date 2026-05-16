#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNEP_DEFAULT_VERSION  0x10
#define SNEP_REQUEST_PUT      0x02
#define SNEP_REQUEST_GET      0x01
#define SNEP_RESPONSE_SUCCESS 0x81
#define SNEP_RESPONSE_REJECT  0xFF

int8_t snep_send(const uint8_t* ndef_data, uint8_t ndef_len, uint16_t timeout_ms);

int16_t snep_receive(uint8_t* buf, uint16_t len, uint16_t timeout_ms);

#ifdef __cplusplus
}
#endif
