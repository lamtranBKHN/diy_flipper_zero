#include "furi_hal_nfc_pn532.h"

#include "furi_hal_pn532.h"
#include "furi_hal_nfc_i.h"
#include <furi.h>
#include <string.h>

#define TAG "FuriHalNfcPn532"

/* MIFARE Classic auth timeout via InCommunicateThru.
 * Card responds to auth (NT) in ~5ms.  PN532 internal auth timeout is ~65ms.
 * 80ms gives 15ms margin over PN532's internal processing.  Lower values
 * cause PN532 wait_ready timeout crash (30ms was too aggressive). */
#define PN532_TIMEOUT_MF_AUTH_MS 80

#define PN532_NFCA_CT              0x88
#define PN532_EVENT_QUEUE_CAPACITY 8
#define PN532_MAX_FRAME_SIZE       192

#define PN532_TARGET_FRESHNESS_TIMEOUT_MS 5000

typedef struct {
    bool active;
    bool target_valid;
    bool listener_active;
    FuriHalNfcMode mode;
    FuriHalNfcTech tech;
    FuriHalPn532Target target;
    uint32_t target_tick;
    uint8_t rx_buffer[PN532_MAX_FRAME_SIZE];
    uint8_t scratch[PN532_MAX_FRAME_SIZE];
    size_t rx_bits;
    FuriHalNfcEvent event_queue[PN532_EVENT_QUEUE_CAPACITY];
    size_t event_head;
    size_t event_count;
    FuriHalPn532Error last_error;
    FuriHalNfcPn532Result last_result;
    uint32_t last_error_tick;
    uint8_t mf_auth_key[6];
    uint8_t mf_auth_key_type;
    bool mf_auth_key_valid;
    uint8_t cached_ats[20];
    size_t cached_ats_len;
    bool iso_dep_mode;
    bool needs_relist;
    uint8_t iso_dep_block_num; /**< Block number toggle for I-block responses */
    bool mf_authed; /**< PN532 native MIFARE Classic auth active */

    /* Listener (target) mode configuration — set via set_col_res_data */
    uint8_t listener_uid[10];
    uint8_t listener_uid_len;
    uint8_t listener_atqa[2];
    uint8_t listener_sak;
    bool listener_configured; /**< true after set_col_res_data called */
} FuriHalNfcPn532State;

static FuriHalNfcPn532State furi_hal_nfc_pn532 = {0};

static bool furi_hal_nfc_pn532_queue_pop(FuriHalNfcEvent* event);

static void furi_hal_nfc_pn532_set_result(FuriHalNfcPn532Result result) {
    furi_hal_nfc_pn532.last_result = result;
}

static FuriHalNfcPn532Result
    furi_hal_nfc_pn532_result_from_error(FuriHalPn532Error err, bool response_ready) {
    if(response_ready) return FuriHalNfcPn532ResultDetected;

    switch(err) {
    case FuriHalPn532ErrorNone:
    case FuriHalPn532ErrorTimeout:
        return FuriHalNfcPn532ResultNotPresent;
    case FuriHalPn532ErrorInvalidAck:
    case FuriHalPn532ErrorComm:
        return FuriHalNfcPn532ResultCommunicationError;
    case FuriHalPn532ErrorInvalidFrame:
    case FuriHalPn532ErrorBufferOverflow:
        return FuriHalNfcPn532ResultParseError;
    case FuriHalPn532ErrorUnsupported:
        return FuriHalNfcPn532ResultUnsupportedByPn532;
    default:
        return FuriHalNfcPn532ResultCommunicationError;
    }
}

static void furi_hal_nfc_pn532_queue_reset(void) {
    FURI_CRITICAL_ENTER();
    furi_hal_nfc_pn532.event_head = 0;
    furi_hal_nfc_pn532.event_count = 0;
    FURI_CRITICAL_EXIT();
}

static bool furi_hal_nfc_pn532_queue_push(FuriHalNfcEvent event) {
    bool ok = false;
    FURI_CRITICAL_ENTER();
    if(furi_hal_nfc_pn532.event_count < PN532_EVENT_QUEUE_CAPACITY) {
        const size_t index = (furi_hal_nfc_pn532.event_head + furi_hal_nfc_pn532.event_count) %
                             PN532_EVENT_QUEUE_CAPACITY;
        furi_hal_nfc_pn532.event_queue[index] = event;
        furi_hal_nfc_pn532.event_count++;
        ok = true;
    }
    FURI_CRITICAL_EXIT();
    return ok;
}

static bool furi_hal_nfc_pn532_queue_pop(FuriHalNfcEvent* event) {
    bool ok = false;
    FURI_CRITICAL_ENTER();
    if(furi_hal_nfc_pn532.event_count > 0) {
        *event = furi_hal_nfc_pn532.event_queue[furi_hal_nfc_pn532.event_head];
        furi_hal_nfc_pn532.event_head =
            (furi_hal_nfc_pn532.event_head + 1) % PN532_EVENT_QUEUE_CAPACITY;
        furi_hal_nfc_pn532.event_count--;
        ok = true;
    }
    FURI_CRITICAL_EXIT();
    return ok;
}

static uint16_t furi_hal_nfc_pn532_crc_a(const uint8_t* data, size_t size) {
    uint16_t crc = 0x6363U;

    for(size_t i = 0; i < size; i++) {
        uint8_t byte = data[i];
        byte ^= (uint8_t)(crc & 0xFFU);
        byte ^= (uint8_t)(byte << 4);
        crc = (crc >> 8) ^ (((uint16_t)byte) << 8) ^ (((uint16_t)byte) << 3) ^ (byte >> 4);
    }

    return crc;
}

static bool furi_hal_nfc_pn532_odd_parity(uint8_t byte) {
    bool parity = true;
    for(size_t i = 0; i < 8; i++) {
        parity ^= ((byte >> i) & 0x01U) != 0;
    }
    return parity;
}

static uint8_t furi_hal_nfc_pn532_get_byte_from_bits(const uint8_t* data, size_t bit_index) {
    const size_t byte_index = bit_index / 8;
    const size_t bit_offset = bit_index % 8;
    uint8_t value = data[byte_index] >> bit_offset;
    if(bit_offset != 0) {
        value |= data[byte_index + 1] << (8 - bit_offset);
    }
    return value;
}

static size_t furi_hal_nfc_pn532_unpack_parity_frame(
    const uint8_t* tx_data,
    size_t tx_bits,
    uint8_t* out_data,
    size_t out_capacity) {
    if((tx_bits % 9U) != 0) return 0;

    const size_t byte_count = tx_bits / 9U;
    if(byte_count > out_capacity) return 0;

    size_t bit_index = 0;
    for(size_t i = 0; i < byte_count; i++) {
        out_data[i] = furi_hal_nfc_pn532_get_byte_from_bits(tx_data, bit_index);
        bit_index += 9;
    }

    return byte_count;
}

static bool furi_hal_nfc_pn532_prepare_rx(
    const uint8_t* data,
    size_t data_len,
    bool append_crc,
    bool add_parity) {
    /* BUG FIX (NTAG zero data): exchange_internal() passes rx_payload which IS
     * furi_hal_nfc_pn532.scratch.  The original code did memset(scratch, 0)
     * BEFORE copying data from scratch into scratch — zeroing the response data
     * before it could be read.  Fix: if data overlaps scratch, copy to a local
     * staging buffer first so the memset does not destroy the source. */
    uint8_t staging[PN532_MAX_FRAME_SIZE];
    if(data_len > 0 && data_len <= PN532_MAX_FRAME_SIZE &&
       data >= furi_hal_nfc_pn532.scratch &&
       data < furi_hal_nfc_pn532.scratch + sizeof(furi_hal_nfc_pn532.scratch)) {
        memcpy(staging, data, data_len);
        data = staging;
    }

    memset(furi_hal_nfc_pn532.scratch, 0, sizeof(furi_hal_nfc_pn532.scratch));
    uint8_t* const frame = furi_hal_nfc_pn532.scratch;
    size_t frame_len = data_len;
    if(frame_len > PN532_MAX_FRAME_SIZE) return false;

    if(frame_len) memcpy(frame, data, frame_len);

    if(append_crc) {
        if((frame_len + sizeof(uint16_t)) > PN532_MAX_FRAME_SIZE) return false;
        const uint16_t crc = furi_hal_nfc_pn532_crc_a(frame, frame_len);
        memcpy(&frame[frame_len], &crc, sizeof(crc));
        frame_len += sizeof(crc);
    }

    memset(furi_hal_nfc_pn532.rx_buffer, 0, sizeof(furi_hal_nfc_pn532.rx_buffer));

    if(add_parity) {
        if(frame_len > (sizeof(furi_hal_nfc_pn532.rx_buffer) * 8U / 9U)) return false;
        size_t bit_pos = 0;
        for(size_t i = 0; i < frame_len; i++) {
            const uint8_t byte = frame[i];
            const bool parity = furi_hal_nfc_pn532_odd_parity(byte);
            const size_t byte_index = bit_pos / 8U;
            const size_t bit_offset = bit_pos % 8U;

            furi_hal_nfc_pn532.rx_buffer[byte_index] |= byte << bit_offset;
            if(bit_offset != 0) {
                furi_hal_nfc_pn532.rx_buffer[byte_index + 1] |= byte >> (8U - bit_offset);
            }
            bit_pos += 8;

            const size_t parity_index = bit_pos / 8U;
            const size_t parity_offset = bit_pos % 8U;
            if(parity) {
                furi_hal_nfc_pn532.rx_buffer[parity_index] |= 1U << parity_offset;
            }
            bit_pos += 1;
        }
        furi_hal_nfc_pn532.rx_bits = frame_len * 9U;
    } else {
        memcpy(furi_hal_nfc_pn532.rx_buffer, frame, frame_len);
        furi_hal_nfc_pn532.rx_bits = frame_len * 8U;
    }

    return true;
}

