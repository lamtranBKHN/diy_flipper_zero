#include "llcp.h"

#include <furi.h>
#include <furi_hal.h>
#include <string.h>

#define TAG "Llcp"

static const uint8_t dep_params[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x01, 0xFE,
                                     0x0F, 0xBB, 0xBA, 0xA6, 0xC9, 0x89, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x01, 0xFE,
                                     0x0F, 0xBB, 0xBA, 0xA6, 0xC9, 0x89, 0x00, 0x00, 0x06,
                                     0x46, 0x66, 0x6D, 0x01, 0x01, 0x10, 0x00};

static const uint8_t symm_pdu[2] = {0, 0};

static uint8_t llcp_get_ptype(const uint8_t* buf) {
    return ((buf[0] & 0x03) << 2) | (buf[1] >> 6);
}

static uint8_t llcp_get_ssap(const uint8_t* buf) {
    return buf[1] & 0x3F;
}

static uint8_t llcp_get_dsap(const uint8_t* buf) {
    return buf[0] >> 2;
}

static int16_t llcp_read_pdu(Llcp* llcp, uint16_t poll_ms) {
    size_t out_len = 0;
    FuriHalPn532Error err =
        furi_hal_pn532_tg_get_data(llcp->rx_buf, LLCP_BUF_SIZE, &out_len, poll_ms);
    if(err == FuriHalPn532ErrorNone && out_len > 0) {
        return (int16_t)out_len;
    }
    return -1;
}

int8_t llcp_activate(uint16_t timeout_ms) {
    FuriHalPn532Error err =
        furi_hal_pn532_tg_init_as_target(dep_params, sizeof(dep_params), timeout_ms);
    if(err == FuriHalPn532ErrorNone) return 1;
    if(err == FuriHalPn532ErrorTimeout) {
        FURI_LOG_E(TAG, "llcp_activate: PN532 target init timed out");
        return 0;
    }
    FURI_LOG_E(TAG, "llcp_activate: PN532 target init failed: %s", furi_hal_pn532_error_str(err));
    return -1;
}

int8_t llcp_wait_for_connection(Llcp* llcp, uint16_t timeout_ms) {
    llcp->mode = 1;
    llcp->ns = 0;
    llcp->nr = 0;

    uint32_t deadline = furi_get_tick() + timeout_ms;
    while(furi_get_tick() < deadline) {
        int16_t rlen = llcp_read_pdu(llcp, LLCP_SYMM_POLL_MS);
        if(rlen < 2) continue;

        uint8_t type = llcp_get_ptype(llcp->rx_buf);
        if(type == LlcpPduConnect) {
            break;
        }
        if(type == LlcpPduSymm) {
            furi_hal_pn532_tg_set_data(symm_pdu, sizeof(symm_pdu));
            continue;
        }
        FURI_LOG_E(TAG, "llcp_wait_for_connection: unexpected PDU type=%d", type);
        return -3;
    }
    if(furi_get_tick() >= deadline) {
        FURI_LOG_E(TAG, "llcp_wait_for_connection: timeout waiting for CONNECT");
        return -1;
    }

    llcp->ssap = llcp_get_dsap(llcp->rx_buf);
    llcp->dsap = llcp_get_ssap(llcp->rx_buf);

    llcp->tx_buf[0] = (llcp->dsap << 2) | ((LlcpPduCc >> 2) & 0x03);
    llcp->tx_buf[1] = ((LlcpPduCc & 0x03) << 6) | llcp->ssap;

    if(furi_hal_pn532_tg_set_data(llcp->tx_buf, 2) != FuriHalPn532ErrorNone) {
        FURI_LOG_E(TAG, "llcp_wait_for_connection: failed to send CC PDU");
        return -2;
    }

    return 1;
}

