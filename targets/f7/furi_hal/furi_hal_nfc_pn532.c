#include "furi_hal_nfc_pn532.h"

#include "furi_hal_pn532.h"
#include "furi_hal_nfc_i.h"
#include <nfc/helpers/iso14443_4_layer.h>
#include <furi.h>
#include <string.h>

#define TAG "FuriHalNfcPn532"

/* MIFARE Classic auth timeout via InCommunicateThru.
 * Card responds to auth (NT) in ~5ms.  Clone PN532 internal processing
 * takes ~130ms (2x genuine PN532).  1000ms gives sufficient margin for
 * CIU register propagation + InCommunicateThru round-trip on clone modules. */
#define PN532_TIMEOUT_MF_AUTH_MS 1000
#define PN532_TIMEOUT_POLL_MS    400

#define PN532_NFCA_CT              0x88
#define PN532_EVENT_QUEUE_CAPACITY 8
#define PN532_MAX_FRAME_SIZE       192

#define PN532_TARGET_FRESHNESS_TIMEOUT_MS 5000

/* PN532 InDataExchange status byte (response[1]) flags.
 * Bit 6 = "More Information" / card-side ISO14443-4 chaining active.
 * When set, the card sent a chained response and the host must issue an
 * R(ACK) via a follow-up InDataExchange call to retrieve the next fragment.
 * See PN532 User Manual §7.3.6 and furi_hal_pn532_in_data_exchange_ex(). */
#define PN532_STATUS_CHAINING 0x40

/* Compile-time diagnostic logging for MIFARE Classic auth debugging.
 * Define NFC_AUTH_DIAG (e.g. -DNFC_AUTH_DIAG in build flags) to enable.
 * When disabled, expands to nothing — zero flash cost. */
#ifdef NFC_AUTH_DIAG
#define AUTH_DIAG_LOG(fmt, ...) FURI_LOG_I("AuthDiag", fmt, ##__VA_ARGS__)
#else
#define AUTH_DIAG_LOG(fmt, ...) \
    do {                        \
    } while(0)
#endif

typedef struct {
    bool active;
    bool target_valid;
    bool listener_active;
    FuriHalNfcMode mode;
    FuriHalNfcTech tech;
    FuriHalPn532Target target;
    uint32_t target_tick;
    uint8_t rx_buffer[ISO14443_4_MAX_APDU_SIZE];
    uint8_t scratch[ISO14443_4_MAX_APDU_SIZE];
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

    /* FeliCa emulation NFCID2t (8 bytes).
     * Set by furi_hal_nfc_pn532_listener_set_felica_params().
     * Used by listener_wait_event() FeliCa branch. */
    uint8_t felica_nfcid2t[8];
    bool felica_nfcid2t_configured; /**< true after set_felica_params called */

    /* Type-4 (ISO-DEP) NDEF emulation state */
    uint8_t emu_ndef_msg[888];
    size_t emu_ndef_len;
} FuriHalNfcPn532State;

static FuriHalNfcPn532State furi_hal_nfc_pn532 = {0};

// Per-session flag: set when PN532 returns 0x13 (DEP bad data) on native
// MIFARE auth, meaning this clone module doesn't support InDataExchange
// native auth. Cleared on full reinit or new target detection.
static bool pn532_mf_auth_unsupported = false;

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