static bool furi_hal_nfc_pn532_build_cascade_response(
    uint8_t cascade_level,
    uint8_t nfcid[4],
    uint8_t* sak) {
    const size_t uid_len = furi_hal_nfc_pn532.target.uid_len;
    const uint8_t* uid = furi_hal_nfc_pn532.target.uid;

    if(uid_len <= 4U) {
        if(cascade_level != 0U) return false;
        memcpy(nfcid, uid, 4U);
        *sak = furi_hal_nfc_pn532.target.sak;
        return true;
    }

    if(uid_len <= 7U) {
        if(cascade_level == 0U) {
            nfcid[0] = PN532_NFCA_CT;
            memcpy(&nfcid[1], uid, 3U);
            *sak = 0x04;
            return true;
        } else if(cascade_level == 1U) {
            memcpy(nfcid, &uid[3], 4U);
            *sak = furi_hal_nfc_pn532.target.sak;
            return true;
        }
        return false;
    }

    if(uid_len <= 10U) {
        if(cascade_level == 0U) {
            nfcid[0] = PN532_NFCA_CT;
            memcpy(&nfcid[1], uid, 3U);
            *sak = 0x04;
            return true;
        } else if(cascade_level == 1U) {
            nfcid[0] = PN532_NFCA_CT;
            memcpy(&nfcid[1], &uid[3], 3U);
            *sak = 0x04;
            return true;
        } else if(cascade_level == 2U) {
            memcpy(nfcid, &uid[6], 4U);
            *sak = furi_hal_nfc_pn532.target.sak;
            return true;
        }
    }

    return false;
}

static FuriHalNfcError
    furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532Error err, bool response_ready) {
    furi_hal_nfc_pn532_queue_push(FuriHalNfcEventTxEnd);
    furi_hal_nfc_pn532_set_result(furi_hal_nfc_pn532_result_from_error(err, response_ready));
    if(err != FuriHalPn532ErrorNone) {
        furi_hal_nfc_pn532.last_error = err;
        furi_hal_nfc_pn532.last_error_tick = furi_get_tick();
        FURI_LOG_D(TAG, "Exchange error: %s (%d)", furi_hal_nfc_pn532_last_error_str(), err);
    }
    if(response_ready) {
        furi_hal_nfc_pn532_queue_push(FuriHalNfcEventRxStart);
        furi_hal_nfc_pn532_queue_push(FuriHalNfcEventRxEnd);
        return FuriHalNfcErrorNone;
    }

    if(err == FuriHalPn532ErrorTimeout) {
        furi_hal_nfc_pn532.rx_bits = 0;
        return FuriHalNfcErrorNone;
    }

    return (err == FuriHalPn532ErrorNone) ? FuriHalNfcErrorNone : FuriHalNfcErrorCommunication;
}

bool furi_hal_nfc_pn532_backend_init(void) {
    if(furi_hal_nfc_pn532.active && furi_hal_pn532_is_ready()) return true;

    furi_hal_nfc_pn532.active = furi_hal_pn532_init();
    furi_hal_nfc_pn532_reset();
    return furi_hal_nfc_pn532.active;
}

bool furi_hal_nfc_pn532_is_active(void) {
    return furi_hal_nfc_pn532.active && furi_hal_pn532_is_ready();
}

/* Send InRelease and consume the response so it does not linger in the
 * PN532 I2C output buffer and confuse the next operation.
 * The response is consumed unconditionally to guarantee no stale data
 * remains even if send_command encountered a transient I2C fault.
 *
 * InRelease is best-effort cleanup.  A transient ACK failure here does NOT
 * mean the PN532 is dead — it usually means the chip had a stale response
 * in its output buffer (from a previous InCommunicateThru timeout) that
 * pn532_drain_output() already consumed.  We use furi_hal_pn532_in_release()
 * which uses pn532_exchange() internally (two-strikes-out ACK mechanism)
 * and will NOT set pn532_ready=false on a single transient failure. */
static void pn532_send_inrelease(void) {
    furi_hal_pn532_in_release();
}

void furi_hal_nfc_pn532_reset(void) {
    // Release PN532 target handle to avoid stale state between protocol detects
    pn532_send_inrelease();
    /* Stop the background I2C poller thread to avoid I2C bus traffic
     * and RTOS thread overhead when the NFC app is idle/closing. */
    furi_hal_pn532_irq_stop();
    furi_hal_nfc_pn532.target_valid = false;
    furi_hal_nfc_pn532.mf_authed = false;
    furi_hal_nfc_pn532.listener_active = false;
    furi_hal_nfc_pn532.listener_configured = false;
    furi_hal_nfc_pn532.listener_uid_len = 0;
    memset(furi_hal_nfc_pn532.listener_uid, 0, sizeof(furi_hal_nfc_pn532.listener_uid));
    memset(furi_hal_nfc_pn532.listener_atqa, 0, sizeof(furi_hal_nfc_pn532.listener_atqa));
    furi_hal_nfc_pn532.listener_sak = 0;
    furi_hal_nfc_pn532.mode = FuriHalNfcModeNum;
    furi_hal_nfc_pn532.tech = FuriHalNfcTechInvalid;
    furi_hal_nfc_pn532.target_tick = 0;
    furi_hal_nfc_pn532.rx_bits = 0;
    memset(&furi_hal_nfc_pn532.target, 0, sizeof(furi_hal_nfc_pn532.target));
    memset(furi_hal_nfc_pn532.rx_buffer, 0, sizeof(furi_hal_nfc_pn532.rx_buffer));
    furi_hal_nfc_pn532_queue_reset();
    furi_hal_nfc_pn532.last_error = FuriHalPn532ErrorNone;
    furi_hal_nfc_pn532.last_result = FuriHalNfcPn532ResultNotPresent;
    furi_hal_nfc_pn532.last_error_tick = 0;
    furi_hal_nfc_pn532.mf_auth_key_valid = false;
}