int8_t llcp_connect(Llcp* llcp, uint16_t timeout_ms) {
    llcp->mode = 0;
    llcp->dsap = LLCP_DEFAULT_DSAP;
    llcp->ssap = LLCP_DEFAULT_SSAP;
    llcp->ns = 0;
    llcp->nr = 0;

    uint32_t deadline = furi_get_tick() + timeout_ms;
    while(furi_get_tick() < deadline) {
        int16_t rlen = llcp_read_pdu(llcp, LLCP_SYMM_POLL_MS);
        if(rlen < 2) continue;

        uint8_t type = llcp_get_ptype(llcp->rx_buf);
        if(type == LlcpPduSymm) {
            break;
        }
    }
    if(furi_get_tick() >= deadline) {
        FURI_LOG_E(TAG, "llcp_connect: timeout waiting for initial SYMM");
        return -1;
    }

    llcp->tx_buf[0] = (LLCP_DEFAULT_DSAP << 2) | (LlcpPduConnect >> 2);
    llcp->tx_buf[1] = ((LlcpPduConnect & 0x03) << 6) | LLCP_DEFAULT_SSAP;

    uint8_t body[17];
    body[0] = 0x06;
    body[1] = 15;
    memcpy(&body[2], "urn:nfc:sn:snep", 15);

    uint8_t total_len = 2 + 17;
    memcpy(&llcp->tx_buf[2], body, 17);

    if(furi_hal_pn532_tg_set_data(llcp->tx_buf, total_len) != FuriHalPn532ErrorNone) {
        FURI_LOG_E(TAG, "llcp_connect: failed to send CONNECT PDU");
        return -2;
    }

    deadline = furi_get_tick() + timeout_ms;
    while(furi_get_tick() < deadline) {
        int16_t rlen = llcp_read_pdu(llcp, LLCP_SYMM_POLL_MS);
        if(rlen < 2) continue;

        uint8_t type = llcp_get_ptype(llcp->rx_buf);
        if(type == LlcpPduCc) {
            return 1;
        }
        if(type == LlcpPduSymm) {
            furi_hal_pn532_tg_set_data(symm_pdu, sizeof(symm_pdu));
            continue;
        }
        FURI_LOG_E(TAG, "llcp_connect: unexpected PDU type=%d (expecting CC)", type);
        return -3;
    }

    FURI_LOG_E(TAG, "llcp_connect: timeout waiting for CC");
    return -1;
}

bool llcp_write(Llcp* llcp, const uint8_t* header, uint8_t hlen, const uint8_t* body, uint8_t blen) {
    if(llcp->mode) {
        uint32_t deadline = furi_get_tick() + LLCP_DEFAULT_TIMEOUT;
        while(furi_get_tick() < deadline) {
            int16_t rlen = llcp_read_pdu(llcp, LLCP_SYMM_POLL_MS);
            if(rlen >= 2 && llcp_get_ptype(llcp->rx_buf) == LlcpPduSymm) break;
        }
        if(furi_get_tick() >= deadline) return false;
    }

    uint16_t total = 3 + (uint16_t)hlen + (uint16_t)blen;
    if(total > LLCP_BUF_SIZE) {
        FURI_LOG_E(
            TAG, "llcp_write: payload too large: hlen=%u, blen=%u, total=%u", hlen, blen, total);
        return false;
    }

    for(int8_t i = (int8_t)hlen - 1; i >= 0; i--) {
        llcp->tx_buf[3 + i] = header[i];
    }
    llcp->tx_buf[0] = (llcp->dsap << 2) | (LlcpPduI >> 2);
    llcp->tx_buf[1] = ((LlcpPduI & 0x03) << 6) | llcp->ssap;
    llcp->tx_buf[2] = (llcp->ns << 4) | llcp->nr;
    if(blen > 0) {
        memcpy(&llcp->tx_buf[3 + hlen], body, blen);
    }

    if(furi_hal_pn532_tg_set_data(llcp->tx_buf, total) != FuriHalPn532ErrorNone) {
        FURI_LOG_E(TAG, "llcp_write: failed to send I PDU (total=%u)", total);
        return false;
    }

    llcp->ns++;

    uint32_t deadline = furi_get_tick() + LLCP_DEFAULT_TIMEOUT;
    while(furi_get_tick() < deadline) {
        int16_t rlen = llcp_read_pdu(llcp, LLCP_SYMM_POLL_MS);
        if(rlen < 2) continue;

        uint8_t type = llcp_get_ptype(llcp->rx_buf);
        if(type == LlcpPduRr) {
            break;
        }
        if(type == LlcpPduSymm) {
            furi_hal_pn532_tg_set_data(symm_pdu, sizeof(symm_pdu));
            continue;
        }
        FURI_LOG_E(TAG, "llcp_write: unexpected PDU type=%d (expecting RR)", type);
        return false;
    }
    if(furi_get_tick() >= deadline) {
        FURI_LOG_E(TAG, "llcp_write: timeout waiting for RR");
        return false;
    }

    if(furi_hal_pn532_tg_set_data(symm_pdu, sizeof(symm_pdu)) != FuriHalPn532ErrorNone) {
        FURI_LOG_E(TAG, "llcp_write: failed to send trailing SYMM");
        return false;
    }

    return true;
}

