#include "furi_hal_pn532.h"
#include "furi_hal_i2c.h"
#include "furi_hal_nfc_i.h"
#include <furi_hal_cortex.h>
#include <furi.h>
#include <string.h>

#define TAG "FuriHalPN532"

#define PN532_PREAMBLE   0x00
#define PN532_STARTCODE1 0x00
#define PN532_STARTCODE2 0xFF
#define PN532_POSTAMBLE  0x00
#define PN532_I2C_SFI    0x00
#define PN532_HOSTTOPN532 0xD4
#define PN532_PN532TOHOST 0xD5
#define PN532_I2C_READY   0x01

#define PN532_CMD_GET_FIRMWARE_VERSION 0x02
#define PN532_CMD_SAM_CONFIGURATION    0x14
#define PN532_CMD_RF_CONFIGURATION     0x32
#define PN532_CMD_IN_DATA_EXCHANGE     0x40
#define PN532_CMD_IN_LIST_PASSIVE      0x4A
#define PN532_CMD_WRITE_REGISTER        0x08

/* CIU_TxControl - enables RF output drivers (required for clone modules) */
#define PN532_REG_CIU_TxControl  0x6330
#define PN532_TXCONTROL_ENABLE    0x83  /* TX1RFEn | TX2RFEn | InitialRFOn */

#define PN532_I2C_RETRIES      3
#define PN532_MAX_TX_PAYLOAD   255
#define PN532_MAX_RX_FRAME     270

static const uint8_t pn532_ack_frame[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
static bool pn532_ready = false;
static uint8_t pn532_i2c_addr = PN532_I2C_ADDR;

/* PN532 8-bit shifted I2C address.
 * Candidates are tried in order; first responsive one wins. */
static bool pn532_wait_ready_with_timeout(uint32_t timeout_us) {
    FuriHalCortexTimer timer = furi_hal_cortex_timer_get(timeout_us);
    while(!pn532_wait_ready(timeout_us)) {
        if(furi_hal_cortex_timer_is_expired(timer)) {
            FURI_LOG_E(TAG, "PN532 wait ready timeout");
            return false;
        }
    }
    return true;
}
static bool pn532_probe_address(void) {
    static const uint8_t candidates[] = {
        PN532_I2C_ADDR, /* 0x48 — 8-bit shifted address */
    };

    for(size_t i = 0; i < COUNT_OF(candidates); i++) {
        uint8_t status = 0;
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, candidates[i], &status, 1, 10);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        if(ok) {
            pn532_i2c_addr = candidates[i];
            FURI_LOG_I(TAG, "PN532 I2C addr detected: 0x%02X", pn532_i2c_addr);
            return true;
        }
    }

    return false;
}

bool pn532_wait_ready(uint32_t timeout_ms) {
    // PN532 I2C state machine requires careful handling.
    // PN532 returns 0x01 when it has data ready to read.
    // We check every 10ms with a maximum timeout.
    // The key is NOT to spam the bus - 10ms between checks is sufficient.
    FuriHalCortexTimer timer = furi_hal_cortex_timer_get(timeout_ms * 1000);
    int retry_count = 0;
    const int max_retries = timeout_ms / 10; // ~10ms per retry
    
    while(!furi_hal_cortex_timer_is_expired(timer) && retry_count < max_retries) {
        // BUG-008 Fix: Check for abort request between I2C polls to avoid 5s lockup.
        // We use thread_flags_get to check without clearing, so the worker can process the abort.
        if(furi_thread_flags_get() & FuriHalNfcEventInternalTypeAbort) {
            return false;
        }

        uint8_t status = 0;
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pn532_i2c_addr, &status, 1, 10);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        
        if(ok && status == PN532_I2C_READY) {
            return true;
        }
        
        retry_count++;
        // 10ms delay between polls - prevents I2C storm
        furi_delay_ms(10);
    }
    return false;
}