FuriHalNfcError furi_hal_nfc_pn532_set_mode(FuriHalNfcMode mode, FuriHalNfcTech tech) {
    if(!furi_hal_nfc_pn532_is_active()) return FuriHalNfcErrorCommunication;
    /* PN532 supports Poller mode for A/B/FeliCa, and Listener mode for
     * ISO14443A (target/emulation). Other techs in listener mode are rejected. */
    if(mode == FuriHalNfcModeListener && tech != FuriHalNfcTechIso14443a &&
       tech != FuriHalNfcTechFelica) {
        FURI_LOG_W(TAG, "set_mode: listener mode only supported for ISO14443A/FeliCa on PN532");
        return FuriHalNfcErrorCommunication;
    }
    if(tech >= FuriHalNfcTechNum) {
        return FuriHalNfcErrorCommunication;
    }

    /* Skip full reinit when mode and tech are unchanged — preserves SAM &
     * RF configuration across repeated protocol detection cycles (e.g.
     * supported-card plugin iteration).  Only release the previous target
     * and clear volatile state. */
    if(mode == furi_hal_nfc_pn532.mode && tech == furi_hal_nfc_pn532.tech) {
        pn532_send_inrelease();
        furi_hal_nfc_pn532.target_valid = false;
        furi_hal_nfc_pn532.mf_authed = false;
        furi_hal_nfc_pn532.target_tick = 0;
        furi_hal_nfc_pn532.rx_bits = 0;
        memset(&furi_hal_nfc_pn532.target, 0, sizeof(furi_hal_nfc_pn532.target));
        memset(furi_hal_nfc_pn532.rx_buffer, 0, sizeof(furi_hal_nfc_pn532.rx_buffer));
        furi_hal_nfc_pn532_queue_reset();
        furi_hal_nfc_pn532.last_error = FuriHalPn532ErrorNone;
        furi_hal_nfc_pn532.last_result = FuriHalNfcPn532ResultNotPresent;
        furi_hal_nfc_pn532.last_error_tick = 0;
        furi_hal_nfc_pn532.mf_auth_key_valid = false;
        /* Clear remaining per-session volatile state.  Without this, a
         * previous session's needs_relist / iso_dep_mode / iso_dep_block_num
         * / cached_ats_len leak into the new session and cause the
         * exchange path to take incorrect interception branches:
         *  - stale cached_ats_len triggers spurious PPS synthesis,
         *  - stale iso_dep_mode pushes plain frames into the I-block
         *    stripping branch,
         *  - stale needs_relist forces a 20ms WUPA on a freshly-listed
         *    target. */
        furi_hal_nfc_pn532.needs_relist = false;
        furi_hal_nfc_pn532.iso_dep_mode = false;
        furi_hal_nfc_pn532.iso_dep_block_num = 0;
        furi_hal_nfc_pn532.cached_ats_len = 0;
        /* Restart the IRQ poller thread if it was stopped by a previous
         * furi_hal_nfc_pn532_reset() call (e.g. from a mode/tech change
         * during supported-card plugin iteration).  Without this, the fast
         * path leaves pn532_irq_event == NULL, forcing pn532_wait_ready_ms()
         * into the blocking-poll fallback.  That fallback sets pn532_ready=false
         * on any timeout, which triggers a full PN532 reinit mid-session and
         * causes the dict-attack poller to crash/freeze. */
        furi_hal_pn532_irq_start();
    } else {
        furi_hal_nfc_pn532_reset();
        furi_hal_nfc_pn532.mode = mode;
        furi_hal_nfc_pn532.tech = tech;
        /* Restart the IRQ poller thread after a full reset so that
         * pn532_wait_ready_ms() uses the event-flag path (not blocking poll)
         * for all subsequent commands in this session. */
        furi_hal_pn532_irq_start();
    }
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_pn532_low_power_mode_start(void) {
    if(!furi_hal_nfc_pn532_is_active()) return FuriHalNfcErrorCommunication;
    // Deinit timers to save power — they'll be re-initialized on mode_stop
    furi_hal_nfc_timers_deinit();
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_pn532_low_power_mode_stop(void) {
    if(!furi_hal_nfc_pn532_is_active()) return FuriHalNfcErrorCommunication;
    // nfc.c poller uses TIM1/TIM17 for frame timing even in PN532 mode
    furi_hal_nfc_timers_init();
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_pn532_field_detect_start(void) {
    return furi_hal_nfc_pn532_is_active() ? FuriHalNfcErrorNone : FuriHalNfcErrorCommunication;
}

FuriHalNfcError furi_hal_nfc_pn532_field_detect_stop(void) {
    return furi_hal_nfc_pn532_is_active() ? FuriHalNfcErrorNone : FuriHalNfcErrorCommunication;
}

bool furi_hal_nfc_pn532_field_is_present(void) {
    return furi_hal_nfc_pn532_is_active();
}

FuriHalNfcError furi_hal_nfc_pn532_poller_field_on(void) {
    return furi_hal_nfc_pn532_is_active() ? FuriHalNfcErrorNone : FuriHalNfcErrorCommunication;
}

FuriHalNfcEvent furi_hal_nfc_pn532_wait_event(uint32_t timeout_ms) {
    /* Check for abort before clearing flags */
    if(furi_thread_flags_get() & FuriHalNfcEventInternalTypeAbort) {
        furi_thread_flags_clear(FuriHalNfcEventInternalTypeAbort);
        furi_hal_nfc_pn532_queue_reset();
        return FuriHalNfcEventAbortRequest;
    }

    FuriHalNfcEvent event = 0;

    if(furi_hal_nfc_pn532_queue_pop(&event)) {
        return event;
    }

    const uint32_t wait_timeout = timeout_ms == FURI_HAL_NFC_EVENT_WAIT_FOREVER ? 100 : timeout_ms;
    const uint32_t event_flag = furi_thread_flags_wait(
        FuriHalNfcEventInternalTypeAbort | FuriHalNfcEventInternalTypeTimerFwtExpired |
            FuriHalNfcEventInternalTypeTimerBlockTxExpired,
        FuriFlagWaitAny,
        wait_timeout);

    if(event_flag == (unsigned)FuriFlagErrorTimeout) {
        return FuriHalNfcEventTimeout;
    }

    if(event_flag & FuriHalNfcEventInternalTypeTimerFwtExpired) {
        event |= FuriHalNfcEventTimerFwtExpired;
        furi_thread_flags_clear(FuriHalNfcEventInternalTypeTimerFwtExpired);
    }
    if(event_flag & FuriHalNfcEventInternalTypeTimerBlockTxExpired) {
        event |= FuriHalNfcEventTimerBlockTxExpired;
        furi_thread_flags_clear(FuriHalNfcEventInternalTypeTimerBlockTxExpired);
    }
    if(event_flag & FuriHalNfcEventInternalTypeAbort) {
        event |= FuriHalNfcEventAbortRequest;
        furi_hal_nfc_pn532_queue_reset();
        furi_thread_flags_clear(FuriHalNfcEventInternalTypeAbort);
    }

    return event;
}

FuriHalNfcError furi_hal_nfc_pn532_trx_reset(void) {
    if(!furi_hal_nfc_pn532_is_active()) return FuriHalNfcErrorCommunication;
    furi_hal_nfc_pn532.rx_bits = 0;
    memset(furi_hal_nfc_pn532.rx_buffer, 0, sizeof(furi_hal_nfc_pn532.rx_buffer));
    furi_hal_nfc_pn532_queue_reset();
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_pn532_trx_short_frame(FuriHalNfcaShortFrame frame) {
    UNUSED(frame);
    if(!furi_hal_nfc_pn532_is_active()) return FuriHalNfcErrorCommunication;

    if(furi_hal_nfc_acquire() != FuriHalNfcErrorNone) {
        return FuriHalNfcErrorBusy;
    }

    FuriHalNfcError ret = FuriHalNfcErrorNone;
    bool use_cached = false;
    if(furi_hal_nfc_pn532.target_valid && furi_hal_nfc_pn532.tech == FuriHalNfcTechIso14443a) {
        int32_t age = (int32_t)(furi_get_tick() - furi_hal_nfc_pn532.target_tick);
        use_cached = (age >= 0 && age < (int32_t)PN532_TARGET_FRESHNESS_TIMEOUT_MS);
    }

    if(use_cached) {
        FURI_LOG_D(
            TAG,
            "trx_short_frame: using cached target (age=%ldms)",
            (int32_t)(furi_get_tick() - furi_hal_nfc_pn532.target_tick));
    } else {
        FuriHalPn532Target target = {0};
        const bool found = furi_hal_pn532_poll_iso14443a(&target);
        furi_hal_nfc_pn532.target = target;
        furi_hal_nfc_pn532.target_valid = found;
        if(found) {
            furi_hal_nfc_pn532.target_tick = furi_get_tick();
        }
        if(found && target.iso_dep_active && target.ats_len > 0) {
            furi_hal_nfc_pn532.cached_ats_len = target.ats_len;
            if(furi_hal_nfc_pn532.cached_ats_len > sizeof(furi_hal_nfc_pn532.cached_ats)) {
                furi_hal_nfc_pn532.cached_ats_len = sizeof(furi_hal_nfc_pn532.cached_ats);
            }
            memcpy(furi_hal_nfc_pn532.cached_ats, target.ats, furi_hal_nfc_pn532.cached_ats_len);
            furi_hal_nfc_pn532.iso_dep_mode = false;
            furi_hal_nfc_pn532.iso_dep_block_num = 0;
            FURI_LOG_D(
                TAG, "Cached ATS (%zu bytes) for ISO-DEP card", furi_hal_nfc_pn532.cached_ats_len);
        } else {
            furi_hal_nfc_pn532.cached_ats_len = 0;
            furi_hal_nfc_pn532.iso_dep_mode = false;
            furi_hal_nfc_pn532.iso_dep_block_num = 0;
        }
    }

    furi_hal_nfc_pn532_queue_push(FuriHalNfcEventTxEnd);
    if(furi_hal_nfc_pn532.target_valid &&
       furi_hal_nfc_pn532_prepare_rx(
           furi_hal_nfc_pn532.target.atqa, sizeof(furi_hal_nfc_pn532.target.atqa), false, false)) {
        furi_hal_nfc_pn532_queue_push(FuriHalNfcEventRxStart);
        furi_hal_nfc_pn532_queue_push(FuriHalNfcEventRxEnd);
    }

    furi_hal_nfc_release();
    return ret;
}

static FuriHalNfcError furi_hal_nfc_pn532_exchange_internal(
    const uint8_t* tx_bytes,
    size_t tx_len,
    bool add_parity_to_rx,
    bool strip_crc_from_tx) {
    if(!furi_hal_nfc_pn532_is_active()) {
        return FuriHalNfcErrorCommunication;
    }
    if(furi_hal_nfc_acquire() != FuriHalNfcErrorNone) {
        return FuriHalNfcErrorBusy;
    }

    FuriHalNfcError err_ret = FuriHalNfcErrorNone;

    memset(furi_hal_nfc_pn532.scratch, 0, sizeof(furi_hal_nfc_pn532.scratch));
    uint8_t* const rx_payload = furi_hal_nfc_pn532.scratch;
    size_t rx_len = 0;

    size_t effective_tx_len = tx_len;
    if(strip_crc_from_tx && (effective_tx_len >= sizeof(uint16_t))) {
        effective_tx_len -= sizeof(uint16_t);
    }

    // Pre-compute effective send length (without CRC) for special command detection
    const size_t send_len = strip_crc_from_tx ? effective_tx_len : tx_len;
    FuriHalPn532Error err;

    // Auto-detect target if not yet valid (Type B, FeliCa)
    // or force re-poll if target is stale (freshness timeout exceeded)
    // IMPORTANT: Never re-poll while PN532 native MIFARE auth is active —
    // InListPassiveTarget destroys the PN532's internal Crypto1 session,
    // causing all subsequent block reads to fail with error 0x06.
    bool target_fresh = false;
    if(furi_hal_nfc_pn532.target_valid) {
        int32_t age = (int32_t)(furi_get_tick() - furi_hal_nfc_pn532.target_tick);
        target_fresh = (age >= 0 && age < (int32_t)PN532_TARGET_FRESHNESS_TIMEOUT_MS);
    }
    if(furi_hal_nfc_pn532.mf_authed) {
        // While authenticated, treat target as always fresh — re-polling
        // would send InListPassiveTarget which resets the PN532's Crypto1
        // state machine, making all subsequent reads return error 0x06.
        target_fresh = true;
    }
    if(!target_fresh) {
        // Log if we're re-polling due to stale target (diagnostic aid)
        if(furi_hal_nfc_pn532.target_valid) {
            FURI_LOG_D(
                TAG,
                "Target stale (last tick %lu, now %lu), re-polling",
                furi_hal_nfc_pn532.target_tick,
                furi_get_tick());
        }
        // Guard: only auto-poll for plausible detection commands (≥3 bytes)
        // when there is genuinely no target. For stale targets, allow
        // short commands (e.g. 2-byte SDD frames) through to InDataExchange.
        if(!furi_hal_nfc_pn532.target_valid && effective_tx_len < 3) {
            err_ret = FuriHalNfcErrorCommunication; goto release;
        }

        FuriHalPn532Target target = {0};
        bool detected = false;

        switch(furi_hal_nfc_pn532.tech) {
        case FuriHalNfcTechIso14443a:
            detected = furi_hal_pn532_poll_iso14443a(&target);
            break;
        case FuriHalNfcTechIso14443b:
            detected = furi_hal_pn532_poll_iso14443b(&target);
            break;
        case FuriHalNfcTechFelica:
            detected = furi_hal_pn532_poll_felica(&target);
            break;
        default:
            err_ret = furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532ErrorUnsupported, false);
    goto release;
        }

        if(!detected) {
            // No card found — push TxEnd with rx_bits=0 so state machine sees timeout
            furi_hal_nfc_pn532_queue_push(FuriHalNfcEventTxEnd);
            furi_hal_nfc_pn532.rx_bits = 0;
            furi_hal_nfc_pn532_set_result(FuriHalNfcPn532ResultNotPresent);
            err_ret = FuriHalNfcErrorNone; goto release;
        }

        // Target found — store it
        furi_hal_nfc_pn532.target = target;
        furi_hal_nfc_pn532.target_valid = true;
        furi_hal_nfc_pn532.needs_relist = false;
        furi_hal_nfc_pn532.target_tick = furi_get_tick();

        // Type B REQB: synthesize ATQB from poll result
        // ATQB = [0x50(1)][PUPI(4)][AppData(4)][ProtoInfo(3)] = 12 bytes
        if(effective_tx_len >= 3 && tx_bytes[0] == 0x05) {
            uint8_t atqb[12] = {0};
            atqb[0] = 0x50; // ATQB flag
            memcpy(&atqb[1], target.uid, 4);
            memcpy(&atqb[5], target.app_data, 4);
            memcpy(&atqb[9], target.proto_info, 3);
            furi_hal_nfc_pn532_prepare_rx(atqb, sizeof(atqb), false, false);
            err_ret = furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532ErrorNone, true);
    goto release;
        }

        // FeliCa: the PN532 handles FeliCa commands natively through InDataExchange
        // when a FeliCa target is in-listed. Fall through to InDataExchange path below.
    }

    // WUPA/REQA interception — return cached ATQA
    // The PN532 already activated the target; forwarding WUPA would break its
    // internal state machine.  We return the ATQA we saved during InListPassiveTarget.
    if(effective_tx_len == 1 && (tx_bytes[0] == 0x26 || tx_bytes[0] == 0x52)) {
        if(furi_hal_nfc_pn532.target_valid) {
            if(furi_hal_nfc_pn532.needs_relist) {
                FuriHalPn532Target target;
                if(!furi_hal_pn532_poll_iso14443a_timeout(&target, 20)) {
                    furi_hal_nfc_pn532.target_valid = false;
                    furi_hal_nfc_pn532.mf_authed = false;
                    furi_hal_nfc_pn532_queue_push(FuriHalNfcEventTxEnd);
                    err_ret = FuriHalNfcErrorCommunication; goto release;
                }
                furi_hal_nfc_pn532.target = target;
                furi_hal_nfc_pn532.needs_relist = false;
            }
            uint8_t atqa[2];
            atqa[0] = furi_hal_nfc_pn532.target.atqa[1];
            atqa[1] = furi_hal_nfc_pn532.target.atqa[0];
            furi_hal_nfc_pn532_prepare_rx(atqa, sizeof(atqa), false, false);
            err_ret = furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532ErrorNone, true);
    goto release;
        }
    }

    // Handle HLTA (Halt Type A) — tag goes to HALT state, target is lost
    if(effective_tx_len == 2U && tx_bytes[0] == 0x50U && tx_bytes[1] == 0x00U) {
        furi_hal_nfc_pn532.needs_relist = true;
        furi_hal_nfc_pn532_queue_push(FuriHalNfcEventTxEnd);
        err_ret = FuriHalNfcErrorNone; goto release;
    }

    if(tx_len >= 2U) {
        const uint8_t sel_cmd = tx_bytes[0];
        const uint8_t sel_par = tx_bytes[1];
        // SDD (Anticollision) — synthesize NFCID+BCC from stored target
        if(((sel_cmd == 0x93U) || (sel_cmd == 0x95U) || (sel_cmd == 0x97U)) &&
           (sel_par == 0x20U)) {
            uint8_t nfcid[4] = {0};
            uint8_t sak = 0;
            const uint8_t cascade_level = (sel_cmd - 0x93U) / 2U;
            if(!furi_hal_nfc_pn532_build_cascade_response(cascade_level, nfcid, &sak)) {
                err_ret = FuriHalNfcErrorCommunication; goto release;
            }

            const uint8_t bcc = nfcid[0] ^ nfcid[1] ^ nfcid[2] ^ nfcid[3];
            uint8_t response[5] = {nfcid[0], nfcid[1], nfcid[2], nfcid[3], bcc};
            furi_hal_nfc_pn532_prepare_rx(response, sizeof(response), false, false);
            err_ret = furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532ErrorNone, true);
    goto release;
        }

        // SELECT — synthesize SAK+CRC from stored target
        if(((sel_cmd == 0x93U) || (sel_cmd == 0x95U) || (sel_cmd == 0x97U)) &&
           (sel_par == 0x70U)) {
            uint8_t nfcid[4] = {0};
            uint8_t sak = 0;
            const uint8_t cascade_level = (sel_cmd - 0x93U) / 2U;
            if(!furi_hal_nfc_pn532_build_cascade_response(cascade_level, nfcid, &sak)) {
                err_ret = FuriHalNfcErrorCommunication; goto release;
            }

            const uint8_t response[1] = {sak};
            furi_hal_nfc_pn532_prepare_rx(response, sizeof(response), true, false);
            err_ret = furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532ErrorNone, true);
    goto release;
        }
    }

    // ISO14443-4A RATS interception — return cached ATS with CRC-A
    // The PN532 already performed RATS/ATS internally during InListPassiveTarget,
    // so forwarding a second RATS via InDataExchange would fail (0x0B).
    if(send_len >= 1 && tx_bytes[0] == 0xE0) {
        if(furi_hal_nfc_pn532.cached_ats_len == 0) {
            FURI_LOG_D(TAG, "RATS received but no ATS cached (non-ISO-DEP card), fast fail");
            furi_hal_nfc_pn532_queue_push(FuriHalNfcEventTxEnd);
            furi_hal_nfc_pn532.rx_bits = 0;
            furi_hal_nfc_pn532_set_result(FuriHalNfcPn532ResultNotPresent);
            err_ret = FuriHalNfcErrorCommunication; goto release;
        }
        FURI_LOG_D(
            TAG,
            "RATS intercepted, returning cached ATS (%zu bytes)",
            furi_hal_nfc_pn532.cached_ats_len);
        furi_hal_nfc_pn532.iso_dep_mode = true;
        uint8_t ats_buf[22] = {0};
        size_t ats_len = furi_hal_nfc_pn532.cached_ats_len;
        if(ats_len > sizeof(ats_buf) - 2) ats_len = sizeof(ats_buf) - 2;
        memcpy(ats_buf, furi_hal_nfc_pn532.cached_ats, ats_len);
        const uint16_t crc = furi_hal_nfc_pn532_crc_a(ats_buf, ats_len);
        ats_buf[ats_len] = (uint8_t)(crc & 0xFFU);
        ats_buf[ats_len + 1] = (uint8_t)(crc >> 8U);
        furi_hal_nfc_pn532_prepare_rx(ats_buf, ats_len + 2, false, false);
        err_ret = furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532ErrorNone, true);
    goto release;
    }

    // PPS (Protocol and Parameter Selection) interception — also synthetic
    // PN532 handles PPS internally during card activation; forwarding a second
    // PPS via InDataExchange would confuse the PN532.  Must check this BEFORE
    // the ISO-DEP mode block below (PPS first byte 0xD0 would match S-block).
    if(send_len >= 1 && (tx_bytes[0] & 0xF0) == 0xD0 && furi_hal_nfc_pn532.cached_ats_len > 0) {
        FURI_LOG_D(TAG, "PPS intercepted");
        uint8_t pps_resp[3] = {tx_bytes[0], 0, 0};
        const uint16_t crc = furi_hal_nfc_pn532_crc_a(pps_resp, 1);
        pps_resp[1] = (uint8_t)(crc & 0xFFU);
        pps_resp[2] = (uint8_t)(crc >> 8U);
        furi_hal_nfc_pn532_prepare_rx(pps_resp, 3, false, false);
        err_ret = furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532ErrorNone, true);
    goto release;
    }

    // All other commands — forward to PN532 via InDataExchange or InCommunicateThru

    // MIFARE Classic auth — use InCommunicateThru to bypass PN532 internal auth
    // The PN532's InDataExchange interprets 0x60/0x61 as MIFARE auth and
    // requires a 12-byte command [cmd, block, key(6), uid(4)], which returns
    // 0x14 for all keys on this PN532 firmware.  InCommunicateThru sends raw
    // frames — the card returns NT (4 bytes) regardless of key correctness,
    // and the MIFARE poller's host-side Crypto1 handles the actual auth.
    // Also intercept 0x64/0x65 (backdoor auth for magic cards) with the
    // full key embedded in the InCommunicateThru frame.
    if(send_len >= 2U && (tx_bytes[0] == 0x60U || tx_bytes[0] == 0x61U || tx_bytes[0] == 0x64U ||
                          tx_bytes[0] == 0x65U)) {
        if(tx_bytes[0] == 0x64U || tx_bytes[0] == 0x65U) {
            // Backdoor auth: build full 9-byte frame with stored key + CRC-A
            // Format: [cmd, block, key_type, key(6), CRC(2)]
            uint8_t backdoor_cmd[11] = {0};
            backdoor_cmd[0] = tx_bytes[0]; // 0x64 or 0x65
            backdoor_cmd[1] = tx_bytes[1]; // block number
            backdoor_cmd[2] = furi_hal_nfc_pn532.mf_auth_key_type;
            memcpy(&backdoor_cmd[3], furi_hal_nfc_pn532.mf_auth_key, 6);
            const uint16_t crc_bd = furi_hal_nfc_pn532_crc_a(backdoor_cmd, 9);
            backdoor_cmd[9] = (uint8_t)(crc_bd & 0xFFU);
            backdoor_cmd[10] = (uint8_t)(crc_bd >> 8U);

            err = furi_hal_pn532_in_communicate_thru_timeout(
                backdoor_cmd,
                sizeof(backdoor_cmd),
                rx_payload,
                PN532_MAX_FRAME_SIZE,
                &rx_len,
                PN532_TIMEOUT_MF_AUTH_MS);
        } else {
            // Standard auth: send complete frame [cmd, block, CRC] via InCommunicateThru.
            // tx_bytes includes CRC-A appended by iso14443_3a_poller layer;
            // InCommunicateThru sends raw bytes without modification by PN532.
            err = furi_hal_pn532_in_communicate_thru_timeout(
                tx_bytes,
                tx_len,
                rx_payload,
                PN532_MAX_FRAME_SIZE,
                &rx_len,
                PN532_TIMEOUT_MF_AUTH_MS);
        }
        if(err == FuriHalPn532ErrorNone) {
            // NT (standard auth) or ACK (backdoor auth) — no CRC append
            furi_hal_nfc_pn532_prepare_rx(rx_payload, rx_len, false, false);
            err_ret = furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532ErrorNone, true);
    goto release;
        }
        // Auth InCommunicateThru failed — likely wrong key, tag enters HALT state.
        // We set needs_relist so the next WUPA command physically wakes it up.
        // Do NOT set target_valid = false, allowing poller retry logic to proceed!
        furi_hal_nfc_pn532.rx_bits = 0;
        furi_hal_nfc_pn532.needs_relist = true;
        furi_hal_nfc_pn532.iso_dep_mode = false;
        err_ret = furi_hal_nfc_pn532_finalize_exchange(err, false);
    goto release;
    } else if(furi_hal_nfc_pn532.iso_dep_mode) {
        // ISO-DEP mode: strip I-block/R-block/S-block headers before InDataExchange
        // The PN532 manages ISO-DEP framing internally once RATS completes.
        // We must strip the PCB header that the Flipper ISO-DEP layer adds, send just
        // the INF bytes, then add back a synthetic PCB + CRC on response.
        const uint8_t pcb = tx_bytes[0];
        const uint8_t block_type = pcb & 0xC0;

        if(block_type == 0x00) {
            // I-block — strip PCB and optional CID/NAD, send INF via InDataExchange
            size_t hdr_len = 1;
            if(pcb & 0x08) hdr_len++; // CID present
            if(pcb & 0x04) hdr_len++; // NAD present

            if(hdr_len < send_len) {
                err = furi_hal_pn532_in_data_exchange(
                    furi_hal_nfc_pn532.target.target_number,
                    &tx_bytes[hdr_len],
                    send_len - hdr_len,
                    rx_payload,
                    PN532_MAX_FRAME_SIZE,
                    &rx_len);
            } else {
                err = FuriHalPn532ErrorComm;
            }

            if(err == FuriHalPn532ErrorNone && rx_len > 0) {
                // Rebuild I-block response: [PCB] [data] [CRC]
                uint8_t resp_pcb = 0x02 | (furi_hal_nfc_pn532.iso_dep_block_num & 1);
                furi_hal_nfc_pn532.iso_dep_block_num ^= 1;

                memmove(&furi_hal_nfc_pn532.scratch[1], furi_hal_nfc_pn532.scratch, rx_len);
                furi_hal_nfc_pn532.scratch[0] = resp_pcb;
                size_t iob_len = 1 + rx_len;
                if(iob_len + 2 > PN532_MAX_FRAME_SIZE) iob_len = PN532_MAX_FRAME_SIZE - 2;
                const uint16_t crc = furi_hal_nfc_pn532_crc_a(furi_hal_nfc_pn532.scratch, iob_len);
                furi_hal_nfc_pn532.scratch[iob_len] = (uint8_t)(crc & 0xFFU);
                furi_hal_nfc_pn532.scratch[iob_len + 1] = (uint8_t)(crc >> 8U);

                furi_hal_nfc_pn532_prepare_rx(
                    furi_hal_nfc_pn532.scratch, iob_len + 2, false, false);
                err_ret = furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532ErrorNone, true);
    goto release;
            }
            // Fall through to error handling if InDataExchange fails

        } else if(block_type == 0x80) {
            /* R-block: ISO14443-4 §7.5.5.
             * R(ACK) PCB = 0b10100010 | (block_num & 1) = 0xA2 | block_num_bit
             * We toggle the sequence number to appease non-compliant readers (hybrid approach).
             * The PN532 handles retransmission internally; we echo R(ACK) back
             * to keep the Flipper ISO-DEP state machine in sync. */
            furi_hal_nfc_pn532.iso_dep_block_num ^= 1U; // Toggle sequence number for hybrid compatibility
            uint8_t r_ack = (uint8_t)(0xA2U | (furi_hal_nfc_pn532.iso_dep_block_num & 1U));
            uint8_t r_resp[3] = {r_ack, 0, 0};
            const uint16_t crc = furi_hal_nfc_pn532_crc_a(r_resp, 1);
            r_resp[1] = (uint8_t)(crc & 0xFFU);
            r_resp[2] = (uint8_t)(crc >> 8U);
            furi_hal_nfc_pn532_prepare_rx(r_resp, 3, false, false);
            err_ret = furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532ErrorNone, true);
            goto release;

        } else if(block_type == 0xC0) {
            // S-block (WTX or DESELECT)
            if((pcb & 0x30) == 0x30) {
                // WTX — return ACK with same WTXM value
                uint8_t s_resp[4] = {pcb, tx_bytes[1], 0, 0};
                const uint16_t crc = furi_hal_nfc_pn532_crc_a(s_resp, 2);
                s_resp[2] = (uint8_t)(crc & 0xFFU);
                s_resp[3] = (uint8_t)(crc >> 8U);
                furi_hal_nfc_pn532_prepare_rx(s_resp, 4, false, false);
                err_ret = furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532ErrorNone, true);
    goto release;
            }
            // DESELECT — fall through to InDataExchange
            err = furi_hal_pn532_in_data_exchange(
                furi_hal_nfc_pn532.target.target_number,
                tx_bytes,
                send_len,
                rx_payload,
                PN532_MAX_FRAME_SIZE,
                &rx_len);
        } else {
            err = FuriHalPn532ErrorComm;
        }
    } else {
        // Normal (non-ISO-DEP) mode: send raw data via InDataExchange
        const size_t ide_len = strip_crc_from_tx ? tx_len : send_len;
        err = furi_hal_pn532_in_data_exchange(
            furi_hal_nfc_pn532.target.target_number,
            tx_bytes,
            ide_len,
            rx_payload,
            PN532_MAX_FRAME_SIZE,
            &rx_len);
    }

    // Target loss detection: mark invalid on communication failure
    // Skip invalidation when abort is pending — the error is from an
    // interrupted exchange, not a genuine target loss.
    if(err == FuriHalPn532ErrorComm || err == FuriHalPn532ErrorTimeout) {
        if(!(furi_thread_flags_get() & FuriHalNfcEventInternalTypeAbort)) {
            FURI_LOG_W("PN532Nfc", "Target communication failed (err=%d), marking invalid", err);
            furi_hal_nfc_pn532.target_valid = false;
            furi_hal_nfc_pn532.mf_authed = false;
            furi_hal_nfc_pn532.iso_dep_mode = false;
        }
    }

    const bool response_ready =
        (err == FuriHalPn532ErrorNone) && (rx_len > 0U) &&
        furi_hal_nfc_pn532_prepare_rx(rx_payload, rx_len, !add_parity_to_rx, add_parity_to_rx);

    err_ret = furi_hal_nfc_pn532_finalize_exchange(err, response_ready);
    goto release;

