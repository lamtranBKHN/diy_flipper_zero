#include "furi_hal_nfc_pn532.h"

#include "furi_hal_pn532.h"
#include "furi_hal_nfc_i.h"

#include <furi.h>
#include <string.h>

#define PN532_NFCA_CT                 0x88
#define PN532_EVENT_QUEUE_CAPACITY    8
#define PN532_MAX_FRAME_SIZE          192

typedef struct {
    bool active;
    bool target_valid;
    FuriHalNfcMode mode;
    FuriHalNfcTech tech;
    FuriHalPn532Target target;
    uint8_t rx_buffer[PN532_MAX_FRAME_SIZE];
    size_t rx_bits;
    FuriHalNfcEvent event_queue[PN532_EVENT_QUEUE_CAPACITY];
    size_t event_head;
    size_t event_count;
} FuriHalNfcPn532State;

static FuriHalNfcPn532State furi_hal_nfc_pn532 = {0};

static void furi_hal_nfc_pn532_queue_reset(void) {
    furi_hal_nfc_pn532.event_head = 0;
    furi_hal_nfc_pn532.event_count = 0;
}

static bool furi_hal_nfc_pn532_queue_push(FuriHalNfcEvent event) {
    if(furi_hal_nfc_pn532.event_count >= PN532_EVENT_QUEUE_CAPACITY) return false;
    const size_t index =
        (furi_hal_nfc_pn532.event_head + furi_hal_nfc_pn532.event_count) %
        PN532_EVENT_QUEUE_CAPACITY;
    furi_hal_nfc_pn532.event_queue[index] = event;
    furi_hal_nfc_pn532.event_count++;
    return true;
}

