#include "furi_hal_pn532.h"
#include "furi_hal_i2c.h"
#include <furi_hal_cortex.h>
#include <furi.h>
#include <string.h>

#define TAG "FuriHalPN532"

#define PN532_PREAMBLE   0x00
#define PN532_STARTCODE1 0x00
#define PN532_STARTCODE2 0xFF
#define PN532_POSTAMBLE  0x00
#define PN532_HOSTTOPN532 0xD4
#define PN532_PN532TOHOST 0xD5
#define PN532_I2C_READY   0x01

#define PN532_CMD_GET_FIRMWARE_VERSION 0x02
#define PN532_CMD_SAM_CONFIGURATION    0x14
#define PN532_CMD_IN_DATA_EXCHANGE     0x40
#define PN532_CMD_IN_LIST_PASSIVE      0x4A

static const uint8_t pn532_ack_frame[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
static bool pn532_ready = false;

static bool pn532_wait_ready(uint32_t timeout_ms) {
    FuriHalCortexTimer timer = furi_hal_cortex_timer_get(timeout_ms * 1000);
    while(!furi_hal_cortex_timer_is_expired(timer)) {
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        uint8_t status = 0;
        bool ok =
            furi_hal_i2c_rx(&furi_hal_i2c_handle_power, PN532_I2C_ADDR_7BIT, &status, 1, 10) &&
            (status == PN532_I2C_READY);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        if(ok) return true;
        furi_delay_ms(2);
    }
    return false;
}

static bool pn532_write_frame(const uint8_t* cmd, size_t cmd_len) {
    if(cmd_len > 64) return false;

    uint8_t frame[80];
    uint8_t pos = 0;
    uint8_t checksum = 0;
    const uint8_t len = (uint8_t)(cmd_len + 1);

    frame[pos++] = PN532_PREAMBLE;
    frame[pos++] = PN532_STARTCODE1;
    frame[pos++] = PN532_STARTCODE2;
    frame[pos++] = len;
    frame[pos++] = (uint8_t)(~len + 1);
    frame[pos++] = PN532_HOSTTOPN532;
    checksum += PN532_HOSTTOPN532;

    for(size_t i = 0; i < cmd_len; i++) {
        frame[pos++] = cmd[i];
        checksum += cmd[i];
    }

    frame[pos++] = (uint8_t)(~checksum + 1);
    frame[pos++] = PN532_POSTAMBLE;

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_tx(&furi_hal_i2c_handle_power, PN532_I2C_ADDR_7BIT, frame, pos, 100);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);
    return ok;
}

static bool pn532_read_ack(void) {
    uint8_t buf[7] = {0};
    if(!pn532_wait_ready(150)) return false;

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, PN532_I2C_ADDR_7BIT, buf, sizeof(buf), 100);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);

    if(!ok) return false;
    if(buf[0] != PN532_I2C_READY) return false;
    return memcmp(&buf[1], pn532_ack_frame, sizeof(pn532_ack_frame)) == 0;
}

static bool pn532_read_response(
    uint8_t* payload,
    size_t payload_size,
    size_t* out_len,
    uint32_t timeout_ms) {
    uint8_t rx[96] = {0};
    if(!pn532_wait_ready(timeout_ms)) return false;

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, PN532_I2C_ADDR_7BIT, rx, sizeof(rx), 150);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);
    if(!ok) return false;

    if(rx[0] != PN532_I2C_READY) return false;
    const uint8_t* frame = &rx[1];
    if(frame[0] != PN532_PREAMBLE || frame[1] != PN532_STARTCODE1 || frame[2] != PN532_STARTCODE2) return false;
    uint8_t len = frame[3];
    uint8_t lcs = frame[4];
    if((uint8_t)(len + lcs) != 0) return false;
    if(len < 2) return false;
    if(frame[5] != PN532_PN532TOHOST) return false;

    size_t content_len = len - 1;
    if(content_len > payload_size) return false;
    memcpy(payload, &frame[6], content_len);
    if(out_len) *out_len = content_len;
    return true;
}

