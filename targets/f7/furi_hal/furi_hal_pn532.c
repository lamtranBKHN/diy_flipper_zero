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

#define PN532_CMD_GET_FIRMWARE_VERSION 0x02
#define PN532_CMD_SAM_CONFIGURATION    0x14

static const uint8_t pn532_ack_frame[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
static bool pn532_ready = false;

static bool pn532_wait_ready(uint32_t timeout_ms) {
    FuriHalCortexTimer timer = furi_hal_cortex_timer_get(timeout_ms * 1000);
    while(!furi_hal_cortex_timer_is_expired(timer)) {
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        bool ok = furi_hal_i2c_is_device_ready(&furi_hal_i2c_handle_power, PN532_I2C_ADDR_7BIT, 10);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        if(ok) return true;
        furi_delay_ms(2);
    }
    return false;
}

static bool pn532_write_frame(const uint8_t* cmd, size_t cmd_len) {
    if(cmd_len > 32) return false;

    uint8_t frame[48];
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
    uint8_t buf[6] = {0};
    if(!pn532_wait_ready(150)) return false;

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, PN532_I2C_ADDR_7BIT, buf, sizeof(buf), 100);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);

    if(!ok) return false;
    return memcmp(buf, pn532_ack_frame, sizeof(pn532_ack_frame)) == 0;
}

static bool pn532_read_response(uint8_t* payload, size_t payload_size, size_t* out_len) {
    uint8_t rx[40] = {0};
    if(!pn532_wait_ready(250)) return false;

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, PN532_I2C_ADDR_7BIT, rx, sizeof(rx), 150);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);
    if(!ok) return false;

    if(rx[0] != PN532_PREAMBLE || rx[1] != PN532_STARTCODE1 || rx[2] != PN532_STARTCODE2) return false;
    uint8_t len = rx[3];
    uint8_t lcs = rx[4];
    if((uint8_t)(len + lcs) != 0) return false;
    if(len < 2) return false;
    if(rx[5] != PN532_PN532TOHOST) return false;

    size_t content_len = len - 1;
    if(content_len > payload_size) return false;
    memcpy(payload, &rx[6], content_len);
    if(out_len) *out_len = content_len;
    return true;
}

bool furi_hal_pn532_init(void) {
    pn532_ready = false;
    if(!pn532_wait_ready(120)) {
        FURI_LOG_W(TAG, "PN532 not ready on I2C");
        return false;
    }

    uint8_t cmd_fw[] = {PN532_CMD_GET_FIRMWARE_VERSION};
    if(!pn532_write_frame(cmd_fw, sizeof(cmd_fw)) || !pn532_read_ack()) {
        FURI_LOG_E(TAG, "PN532 firmware command failed");
        return false;
    }
    uint8_t fw_rsp[16];
    size_t fw_len = 0;
    if(!pn532_read_response(fw_rsp, sizeof(fw_rsp), &fw_len) || fw_len < 5 || fw_rsp[0] != (PN532_CMD_GET_FIRMWARE_VERSION + 1)) {
        FURI_LOG_E(TAG, "PN532 invalid firmware response");
        return false;
    }

    uint8_t cmd_sam[] = {PN532_CMD_SAM_CONFIGURATION, 0x01, 0x14, 0x01};
    if(!pn532_write_frame(cmd_sam, sizeof(cmd_sam)) || !pn532_read_ack()) {
        FURI_LOG_E(TAG, "PN532 SAM config command failed");
        return false;
    }
    uint8_t sam_rsp[8];
    size_t sam_len = 0;
    if(!pn532_read_response(sam_rsp, sizeof(sam_rsp), &sam_len) || sam_len < 1 || sam_rsp[0] != (PN532_CMD_SAM_CONFIGURATION + 1)) {
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