release:
    furi_hal_nfc_release();
    return err_ret;
}

FuriHalNfcError furi_hal_nfc_pn532_tx(const uint8_t* tx_data, size_t tx_bits) {
    if((tx_bits % 8U) != 0U) return FuriHalNfcErrorDataFormat;
    return furi_hal_nfc_pn532_exchange_internal(tx_data, tx_bits / 8U, false, true);
}

FuriHalNfcError furi_hal_nfc_pn532_tx_custom_parity(const uint8_t* tx_data, size_t tx_bits) {
    uint8_t tx_bytes[PN532_MAX_FRAME_SIZE] = {0};
    const size_t tx_len =
        furi_hal_nfc_pn532_unpack_parity_frame(tx_data, tx_bits, tx_bytes, PN532_MAX_FRAME_SIZE);
    if(tx_len == 0U) return FuriHalNfcErrorDataFormat;

    return furi_hal_nfc_pn532_exchange_internal(tx_bytes, tx_len, true, false);
}

FuriHalNfcError furi_hal_nfc_pn532_rx(uint8_t* rx_data, size_t rx_data_size, size_t* rx_bits) {
    if(furi_hal_nfc_acquire() != FuriHalNfcErrorNone) return FuriHalNfcErrorBusy;
    if(!rx_data || !rx_bits) { furi_hal_nfc_release(); return FuriHalNfcErrorDataFormat; }
    const size_t rx_bytes = (furi_hal_nfc_pn532.rx_bits + 7U) / 8U;
    if(rx_bytes > rx_data_size) { furi_hal_nfc_release(); return FuriHalNfcErrorBufferOverflow; }

    memcpy(rx_data, furi_hal_nfc_pn532.rx_buffer, rx_bytes);
    *rx_bits = furi_hal_nfc_pn532.rx_bits;
    furi_hal_nfc_release();
    return FuriHalNfcErrorNone;
}

