#include "furi_hal_pn532.h"
#include "furi_hal_i2c.h"
#include "furi_hal_nfc_i.h"
#include <furi_hal_cortex.h>
#include <stm32wbxx_ll_i2c.h>
#include <furi.h>
#include <string.h>

extern void furi_hal_i2c_bus_reset(I2C_TypeDef* i2c);

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
#define PN532_TXCONTROL_ENABLE    0x82  /* TX1RFEn | InitialRFOn (RFOff=0: keep RF on between commands) */

#define PN532_I2C_RETRIES      3
#define PN532_MAX_TX_PAYLOAD   255
#define PN532_MAX_RX_FRAME     270

static const uint8_t pn532_ack_frame[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
static bool pn532_ready = false;
#define PN532_I2C_ADDR PN532_I2C_ADDR_7BIT

/* Drain any stale data from the PN532 output buffer before a new write.
 * PN532 I2C slave NACKs writes while it has pending output data.
 * A 1-byte probe detects readiness; if ready, read the full 270-byte
 * buffer to flush stale frames (e.g. unread responses from a prior
 * aborted or partially-failed exchange). */
static void pn532_drain_output(void) {
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    uint8_t status = 0;
    bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, PN532_I2C_ADDR, &status, 1, 5);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);
    if(ok && status == PN532_I2C_READY) {
        uint8_t drain[PN532_MAX_RX_FRAME];
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        furi_hal_i2c_rx(&furi_hal_i2c_handle_power, PN532_I2C_ADDR, drain, sizeof(drain), 20);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        FURI_LOG_D(TAG, "drained %zu bytes of stale PN532 output", sizeof(drain));
    }
}

/* Note: furi_hal_i2c_bus_reset is NOT called on PN532 errors.
 * The 9 SCL pulse bus reset desyncs the PN532's I2C slave state machine,
 * causing permanent address NACK. With no hardware RST pin on this DIY board,
 * there is no way to recover once desynced. PN532 errors are treated as
 * transient timeouts and resolved by retrying the operation directly. */

static uint8_t pn532_i2c_addr = PN532_I2C_ADDR;

static bool pn532_wait_ready_ms(uint32_t timeout_ms) {
    /* NOTE: Do NOT check the abort flag on the very first attempt.
     * The FWT timer fires at ~5ms while InListPassiveTarget needs up to 150ms.
     * However, once we know the PN532 is still busy (at least one NACK seen),
     * we can exit early on abort to avoid a full timeout stall at shutdown.
     * pn532_ready is only set false on genuine hardware timeout, not on abort. */
    uint32_t start = furi_get_tick();
    for(uint8_t attempt = 0;; attempt++) {
        uint8_t status = 0;
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pn532_i2c_addr, &status, 1, 10);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        if(ok && status == PN532_I2C_READY) {
            FURI_LOG_D(TAG, "PN532 ready after %d retries", attempt);
            return true;
        }
        if((furi_get_tick() - start) >= timeout_ms) {
            if(furi_thread_flags_get() & FuriHalNfcEventInternalTypeAbort) {
                /* Timeout during a clean abort — PN532 hardware is fine */
                FURI_LOG_D(TAG, "PN532 wait ready timeout (abort pending, not a hw error)");
            } else {
                FURI_LOG_E(TAG, "PN532 wait ready timeout");
                pn532_ready = false;
            }
            return false;
        }
        /* Early exit on abort after first polling attempt to avoid full timeout stall */
        if(attempt > 0 && (furi_thread_flags_get() & FuriHalNfcEventInternalTypeAbort)) {
            FURI_LOG_D(TAG, "PN532 wait ready: abort early exit after %d attempts", attempt);
            return false;
        }
        furi_delay_ms(10); // 10ms poll interval — enough for PN532 RF response, 90% less I2C bus saturation vs 1ms
    }
}

static bool pn532_probe_address(void) {
    uint8_t status = 0;
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, PN532_I2C_ADDR, &status, 1, 10);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);
    if(ok) {
        pn532_i2c_addr = PN532_I2C_ADDR;
        FURI_LOG_I(TAG, "PN532 I2C addr detected: 0x%02X", pn532_i2c_addr);
        return true;
    }
    return false;
}