static bool pn532_write_frame(const uint8_t* cmd, size_t cmd_len) {
    if(cmd_len > PN532_MAX_TX_PAYLOAD) return false;

    uint8_t frame[PN532_MAX_TX_PAYLOAD + 10];
    uint8_t pos = 0;
    uint8_t checksum = 0;
    const uint8_t len = (uint8_t)(cmd_len + 1);

    /* PN532 frame format: PREAMBLE + STARTCODE + LEN + LCS + DATA[HOSTTOPN532 + cmd] + DCS + POSTAMBLE */
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

    /* Give PN532 1ms to switch from address-ACK mode to receive mode.
     * With large bulk capacitors on the power rail, the PN532 I2C state machine
     * needs extra settle time. The ACK from address probe is instant, but the
     * PN532 needs a brief pause before it can accept a data frame. */
    furi_delay_us(1000);

    for(uint8_t attempt = 0; attempt < PN532_I2C_RETRIES; attempt++) {
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        bool ok = furi_hal_i2c_tx(
            &furi_hal_i2c_handle_power, pn532_i2c_addr, frame, pos, 100);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        if(ok) {
            FURI_LOG_D(TAG, "TX frame OK, size: %u", pos);
            return true;
        }
        FURI_LOG_W(TAG, "I2C TX retry %u/%u", attempt + 1, PN532_I2C_RETRIES);
        furi_delay_ms(5);
    }
    FURI_LOG_E(TAG, "TX frame failed after %u retries", PN532_I2C_RETRIES);
    return false;
}

static bool pn532_read_ack(void) {
    uint8_t buf[7] = {0};
    if(!pn532_wait_ready(150)) return false;

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pn532_i2c_addr, buf, sizeof(buf), 100);
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
    uint8_t rx[PN532_MAX_RX_FRAME] = {0};
    if(!pn532_wait_ready(timeout_ms)) return false;

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_rx(
        &furi_hal_i2c_handle_power, pn532_i2c_addr, rx, sizeof(rx), 150);
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

    uint8_t dcs = frame[5 + len];
    uint8_t checksum = 0;
    for(size_t i = 0; i < len; i++) {
        checksum += frame[5 + i];
    }
    if((uint8_t)(checksum + dcs) != 0) {
        FURI_LOG_E(TAG, "DCS checksum failure");
        return false;
    }

    memcpy(payload, &frame[6], content_len);
    if(out_len) *out_len = content_len;
    return true;
}

/* Write a register in the PN532 CIU register bank */
static bool pn532_write_register(uint16_t reg, uint8_t value) {
    uint8_t cmd[] = {
        PN532_CMD_WRITE_REGISTER,
        (uint8_t)(reg >> 8),   /* High byte */
        (uint8_t)(reg & 0xFF), /* Low byte */
        value
    };
    if(!pn532_write_frame(cmd, sizeof(cmd)) || !pn532_read_ack()) {
        FURI_LOG_W(TAG, "WriteRegister 0x%04X failed", reg);
        return false;
    }
    /* Read and discard response */
    uint8_t dummy[8];
    size_t dummy_len = 0;
    pn532_read_response(dummy, sizeof(dummy), &dummy_len, 50);
    return true;
}