bool furi_hal_pn532_init(void) {
    pn532_ready = false;
    FURI_LOG_I(TAG, "PN532 init start");
    if(!pn532_wait_ready(120)) {
        FURI_LOG_W(TAG, "PN532 not ready on I2C");
        return false;
    }
    FURI_LOG_I(TAG, "PN532 ready status received");

    uint8_t cmd_fw[] = {PN532_CMD_GET_FIRMWARE_VERSION};
    if(!pn532_write_frame(cmd_fw, sizeof(cmd_fw)) || !pn532_read_ack()) {
        FURI_LOG_E(TAG, "PN532 firmware command failed");
        return false;
    }
    FURI_LOG_I(TAG, "PN532 firmware ACK received");
    uint8_t fw_rsp[16];
    size_t fw_len = 0;
    if(!pn532_read_response(fw_rsp, sizeof(fw_rsp), &fw_len, 250) || fw_len < 5 ||
       fw_rsp[0] != (PN532_CMD_GET_FIRMWARE_VERSION + 1)) {
        FURI_LOG_E(TAG, "PN532 invalid firmware response");
        return false;
    }
    FURI_LOG_I(TAG, "PN532 firmware response OK");

    uint8_t cmd_sam[] = {PN532_CMD_SAM_CONFIGURATION, 0x01, 0x14, 0x01};
    if(!pn532_write_frame(cmd_sam, sizeof(cmd_sam)) || !pn532_read_ack()) {
        FURI_LOG_E(TAG, "PN532 SAM config command failed");
        return false;
    }
    FURI_LOG_I(TAG, "PN532 SAM ACK received");
    uint8_t sam_rsp[8];
    size_t sam_len = 0;
    if(!pn532_read_response(sam_rsp, sizeof(sam_rsp), &sam_len, 250) || sam_len < 1 ||
       sam_rsp[0] != (PN532_CMD_SAM_CONFIGURATION + 1)) {
        FURI_LOG_E(TAG, "PN532 invalid SAM response");
        return false;
    }

    pn532_ready = true;
    FURI_LOG_I(TAG, "PN532 initialized over I2C");
    return true;
}

bool furi_hal_pn532_is_ready(void) {
    return pn532_ready;
}

static FuriHalPn532Error pn532_exchange(
    const uint8_t* cmd,
    size_t cmd_len,
    uint8_t expected_response,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len,
    uint32_t timeout_ms) {
    if(!pn532_ready && !furi_hal_pn532_init()) return FuriHalPn532ErrorComm;
    if(!pn532_write_frame(cmd, cmd_len)) return FuriHalPn532ErrorComm;
    if(!pn532_read_ack()) return FuriHalPn532ErrorInvalidAck;

    size_t payload_len = 0;
    if(!pn532_read_response(rx_data, rx_size, &payload_len, timeout_ms)) {
        return FuriHalPn532ErrorTimeout;
    }
    if((payload_len == 0) || (rx_data[0] != expected_response)) {
        return FuriHalPn532ErrorInvalidFrame;
    }

    if(rx_len) *rx_len = payload_len;
    return FuriHalPn532ErrorNone;
}

bool furi_hal_pn532_poll_iso14443a(FuriHalPn532Target* target) {
    if(target) memset(target, 0, sizeof(*target));

    uint8_t cmd[] = {PN532_CMD_IN_LIST_PASSIVE, 0x01, 0x00};
    uint8_t response[32] = {0};
    size_t response_len = 0;

    FuriHalPn532Error error =
        pn532_exchange(
            cmd,
            sizeof(cmd),
            PN532_CMD_IN_LIST_PASSIVE + 1,
            response,
            sizeof(response),
            &response_len,
            150);
    if(error != FuriHalPn532ErrorNone) return false;
    if(response_len < 2) return false;
    if(response[1] == 0) return false;
    if((response_len < 7) || !target) return true;

    target->target_number = response[2];
    target->atqa[0] = response[3];
    target->atqa[1] = response[4];
    target->sak = response[5];
    target->uid_len = response[6];
    if((7 + target->uid_len) > response_len) return false;
    if(target->uid_len > sizeof(target->uid)) return false;
    memcpy(target->uid, &response[7], target->uid_len);

    return true;
}

FuriHalPn532Error furi_hal_pn532_in_data_exchange(
    uint8_t target_number,
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len) {
    if(tx_len > 60) return FuriHalPn532ErrorComm;

    uint8_t cmd[64] = {0};
    cmd[0] = PN532_CMD_IN_DATA_EXCHANGE;
    cmd[1] = target_number;
    if(tx_len) memcpy(&cmd[2], tx_data, tx_len);

    uint8_t response[64] = {0};
    size_t response_len = 0;
    FuriHalPn532Error error =
        pn532_exchange(
            cmd,
            tx_len + 2,
            PN532_CMD_IN_DATA_EXCHANGE + 1,
            response,
            sizeof(response),
            &response_len,
            250);
    if(error != FuriHalPn532ErrorNone) return error;
    if(response_len < 2) return FuriHalPn532ErrorInvalidFrame;
    if(response[1] != 0x00) return FuriHalPn532ErrorComm;

    const size_t payload_len = response_len - 2;
    if(payload_len > rx_size) return FuriHalPn532ErrorComm;
    if(payload_len) memcpy(rx_data, &response[2], payload_len);
    if(rx_len) *rx_len = payload_len;
    return FuriHalPn532ErrorNone;
}