// Listener (target) mode: configure UID/ATQA/SAK for emulation
FuriHalNfcError furi_hal_nfc_pn532_listener_set_col_res_data(
    const uint8_t* uid,
    uint8_t uid_len,
    const uint8_t* atqa,
    uint8_t sak) {
    if(!furi_hal_nfc_pn532_is_active()) return FuriHalNfcErrorCommunication;
    if(!uid || uid_len == 0 || uid_len > 10) return FuriHalNfcErrorDataFormat;
    if(!atqa) return FuriHalNfcErrorDataFormat;

    furi_hal_nfc_pn532.listener_uid_len = uid_len;
    memcpy(furi_hal_nfc_pn532.listener_uid, uid, uid_len);
    memcpy(furi_hal_nfc_pn532.listener_atqa, atqa, 2);
    furi_hal_nfc_pn532.listener_sak = sak;
    furi_hal_nfc_pn532.listener_configured = true;
    furi_hal_nfc_pn532.listener_active = false; /* force re-init on next wait_event */

    FURI_LOG_D(TAG, "Listener configured: UID len=%u SAK=0x%02X", uid_len, sak);
    return FuriHalNfcErrorNone;
}

// Listener (target) mode: wait for reader activation or data
FuriHalNfcEvent furi_hal_nfc_pn532_listener_wait_event(uint32_t timeout_ms) {
    if(!furi_hal_nfc_pn532_is_active()) return FuriHalNfcEventTimeout;

    // Poll in 200ms slices with abort checks between them
    // so the UI can respond within ~200ms instead of waiting for PN532 timeout
    const uint32_t slice_ms = 200;
    uint32_t remaining = (timeout_ms == FURI_HAL_NFC_EVENT_WAIT_FOREVER) ? slice_ms : timeout_ms;

    do {
        // Check abort before each polling slice
        if(furi_thread_flags_get() & FuriHalNfcEventInternalTypeAbort) {
            furi_thread_flags_clear(FuriHalNfcEventInternalTypeAbort);
            furi_hal_nfc_pn532_queue_reset();
            return FuriHalNfcEventAbortRequest;
        }

        if(!furi_hal_nfc_pn532.listener_active) {
            // First call: initialize as target (TGINITASTARGET)

            FuriHalPn532Error err;
            if(furi_hal_nfc_pn532.tech == FuriHalNfcTechFelica) {
                // FeliCa target mode (mode=0x02)
                // Params: [mode=0x02] [NFCID2t: 8 bytes] [PAD: 8 bytes] [SENSF_RES: 4 bytes]
                uint8_t params[] = {0x02, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                    0x07, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x12, 0xFC, 0x00, 0x00};
                if(furi_hal_nfc_pn532.listener_configured) {
                    for(uint8_t i = 0; i < 8 && i < furi_hal_nfc_pn532.listener_uid_len; i++) {
                        params[1 + i] = furi_hal_nfc_pn532.listener_uid[i];
                    }
                }
                err = furi_hal_pn532_tg_init_as_target(params, sizeof(params), slice_ms);
            } else {
                // ISO14443-4A target mode — use configured UID/ATQA/SAK if available
                // Format: [mode(1)] [SENS_RES(2)] [NFCID1(3)] [SEL_RES(1)]
                //         [FeliCa params(18)] [NFCID3t(10)] [Gt_len(1)] [Tk_len(1)] = 36 bytes
                uint8_t params[36];
                memset(params, 0, sizeof(params));
                params[0] = 0x05; /* mode: passive 106kbps, PICC only */

                if(furi_hal_nfc_pn532.listener_configured) {
                    params[1] = furi_hal_nfc_pn532.listener_atqa[0];
                    params[2] = furi_hal_nfc_pn532.listener_atqa[1];
                    params[3] = (furi_hal_nfc_pn532.listener_uid_len >= 1) ?
                                    furi_hal_nfc_pn532.listener_uid[0] : 0x00;
                    params[4] = (furi_hal_nfc_pn532.listener_uid_len >= 2) ?
                                    furi_hal_nfc_pn532.listener_uid[1] : 0x00;
                    params[5] = (furi_hal_nfc_pn532.listener_uid_len >= 3) ?
                                    furi_hal_nfc_pn532.listener_uid[2] : 0x00;
                    params[6] = furi_hal_nfc_pn532.listener_sak;
                } else {
                    /* Defaults: generic ISO14443-4 tag */
                    params[1] = 0x04; params[2] = 0x00; /* ATQA */
                    params[3] = 0x08; params[4] = 0x00; params[5] = 0x00; /* NFCID1 */
                    params[6] = 0x20; /* SAK: ISO-DEP capable */
                }
                /* NFCID3t: copy UID into bytes 25-34 */
                for(uint8_t i = 0; i < furi_hal_nfc_pn532.listener_uid_len && i < 10; i++) {
                    params[25 + i] = furi_hal_nfc_pn532.listener_uid[i];
                }
                err = furi_hal_pn532_tg_init_as_target(params, sizeof(params), slice_ms);
            }

            if(err == FuriHalPn532ErrorNone) {
                furi_hal_nfc_pn532.listener_active = true;
                return FuriHalNfcEventFieldOn | FuriHalNfcEventListenerActive;
            }
        } else {
            // Subsequent calls: wait for command from reader (TGGETDATA)
            memset(furi_hal_nfc_pn532.scratch, 0, sizeof(furi_hal_nfc_pn532.scratch));
            uint8_t* const buf = furi_hal_nfc_pn532.scratch;
            size_t len = 0;

            FuriHalPn532Error err =
                furi_hal_pn532_tg_get_data(buf, PN532_MAX_FRAME_SIZE, &len, slice_ms);

            if(err == FuriHalPn532ErrorNone && len > 0) {
                if(furi_hal_nfc_pn532.tech == FuriHalNfcTechFelica) {
                    // FeliCa: store raw data as parity-encoded bits
                    // No I-block wrapping needed — FeliCa uses its own protocol format
                    if(len <= (sizeof(furi_hal_nfc_pn532.rx_buffer) * 8U / 9U)) {
                        size_t bit_pos = 0;
                        memset(
                            furi_hal_nfc_pn532.rx_buffer, 0, sizeof(furi_hal_nfc_pn532.rx_buffer));
                        for(size_t i = 0; i < len; i++) {
                            const uint8_t byte = buf[i];
                            bool parity = true;
                            for(size_t j = 0; j < 8; j++)
                                parity ^= ((byte >> j) & 1);
                            const size_t byte_idx = bit_pos / 8U;
                            const size_t bit_off = bit_pos % 8U;
                            furi_hal_nfc_pn532.rx_buffer[byte_idx] |= byte << bit_off;
                            if(bit_off != 0) {
                                furi_hal_nfc_pn532.rx_buffer[byte_idx + 1] |= byte >>
                                                                              (8U - bit_off);
                            }
                            bit_pos += 8;
                            if(parity) {
                                const size_t par_idx = bit_pos / 8U;
                                const size_t par_off = bit_pos % 8U;
                                furi_hal_nfc_pn532.rx_buffer[par_idx] |= 1U << par_off;
                            }
                            bit_pos += 1;
                        }
                        furi_hal_nfc_pn532.rx_bits = len * 9U;
                    } else {
                        furi_hal_nfc_pn532.rx_bits = 0;
                    }
                } else {
                    // ISO14443-4A: wrap received data in I-block + CRC
                    // for 3A→4A→Type4Tag listener chain to process correctly
                    if(len + 3U <= (sizeof(furi_hal_nfc_pn532.rx_buffer) * 8U / 9U)) {
                        memmove(&furi_hal_nfc_pn532.scratch[1], furi_hal_nfc_pn532.scratch, len);
                        /* IMP-6: PCB byte must toggle between 0x02/0x03 per ISO14443-4.
                         * iso_dep_block_num tracks the current block number (0 or 1). */
                        furi_hal_nfc_pn532.scratch[0] =
                            (uint8_t)(0x02U | (furi_hal_nfc_pn532.iso_dep_block_num & 1U));
                        furi_hal_nfc_pn532.iso_dep_block_num ^= 1U;
                        const uint16_t crc =
                            furi_hal_nfc_pn532_crc_a(furi_hal_nfc_pn532.scratch, len + 1);
                        furi_hal_nfc_pn532.scratch[len + 1] = (uint8_t)(crc & 0xFFU);
                        furi_hal_nfc_pn532.scratch[len + 2] = (uint8_t)(crc >> 8U);
                        const size_t i_block_len = len + 3U;

                        size_t bit_pos = 0;
                        memset(
                            furi_hal_nfc_pn532.rx_buffer, 0, sizeof(furi_hal_nfc_pn532.rx_buffer));
                        for(size_t i = 0; i < i_block_len; i++) {
                            const uint8_t byte = furi_hal_nfc_pn532.scratch[i];
                            bool parity = true;
                            for(size_t j = 0; j < 8; j++)
                                parity ^= ((byte >> j) & 1);
                            const size_t byte_idx = bit_pos / 8U;
                            const size_t bit_off = bit_pos % 8U;
                            furi_hal_nfc_pn532.rx_buffer[byte_idx] |= byte << bit_off;
                            if(bit_off != 0) {
                                furi_hal_nfc_pn532.rx_buffer[byte_idx + 1] |= byte >>
                                                                              (8U - bit_off);
                            }
                            bit_pos += 8;
                            if(parity) {
                                const size_t par_idx = bit_pos / 8U;
                                const size_t par_off = bit_pos % 8U;
                                furi_hal_nfc_pn532.rx_buffer[par_idx] |= 1U << par_off;
                            }
                            bit_pos += 1;
                        }
                        furi_hal_nfc_pn532.rx_bits = i_block_len * 9U;
                    } else {
                        furi_hal_nfc_pn532.rx_bits = 0;
                    }
                }
                return FuriHalNfcEventRxEnd;
            }

            if(err == FuriHalPn532ErrorTimeout) {
                // No data this slice — re-enter loop unless overall timeout reached
            } else {
                // Communication error — reader likely went away
                furi_hal_nfc_pn532.listener_active = false;
                furi_hal_nfc_pn532.rx_bits = 0;
                return FuriHalNfcEventFieldOff;
            }
        }

        // Check overall timeout (only for finite timeouts)
        if(timeout_ms != FURI_HAL_NFC_EVENT_WAIT_FOREVER) {
            if(remaining <= slice_ms) break;
            remaining -= slice_ms;
        }
    } while(true);

    // Overall timeout reached with no activation/data
    if(furi_hal_nfc_pn532.listener_active) {
        // Was previously active — reader appears to have left
        furi_hal_nfc_pn532.listener_active = false;
        furi_hal_nfc_pn532.rx_bits = 0;
        return FuriHalNfcEventFieldOff;
    }
    return FuriHalNfcEventTimeout;
}