bool furi_hal_pn532_init(void) {
    pn532_ready = false;
    FURI_LOG_I(TAG, "PN532 init start");
    if(!pn532_probe_address()) {
        FURI_LOG_W(TAG, "PN532 not detected on I2C");
        return false;
    }
    FURI_LOG_I(TAG, "PN532 detected at 0x%02X, querying firmware...", pn532_i2c_addr);

    uint8_t cmd_fw[] = {PN532_CMD_GET_FIRMWARE_VERSION};
    if(!pn532_write_frame(cmd_fw, sizeof(cmd_fw)) || !pn532_read_ack()) {
        FURI_LOG_E(TAG, "PN532 firmware command failed");
        return false;
    }
    FURI_LOG_I(TAG, "PN532 firmware ACK received");
    uint8_t fw_rsp[24];
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
    uint8_t sam_rsp[16];
    size_t sam_len = 0;
    if(!pn532_read_response(sam_rsp, sizeof(sam_rsp), &sam_len, 250) || sam_len < 1 ||
       sam_rsp[0] != (PN532_CMD_SAM_CONFIGURATION + 1)) {
        FURI_LOG_E(TAG, "PN532 invalid SAM response");
        return false;
    }
    FURI_LOG_I(TAG, "PN532 SAM configured");

    /* Enable RF output drivers (critical for clone modules) */
    /* CIU_TxControl: TX1RFEn | TX2RFEn | InitialRFOn */
    if(!pn532_write_register(PN532_REG_CIU_TxControl, PN532_TXCONTROL_ENABLE)) {
        FURI_LOG_W(TAG, "TxControl write failed, continuing anyway");
    } else {
        FURI_LOG_I(TAG, "RF output drivers enabled");
    }
    furi_delay_ms(10);

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

    for(uint8_t attempt = 0; attempt < PN532_I2C_RETRIES; attempt++) {
        if(furi_thread_flags_get() & FuriHalNfcEventInternalTypeAbort) {
            return FuriHalPn532ErrorComm;
        }
        if(!pn532_write_frame(cmd, cmd_len)) {
            FURI_LOG_W(TAG, "Exchange write failed, attempt %u", attempt + 1);
            furi_delay_ms(10);
            continue;
        }
        if(!pn532_read_ack()) {
            FURI_LOG_W(TAG, "Exchange ACK failed, attempt %u", attempt + 1);
            furi_delay_ms(10);
            continue;
        }

        size_t payload_len = 0;
        if(!pn532_read_response(rx_data, rx_size, &payload_len, timeout_ms)) {
            /* The PN532 ACK'd the command but the response didn't arrive in
             * time. The PN532 may still be processing (e.g. InListPassiveTarget
             * polling the field). Wait briefly and try to drain the pending
             * response so the next command doesn't read stale data.
             * Skip if aborting to avoid unnecessary exit delays. */
            if(!(furi_thread_flags_get() & FuriHalNfcEventInternalTypeAbort)) {
                FURI_LOG_W(TAG, "Exchange response timeout, draining");
                furi_delay_ms(50);
                uint8_t drain[PN532_MAX_RX_FRAME];
                size_t drain_len = 0;
                pn532_read_response(drain, sizeof(drain), &drain_len, 100);
            }
            return FuriHalPn532ErrorTimeout;
        }
        if((payload_len == 0) || (rx_data[0] != expected_response)) {
            return FuriHalPn532ErrorInvalidFrame;
        }

        if(rx_len) *rx_len = payload_len;
        return FuriHalPn532ErrorNone;
    }

    return FuriHalPn532ErrorComm;
}

bool furi_hal_pn532_poll_iso14443a(FuriHalPn532Target* target) {
    if(target) memset(target, 0, sizeof(*target));
    
    uint8_t cmd[] = {PN532_CMD_IN_LIST_PASSIVE, 0x01, 0x00};
    uint8_t response[64] = {0};
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
    if((7U + target->uid_len) > response_len) return false;
    if(target->uid_len > sizeof(target->uid)) return false;
    memcpy(target->uid, &response[7], target->uid_len);
    
    return true;
}

bool furi_hal_pn532_poll_felica(FuriHalPn532Target* target) {
    if(target) memset(target, 0, sizeof(*target));
    
    uint8_t cmd[] = {PN532_CMD_IN_LIST_PASSIVE, 0x02, 0x00};
    uint8_t response[64] = {0};
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
    if((response_len < 11) || !target) return true;
    
    // FeliCa target format: [TargetNum][IDm(8)][PMm(8)][SysCode(2)]...
    target->target_number = response[2];
    target->uid_len = 8;
    memcpy(target->uid, &response[3], 8);
    target->sak = 0; // Not applicable for FeliCa, but set to 0
    
    return true;
}

bool furi_hal_pn532_poll_iso14443b(FuriHalPn532Target* target) {
    if(target) memset(target, 0, sizeof(*target));

    /* InListPassiveTarget: cmd[1] = BrTy = 0x03 (ISO14443-B at 106 kbps)
     * cmd[2] = AFI (0x00 = any application)
     * This was previously 0x01 which is BrTy for ISO14443-A, causing all
     * Type B polls to silently act as duplicate Type A polls. */
    uint8_t cmd[] = {PN532_CMD_IN_LIST_PASSIVE, 0x03, 0x00};
    uint8_t response[64] = {0};
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
    if((response_len < 13) || !target) return true;

    // Type B response: [cmd+1][NbTg][Tg][ATQB(12 bytes minimum)]
    target->target_number = response[2];
    // Store PUPI (4 bytes) as UID â€” bytes 4..7 of ATQB, i.e. response[6..9]
    target->uid_len = 4;
    memcpy(target->uid, &response[6], 4);
    target->sak = 0; // not applicable for Type B
    target->atqa[0] = 0x50; // marker for Type B
    target->atqa[1] = 0x00;

    return true;
}