void furi_hal_nfc_pn532_queue_reset(void) {
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
    uint8_t staging[ISO14443_4_MAX_APDU_SIZE];
    if(data_len > 0 && data_len <= ISO14443_4_MAX_APDU_SIZE &&
       data >= furi_hal_nfc_pn532.scratch &&
       data < furi_hal_nfc_pn532.scratch + sizeof(furi_hal_nfc_pn532.scratch)) {
        memcpy(staging, data, data_len);
        data = staging;
    }

    memset(furi_hal_nfc_pn532.scratch, 0, sizeof(furi_hal_nfc_pn532.scratch));
    uint8_t* const frame = furi_hal_nfc_pn532.scratch;
    size_t frame_len = data_len;
    if(frame_len > sizeof(furi_hal_nfc_pn532.scratch)) return false;

    if(frame_len) memcpy(frame, data, frame_len);

    if(append_crc) {
        if((frame_len + sizeof(uint16_t)) > sizeof(furi_hal_nfc_pn532.scratch)) return false;
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

    FURI_LOG_D(
        TAG,
        "build_cascade: level=%d uid_len=%zu sak=0x%02X uid=[%02X%02X%02X%02X]",
        cascade_level,
        uid_len,
        furi_hal_nfc_pn532.target.sak,
        uid_len > 0 ? uid[0] : 0,
        uid_len > 1 ? uid[1] : 0,
        uid_len > 2 ? uid[2] : 0,
        uid_len > 3 ? uid[3] : 0);

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
        FURI_LOG_D(TAG, "Exchange error: %s", furi_hal_pn532_error_str(err));
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
    /* Settle delay before InRelease: after a poll timeout (~300ms vs PN532's
     * ~325ms internal timeout), the PN532 may still be processing the previous
     * InListPassiveTarget.  Sending InRelease while PN532 is busy causes NACK
     * → strike accumulation → pn532_ready=false cascade.  500ms covers shared
     * I2C1 bus (PN532+PCF8574+OLED), no INT/RST pin, voltage drop during
     * read/write/emulate, plus clone module extra settle time. 1000ms gives
     * clone PN532 ample time to finish internal CIU pipeline processing. */
    furi_delay_ms(1000);
    // Release PN532 target handle to avoid stale state between protocol detects
    pn532_send_inrelease();
    /* Stop the background I2C1 ready poller thread to avoid I2C bus traffic
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
    furi_hal_nfc_pn532.felica_nfcid2t_configured = false;
    memset(furi_hal_nfc_pn532.felica_nfcid2t, 0, sizeof(furi_hal_nfc_pn532.felica_nfcid2t));
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
    furi_hal_nfc_pn532.emu_ndef_len = 0;
    memset(furi_hal_nfc_pn532.emu_ndef_msg, 0, sizeof(furi_hal_nfc_pn532.emu_ndef_msg));
}

/* Lightweight reset for reset_mode() — clears volatile state but preserves
 * target activation cache (target_valid, target, target_tick, cached_ats)
 * AND mode/tech.  Does NOT send InRelease or stop the IRQ poller thread.
 *
 * Preserving mode/tech is critical: set_mode() uses them to decide whether
 * to do a full reset (tech change) or preserve the cache (same tech). If we
 * cleared mode/tech here, every next set_mode() call would see a "tech change"
 * and do a full reset, negating the cache preservation.
 *
 * Used by furi_hal_nfc_reset_mode() which is called between scanner probe
 * cycles.  Full cache invalidation happens only in set_mode() on tech change. */
void furi_hal_nfc_pn532_reset_keep_target(void) {
    furi_hal_nfc_pn532.mf_authed = false;
    furi_hal_nfc_pn532.iso_dep_mode = false;
    furi_hal_nfc_pn532.iso_dep_block_num = 0;
    furi_hal_nfc_pn532.needs_relist = false;
    furi_hal_nfc_pn532.listener_active = false;
    furi_hal_nfc_pn532.listener_configured = false;
    furi_hal_nfc_pn532.rx_bits = 0;
    furi_hal_nfc_pn532.last_error = FuriHalPn532ErrorNone;
    furi_hal_nfc_pn532.last_result = FuriHalNfcPn532ResultNotPresent;
    furi_hal_nfc_pn532.last_error_tick = 0;
    furi_hal_nfc_pn532.mf_auth_key_valid = false;
    furi_hal_nfc_pn532.target_valid = false;
    pn532_mf_auth_unsupported = false;
    /* target, target_tick, cached_ats, cached_ats_len preserved for reference
     * but target_valid=false forces fresh InListPassiveTarget on next
     * trx_short_frame(), waking card from IDLE state after long delays. */
}

FuriHalNfcError furi_hal_nfc_pn532_set_mode(FuriHalNfcMode mode, FuriHalNfcTech tech) {
    FURI_LOG_W(
        TAG,
        "set_mode: is_active=%d active=%d ready=%d mode=%d tech=%d",
        furi_hal_nfc_pn532_is_active(),
        furi_hal_nfc_pn532.active,
        furi_hal_pn532_is_ready(),
        mode,
        tech);
    if(!furi_hal_nfc_pn532_is_active()) {
        /* Attempt recovery if PN532 was previously active (first init succeeded)
         * but became unresponsive (pn532_ready false from I2C timeout, etc.).
         * Without this recovery, a stale pn532_ready=false cascades through
         * all subsequent mode/tech changes during e.g. supported-card plugin
         * iteration and eventually causes a crash or frozen reader. */
        if(furi_hal_nfc_pn532.active && !furi_hal_pn532_is_ready()) {
            FURI_LOG_W(TAG, "set_mode: PN532 not ready, attempting I2C/SAM reinit");
            /* Drain any stale frames before reinit — plugin auth timeouts can
             * leave 6+ frames queued, keeping PN532 in READY state and NACKing
             * writes.  Draining here avoids a false pn532_ready=false that
             * would otherwise trigger a mid-session reinit cascade.
             * NOTE: Don't start IRQ poller here — furi_hal_pn532_init() will
             * stop/restart it internally. Starting it here wastes ~30ms. */
            if(!furi_hal_pn532_init()) {
                FURI_LOG_E(TAG, "set_mode: PN532 I2C/SAM reinit failed");
                return FuriHalNfcErrorCommunication;
            }
            /* Reinit succeeded — fall through to full state reset below */
        } else {
            return FuriHalNfcErrorCommunication;
        }
    }
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
     * RF configuration AND target activation cache across repeated protocol
     * detection cycles (e.g. scanner probing multiple protocols).
     *
     * CRITICAL: Do NOT send InRelease or clear target cache here. The scanner
     * allocates a new NfcPoller per protocol it tries (3A -> 3B -> FeliCa ->
     * Jewel). Each cycle calls set_mode() which would wipe the activation
     * cache, forcing the chip to re-poll. PN532's InListPassiveTarget issues
     * REQA, which doesn't wake a card already in IDLE-in-field state, so
     * subsequent probes fail to activate. By preserving target/atqa/sak/uid/ats
     * across same-tech transitions, the next poller can use the cached target.
     *
     * Cache invalidation happens only on:
     *   - tech change (below)
     *   - HALT (50 00) interception in exchange path
     *   - low_power_mode_start (RF off) */
    if(mode == furi_hal_nfc_pn532.mode && tech == furi_hal_nfc_pn532.tech) {
        furi_hal_pn532_irq_start();
        furi_hal_nfc_pn532.mf_authed = false;
        furi_hal_nfc_pn532.rx_bits = 0;
        furi_hal_nfc_pn532_queue_reset();
        furi_hal_nfc_pn532.last_error = FuriHalPn532ErrorNone;
        furi_hal_nfc_pn532.last_result = FuriHalNfcPn532ResultNotPresent;
        furi_hal_nfc_pn532.last_error_tick = 0;
        furi_hal_nfc_pn532.mf_auth_key_valid = false;
        furi_hal_nfc_pn532.needs_relist = false;
        furi_hal_nfc_pn532.iso_dep_mode = false;
        furi_hal_nfc_pn532.iso_dep_block_num = 0;
        /* Preserve target cache — trx_short_frame() uses cached ATQA/SAK/UID
         * to avoid REQA-won't-wake-IDLE-card issue. InListPassiveTarget sends
         * REQA which doesn't wake cards in IDLE state. By keeping the cached
         * target from quick_poll, the poller can proceed directly to SDD/SELECT
         * without re-polling. Cache invalidation happens only on:
         *   - tech change (else branch below)
         *   - HALT interception in exchange_internal()
         *   - low_power_mode_start() when RF actually turns off */
    } else {
        furi_hal_nfc_pn532_reset();
        furi_hal_nfc_pn532.mode = mode;
        furi_hal_nfc_pn532.tech = tech;
        /* Restart the software ready poller thread after a full reset so that
         * pn532_wait_ready_ms() uses the event-flag path (not blocking poll)
         * for all subsequent commands in this session. */
        furi_hal_pn532_irq_start();
    }
    /* I2C1 bus settle: give PCF8574 time to finish any pending transaction
     * before the poller starts I2C ops with PN532. Without this, the first
     * InListPassiveTarget can NACK if PCF8574 is mid-transaction.
     * Also gives IRQ thread time for its first PN532 READY poll cycle.
     * IRQ thread needs ~10-20ms for first I2C poll; 150ms covers shared
     * I2C1 bus (PN532+PCF8574+OLED), no INT/RST pin, voltage drop during
     * read/write/emulate.
     *
     * Note: PN532 uses only SCL/SDA on I2C1 — no INT/IRQ/RST GPIOs.
     * All readiness detection is via the I2C status byte (0x01 == READY),
     * polled by the software ready-poller thread at 40 Hz.  Without HW
     * RST, the only recovery from I2C bus desync is retrying the operation
     * (two-strikes-out ACK mechanism) or a full SAM reinit. */
    furi_delay_ms(1000); /* Clone module: 1000ms I2C settle after mode change.
                         * Covers: (1) PN532 internal state transition,
                         * (2) PCF8574 I/O expander bus release on shared I2C1,
                         * (3) OLED refresh cycle completion on shared I2C1,
                         * (4) Clone PN532 power rail settle after RF reconfiguration.
     * 150ms was insufficient — clone PN532 sometimes NACKs first
     * InListPassiveTarget when PCF8574/OLED still hold bus. */
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_pn532_low_power_mode_start(void) {
    if(!furi_hal_nfc_pn532_is_active()) return FuriHalNfcErrorCommunication;
    // Deinit timers to save power — they'll be re-initialized on mode_stop
    furi_hal_nfc_timers_deinit();
    /* Do NOT clear target cache here. low_power_mode_start() is called
     * between every scanner probe cycle (via nfc_stop()). Clearing the cache
     * here would negate the target preservation in set_mode() same-tech
     * branch, forcing each probe to re-poll via InListPassiveTarget (REQA).
     * REQA doesn't wake cards in IDLE state — only WUPA does.
     * Target cache is preserved across probes so the poller can use cached
     * ATQA/SAK/UID from quick_poll detection.
     * Cache invalidation: tech change in set_mode(), or explicit reset(). */
    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_pn532_low_power_mode_stop(void) {
    if(!furi_hal_nfc_pn532_is_active()) return FuriHalNfcErrorCommunication;
    // nfc.c poller uses TIM1/TIM17 for frame timing even in PN532 mode
    furi_hal_nfc_timers_init();
    /* I2C1 bus settle: PCF8574 may have preempted bus during power-down.
     * Give it time to finish before PN532 starts I2C ops. */
    furi_delay_ms(2);
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
    FURI_LOG_D(
        TAG, "trx_short_frame: ENTER frame=%d is_active=%d", frame, furi_hal_nfc_pn532_is_active());
    UNUSED(frame);
    if(!furi_hal_nfc_pn532_is_active()) {
        /* Wait for PN532 to become ready — the IRQ thread may not have
         * completed its first I2C poll cycle yet (~10-20ms after irq_start).
         * Without this wait, detection always fails because the callback
         * fires before the IRQ thread confirms readiness.
         * 50ms timeout covers worst-case IRQ thread startup + I2C bus settle
         * across multiple I2C1 devices (PCF8574, display). */
        uint32_t wait_start = furi_get_tick();
        while(!furi_hal_nfc_pn532_is_active()) {
            if((furi_get_tick() - wait_start) > 150) {
                FURI_LOG_W(TAG, "trx_short_frame: PN532 not ready after 150ms wait, giving up");
                return FuriHalNfcErrorCommunication;
            }
            furi_delay_ms(5);
        }
        FURI_LOG_D(
            TAG,
            "trx_short_frame: waited %lums for PN532 ready",
            (unsigned long)(furi_get_tick() - wait_start));
    }

    if(furi_hal_nfc_acquire() != FuriHalNfcErrorNone) {
        FURI_LOG_W(TAG, "trx_short_frame: acquire failed (mutex held?)");
        return FuriHalNfcErrorBusy;
    }

    FuriHalNfcError ret = FuriHalNfcErrorNone;

    /* If needs_relist is set, the card was left in HALT/ERROR state by a
     * previous failed auth (native InDataExchange 0x14) or an explicit HLTA.
     * Force a fresh InListPassiveTarget by invalidating the cached target so
     * the re-poll branch below runs.  Without this, the 5-second freshness
     * window causes the cached ATQA to be returned to the Crypto1 fallback
     * even though the card is no longer in ACTIVE state — every subsequent
     * InCommunicateThru auth then times out (0x01), causing the dict-attack
     * crash/freeze. */
    if(furi_hal_nfc_pn532.needs_relist) {
        furi_hal_nfc_pn532.target_valid = false;
        furi_hal_nfc_pn532.needs_relist = false;
    }

    bool use_cached = false;
    if(furi_hal_nfc_pn532.target_valid && furi_hal_nfc_pn532.tech == FuriHalNfcTechIso14443a) {
        int32_t age = (int32_t)(furi_get_tick() - furi_hal_nfc_pn532.target_tick);
        use_cached = (age >= 0 && age < (int32_t)PN532_TARGET_FRESHNESS_TIMEOUT_MS);
    }

    FURI_LOG_D(
        TAG,
        "trx_short_frame: target_valid=%d use_cached=%d tech=%d tick=%lu",
        furi_hal_nfc_pn532.target_valid,
        use_cached,
        furi_hal_nfc_pn532.tech,
        furi_hal_nfc_pn532.target_tick);

    if(use_cached) {
        FURI_LOG_D(
            TAG,
            "trx_short_frame: using cached target (age=%ldms uid_len=%zu)",
            (int32_t)(furi_get_tick() - furi_hal_nfc_pn532.target_tick),
            furi_hal_nfc_pn532.target.uid_len);
    } else {
        FURI_LOG_D(TAG, "trx_short_frame: no cached target, polling...");
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

/* Maximum INF field size for a single ISO-DEP I-block (poller-side).
 * Conservative default matching FSCI=3 (255 bytes). Cards with larger FSCI
 * can handle bigger frames, but 255 is safe for all ISO14443-4 cards. */
#define PN532_ISO_DEP_MAX_INF_SIZE 255

/** Send a large ISO-DEP APDU via poller-side I-block chaining.
 *
 * When the INF payload exceeds PN532_ISO_DEP_MAX_INF_SIZE bytes, this
 * function splits it into multiple I-block fragments. Each fragment is sent
 * as a separate InDataExchange call with the CHAIN bit (PCB bit 4) set.
 * After each fragment, we wait for the card's R(ACK) response before sending
 * the next. The final fragment has CHAIN=0 and the card responds with data.
 *
 * @return FuriHalPn532ErrorNone on success.
 */
static FuriHalPn532Error furi_hal_nfc_pn532_tx_chained(
    uint8_t target_number,
    const uint8_t* tx_inf,
    size_t tx_inf_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len,
    uint8_t* pn532_status) {
    furi_assert(tx_inf);
    furi_assert(rx_data);
    furi_assert(rx_len);

    *rx_len = 0;
    if(pn532_status) *pn532_status = 0;

    FuriHalPn532Error err = FuriHalPn532ErrorNone;
    size_t offset = 0;

    while(offset < tx_inf_len) {
        size_t remaining = tx_inf_len - offset;
        size_t chunk = (remaining > PN532_ISO_DEP_MAX_INF_SIZE) ? PN532_ISO_DEP_MAX_INF_SIZE :
                                                                  remaining;
        bool more = (remaining > chunk);

        /* Build I-block: [PCB] [INF chunk] */
        uint8_t frag_pcb = 0x02; /* I-block, block_num=0 */
        if(more) frag_pcb |= 0x10; /* CHAIN bit */

        uint8_t frag_buf[PN532_ISO_DEP_MAX_INF_SIZE + 1];
        frag_buf[0] = frag_pcb;
        memcpy(&frag_buf[1], &tx_inf[offset], chunk);
        size_t frag_len = 1 + chunk;

        uint8_t frag_rx[ISO14443_4_MAX_APDU_SIZE];
        size_t frag_rx_len = 0;
        uint8_t frag_status = 0;

        err = furi_hal_pn532_in_data_exchange_ex(
            target_number,
            frag_buf,
            frag_len,
            frag_rx,
            sizeof(frag_rx),
            &frag_rx_len,
            &frag_status);

        if(err != FuriHalPn532ErrorNone) {
            FURI_LOG_W(
                TAG,
                "Chained TX: fragment failed at offset %zu: %s",
                offset,
                furi_hal_pn532_error_str(err));
            break;
        }

        if(more) {
            /* Card should respond with R(ACK). Verify it's an R-block ACK. */
            if(frag_rx_len >= 1) {
                uint8_t resp_pcb = frag_rx[0];
                if((resp_pcb & 0xC0) == 0x80 && !(resp_pcb & 0x10)) {
                    FURI_LOG_D(
                        TAG, "Chained TX: fragment %zu/%zu ACKed", offset + chunk, tx_inf_len);
                } else {
                    FURI_LOG_W(TAG, "Chained TX: expected R(ACK), got PCB=0x%02X", resp_pcb);
                    err = FuriHalPn532ErrorComm;
                    break;
                }
            }
        } else {
            /* Final fragment — card responds with data. Capture it. */
            if(frag_rx_len > 0 && frag_rx_len <= rx_size) {
                memcpy(rx_data, frag_rx, frag_rx_len);
                *rx_len = frag_rx_len;
            }
            if(pn532_status) *pn532_status = frag_status;
        }

        offset += chunk;
    }

    return err;
}

static FuriHalNfcError furi_hal_nfc_pn532_exchange_internal(
    const uint8_t* tx_bytes,
    size_t tx_len,
    bool add_parity_to_rx,
    bool strip_crc_from_tx) {
    if(!furi_hal_nfc_pn532_is_active()) {
        FURI_LOG_W(
            TAG,
            "exchange_internal: PN532 not active (active=%d ready=%d)",
            furi_hal_nfc_pn532.active,
            furi_hal_pn532_is_ready());
        return FuriHalNfcErrorCommunication;
    }

    size_t effective_tx_len = tx_len;
    if(strip_crc_from_tx && (effective_tx_len >= sizeof(uint16_t))) {
        effective_tx_len -= sizeof(uint16_t);
    }

    // Pre-compute effective send length (without CRC) for special command detection
    const size_t send_len = strip_crc_from_tx ? effective_tx_len : tx_len;

    // === Backdoor Auth Bypass (FM11RF08 clones) ===
    if(send_len >= 2U && (tx_bytes[0] == 0x64U || tx_bytes[0] == 0x65U)) {
        if(furi_hal_nfc_acquire() != FuriHalNfcErrorNone) {
            FURI_LOG_W(TAG, "exchange_internal: acquire failed (mutex held?)");
            return FuriHalNfcErrorBusy;
        }
        furi_hal_nfc_pn532_queue_push(FuriHalNfcEventTxEnd);
        furi_hal_nfc_pn532_queue_push(FuriHalNfcEventTimerFwtExpired);
        furi_hal_nfc_pn532.rx_bits = 0;
        furi_hal_nfc_pn532_set_result(FuriHalNfcPn532ResultNotPresent);
        furi_hal_nfc_release();
        return FuriHalNfcErrorNone;
    }

    if(furi_hal_nfc_acquire() != FuriHalNfcErrorNone) {
        return FuriHalNfcErrorBusy;
    }

    FuriHalNfcError err_ret = FuriHalNfcErrorNone;

    memset(furi_hal_nfc_pn532.scratch, 0, sizeof(furi_hal_nfc_pn532.scratch));
    uint8_t* const rx_payload = furi_hal_nfc_pn532.scratch;
    size_t rx_len = 0;

    FuriHalPn532Error err;

    // Auto-detect target if not yet valid (Type B, FeliCa)
    // or force re-poll if target is stale (freshness timeout exceeded)
    // IMPORTANT: Never re-poll while PN532 native MIFARE auth is active —
    // InListPassiveTarget destroys the PN532's internal Crypto1 session,
    // causing all subsequent block reads to fail with error 0x06.
    bool target_fresh = false;
    if(furi_hal_nfc_pn532.target_valid) {
        // Explicit check: target_tick==0 means deauth was called and target
        // must be re-polled regardless of elapsed time (Requirement 3.3).
        if(furi_hal_nfc_pn532.target_tick == 0) {
            target_fresh = false;
        } else {
            int32_t age = (int32_t)(furi_get_tick() - furi_hal_nfc_pn532.target_tick);
            target_fresh = (age >= 0 && age < (int32_t)PN532_TARGET_FRESHNESS_TIMEOUT_MS);
        }
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
        FURI_LOG_D(
            TAG,
            "exchange: target_fresh=0 target_valid=%d tx_len=%zu",
            furi_hal_nfc_pn532.target_valid,
            effective_tx_len);
        if(!furi_hal_nfc_pn532.target_valid && effective_tx_len < 3) {
            err_ret = FuriHalNfcErrorCommunication;
            goto release;
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
            err_ret = FuriHalNfcErrorNone;
            goto release;
        }

        // Target found — store it
        furi_hal_nfc_pn532.target = target;
        furi_hal_nfc_pn532.target_valid = true;
        furi_hal_nfc_pn532.needs_relist = false;
        furi_hal_nfc_pn532.target_tick = furi_get_tick();
        AUTH_DIAG_LOG("InListPassiveTarget OK, target_tick=%lu", furi_hal_nfc_pn532.target_tick);

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
                    err_ret = FuriHalNfcErrorCommunication;
                    goto release;
                }
                furi_hal_nfc_pn532.target = target;
                furi_hal_nfc_pn532.needs_relist = false;
            }
            uint8_t atqa[2];
            atqa[0] = furi_hal_nfc_pn532.target.atqa[0];
            atqa[1] = furi_hal_nfc_pn532.target.atqa[1];
            furi_hal_nfc_pn532_prepare_rx(atqa, sizeof(atqa), false, false);
            err_ret = furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532ErrorNone, true);
            goto release;
        }
    }

    // Handle HLTA (Halt Type A) — tag goes to HALT state, target is lost
    if(effective_tx_len == 2U && tx_bytes[0] == 0x50U && tx_bytes[1] == 0x00U) {
        furi_hal_nfc_pn532.needs_relist = true;
        furi_hal_nfc_pn532_queue_push(FuriHalNfcEventTxEnd);
        err_ret = FuriHalNfcErrorNone;
        goto release;
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
                err_ret = FuriHalNfcErrorCommunication;
                goto release;
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
                err_ret = FuriHalNfcErrorCommunication;
                goto release;
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
            err_ret = FuriHalNfcErrorCommunication;
            goto release;
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

    // MIFARE Classic auth — send full frame with CRC via InCommunicateThru
    // InCommunicateThru is a raw transport: PN532 does NOT apply TxCRCEn.
    // The stack's iso14443_crc_append already added CRC-A to tx_bytes.
    // We must send the FULL frame (including CRC) so the card receives a
    // valid ISO14443-3A frame. CIU register writes are not needed —
    // InCommunicateThru ignores TxCRCEn/RxCRCEn entirely.
    if(send_len >= 2U && (tx_bytes[0] == 0x60U || tx_bytes[0] == 0x61U)) {
        AUTH_DIAG_LOG("Auth cmd=0x%02X block=%u len=%zu", tx_bytes[0], tx_bytes[1], tx_len);

        // Send full frame including CRC — InCommunicateThru sends raw bytes
        err = furi_hal_pn532_in_communicate_thru_timeout(
            tx_bytes, tx_len, rx_payload, PN532_MAX_FRAME_SIZE, &rx_len, PN532_TIMEOUT_MF_AUTH_MS);

        AUTH_DIAG_LOG("Auth result: err=%s rx_len=%zu", furi_hal_pn532_error_str(err), rx_len);

        if(err == FuriHalPn532ErrorNone && rx_len == 4) {
            // NT (standard auth) — raw 4-byte nonce, no CRC append
            AUTH_DIAG_LOG(
                "NT: %02X %02X %02X %02X",
                rx_payload[0],
                rx_payload[1],
                rx_payload[2],
                rx_payload[3]);
            furi_hal_nfc_pn532_prepare_rx(rx_payload, rx_len, false, true);
            furi_hal_nfc_pn532.mf_authed = true;
            err_ret = furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532ErrorNone, true);
            goto release;
        }
        if(err == FuriHalPn532ErrorNone && rx_len != 4) {
            FURI_LOG_W(TAG, "Auth: unexpected rx_len=%zu (expected 4)", rx_len);
            err = FuriHalPn532ErrorComm;
        }
        // Auth failed — wrong key, tag enters HALT state.
        // Set needs_relist so next WUPA wakes it up.
        AUTH_DIAG_LOG("Auth timeout, needs_relist set");
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
            // I-block — strip PCB and optional CID/NAD, send INF via InDataExchange.
            // ISO14443-4 chaining: a card may split a long response across multiple
            // I-block fragments (PCB chaining bit set). The PN532 surfaces this in
            // the status byte bit 0x40 (PN532_STATUS_CHAINING). When chaining is
            // active we must drive an R(ACK) loop until the card delivers the
            // final fragment, concatenating each INF payload into a single
            // assembled buffer. The reassembled payload is then reframed as a
            // single I-block to the upper ISO-DEP layer.
            size_t hdr_len = 1;
            if(pcb & 0x08) hdr_len++; // CID present
            if(pcb & 0x04) hdr_len++; // NAD present

            if(hdr_len < send_len) {
                const size_t inf_len = send_len - hdr_len;

                if(inf_len > PN532_ISO_DEP_MAX_INF_SIZE) {
                    /* Poller-side chaining: INF exceeds single I-block limit.
                     * Split into multiple chained I-blocks via tx_chained(). */
                    uint8_t assembled[ISO14443_4_MAX_APDU_SIZE];
                    size_t assembled_len = 0;
                    uint8_t pn532_status = 0;

                    err = furi_hal_nfc_pn532_tx_chained(
                        furi_hal_nfc_pn532.target.target_number,
                        &tx_bytes[hdr_len],
                        inf_len,
                        assembled,
                        sizeof(assembled),
                        &assembled_len,
                        &pn532_status);

                    if(err == FuriHalPn532ErrorNone && assembled_len > 0) {
                        memcpy(rx_payload, assembled, assembled_len);
                        rx_len = assembled_len;
                    }
                } else {
                    /* Single I-block: send INF directly via InDataExchange. */
                    /* First fragment: forward INF bytes via _ex to capture status. */
                    uint8_t assembled[ISO14443_4_MAX_APDU_SIZE];
                    size_t assembled_len = 0;
                    uint8_t pn532_status = 0;

                    err = furi_hal_pn532_in_data_exchange_ex(
                        furi_hal_nfc_pn532.target.target_number,
                        &tx_bytes[hdr_len],
                        inf_len,
                        assembled,
                        sizeof(assembled),
                        &assembled_len,
                        &pn532_status);

                    /* R(ACK) loop: while the PN532 reports chaining, fetch fragments.
                 * R(ACK) PCB per ISO14443-4 §7.5.5 = 0xA2 | (block_num & 1). The
                 * Flipper-side iso_dep_block_num must NOT be toggled per fragment
                 * (the upper layer expects exactly one block-number toggle per
                 * complete response); the bit is just used to build the R(ACK).
                 * On a failed fragment, retry with R(NAK) = 0xB2 | bit0 per
                 * ISO/IEC 14443-4:2016 §7.1.6.2; abandon after 3 failed attempts. */
                    size_t fragment_retry_count = 0;
                    while(err == FuriHalPn532ErrorNone && assembled_len > 0 &&
                          (pn532_status & PN532_STATUS_CHAINING)) {
                        const uint8_t r_pcb = (fragment_retry_count > 0) ?
                            (uint8_t)(0xA2U | 0x10U |
                                      (furi_hal_nfc_pn532.iso_dep_block_num & 1U)) :
                            (uint8_t)(0xA2U | (furi_hal_nfc_pn532.iso_dep_block_num & 1U));

                        uint8_t frag[PN532_MAX_FRAME_SIZE];
                        size_t frag_len = 0;
                        pn532_status = 0;

                        err = furi_hal_pn532_in_data_exchange_ex(
                            furi_hal_nfc_pn532.target.target_number,
                            &r_pcb,
                            1,
                            frag,
                            sizeof(frag),
                            &frag_len,
                            &pn532_status);

                        if(err != FuriHalPn532ErrorNone || frag_len == 0) {
                            fragment_retry_count++;
                            if(fragment_retry_count > 2) {
                                FURI_LOG_E(TAG, "Chained TX: fragment failed after 3 retries");
                                err_ret = FuriHalNfcErrorCommunicationTimeout;
                                goto release;
                            }
                            err = FuriHalPn532ErrorNone;
                            continue;
                        }
                        fragment_retry_count = 0;

                        /* Overflow guard: assembled buffer cannot exceed max APDU size. */
                        if(assembled_len + frag_len > ISO14443_4_MAX_APDU_SIZE) {
                            FURI_LOG_E(
                                TAG,
                                "ISO-DEP chaining overflow: %zu + %zu > %u",
                                assembled_len,
                                frag_len,
                                (unsigned)ISO14443_4_MAX_APDU_SIZE);
                            furi_hal_nfc_pn532.last_error = FuriHalPn532ErrorBufferOverflow;
                            furi_hal_nfc_pn532.last_error_tick = furi_get_tick();
                            err_ret = FuriHalNfcErrorBufferOverflow;
                            goto release;
                        }

                        memcpy(&assembled[assembled_len], frag, frag_len);
                        assembled_len += frag_len;
                    }

                    if(err == FuriHalPn532ErrorNone && assembled_len > 0) {
                        /* Copy assembled payload into scratch where prepare_rx / CRC
                     * helpers expect it, then build the wrapper I-block. */
                        memcpy(rx_payload, assembled, assembled_len);
                        rx_len = assembled_len;
                    }
                } /* end single I-block path */
            } else {
                err = FuriHalPn532ErrorComm;
            }

            if(err == FuriHalPn532ErrorNone && rx_len > 0) {
                // Rebuild I-block response: [PCB] [data] [CRC]
                uint8_t resp_pcb = 0x02 | (furi_hal_nfc_pn532.iso_dep_block_num & 1);
                furi_hal_nfc_pn532.iso_dep_block_num ^= 1;

                if(rx_len > sizeof(furi_hal_nfc_pn532.scratch) - 3)
                    rx_len = sizeof(furi_hal_nfc_pn532.scratch) - 3;
                memmove(&furi_hal_nfc_pn532.scratch[1], furi_hal_nfc_pn532.scratch, rx_len);
                furi_hal_nfc_pn532.scratch[0] = resp_pcb;
                size_t iob_len = 1 + rx_len;
                if(iob_len + 2 > sizeof(furi_hal_nfc_pn532.scratch))
                    iob_len = sizeof(furi_hal_nfc_pn532.scratch) - 2;
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
            furi_hal_nfc_pn532.iso_dep_block_num ^=
                1U; // Toggle sequence number for hybrid compatibility
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
        // Normal (non-ISO-DEP) mode: send data via InDataExchange.
        // After native auth (mf_authed=true), PN532 handles Crypto1 transparently.
        // Before auth, PN532 adds proper ISO14443-3A CRC automatically.
        // Auth (0x60/0x61) is handled by the early-return branch above.
        const size_t ide_len = strip_crc_from_tx ? tx_len : send_len;

        err = furi_hal_pn532_in_data_exchange(
            furi_hal_nfc_pn532.target.target_number,
            tx_bytes,
            ide_len,
            rx_payload,
            PN532_MAX_FRAME_SIZE,
            &rx_len);
    }

    /* Post-error settle: clone PN532 modules need recovery time after
     * genuine communication failure (no response at all).  Timeouts from
     * normal protocol events (wrong key, card HALT) are handled by the
     * target_valid logic below and don't need the settle delay. */
    if(err == FuriHalPn532ErrorComm) {
        furi_delay_ms(50);
        /* Do NOT force full reinit on every comm error — transient I2C
         * glitches (bus contention) are common on shared I2C1 and do not
         * indicate PN532 state corruption.  The exchange_fail_count in
         * pn532_exchange() forces reinit after N consecutive failures,
         * and the two-strikes ACK handler catches genuine chip failures. */
    }

    // Target loss detection: mark invalid on communication failure
    // Skip invalidation when abort is pending — the error is from an
    // interrupted exchange, not a genuine target loss.
    // Don't invalidate target on timeout — transient InCommunicateThru
    // failures (wrong command format, wrong key, RF glitch) don't mean
    // the tag vanished.  keeps target_valid=true so the poller can retry
    // with needs_relist (handled by trx_short_frame).  Only invalidate on
    // genuine communication error (no PN532 response at all).
    if(err == FuriHalPn532ErrorComm) {
        if(!(furi_thread_flags_get() & FuriHalNfcEventInternalTypeAbort)) {
            FURI_LOG_W(
                "PN532Nfc",
                "Target communication failed (err=%s), marking invalid",
                furi_hal_pn532_error_str(err));
            furi_hal_nfc_pn532.target_valid = false;
            furi_hal_nfc_pn532.mf_authed = false;
            furi_hal_nfc_pn532.iso_dep_mode = false;
        }
    } else if(err == FuriHalPn532ErrorTimeout) {
        /* Clone PN532 timeout: target may be in undefined state (PN532 internal
         * RF state machine stuck). Force re-poll on next attempt. */
        furi_hal_nfc_pn532.target_valid = false;
        furi_hal_nfc_pn532.mf_authed = false;
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
    if(!rx_data || !rx_bits) {
        furi_hal_nfc_release();
        return FuriHalNfcErrorDataFormat;
    }
    const size_t rx_bytes = (furi_hal_nfc_pn532.rx_bits + 7U) / 8U;
    if(rx_bytes > rx_data_size) {
        furi_hal_nfc_release();
        return FuriHalNfcErrorBufferOverflow;
    }

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

// Listener (target) mode: configure FeliCa NFCID2t for emulation
void furi_hal_nfc_pn532_listener_set_felica_params(const uint8_t* nfcid2t) {
    furi_check(nfcid2t);
    memcpy(furi_hal_nfc_pn532.felica_nfcid2t, nfcid2t, 8);
    furi_hal_nfc_pn532.felica_nfcid2t_configured = true;
    furi_hal_nfc_pn532.listener_active = false;
}

void furi_hal_nfc_pn532_emu_set_ndef(const uint8_t* msg, size_t len) {
    if(msg == NULL || len == 0 || len > sizeof(furi_hal_nfc_pn532.emu_ndef_msg)) {
        furi_hal_nfc_pn532.emu_ndef_len = 0;
        return;
    }
    memcpy(furi_hal_nfc_pn532.emu_ndef_msg, msg, len);
    furi_hal_nfc_pn532.emu_ndef_len = len;
}

static FuriHalNfcEvent pn532_type4_ndef_emulate(void) {
    /* CCLEN=000F, mapping v2.0, MLe=003B, MLc=0034,
     * NDEF File Control TLV: T=04 L=06 FileID=E104 MaxLen WriteAccess(FF=ro) */
    size_t ndef_file_len = 2 + furi_hal_nfc_pn532.emu_ndef_len; /* NLEN(2) + message */
    uint16_t ndef_cap = (ndef_file_len > 0xFFFE) ? 0xFFFE : (uint16_t)ndef_file_len;
    uint8_t cc[15] = {
        0x00,
        0x0F,
        0x20,
        0x00,
        0x3B,
        0x00,
        0x34,
        0x04,
        0x06,
        0xE1,
        0x04,
        (uint8_t)(ndef_cap >> 8),
        (uint8_t)(ndef_cap & 0xFF),
        0x00,
        0xFF};

    uint8_t tg_init[] = {0x00, 0x08, 0x00, 0xDC, 0x44, 0x20, 0x60, 0x01, 0xFE, 0xA2, 0xA3,
                         0xA4, 0xA5, 0xA6, 0xA7, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6,
                         0xC7, 0xFF, 0xFF, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33,
                         0x22, 0x11, 0x01, 0x00, 0x0D, 0x52, 0x46, 0x49, 0x44, 0x49, 0x4F,
                         0x74, 0x20, 0x50, 0x4E, 0x35, 0x33, 0x32};

    enum {
        FILE_NONE,
        FILE_CC,
        FILE_NDEF
    } cur_file = FILE_NONE;
    bool armed = false;
    uint32_t tginit_tries = 0;

    FURI_LOG_I(TAG, "Type-4 NDEF emu: enter (ndef_len=%zu)", furi_hal_nfc_pn532.emu_ndef_len);

    while(true) {
        if(furi_thread_flags_get() & FuriHalNfcEventInternalTypeAbort) {
            furi_thread_flags_clear(FuriHalNfcEventInternalTypeAbort);
            return FuriHalNfcEventAbortRequest;
        }

        if(!armed) {
            FuriHalPn532Error e = furi_hal_pn532_tg_init_as_target(tg_init, sizeof(tg_init), 1500);
            if(e == FuriHalPn532ErrorNone) {
                armed = true;
                cur_file = FILE_NONE;
                FURI_LOG_I(
                    TAG,
                    "Type-4 emu: target ACTIVATED after %lu tries",
                    (unsigned long)tginit_tries);
            } else {
                if(tginit_tries == 0 || (tginit_tries % 100) == 0) {
                    FURI_LOG_I(
                        TAG,
                        "Type-4 emu: TgInitAsTarget waiting (err=%s try=%lu)",
                        furi_hal_pn532_error_str(e),
                        (unsigned long)tginit_tries);
                }
                tginit_tries++;
                furi_delay_ms(20);
                continue;
            }
        }

        /* TgGetData: fetch reader APDU */
        uint8_t req[262];
        size_t req_len = 0;
        FuriHalPn532Error ge = furi_hal_pn532_tg_get_data(req, sizeof(req), &req_len, 500);

        if(ge != FuriHalPn532ErrorNone) {
            if(ge == FuriHalPn532ErrorTimeout) {
                /* No data this slice, check abort and loop again */
                furi_delay_ms(10);
                continue;
            } else {
                /* RF lost or other comm error — re-arm target */
                FURI_LOG_I(
                    TAG,
                    "Type-4 emu: RF lost or comm error (err=%s) — re-arming",
                    furi_hal_pn532_error_str(ge));
                armed = false;
                furi_delay_ms(10);
                continue;
            }
        }

        if(req_len < 4) {
            furi_delay_ms(5);
            continue;
        }

        /* APDU = req : CLA INS P1 P2 LC [DATA] [LE] */
        uint8_t ins = req[1];
        uint8_t p1 = req[2];
        uint8_t p2 = req[3];
        uint8_t lc = (req_len >= 5) ? req[4] : 0;
        uint16_t off = ((uint16_t)p1 << 8) | p2;

        FURI_LOG_I(
            TAG,
            "Type-4 emu: APDU CLA=%02X INS=%02X P1P2=%04X Lc=%02X (req_len=%u)",
            req[0],
            ins,
            off,
            lc,
            (unsigned)req_len);

        uint8_t resp[260];
        size_t resp_len = 0;

        if(ins == 0xA4) { /* SELECT */
            if(p1 == 0x04) { /* by name (AID) */
                resp[0] = 0x90;
                resp[1] = 0x00;
                resp_len = 2;
            } else if(p1 == 0x00 && lc == 2 && req_len >= 7) { /* by file id */
                uint8_t f0 = req[5], f1 = req[6];
                if(f0 == 0xE1 && f1 == 0x03) {
                    cur_file = FILE_CC;
                    resp[0] = 0x90;
                    resp[1] = 0x00;
                    resp_len = 2;
                } else if(f0 == 0xE1 && f1 == 0x04) {
                    cur_file = FILE_NDEF;
                    resp[0] = 0x90;
                    resp[1] = 0x00;
                    resp_len = 2;
                } else {
                    resp[0] = 0x6A;
                    resp[1] = 0x82;
                    resp_len = 2;
                }
            } else {
                resp[0] = 0x6A;
                resp[1] = 0x82;
                resp_len = 2;
            }
        } else if(ins == 0xB0) { /* READ BINARY */
            uint8_t le = (req_len >= 5) ? req[4] : 0;
            size_t want = (le == 0) ? 0xFF : le;
            if(want > sizeof(resp) - 2) want = sizeof(resp) - 2;
            if(cur_file == FILE_CC) {
                for(size_t i = 0; i < want; i++)
                    resp[i] = (off + i < sizeof(cc)) ? cc[off + i] : 0x00;
                resp_len = want;
            } else if(cur_file == FILE_NDEF) {
                for(size_t i = 0; i < want; i++) {
                    size_t idx = off + i;
                    uint8_t b;
                    if(idx == 0)
                        b = (uint8_t)(furi_hal_nfc_pn532.emu_ndef_len >> 8);
                    else if(idx == 1)
                        b = (uint8_t)(furi_hal_nfc_pn532.emu_ndef_len & 0xFF);
                    else if(idx - 2 < furi_hal_nfc_pn532.emu_ndef_len)
                        b = furi_hal_nfc_pn532.emu_ndef_msg[idx - 2];
                    else
                        b = 0x00;
                    resp[i] = b;
                }
                resp_len = want;
            } else {
                resp_len = 0;
            }
            resp[resp_len++] = 0x90;
            resp[resp_len++] = 0x00;
        } else { /* UPDATE BINARY / unsupported → read-only */
            resp[0] = 0x6A;
            resp[1] = 0x81;
            resp_len = 2;
        }

        /* TgSetData: send R-APDU back to the reader */
        FuriHalPn532Error se = furi_hal_pn532_tg_set_data(resp, resp_len);
        if(se != FuriHalPn532ErrorNone) {
            FURI_LOG_W(TAG, "Type-4 emu: TgSetData failed (err=%s)", furi_hal_pn532_error_str(se));
            armed = false;
            furi_delay_ms(10);
        }
    }
}

// Listener (target) mode: wait for reader activation or data
FuriHalNfcEvent furi_hal_nfc_pn532_listener_wait_event(uint32_t timeout_ms) {
    if(!furi_hal_nfc_pn532_is_active()) return FuriHalNfcEventTimeout;

    if(furi_hal_nfc_pn532.emu_ndef_len > 0) {
        return pn532_type4_ndef_emulate();
    }

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
                /* FeliCa TgInitAsTarget params (19 bytes total per NXP UM0701-02
                 * §7.3.16 and nfcpy pn532.tg_init_as_target which enforces
                 * len(felica_params) == 18 — the 18-byte sub-block plus a
                 * separate 1-byte mode prefix):
                 * [0]      mode = 0x02 (passive 212/424 kbps)
                 * [1..8]   NFCID2t (8 bytes) — the FeliCa card's IDm
                 * [9..16]  PAD (8 bytes, zeroed)
                 * [17..18] SystemCode (2 bytes; 0x12,0xFC = NFC-F generic) */
                uint8_t params[19];
                memset(params, 0, sizeof(params));
                params[0] = 0x02; /* mode: passive FeliCa */

                /* NFCID2t default per NFC-F spec section 6.2.1: manufacturer
                 * code 0x01 with random prefix 0xFE. Used when no NFCID2t has
                 * been explicitly configured via set_felica_params(). */
                static const uint8_t felica_nfcid2t_default[8] = {
                    0x01, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

                const uint8_t* const nfcid2t = furi_hal_nfc_pn532.felica_nfcid2t_configured ?
                                                   furi_hal_nfc_pn532.felica_nfcid2t :
                                                   felica_nfcid2t_default;

                memcpy(&params[1], nfcid2t, 8); /* NFCID2t at bytes 1..8 */
                params[17] = 0x12; /* SENSF_RES system code high */
                params[18] = 0xFC; /* SENSF_RES system code low  */

                err = furi_hal_pn532_tg_init_as_target(params, sizeof(params), slice_ms);
            } else {
                // ISO14443-4A target mode — use configured UID/ATQA/SAK if available
                // Format: [mode(1)] [SENS_RES(2)] [NFCID1(3)] [SEL_RES(1)]
                //         [FeliCa params(18)] [NFCID3t(10)] [Gt_len(1)] [Tk_len(1)] = 36 bytes
                uint8_t params[36];
                memset(params, 0, sizeof(params));
                params[0] = 0x05; /* mode: passive 106kbps, PICC only */

                if(furi_hal_nfc_pn532.listener_configured) {
                    const uint8_t* const uid = furi_hal_nfc_pn532.listener_uid;
                    const uint8_t uid_len = furi_hal_nfc_pn532.listener_uid_len;
                    const uint8_t sak = furi_hal_nfc_pn532.listener_sak;

                    params[1] = furi_hal_nfc_pn532.listener_atqa[0];
                    params[2] = furi_hal_nfc_pn532.listener_atqa[1];

                    if(uid_len <= 3) {
                        /* 3-byte UID: copy directly into NFCID1, SAK unmodified */
                        params[3] = (uid_len >= 1) ? uid[0] : 0x00;
                        params[4] = (uid_len >= 2) ? uid[1] : 0x00;
                        params[5] = (uid_len >= 3) ? uid[2] : 0x00;
                        params[6] = sak;
                    } else {
                        /* 4 or 7-byte UID: cascade level 1.
                         * NFCID1 = [CT=0x88, uid[0], uid[1]] tells the reader the
                         * UID continues at the next anticollision level.
                         * For 4-byte UIDs, final SAK = sak | 0x04 (cascade bit).
                         * For 7-byte UIDs, level-1 SAK = 0x04; the PN532 handles
                         * cascade level 2 internally using the NFCID3t field. */
                        params[3] = 0x88; /* CT — Cascade Tag */
                        params[4] = uid[0];
                        params[5] = uid[1];
                        params[6] = (uid_len == 4) ? (uint8_t)(sak | 0x04) : 0x04;
                    }
                } else {
                    /* Defaults: generic ISO14443-4 tag */
                    params[1] = 0x04;
                    params[2] = 0x00; /* ATQA */
                    params[3] = 0x08;
                    params[4] = 0x00;
                    params[5] = 0x00; /* NFCID1 */
                    params[6] = 0x20; /* SAK: ISO-DEP capable */
                }
                /* NFCID3t: copy full UID into bytes 25-34 (used by ISO-DEP readers
                 * and by the PN532 for cascade level 2 of 7-byte UIDs) */
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
            const size_t bit_off = bit_index % 8U;
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

    // Strip 2 CRC bytes only for raw byte (non-parity) ISO14443 path
    // FeliCa uses parity-encoded 9-bit streams and has no trailing CRC to strip
    if(byte_count >= 3 && (tx_bits % 9U) != 0U) {
        byte_count -= 2;
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
    return furi_hal_pn532_error_str(furi_hal_nfc_pn532.last_error);
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
    // Skip native auth if this PN532 clone doesn't support it (0x13 on first try)
    if(pn532_mf_auth_unsupported) {
        return FuriHalNfcErrorCommunication;
    }

    if(furi_hal_nfc_pn532.needs_relist) {
        FuriHalPn532Target target;
        if(!furi_hal_pn532_poll_iso14443a_timeout(&target, PN532_TIMEOUT_POLL_MS)) {
            furi_hal_nfc_pn532.target_valid = false;
            furi_hal_nfc_pn532.mf_authed = false;
            furi_hal_nfc_pn532.needs_relist = false;
            return FuriHalNfcErrorCommunication;
        }
        furi_hal_nfc_pn532.target = target;
        furi_hal_nfc_pn532.needs_relist = false;
        FURI_LOG_D(TAG, "Re-detected target before auth");
    }

    AUTH_DIAG_LOG("Native auth: block=%d key_type=%d", block_num, key_type);
    FuriHalPn532Error err = furi_hal_pn532_mf_auth(
        furi_hal_nfc_pn532.target.target_number, block_num, key, key_type, uid, uid_len);
    AUTH_DIAG_LOG("Native auth result: err=%s", furi_hal_pn532_error_str(err));
    if(err == FuriHalPn532ErrorNone) {
        furi_hal_nfc_pn532.mf_authed = true;
        FURI_LOG_D(TAG, "PN532 native auth OK: block=%d key_type=%d", block_num, key_type);
        return FuriHalNfcErrorNone;
    }
    if(err == FuriHalPn532ErrorUnsupported) {
        // 0x13: clone PN532 doesn't support native auth — set flag, skip deauth
        pn532_mf_auth_unsupported = true;
        furi_hal_nfc_pn532.mf_authed = false;
        FURI_LOG_W(TAG, "Native MF auth unsupported (0x13), disabling for session");
        return FuriHalNfcErrorNotSupported;
    }
    furi_hal_nfc_pn532.mf_authed = false;
    FURI_LOG_D(
        TAG, "PN532 native auth failed: block=%d err=%s", block_num, furi_hal_pn532_error_str(err));
    return FuriHalNfcErrorCommunication;
}

bool furi_hal_nfc_pn532_mf_is_authed(void) {
    return furi_hal_nfc_pn532.mf_authed;
}

void furi_hal_nfc_pn532_mf_deauth(void) {
    /* Always set needs_relist so the next WUPA/REQA (via trx_short_frame or
     * exchange_internal) forces a fresh InListPassiveTarget.
     *
     * After a FAILED native auth (0x14): the card is in HALT/ERROR state.
     * needs_relist=true causes trx_short_frame to invalidate the cached target
     * and re-poll, waking the card before the Crypto1 fallback auth is sent.
     *
     * After a SUCCESSFUL auth (mf_authed=true, called from mf_classic_poller_halt):
     * needs_relist=true is also correct — the card must be re-selected before
     * the next sector's auth sequence.
     *
     * CRITICAL: Also clear target_tick so that exchange_internal() sees a STALE
     * target on the next call (e.g. Crypto1 fallback auth via InCommunicateThru).
     * Without this, the target's timestamp remains fresh within the 5-second
     * window, exchange_internal() skips the re-poll, and InCommunicateThru sends
     * auth commands to a card still in HALT state — consistently timing out. */
    furi_hal_nfc_pn532.needs_relist = true;
    furi_hal_nfc_pn532.mf_authed = false;
    furi_hal_nfc_pn532.target_tick = 0;
    furi_hal_nfc_pn532_release_if_listed();
}

void furi_hal_nfc_pn532_release_if_listed(void) {
    /* Idempotent teardown helper. Sends InRelease(0x52, 0x00) only if a
     * target is currently in-listed at the PN532, then clears the local
     * state. Called from every *_poller_free() path (15 pollers) and from
     * furi_hal_nfc_pn532_mf_deauth() so the PN532 internal "in-listed
     * targets" table is always in sync with our HAL's idea of the
     * session lifecycle. Errors are swallowed — InRelease is best-effort
     * cleanup, not a protocol operation. */
    if(!furi_hal_nfc_pn532_is_active()) return;
    if(!furi_hal_nfc_pn532.target_valid) return;

    furi_hal_pn532_in_release();

    furi_hal_nfc_pn532.target_valid = false;
    furi_hal_nfc_pn532.needs_relist = false;
    furi_hal_nfc_pn532.mf_authed = false;
    furi_hal_nfc_pn532.target_tick = 0;
    furi_hal_nfc_pn532.iso_dep_mode = false;
}

/* =========================================================================
 * Test accessors — expose internal state for property-based testing.
 * These are intentionally simple getters/setters with no side effects.
 * ========================================================================= */

uint8_t furi_hal_nfc_pn532_get_target_number(void) {
    return furi_hal_nfc_pn532.target.target_number;
}

bool furi_hal_nfc_pn532_test_get_needs_relist(void) {
    return furi_hal_nfc_pn532.needs_relist;
}

uint32_t furi_hal_nfc_pn532_test_get_target_tick(void) {
    return furi_hal_nfc_pn532.target_tick;
}

void furi_hal_nfc_pn532_test_set_state(bool mf_authed, bool needs_relist, uint32_t target_tick) {
    furi_hal_nfc_pn532.mf_authed = mf_authed;
    furi_hal_nfc_pn532.needs_relist = needs_relist;
    furi_hal_nfc_pn532.target_tick = target_tick;
}

FuriHalNfcError
    furi_hal_nfc_pn532_mf_read_block(uint8_t block_num, uint8_t* data, size_t data_size) {
    if(!furi_hal_nfc_pn532_is_active()) return FuriHalNfcErrorCommunication;
    if(!furi_hal_nfc_pn532.mf_authed) return FuriHalNfcErrorCommunication;
    if(data_size < 16U) return FuriHalNfcErrorBufferOverflow;

    uint8_t cmd[2] = {0x30, block_num}; // MIFARE READ command
    uint8_t resp[19]; // 1 PN532 status byte + 16 data + 2 CRC
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

FuriHalNfcError
    furi_hal_nfc_pn532_mf_write_block(uint8_t block_num, const uint8_t* data, size_t data_size) {
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
        furi_hal_nfc_pn532.target.target_number, data, 16U, resp2, sizeof(resp2), &resp2_len);

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
    const bool found = furi_hal_pn532_poll_iso14443a_timeout(
        &target, 350); /* PN532_TIMEOUT_POLL_MS — InListPassiveTarget needs up to 350ms */

    if(found) {
        /* Log target details for false-positive rate tracking.
         * A false-positive quick_poll (returns true but no genuine card)
         * typically has all-zero UID or a fixed noise pattern like 0x00/0xFF
         * repeated.  Log the full UID, SAK, and ATQA for analysis. */
        FURI_LOG_D(
            TAG,
            "quick_poll: found target UID=%02X%02X%02X%02X%02X%02X%02X SAK=0x%02X ATQA=0x%02X%02X len=%u",
            (unsigned)(target.uid_len > 0 ? target.uid[0] : 0),
            (unsigned)(target.uid_len > 1 ? target.uid[1] : 0),
            (unsigned)(target.uid_len > 2 ? target.uid[2] : 0),
            (unsigned)(target.uid_len > 3 ? target.uid[3] : 0),
            (unsigned)(target.uid_len > 4 ? target.uid[4] : 0),
            (unsigned)(target.uid_len > 5 ? target.uid[5] : 0),
            (unsigned)(target.uid_len > 6 ? target.uid[6] : 0),
            (unsigned)target.sak,
            (unsigned)target.atqa[0],
            (unsigned)target.atqa[1],
            (unsigned)target.uid_len);
    }

    return found;
}

bool furi_hal_nfc_pn532_poll_jewel(FuriHalPn532Target* target) {
    if(!furi_hal_nfc_pn532_is_active()) return false;
    return furi_hal_pn532_poll_jewel(target);
}