static bool pn532_write_frame(const uint8_t* cmd, size_t cmd_len) {
    if(cmd_len > PN532_MAX_TX_PAYLOAD) return false;

    uint8_t frame[PN532_MAX_TX_PAYLOAD + 10];
    uint8_t pos = 0;
    uint8_t checksum = 0;
    furi_check(cmd_len < PN532_MAX_TX_PAYLOAD - 1);
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

    /* Give PN532 2ms to switch from address-ACK mode to receive mode.
     * One I2C byte-time at 100kHz is ~90µs; 2ms is well above spec and
     * sufficient for clone boards with 4.7kΩ pull-ups to 3.3V. */
    furi_delay_us(2000); // PN532 requires ~2ms settling between I2C transactions

    /* Drain stale output before writing — avoids write-NACK when PN532
     * has pending data from a previous incomplete read. */
    pn532_drain_output();

    for(uint8_t attempt = 0; attempt < PN532_I2C_RETRIES; attempt++) {
        if(attempt > 0) {
            static const uint8_t ack_cancel[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
            furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
            furi_hal_i2c_tx(&furi_hal_i2c_handle_power, pn532_i2c_addr,
                            ack_cancel, sizeof(ack_cancel), 10);
            furi_hal_i2c_release(&furi_hal_i2c_handle_power);
            furi_delay_ms(10);
        }
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        bool ok = furi_hal_i2c_tx(
            &furi_hal_i2c_handle_power, pn532_i2c_addr, frame, pos, 100);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        if(ok) {
            return true;
        }
        FURI_LOG_W(TAG, "I2C TX retry %u/%u", attempt + 1, PN532_I2C_RETRIES);
        furi_delay_ms(50);
    }
    FURI_LOG_E(TAG, "TX frame failed after %u retries", PN532_I2C_RETRIES);
    return false;
}

static bool pn532_read_ack(void) {
    uint8_t buf[7] = {0};
    if(!pn532_wait_ready_ms(150)) {
        FURI_LOG_E(TAG, "pn532_read_ack: wait_ready failed");
        return false;
    }

    furi_delay_ms(1);

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = false;
    for(uint8_t retry = 0; retry < 3; retry++) {
        ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pn532_i2c_addr, buf, sizeof(buf), 100);
        if(!ok) {
            FURI_LOG_W(TAG, "pn532_read_ack: RX failed, retry %u/3", retry + 1);
            furi_delay_ms(5);
            continue;
        }
        if(buf[0] == PN532_I2C_READY) {
            break;
        }
        FURI_LOG_W(TAG, "pn532_read_ack: status 0x%02X != 0x01, retry %u/3", buf[0], retry + 1);
        furi_delay_ms(5);
    }
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);

    if(!ok || buf[0] != PN532_I2C_READY) {
        FURI_LOG_E(TAG, "pn532_read_ack: failed after retries, buf[0]=0x%02X ok=%d", buf[0], ok);
        return false;
    }
    bool match = memcmp(&buf[1], pn532_ack_frame, sizeof(pn532_ack_frame)) == 0;
    if(!match) {
        FURI_LOG_E(TAG, "pn532_read_ack: ACK mismatch");
        FURI_LOG_E(TAG, "  got:      %02X %02X %02X %02X %02X %02X", buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);
        FURI_LOG_E(TAG, "  expected: %02X %02X %02X %02X %02X %02X", pn532_ack_frame[0], pn532_ack_frame[1], pn532_ack_frame[2], pn532_ack_frame[3], pn532_ack_frame[4], pn532_ack_frame[5]);
    }
    return match;
}

