#include "p2p.h"
#include "snep.h"
#include "helpers/ndef.h"

#include <stdlib.h>
#include <string.h>

P2pSession* p2p_session_alloc(void) {
    P2pSession* session = malloc(sizeof(P2pSession));
    if(session) {
        memset(session, 0, sizeof(P2pSession));
    }
    return session;
}

void p2p_session_free(P2pSession* session) {
    if(session) {
        free(session);
    }
}

bool p2p_send_url(P2pSession* session, const char* url, uint32_t timeout_ms) {
    if(!session || !url) return false;

    size_t url_len = strlen(url);
    uint8_t ndef_buf[256];
    size_t ndef_len = sizeof(ndef_buf);

    if(!ndef_build_uri(url, url_len, NdefUriPrefixNoPrefix, ndef_buf, &ndef_len)) {
        return false;
    }

    int8_t ret = snep_send(ndef_buf, (uint8_t)ndef_len, timeout_ms);
    if(ret <= 0) return false;

    return true;
}

bool p2p_send_text(P2pSession* session, const char* text, uint32_t timeout_ms) {
    if(!session || !text) return false;

    size_t text_len = strlen(text);
    uint8_t ndef_buf[256];
    size_t ndef_len = sizeof(ndef_buf);

    if(!ndef_build_text(text, text_len, "en", ndef_buf, &ndef_len)) {
        return false;
    }

    int8_t ret = snep_send(ndef_buf, (uint8_t)ndef_len, timeout_ms);
    if(ret <= 0) return false;

    return true;
}

bool p2p_receive(P2pSession* session, char* out_text, size_t out_size, uint32_t timeout_ms) {
    if(!session || !out_text || out_size == 0) return false;

    uint8_t buf[256];
    int16_t len = snep_receive(buf, sizeof(buf), timeout_ms);
    if(len <= 0) return false;

    session->ndef_len = (size_t)len;
    memcpy(session->ndef_buf, buf, session->ndef_len);

    NdefMessage msg;
    if(!ndef_parse(buf, (size_t)len, &msg)) return false;

    bool result = false;
    if(ndef_extract_url(&msg, out_text, out_size)) {
        result = true;
    } else if(ndef_extract_text(&msg, out_text, out_size)) {
        result = true;
    }

    ndef_message_clear(&msg);
    return result;
}