// Listener (target) mode: send response to reader
FuriHalNfcError furi_hal_nfc_pn532_listener_tx(const uint8_t* tx_data, size_t tx_bits) {
    if(!furi_hal_nfc_pn532_is_active() || !furi_hal_nfc_pn532.listener_active) {
        return FuriHalNfcErrorCommunication;
    }

    const uint8_t* raw = NULL;
    size_t byte_count = 0;

    if((tx_bits % 9U) == 0U && tx_bits > 0U) {
        /* 9-bit parity-encoded stream (from listener_tx_custom_parity path).
         * Each 9 bits = 8 data bits + 1 parity bit. Unpack to raw bytes. */
        byte_count = tx_bits / 9U;
        if(byte_count > PN532_MAX_FRAME_SIZE) return FuriHalNfcErrorBufferOverflow;

        memset(furi_hal_nfc_pn532.scratch, 0, sizeof(furi_hal_nfc_pn532.scratch));
        size_t bit_index = 0;
        for(size_t i = 0; i < byte_count; i++) {
            const size_t byte_idx = bit_index / 8U;
            const size_t bit_off  = bit_index % 8U;
            furi_hal_nfc_pn532.scratch[i] = tx_data[byte_idx] >> bit_off;
            if(bit_off != 0U) {
                furi_hal_nfc_pn532.scratch[i] |= tx_data[byte_idx + 1] << (8U - bit_off);
            }
            bit_index += 9U;
        }
        raw = furi_hal_nfc_pn532.scratch;
    } else {
        /* Raw byte stream (from furi_hal_nfc_iso14443a_listener_tx path).
         * tx_bits is the count of raw data bits — no parity interleaving. */
        byte_count = (tx_bits + 7U) / 8U;
        if(byte_count > PN532_MAX_FRAME_SIZE) return FuriHalNfcErrorBufferOverflow;
        raw = tx_data;
    }

    FuriHalPn532Error err = furi_hal_pn532_tg_set_data(raw, byte_count);
    return (err == FuriHalPn532ErrorNone) ? FuriHalNfcErrorNone : FuriHalNfcErrorCommunication;
}