static bool pn532_read_response(
    uint8_t* payload,
    size_t payload_size,
    size_t* out_len,
    uint32_t timeout_ms) {
    if(!pn532_wait_ready_ms(timeout_ms)) return false;

    furi_delay_ms(1);

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = false;
    uint8_t rx[PN532_MAX_RX_FRAME] = {0};

    /* Read the full response in a single transaction to prevent PN532 output restart */
    for(uint8_t retry = 0; retry < 3; retry++) {
        ok = furi_hal_i2c_rx(
            &furi_hal_i2c_handle_power, pn532_i2c_addr, rx, sizeof(rx), 150);
        if(ok && rx[0] == PN532_I2C_READY) break;
        FURI_LOG_W(TAG, "pn532_read_response: retry %u/3 ok=%d status=0x%02X",
                   retry + 1, ok, rx[0]);
        furi_delay_ms(5);
    }
    
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);

    if(!ok || rx[0] != PN532_I2C_READY) {
        return false;
    }
    
    const uint8_t* frame = &rx[1];
    
    if(frame[0] != PN532_PREAMBLE || frame[1] != PN532_STARTCODE1 || frame[2] != PN532_STARTCODE2) {
        FURI_LOG_E(TAG, "pn532_read_response: bad header %02X %02X %02X", frame[0], frame[1], frame[2]);
        return false;
    }
    
    uint8_t len = frame[3];
    uint8_t lcs = frame[4];
    if((uint8_t)(len + lcs) != 0) {
        FURI_LOG_E(TAG, "pn532_read_response: bad LCS %02X + %02X", len, lcs);
        return false;
    }
    
    if(len < 2) {
        return false;
    }


    if(frame[5] != PN532_PN532TOHOST) {
        FURI_LOG_E(TAG, "pn532_read_response: bad body[0]=0x%02X (expected 0xD5)", frame[5]);
        return false;
    }

    size_t content_len = len - 1;
    if(content_len > payload_size) {
        FURI_LOG_E(TAG, "pn532_read_response: content_len (%zu) > payload_size (%zu)", content_len, payload_size);
        return false;
    }

    uint8_t dcs = frame[5 + len];
    uint8_t checksum = 0;
    for(size_t i = 0; i < len; i++) {
        checksum += frame[5 + i];
    }
    if((uint8_t)(checksum + dcs) != 0) {
        FURI_LOG_E(TAG, "DCS checksum failure");
        return false;
    }

    if(frame[6 + len] != PN532_POSTAMBLE) return false;

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
    if(!pn532_write_frame(cmd, sizeof(cmd))) {
        FURI_LOG_W(TAG, "WriteRegister 0x%04X write failed", reg);
        return false;
    }
    if(!pn532_read_ack()) {
        uint8_t dummy_buf[PN532_MAX_RX_FRAME];
        size_t dummy_len;
        pn532_read_response(dummy_buf, sizeof(dummy_buf), &dummy_len, 50);
        FURI_LOG_W(TAG, "WriteRegister 0x%04X ACK failed", reg);
        return false;
    }
    /* Read and discard response */
    uint8_t dummy[8];
    size_t dummy_len = 0;
    if(!pn532_read_response(dummy, sizeof(dummy), &dummy_len, 50)) {
        return false;
    }
    return true;
}

bool furi_hal_pn532_init(void) {
    pn532_ready = false;
    /* Clear any stale abort flag left from a previous NFC session */
    furi_thread_flags_clear(FuriHalNfcEventInternalTypeAbort);
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

    uint8_t dummy[8];
    size_t dummy_len = 0;

    /* RFConfiguration: Enable RF field (RFCFG_FIELD=1) — critical for clone modules */
    uint8_t cmd_rf_on[] = {PN532_CMD_RF_CONFIGURATION, 0x01, 0x01};
    if(!pn532_write_frame(cmd_rf_on, sizeof(cmd_rf_on))) {
        FURI_LOG_W(TAG, "RFConfiguration FIELD_ON write failed");
    } else if(!pn532_read_ack()) {
        pn532_read_response(dummy, sizeof(dummy), &dummy_len, 50);
        FURI_LOG_W(TAG, "RFConfiguration FIELD_ON ACK failed");
    } else {
        pn532_read_response(dummy, sizeof(dummy), &dummy_len, 100);
        FURI_LOG_I(TAG, "RFConfiguration FIELD_ON applied");
    }

    /* RFConfiguration: Max retries for clone module reliability
     * MxRtyATR=0xFF (not used, P2P only), MxRtyPSL=0x01, MxRtyPassiveActivation=0x05
     * PassiveActivation must NOT be 0xFF — infinite retries block the I2C bus for the
     * full 150ms poll timeout on every "no tag found" result. 5 retries is sufficient. */
    uint8_t cmd_retry[] = {PN532_CMD_RF_CONFIGURATION, 0x05, 0xFF, 0x01, 0x05};
    if(!pn532_write_frame(cmd_retry, sizeof(cmd_retry))) {
        FURI_LOG_W(TAG, "RFConfiguration retries config write failed");
    } else if(!pn532_read_ack()) {
        pn532_read_response(dummy, sizeof(dummy), &dummy_len, 50);
        FURI_LOG_W(TAG, "RFConfiguration retries config ACK failed");
    } else {
        pn532_read_response(dummy, sizeof(dummy), &dummy_len, 100);
        FURI_LOG_I(TAG, "RFConfiguration max retries applied");
    }
    furi_delay_ms(50); // Allow RF field to stabilize after RFConfiguration

    /* Enable RF output drivers (critical for clone modules) */
    /* CIU_TxControl: TX1RFEn | TX2RFEn | InitialRFOn */
    if(!pn532_write_register(PN532_REG_CIU_TxControl, PN532_TXCONTROL_ENABLE)) {
        FURI_LOG_W(TAG, "TxControl write failed, continuing anyway");
    } else {
        FURI_LOG_I(TAG, "RF output drivers enabled");
    }
    furi_delay_ms(50); // Increased from 10ms for clone board stabilization

    uint8_t post_init_status = 0;
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool status_ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pn532_i2c_addr, &post_init_status, 1, 10);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);
    FURI_LOG_I(TAG, "Post-init PN532 status: 0x%02X (ok=%d)", post_init_status, status_ok);

    pn532_ready = true;
    FURI_LOG_I(TAG, "PN532 initialized over I2C");
    return true;
}