FuriHalPn532Error furi_hal_pn532_in_data_exchange(
    uint8_t target_number,
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len) {
    if(tx_len > PN532_MAX_TX_PAYLOAD - 2) return FuriHalPn532ErrorComm;

    uint8_t cmd[PN532_MAX_TX_PAYLOAD + 2] = {0};
    cmd[0] = PN532_CMD_IN_DATA_EXCHANGE;
    cmd[1] = target_number;
    if(tx_len) memcpy(&cmd[2], tx_data, tx_len);

    uint8_t response[PN532_MAX_RX_FRAME] = {0};
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

FuriHalPn532Error furi_hal_pn532_mf_auth(
    uint8_t target_number,
    uint8_t block_num,
    const uint8_t* key,
    uint8_t key_type,
    const uint8_t* uid,
    uint8_t uid_len) {
    uint8_t cmd[14];
    cmd[0] = PN532_CMD_IN_DATA_EXCHANGE;
    cmd[1] = target_number;
    cmd[2] = key_type ? 0x61 : 0x60;
    cmd[3] = block_num;
    memcpy(&cmd[4], key, 6);
    size_t copy_len = (uid_len >= 4) ? 4 : uid_len;
    memcpy(&cmd[10], uid, copy_len);
    if(copy_len < 4) memset(&cmd[10 + copy_len], 0, 4 - copy_len);

    uint8_t resp[4];
    size_t resp_len = sizeof(resp);
    FuriHalPn532Error err = pn532_exchange(
        cmd, sizeof(cmd), PN532_CMD_IN_DATA_EXCHANGE + 1, resp, sizeof(resp), &resp_len, 1000);

    if(err == FuriHalPn532ErrorNone && resp_len >= 1 && resp[0] == 0x00) {
        return FuriHalPn532ErrorNone;
    }

    return FuriHalPn532ErrorComm;
}

FuriHalPn532Error furi_hal_pn532_send_command(const uint8_t* cmd, size_t cmd_len) {
    if(!pn532_ready && !furi_hal_pn532_init()) return FuriHalPn532ErrorComm;
    if(cmd_len == 0) return FuriHalPn532ErrorNone;
    
    if(!pn532_write_frame(cmd, cmd_len)) return FuriHalPn532ErrorComm;
    if(!pn532_read_ack()) return FuriHalPn532ErrorInvalidAck;
    return FuriHalPn532ErrorNone;
}

FuriHalPn532Error furi_hal_pn532_read_response(uint8_t* data, size_t data_size, size_t* data_len, uint32_t timeout_ms) {
    if(!pn532_ready) return FuriHalPn532ErrorComm;
    
    uint8_t rx[PN532_MAX_RX_FRAME] = {0};
    if(!pn532_wait_ready_with_timeout(
           timeout_ms > 0 ? timeout_ms : PN532_LISTENER_TIMEOUT_MS)) {
        furi_hal_i2c_bus_reset(&furi_hal_i2c_handle_external);
        return FuriHalPn532ErrorCommunicationTimeout;
    }
    
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pn532_i2c_addr, rx, sizeof(rx), 150);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);
    if(!ok) return FuriHalPn532ErrorComm;
    
    if(rx[0] != PN532_I2C_READY) return FuriHalPn532ErrorComm;
    const uint8_t* frame = &rx[1];
    if(frame[0] != PN532_PREAMBLE || frame[1] != PN532_STARTCODE1 || frame[2] != PN532_STARTCODE2) {
        return FuriHalPn532ErrorInvalidFrame;
    }
    
    uint8_t len = frame[3];
    uint8_t lcs = frame[4];
    if((uint8_t)(len + lcs) != 0) return FuriHalPn532ErrorInvalidFrame;
    if(len < 2) return FuriHalPn532ErrorInvalidFrame;
    if(frame[5] != PN532_PN532TOHOST) return FuriHalPn532ErrorInvalidFrame;
    
    size_t content_len = len - 1;
    if(content_len > data_size) return FuriHalPn532ErrorComm;

    uint8_t dcs = frame[5 + len];
    uint8_t checksum = 0;
    for(size_t i = 0; i < len; i++) {
        checksum += frame[5 + i];
    }
    if((uint8_t)(checksum + dcs) != 0) {
        FURI_LOG_E(TAG, "DCS checksum failure");
        return FuriHalPn532ErrorInvalidFrame;
    }

    memcpy(data, &frame[6], content_len);
    if(data_len) *data_len = content_len;
    return FuriHalPn532ErrorNone;
}
