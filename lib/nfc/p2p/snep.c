#include "snep.h"
#include "llcp.h"

#include <string.h>
#include <furi.h>

#define TAG "Snep"

int8_t snep_send(const uint8_t* ndef_data, uint8_t ndef_len, uint16_t timeout_ms) {
    Llcp llcp;
    memset(&llcp, 0, sizeof(llcp));

    int8_t ret = llcp_activate(timeout_ms);
    if(ret <= 0) {
        FURI_LOG_E(TAG, "snep_send: llcp_activate failed: %d", ret);
        return -1;
    }

    ret = llcp_connect(&llcp, timeout_ms);
    if(ret <= 0) {
        FURI_LOG_E(TAG, "snep_send: llcp_connect failed: %d", ret);
        return -2;
    }

    uint8_t snep_header[6];
    snep_header[0] = SNEP_DEFAULT_VERSION;
    snep_header[1] = SNEP_REQUEST_PUT;
    snep_header[2] = 0;
    snep_header[3] = 0;
    snep_header[4] = 0;
    snep_header[5] = ndef_len;

    if(!llcp_write(&llcp, snep_header, 6, ndef_data, ndef_len)) {
        FURI_LOG_E(TAG, "snep_send: llcp_write failed");
        llcp_disconnect(&llcp, timeout_ms);
        return -3;
    }

    uint8_t rbuf[16];
    int16_t read_len = llcp_read(&llcp, rbuf, sizeof(rbuf));
    if(read_len < 6) {
        FURI_LOG_E(TAG, "snep_send: llcp_read returned short response (%d)", read_len);
        llcp_disconnect(&llcp, timeout_ms);
        return -4;
    }

    if(rbuf[1] != SNEP_RESPONSE_SUCCESS) {
        FURI_LOG_E(TAG, "snep_send: unexpected response code 0x%02X", rbuf[1]);
        llcp_disconnect(&llcp, timeout_ms);
        return -4;
    }

    llcp_disconnect(&llcp, timeout_ms);
    return 1;
}

int16_t snep_receive(uint8_t* buf, uint16_t len, uint16_t timeout_ms) {
    Llcp llcp;
    memset(&llcp, 0, sizeof(llcp));

    int8_t ret = llcp_activate(timeout_ms);
    if(ret <= 0) {
        FURI_LOG_E(TAG, "snep_receive: llcp_activate failed: %d", ret);
        return -1;
    }

    ret = llcp_wait_for_connection(&llcp, timeout_ms);
    if(ret <= 0) {
        FURI_LOG_E(TAG, "snep_receive: llcp_wait_for_connection failed: %d", ret);
        return -2;
    }

    int16_t read_len = llcp_read(&llcp, buf, len);
    if(read_len < 6) {
        FURI_LOG_E(TAG, "snep_receive: llcp_read returned short (%d)", read_len);
        llcp_disconnect(&llcp, timeout_ms);
        return -3;
    }

    if(buf[1] != SNEP_REQUEST_PUT) {
        FURI_LOG_E(TAG, "snep_receive: expected PUT request, got 0x%02X", buf[1]);
        llcp_disconnect(&llcp, timeout_ms);
        return -4;
    }

    uint32_t ndef_length = ((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16) |
                           ((uint32_t)buf[4] << 8) | buf[5];
    if(ndef_length > (uint32_t)(read_len - 6)) {
        FURI_LOG_E(
            TAG, "snep_receive: NDEF length %lu exceeds available %d", ndef_length, read_len - 6);
        llcp_disconnect(&llcp, timeout_ms);
        return -4;
    }

    for(uint8_t i = 0; i < ndef_length; i++) {
        buf[i] = buf[i + 6];
    }

    uint8_t response[6];
    response[0] = SNEP_DEFAULT_VERSION;
    response[1] = SNEP_RESPONSE_SUCCESS;
    response[2] = 0;
    response[3] = 0;
    response[4] = 0;
    response[5] = 0;
    if(!llcp_write(&llcp, response, 6, NULL, 0)) {
        FURI_LOG_E(TAG, "snep_receive: failed to send success response");
        llcp_disconnect(&llcp, timeout_ms);
        return -5;
    }

    llcp_disconnect(&llcp, timeout_ms);

    return (int16_t)ndef_length;
}