bool furi_hal_pn532_is_ready(void) {
    return pn532_ready;
}

bool furi_hal_pn532_read_status(void) {
    if(!pn532_ready) return false;

    uint8_t status = 0;
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pn532_i2c_addr, &status, 1, 10);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);

    return ok && (status == PN532_I2C_READY);
}

static FuriHalPn532Error pn532_exchange(
    const uint8_t* cmd,
    size_t cmd_len,
    uint8_t expected_response,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len,
    uint32_t timeout_ms) {
    if(!pn532_ready && !furi_hal_pn532_init()) {
        FURI_LOG_E(TAG, "pn532_exchange: PN532 not ready and init failed");
        return FuriHalPn532ErrorComm;
    }

    if(furi_thread_flags_get() & FuriHalNfcEventInternalTypeAbort) {
        FURI_LOG_W(TAG, "pn532_exchange: abort set, giving up");
        return FuriHalPn532ErrorComm;
    }

     if(!pn532_write_frame(cmd, cmd_len)) {
         FURI_LOG_W(TAG, "pn532_exchange: write failed");
         return FuriHalPn532ErrorComm;
     }

    if(!pn532_read_ack()) {
        FURI_LOG_W(TAG, "pn532_exchange: ACK failed");
        pn532_ready = false;
        return FuriHalPn532ErrorComm;
    }

     size_t payload_len = 0;
     if(!pn532_read_response(rx_data, rx_size, &payload_len, timeout_ms)) {
         if(furi_thread_flags_get() & FuriHalNfcEventInternalTypeAbort) {
             /* Clean abort — the PN532 hardware is still alive.
              * Do NOT set pn532_ready = false here: that would make
              * furi_hal_nfc_pn532_is_active() return false and corrupt the
              * NFC worker state machine while it is still running.
              * Return Comm (not Timeout) so the caller treats this as an
              * interrupted exchange rather than a hardware error. */
             FURI_LOG_D(TAG, "pn532_exchange: aborted mid-exchange (clean stop)");
             return FuriHalPn532ErrorComm;
         }
         FURI_LOG_W(TAG, "pn532_exchange: response timeout (hardware)");
         return FuriHalPn532ErrorTimeout;
     }

    if((payload_len == 0) || (rx_data[0] != expected_response)) {
        FURI_LOG_W(TAG, "pn532_exchange: bad response[0]=0x%02X expected=0x%02X",
            rx_data[0], expected_response);
        return FuriHalPn532ErrorInvalidFrame;
    }

    if(rx_len) *rx_len = payload_len;
    return FuriHalPn532ErrorNone;
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
            250);
    if(error != FuriHalPn532ErrorNone) return false;
    if(response_len < 2) {
        FURI_LOG_W(TAG, "poll_iso14443a: response too short=%zu", response_len);
        return false;
    }
    if(response[1] == 0) {
        FURI_LOG_W(TAG, "poll_iso14443a: no targets found");
        return false;
    }
    if(response_len < 7) return false;
    if(!target) return false;

    FURI_LOG_D(TAG, "poll_iso14443a: Tg=%d ATQA=%02X%02X SAK=%02X UIDlen=%d",
        response[2], response[3], response[4], response[5], response[6]);
    target->target_number = response[2];
    /* NOTE: byte order swap.
     * PN532 delivers ATQA big-endian (high byte first = response[3],
     * low byte = response[4]).  The MIFARE Classic detect and type
     * handlers expect little-endian (atqa[0]=low, atqa[1]=high).
     * Swap here so all downstream consumers see the correct byte order. */
    target->atqa[0] = response[4];
    target->atqa[1] = response[3];
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
    if(response_len < 11) return false;
    if(!target) return false;
    
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
    if((response_len < 15) || !target) return true;

    // Type B response: [cmd+1][NbTg][Tg][ATQB(12 bytes minimum)]
    target->target_number = response[2];
    // Store PUPI (4 bytes) as UID — bytes 4..7 of ATQB, i.e. response[6..9]
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
    if(tx_len > PN532_MAX_TX_PAYLOAD - 3) return FuriHalPn532ErrorComm;

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
            1000);
    if(error != FuriHalPn532ErrorNone) return error;
    if(response_len < 2) return FuriHalPn532ErrorInvalidFrame;
    if(response[1] != 0x00) {
        FURI_LOG_W(TAG, "in_data_exchange status error: 0x%02X", response[1]);
        if(response[1] == 0x01) return FuriHalPn532ErrorTimeout; // Timeout / Target removed
        if(response[1] == 0x14) return FuriHalPn532ErrorInvalidFrame; // MIFARE auth error
        return FuriHalPn532ErrorComm;
    }

    const size_t payload_len = response_len - 2;
    if(payload_len > rx_size) return FuriHalPn532ErrorComm;
    if(payload_len) memcpy(rx_data, &response[2], payload_len);
    if(rx_len) *rx_len = payload_len;
    return FuriHalPn532ErrorNone;
}