int16_t llcp_read(Llcp* llcp, uint8_t* buf, uint16_t len) {
    uint32_t deadline = furi_get_tick() + LLCP_DEFAULT_TIMEOUT;
    while(furi_get_tick() < deadline) {
        int16_t rlen = llcp_read_pdu(llcp, LLCP_SYMM_POLL_MS);
        if(rlen < 2) continue;

        uint8_t type = llcp_get_ptype(llcp->rx_buf);
        if(type == LlcpPduI) {
            uint16_t i_len = (uint16_t)rlen - 3;
            if(i_len > len) i_len = len;

            llcp->ssap = llcp_get_dsap(llcp->rx_buf);
            llcp->dsap = llcp_get_ssap(llcp->rx_buf);

            memcpy(buf, &llcp->rx_buf[3], i_len);

            llcp->tx_buf[0] = (llcp->dsap << 2) | (LlcpPduRr >> 2);
            llcp->tx_buf[1] = ((LlcpPduRr & 0x03) << 6) | llcp->ssap;
            llcp->tx_buf[2] = (llcp->rx_buf[2] >> 4) + 1;

            if(furi_hal_pn532_tg_set_data(llcp->tx_buf, 3) != FuriHalPn532ErrorNone) {
                FURI_LOG_E(TAG, "llcp_read: failed to send RR PDU");
                return -2;
            }

            llcp->nr++;

            return (int16_t)i_len;
        }
        if(type == LlcpPduSymm) {
            if(furi_hal_pn532_tg_set_data(symm_pdu, sizeof(symm_pdu)) != FuriHalPn532ErrorNone) {
                FURI_LOG_E(TAG, "llcp_read: failed to send SYMM during poll");
                return -2;
            }
            continue;
        }
        FURI_LOG_E(TAG, "llcp_read: unexpected PDU type=%d (expecting I)", type);
        return -3;
    }

    FURI_LOG_E(TAG, "llcp_read: timeout waiting for I PDU");
    return -1;
}

int8_t llcp_disconnect(Llcp* llcp, uint16_t timeout_ms) {
    uint32_t deadline = furi_get_tick() + timeout_ms;
    while(furi_get_tick() < deadline) {
        int16_t rlen = llcp_read_pdu(llcp, LLCP_SYMM_POLL_MS);
        if(rlen < 2) continue;

        uint8_t type = llcp_get_ptype(llcp->rx_buf);
        if(type == LlcpPduSymm) {
            break;
        }
    }
    if(furi_get_tick() >= deadline) {
        FURI_LOG_E(TAG, "llcp_disconnect: timeout waiting for initial SYMM");
        return -1;
    }

    llcp->tx_buf[0] = (LLCP_DEFAULT_DSAP << 2) | (LlcpPduDisc >> 2);
    llcp->tx_buf[1] = ((LlcpPduDisc & 0x03) << 6) | LLCP_DEFAULT_SSAP;

    if(furi_hal_pn532_tg_set_data(llcp->tx_buf, 2) != FuriHalPn532ErrorNone) {
        FURI_LOG_E(TAG, "llcp_disconnect: failed to send DISC PDU");
        return -2;
    }

    deadline = furi_get_tick() + timeout_ms;
    while(furi_get_tick() < deadline) {
        int16_t rlen = llcp_read_pdu(llcp, LLCP_SYMM_POLL_MS);
        if(rlen < 2) continue;

        uint8_t type = llcp_get_ptype(llcp->rx_buf);
        if(type == LlcpPduDm) {
            return 1;
        }
        if(type == LlcpPduSymm) {
            if(furi_hal_pn532_tg_set_data(symm_pdu, sizeof(symm_pdu)) != FuriHalPn532ErrorNone) {
                FURI_LOG_E(TAG, "llcp_disconnect: failed to send SYMM during poll");
                return -2;
            }
            continue;
        }
        FURI_LOG_E(TAG, "llcp_disconnect: unexpected PDU type=%d (expecting DM)", type);
        return -3;
    }

    FURI_LOG_E(TAG, "llcp_disconnect: timeout waiting for DM");
    return -1;
}