// Listener (target) mode: read received data
FuriHalNfcError
    furi_hal_nfc_pn532_listener_rx(uint8_t* rx_data, size_t rx_data_size, size_t* rx_bits) {
    if(!rx_data || !rx_bits) return FuriHalNfcErrorDataFormat;
    if(!furi_hal_nfc_pn532_is_active()) return FuriHalNfcErrorCommunication;

    const size_t rx_bytes = (furi_hal_nfc_pn532.rx_bits + 7U) / 8U;
    if(rx_bytes > rx_data_size) return FuriHalNfcErrorBufferOverflow;

    memcpy(rx_data, furi_hal_nfc_pn532.rx_buffer, rx_bytes);
    *rx_bits = furi_hal_nfc_pn532.rx_bits;
    return FuriHalNfcErrorNone;
}

// Listener lifecycle: idle — reset state so next call re-initializes as target
FuriHalNfcError furi_hal_nfc_pn532_listener_idle(void) {
    if(!furi_hal_nfc_pn532_is_active()) return FuriHalNfcErrorCommunication;
    furi_hal_nfc_pn532.listener_active = false;
    furi_hal_nfc_pn532.rx_bits = 0;
    return FuriHalNfcErrorNone;
}

// Listener lifecycle: sleep — same as idle for PN532
FuriHalNfcError furi_hal_nfc_pn532_listener_sleep(void) {
    return furi_hal_nfc_pn532_listener_idle();
}

