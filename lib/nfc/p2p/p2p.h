#pragma once

#include "llcp.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    Llcp llcp;
    uint8_t ndef_buf[256];
    size_t ndef_len;
} P2pSession;

P2pSession* p2p_session_alloc(void);

void p2p_session_free(P2pSession* session);

bool p2p_send_url(P2pSession* session, const char* url, uint32_t timeout_ms);

bool p2p_send_text(P2pSession* session, const char* text, uint32_t timeout_ms);

bool p2p_receive(P2pSession* session, char* out_text, size_t out_size, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