FuriHalPn532Error furi_hal_pn532_in_communicate_thru(
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len) {
    if(tx_len > PN532_MAX_TX_PAYLOAD - 2) return FuriHalPn532ErrorComm;

    uint8_t cmd[PN532_MAX_TX_PAYLOAD + 2] = {0};
    cmd[0] = 0x42;
    if(tx_len) memcpy(&cmd[1], tx_data, tx_len);

    uint8_t response[PN532_MAX_RX_FRAME] = {0};
    size_t response_len = 0;
    FuriHalPn532Error error =
        pn532_exchange(
            cmd,
            tx_len + 1,
            0x43,
            response,
            sizeof(response),
            &response_len,
            250);
    if(error != FuriHalPn532ErrorNone) return error;
    if(response_len < 2) return FuriHalPn532ErrorInvalidFrame;
    if(response[1] != 0x00) {
        FURI_LOG_W(TAG, "in_communicate_thru status error: 0x%02X", response[1]);
        if(response[1] == 0x01) return FuriHalPn532ErrorTimeout;
        return FuriHalPn532ErrorComm;
    }

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
    furi_check(key);
    furi_check(uid);

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

    if(err == FuriHalPn532ErrorNone && resp_len >= 2) {
        if(resp[1] == 0x00) {
            return FuriHalPn532ErrorNone;
        }
        if(resp[1] == 0x14) {
            return FuriHalPn532ErrorInvalidFrame;
        }
    }

    return FuriHalPn532ErrorComm;
}

FuriHalPn532Error furi_hal_pn532_send_command(const uint8_t* cmd, size_t cmd_len) {
    if(!pn532_ready && !furi_hal_pn532_init()) return FuriHalPn532ErrorComm;
    if(cmd_len == 0) return FuriHalPn532ErrorNone;
    
    if(!pn532_write_frame(cmd, cmd_len)) return FuriHalPn532ErrorComm;
    if(!pn532_read_ack()) {
        pn532_ready = false;
        return FuriHalPn532ErrorInvalidAck;
    }
    return FuriHalPn532ErrorNone;
}

FuriHalPn532Error furi_hal_pn532_read_response(uint8_t* data, size_t data_size, size_t* data_len, uint32_t timeout_ms) {
    if(!pn532_ready) return FuriHalPn532ErrorComm;

    bool success = pn532_read_response(data, data_size, data_len, timeout_ms > 0 ? timeout_ms : 500);
    if(!success) {
        return FuriHalPn532ErrorComm;
    }

    return FuriHalPn532ErrorNone;
}
