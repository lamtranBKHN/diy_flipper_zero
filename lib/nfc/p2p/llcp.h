#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LLCP_DEFAULT_TIMEOUT 20000
#define LLCP_DEFAULT_DSAP    0x04
#define LLCP_DEFAULT_SSAP    0x20

#define LLCP_BUF_SIZE     256
#define LLCP_SYMM_POLL_MS 50

typedef enum {
    LlcpPduSymm = 0x00,
    LlcpPduPax = 0x01,
    LlcpPduConnect = 0x04,
    LlcpPduDisc = 0x05,
    LlcpPduCc = 0x06,
    LlcpPduDm = 0x07,
    LlcpPduI = 0x0C,
    LlcpPduRr = 0x0D,
} LlcpPduType;

typedef struct {
    uint8_t mode;
    uint8_t ssap;
    uint8_t dsap;
    uint8_t ns;
    uint8_t nr;
    uint8_t rx_buf[LLCP_BUF_SIZE];
    uint8_t tx_buf[LLCP_BUF_SIZE];
} Llcp;

int8_t llcp_activate(uint16_t timeout_ms);

int8_t llcp_wait_for_connection(Llcp* llcp, uint16_t timeout_ms);

int8_t llcp_connect(Llcp* llcp, uint16_t timeout_ms);

bool llcp_write(Llcp* llcp, const uint8_t* header, uint8_t hlen, const uint8_t* body, uint8_t blen);

int16_t llcp_read(Llcp* llcp, uint8_t* buf, uint16_t len);

int8_t llcp_disconnect(Llcp* llcp, uint16_t timeout_ms);

#ifdef __cplusplus
}
#endif