static bool furi_hal_nfc_pn532_queue_pop(FuriHalNfcEvent* event) {
    if(furi_hal_nfc_pn532.event_count == 0) return false;
    *event = furi_hal_nfc_pn532.event_queue[furi_hal_nfc_pn532.event_head];
    furi_hal_nfc_pn532.event_head =
        (furi_hal_nfc_pn532.event_head + 1) % PN532_EVENT_QUEUE_CAPACITY;
    furi_hal_nfc_pn532.event_count--;
    return true;
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
    uint8_t frame[PN532_MAX_FRAME_SIZE] = {0};
    size_t frame_len = data_len;
    if(frame_len > sizeof(frame)) return false;

    if(frame_len) memcpy(frame, data, frame_len);

    if(append_crc) {
        if((frame_len + sizeof(uint16_t)) > sizeof(frame)) return false;
        const uint16_t crc = furi_hal_nfc_pn532_crc_a(frame, frame_len);
        memcpy(&frame[frame_len], &crc, sizeof(crc));
        frame_len += sizeof(crc);
    }

    memset(furi_hal_nfc_pn532.rx_buffer, 0, sizeof(furi_hal_nfc_pn532.rx_buffer));

    if(add_parity) {
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

void furi_hal_nfc_pn532_reset(void) {
    furi_hal_nfc_pn532.target_valid = false;
    furi_hal_nfc_pn532.mode = FuriHalNfcModeNum;
    furi_hal_nfc_pn532.tech = FuriHalNfcTechInvalid;
    furi_hal_nfc_pn532.rx_bits = 0;
    memset(&furi_hal_nfc_pn532.target, 0, sizeof(furi_hal_nfc_pn532.target));
    memset(furi_hal_nfc_pn532.rx_buffer, 0, sizeof(furi_hal_nfc_pn532.rx_buffer));
    furi_hal_nfc_pn532_queue_reset();
}

FuriHalNfcError furi_hal_nfc_pn532_set_mode(FuriHalNfcMode mode, FuriHalNfcTech tech) {
    if(!furi_hal_nfc_pn532_is_active()) return FuriHalNfcErrorCommunication;
    if((mode != FuriHalNfcModePoller) || (tech != FuriHalNfcTechIso14443a)) {
        return FuriHalNfcErrorCommunication;
    }

    furi_hal_nfc_pn532_reset();
    furi_hal_nfc_pn532.mode = mode;
    furi_hal_nfc_pn532.tech = tech;
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
    FuriHalPn532Target target = {0};
    return furi_hal_nfc_pn532_is_active() && furi_hal_pn532_poll_iso14443a(&target);
}

FuriHalNfcError furi_hal_nfc_pn532_poller_field_on(void) {
    return furi_hal_nfc_pn532_is_active() ? FuriHalNfcErrorNone : FuriHalNfcErrorCommunication;
}

FuriHalNfcEvent furi_hal_nfc_pn532_wait_event(uint32_t timeout_ms) {
    FuriHalNfcEvent event = 0;
    if(furi_hal_nfc_pn532_queue_pop(&event)) {
        return event;
    }

    const uint32_t wait_timeout =
        timeout_ms == FURI_HAL_NFC_EVENT_WAIT_FOREVER ? FuriWaitForever : timeout_ms;
    const uint32_t event_flag =
        furi_thread_flags_wait(
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

    FuriHalPn532Target target = {0};
    const bool found = furi_hal_pn532_poll_iso14443a(&target);
    furi_hal_nfc_pn532.target = target;
    furi_hal_nfc_pn532.target_valid = found;

    furi_hal_nfc_pn532_queue_push(FuriHalNfcEventTxEnd);
    if(found && furi_hal_nfc_pn532_prepare_rx(target.atqa, sizeof(target.atqa), false, false)) {
        furi_hal_nfc_pn532_queue_push(FuriHalNfcEventRxStart);
        furi_hal_nfc_pn532_queue_push(FuriHalNfcEventRxEnd);
    }

    return FuriHalNfcErrorNone;
}

static FuriHalNfcError furi_hal_nfc_pn532_exchange_internal(
    const uint8_t* tx_bytes,
    size_t tx_len,
    bool add_parity_to_rx,
    bool strip_crc_from_tx) {
    if(!furi_hal_nfc_pn532_is_active() || !furi_hal_nfc_pn532.target_valid) {
        return FuriHalNfcErrorCommunication;
    }

    uint8_t rx_payload[PN532_MAX_FRAME_SIZE] = {0};
    size_t rx_len = 0;

    size_t effective_tx_len = tx_len;
    if(strip_crc_from_tx && (effective_tx_len >= sizeof(uint16_t))) {
        effective_tx_len -= sizeof(uint16_t);
    }

    // Handle HLTA (Halt Type A) — tag goes to HALT state, target is lost
    if(effective_tx_len == 2U && tx_bytes[0] == 0x50U && tx_bytes[1] == 0x00U) {
        furi_hal_nfc_pn532.target_valid = false;
        furi_hal_nfc_pn532_queue_push(FuriHalNfcEventTxEnd);
        return FuriHalNfcErrorNone;
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
                return FuriHalNfcErrorCommunication;
            }

            const uint8_t bcc = nfcid[0] ^ nfcid[1] ^ nfcid[2] ^ nfcid[3];
            uint8_t response[5] = {nfcid[0], nfcid[1], nfcid[2], nfcid[3], bcc};
            furi_hal_nfc_pn532_prepare_rx(response, sizeof(response), false, false);
            return furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532ErrorNone, true);
        }

        // SELECT — synthesize SAK+CRC from stored target
        if(((sel_cmd == 0x93U) || (sel_cmd == 0x95U) || (sel_cmd == 0x97U)) &&
           (sel_par == 0x70U)) {
            uint8_t nfcid[4] = {0};
            uint8_t sak = 0;
            const uint8_t cascade_level = (sel_cmd - 0x93U) / 2U;
            if(!furi_hal_nfc_pn532_build_cascade_response(cascade_level, nfcid, &sak)) {
                return FuriHalNfcErrorCommunication;
            }

            const uint8_t response[1] = {sak};
            furi_hal_nfc_pn532_prepare_rx(response, sizeof(response), true, false);
            return furi_hal_nfc_pn532_finalize_exchange(FuriHalPn532ErrorNone, true);
        }
    }

    // RATS, APDU, and all other commands — forward to PN532 InDataExchange
    const FuriHalPn532Error err =
        furi_hal_pn532_in_data_exchange(
            furi_hal_nfc_pn532.target.target_number,
            tx_bytes,
            effective_tx_len,
            rx_payload,
            sizeof(rx_payload),
            &rx_len);

    // Target loss detection: mark invalid on communication failure
    if(err == FuriHalPn532ErrorComm || err == FuriHalPn532ErrorTimeout) {
        FURI_LOG_W("PN532Nfc", "Target communication failed (err=%d), marking invalid", err);
        furi_hal_nfc_pn532.target_valid = false;
    }

    const bool response_ready =
        (err == FuriHalPn532ErrorNone) &&
        (rx_len > 0U) &&
        furi_hal_nfc_pn532_prepare_rx(rx_payload, rx_len, !add_parity_to_rx, add_parity_to_rx);

    return furi_hal_nfc_pn532_finalize_exchange(err, response_ready);
}

FuriHalNfcError furi_hal_nfc_pn532_tx(const uint8_t* tx_data, size_t tx_bits) {
    if((tx_bits % 8U) != 0U) return FuriHalNfcErrorDataFormat;
    return furi_hal_nfc_pn532_exchange_internal(tx_data, tx_bits / 8U, false, true);
}

FuriHalNfcError furi_hal_nfc_pn532_tx_custom_parity(const uint8_t* tx_data, size_t tx_bits) {
    uint8_t tx_bytes[PN532_MAX_FRAME_SIZE] = {0};
    const size_t tx_len =
        furi_hal_nfc_pn532_unpack_parity_frame(tx_data, tx_bits, tx_bytes, sizeof(tx_bytes));
    if(tx_len == 0U) return FuriHalNfcErrorDataFormat;

    return furi_hal_nfc_pn532_exchange_internal(tx_bytes, tx_len, true, false);
}

FuriHalNfcError furi_hal_nfc_pn532_rx(uint8_t* rx_data, size_t rx_data_size, size_t* rx_bits) {
    if(!rx_data || !rx_bits) return FuriHalNfcErrorDataFormat;
    const size_t rx_bytes = (furi_hal_nfc_pn532.rx_bits + 7U) / 8U;
    if(rx_bytes > rx_data_size) return FuriHalNfcErrorBufferOverflow;

    memcpy(rx_data, furi_hal_nfc_pn532.rx_buffer, rx_bytes);
    *rx_bits = furi_hal_nfc_pn532.rx_bits;
    return FuriHalNfcErrorNone;
}