// Listener lifecycle: enable RX — no-op on PN532 (next get_data handles it)
FuriHalNfcError furi_hal_nfc_pn532_listener_enable_rx(void) {
    if(!furi_hal_nfc_pn532_is_active()) return FuriHalNfcErrorCommunication;
    return FuriHalNfcErrorNone;
}

FuriHalPn532Error furi_hal_nfc_pn532_last_error_get(void) {
    return furi_hal_nfc_pn532.last_error;
}

FuriHalNfcPn532Result furi_hal_nfc_pn532_last_result_get(void) {
    return furi_hal_nfc_pn532.last_result;
}

const char* furi_hal_nfc_pn532_last_error_str(void) {
    switch(furi_hal_nfc_pn532.last_error) {
    case FuriHalPn532ErrorNone:
        return "None";
    case FuriHalPn532ErrorTimeout:
        return "Timeout";
    case FuriHalPn532ErrorComm:
        return "Comm";
    case FuriHalPn532ErrorInvalidAck:
        return "InvalidAck";
    case FuriHalPn532ErrorInvalidFrame:
        return "InvalidFrame";
    case FuriHalPn532ErrorBufferOverflow:
        return "BufferOverflow";
    case FuriHalPn532ErrorUnsupported:
        return "Unsupported";
    default:
        return "Unknown";
    }
}

const char* furi_hal_nfc_pn532_last_result_str(void) {
    switch(furi_hal_nfc_pn532.last_result) {
    case FuriHalNfcPn532ResultDetected:
        return "detected";
    case FuriHalNfcPn532ResultNotPresent:
        return "not_present";
    case FuriHalNfcPn532ResultUnsupportedByPn532:
        return "unsupported_by_pn532";
    case FuriHalNfcPn532ResultCommunicationError:
        return "communication_error";
    case FuriHalNfcPn532ResultParseError:
        return "parse_error";
    default:
        return "unknown";
    }
}

void furi_hal_nfc_pn532_mf_key_store(const uint8_t* key, uint8_t key_type) {
    if(key) {
        memcpy(furi_hal_nfc_pn532.mf_auth_key, key, 6);
        furi_hal_nfc_pn532.mf_auth_key_type = key_type;
        furi_hal_nfc_pn532.mf_auth_key_valid = true;
    } else {
        furi_hal_nfc_pn532.mf_auth_key_valid = false;
    }
}

FuriHalNfcError furi_hal_nfc_pn532_mf_auth(
    uint8_t block_num,
    const uint8_t* key,
    uint8_t key_type,
    const uint8_t* uid,
    uint8_t uid_len) {
    FuriHalPn532Error err =
        furi_hal_pn532_mf_auth(furi_hal_nfc_pn532.target.target_number, block_num, key, key_type, uid, uid_len);
    if(err == FuriHalPn532ErrorNone) {
        furi_hal_nfc_pn532.mf_authed = true;
        FURI_LOG_D(TAG, "PN532 native auth OK: block=%d key_type=%d", block_num, key_type);
        return FuriHalNfcErrorNone;
    }
    furi_hal_nfc_pn532.mf_authed = false;
    FURI_LOG_D(TAG, "PN532 native auth failed: block=%d err=%d", block_num, err);
    return FuriHalNfcErrorCommunication;
}

bool furi_hal_nfc_pn532_mf_is_authed(void) {
    return furi_hal_nfc_pn532.mf_authed;
}

void furi_hal_nfc_pn532_mf_deauth(void) {
    furi_hal_nfc_pn532.mf_authed = false;
    /* After a failed PN532 native InDataExchange auth (status 0x14), the
     * card is left in HALT/ERROR state.  Force the next WUPA/REQA
     * interception in exchange_internal() to physically re-select the
     * card before any further commands are sent — otherwise the Crypto1
     * fallback issues an auth via InCommunicateThru to a HALTed card and
     * times out on every attempt. */
    furi_hal_nfc_pn532.needs_relist = true;
}

FuriHalNfcError furi_hal_nfc_pn532_mf_read_block(
    uint8_t block_num,
    uint8_t* data,
    size_t data_size) {
    if(!furi_hal_nfc_pn532_is_active()) return FuriHalNfcErrorCommunication;
    if(!furi_hal_nfc_pn532.mf_authed) return FuriHalNfcErrorCommunication;
    if(data_size < 16U) return FuriHalNfcErrorBufferOverflow;

    uint8_t cmd[2] = {0x30, block_num}; // MIFARE READ command
    uint8_t resp[18]; // 16 data + 2 CRC
    size_t resp_len = sizeof(resp);

    FuriHalPn532Error err = furi_hal_pn532_in_data_exchange(
        furi_hal_nfc_pn532.target.target_number, cmd, sizeof(cmd), resp, sizeof(resp), &resp_len);

    if(err != FuriHalPn532ErrorNone) {
        furi_hal_nfc_pn532.mf_authed = false;
        return FuriHalNfcErrorCommunication;
    }
    if(resp_len < 16U) {
        return FuriHalNfcErrorIncompleteFrame;
    }

    memcpy(data, resp, 16U);
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_pn532_mf_write_block(
    uint8_t block_num,
    const uint8_t* data,
    size_t data_size) {
    if(!furi_hal_nfc_pn532_is_active()) return FuriHalNfcErrorCommunication;
    if(!furi_hal_nfc_pn532.mf_authed) return FuriHalNfcErrorCommunication;
    if(data_size < 16U) return FuriHalNfcErrorDataFormat;

    // Phase 1: Send WRITE command
    uint8_t cmd1[2] = {0xA0, block_num}; // MIFARE WRITE command
    uint8_t resp1[2];
    size_t resp1_len = sizeof(resp1);

    FuriHalPn532Error err = furi_hal_pn532_in_data_exchange(
        furi_hal_nfc_pn532.target.target_number,
        cmd1,
        sizeof(cmd1),
        resp1,
        sizeof(resp1),
        &resp1_len);

    if(err != FuriHalPn532ErrorNone) {
        furi_hal_nfc_pn532.mf_authed = false;
        return FuriHalNfcErrorCommunication;
    }
    if(resp1_len < 1U || resp1[0] != 0x0A) { // ACK = 0x0A
        return FuriHalNfcErrorIncompleteFrame;
    }

    // Phase 2: Send 16 bytes of data
    uint8_t resp2[2];
    size_t resp2_len = sizeof(resp2);

    err = furi_hal_pn532_in_data_exchange(
        furi_hal_nfc_pn532.target.target_number,
        data,
        16U,
        resp2,
        sizeof(resp2),
        &resp2_len);

    if(err != FuriHalPn532ErrorNone) {
        furi_hal_nfc_pn532.mf_authed = false;
        return FuriHalNfcErrorCommunication;
    }
    if(resp2_len < 1U || resp2[0] != 0x0A) { // ACK = 0x0A
        return FuriHalNfcErrorIncompleteFrame;
    }

    return FuriHalNfcErrorNone;
}

uint8_t furi_hal_nfc_pn532_get_sak(void) {
    return furi_hal_nfc_pn532.target.sak;
}

bool furi_hal_nfc_pn532_target_is_valid(void) {
    return furi_hal_nfc_pn532.target_valid;
}

bool furi_hal_nfc_quick_poll(void) {
    if(!furi_hal_nfc_pn532_is_active()) return false;

    FuriHalPn532Target target = {0};
    // 50ms timeout — PN532 firmware waits at most 50ms for a card;
    // when no card is present this saves ~250ms vs the standard 300ms poll.
    return furi_hal_pn532_poll_iso14443a_timeout(&target, 50);
}

bool furi_hal_nfc_pn532_poll_jewel(FuriHalPn532Target* target) {
    if(!furi_hal_nfc_pn532_is_active()) return false;
    return furi_hal_pn532_poll_jewel(target);
}
