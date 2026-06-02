#include "iso14443_4a_poller_i.h"

#include <furi.h>

#include "iso14443_4a_i.h"

#define TAG "Iso14443_4aPoller"

#define ISO14443_4A_FSDI_256                 (0x8U)
#define ISO14443_4A_SEND_BLOCK_MAX_ATTEMPTS  (20)
#define ISO14443_4A_FWT_MAX                  (4096UL << 14)
#define ISO14443_4A_WTXM_MASK                (0x3FU)
#define ISO14443_4A_WTXM_MAX                 (0x3BU)
#define ISO14443_4A_SWTX                     (0xF2U)
#define ISO14443_4A_POLLER_INTERNAL_BUF_SIZE (256U)

Iso14443_4aError iso14443_4a_poller_halt(Iso14443_4aPoller* instance) {
    furi_check(instance);

    iso14443_3a_poller_halt(instance->iso14443_3a_poller);
    instance->poller_state = Iso14443_4aPollerStateIdle;

    return Iso14443_4aErrorNone;
}

Iso14443_4aError
    iso14443_4a_poller_read_ats(Iso14443_4aPoller* instance, Iso14443_4aAtsData* data) {
    furi_check(instance);
    furi_check(data);

    bit_buffer_reset(instance->tx_buffer);
    bit_buffer_append_byte(instance->tx_buffer, ISO14443_4A_CMD_READ_ATS);
    bit_buffer_append_byte(instance->tx_buffer, ISO14443_4A_FSDI_256 << 4);

    Iso14443_4aError error = Iso14443_4aErrorNone;

    do {
        const Iso14443_3aError iso14443_3a_error = iso14443_3a_poller_send_standard_frame(
            instance->iso14443_3a_poller,
            instance->tx_buffer,
            instance->rx_buffer,
            ISO14443_4A_POLLER_ATS_FWT_FC);

        if(iso14443_3a_error != Iso14443_3aErrorNone) {
            FURI_LOG_E(TAG, "ATS request failed");
            error = iso14443_4a_process_error(iso14443_3a_error);
            break;

        } else if(!iso14443_4a_ats_parse(data, instance->rx_buffer)) {
            FURI_LOG_E(TAG, "Failed to parse ATS response");
            error = Iso14443_4aErrorProtocol;
            break;
        }

    } while(false);

    return error;
}

Iso14443_4aError iso14443_4a_poller_send_block(
    Iso14443_4aPoller* instance,
    const BitBuffer* tx_buffer,
    BitBuffer* rx_buffer) {
    furi_check(instance);
    furi_check(tx_buffer);
    furi_check(rx_buffer);

    bit_buffer_reset(instance->tx_buffer);
    iso14443_4_layer_encode_command(instance->iso14443_4_layer, tx_buffer, instance->tx_buffer);

    Iso14443_4aError error = Iso14443_4aErrorNone;

    do {
        Iso14443_3aError iso14443_3a_error = iso14443_3a_poller_send_standard_frame(
            instance->iso14443_3a_poller,
            instance->tx_buffer,
            instance->rx_buffer,
            iso14443_4a_get_fwt_fc_max(instance->data));

        if(iso14443_3a_error != Iso14443_3aErrorNone) {
            error = iso14443_4a_process_error(iso14443_3a_error);
            break;
        }

        if(bit_buffer_starts_with_byte(instance->rx_buffer, ISO14443_4A_SWTX)) {
            uint8_t wtx_attempts = 0;
            do {
                wtx_attempts++;
                if(wtx_attempts > ISO14443_4A_SEND_BLOCK_MAX_ATTEMPTS) {
                    error = Iso14443_4aErrorProtocol;
                    break;
                }
                uint8_t wtxm = bit_buffer_get_byte(instance->rx_buffer, 1) & ISO14443_4A_WTXM_MASK;
                if(wtxm > ISO14443_4A_WTXM_MAX) {
                    return Iso14443_4aErrorProtocol;
                }
                /* Per ISO/IEC 14443-4:2016 §7.1.5: WTXM=0 is reserved.
                 * Some PICCs send it as "request minimum extension"; clamp
                 * to 1 so the echoed S(WTX) response gives the card a real
                 * timeout extension instead of FWT (which would hard-timeout
                 * the card mid-operation). */
                if(wtxm == 0) {
                    wtxm = 1;
                }

                bit_buffer_reset(instance->tx_buffer);
                bit_buffer_copy_left(instance->tx_buffer, instance->rx_buffer, 1);
                bit_buffer_append_byte(instance->tx_buffer, wtxm);

                iso14443_3a_error = iso14443_3a_poller_send_standard_frame(
                    instance->iso14443_3a_poller,
                    instance->tx_buffer,
                    instance->rx_buffer,
                    MAX(iso14443_4a_get_fwt_fc_max(instance->data) * wtxm, ISO14443_4A_FWT_MAX));

                if(iso14443_3a_error != Iso14443_3aErrorNone) {
                    error = iso14443_4a_process_error(iso14443_3a_error);
                    return error;
                }

            } while(bit_buffer_starts_with_byte(instance->rx_buffer, ISO14443_4A_SWTX));
        }

        if(!iso14443_4_layer_decode_response(
               instance->iso14443_4_layer, rx_buffer, instance->rx_buffer)) {
            if(iso14443_4_layer_is_chaining(instance->iso14443_4_layer)) {
                /* Receive-side chaining: card sending fragmented response.
                 * Send R(ACK) for each fragment until chaining completes. */
                while(iso14443_4_layer_is_chaining(instance->iso14443_4_layer)) {
                    /* Construct R(ACK) with PCB bit matching received I-block */
                    uint8_t rx_pcb = bit_buffer_get_byte(instance->rx_buffer, 0);
                    iso14443_4_layer_encode_r_ack(
                        instance->iso14443_4_layer, rx_pcb, instance->tx_buffer);

                    iso14443_3a_error = iso14443_3a_poller_send_standard_frame(
                        instance->iso14443_3a_poller,
                        instance->tx_buffer,
                        instance->rx_buffer,
                        iso14443_4a_get_fwt_fc_max(instance->data));

                    if(iso14443_3a_error != Iso14443_3aErrorNone) {
                        error = iso14443_4a_process_error(iso14443_3a_error);
                        break;
                    }

                    /* Decode next fragment - accumulates into chain_buf */
                    if(!iso14443_4_layer_decode_response(
                           instance->iso14443_4_layer, rx_buffer, instance->rx_buffer)) {
                        if(!iso14443_4_layer_is_chaining(instance->iso14443_4_layer)) {
                            error = Iso14443_4aErrorProtocol;
                            break;
                        }
                        /* Still chaining, continue loop to send next R(ACK) */
                    }
                }
                if(error != Iso14443_4aErrorNone) break;
            } else {
                error = Iso14443_4aErrorProtocol;
                break;
            }
        }
    } while(false);

    return error;
}

Iso14443_4aError iso14443_4a_poller_send_chain_block(
    Iso14443_4aPoller* instance,
    const BitBuffer* tx_buffer,
    BitBuffer* rx_buffer) {
    iso14443_4_layer_set_i_block(instance->iso14443_4_layer, true, false);
    Iso14443_4aError error = iso14443_4a_poller_send_block(instance, tx_buffer, rx_buffer);
    return error;
}

Iso14443_4aError iso14443_4a_poller_send_blocks_chained(
    Iso14443_4aPoller* instance,
    const BitBuffer* tx_buffer,
    BitBuffer* rx_buffer) {
    furi_check(instance);
    furi_check(tx_buffer);
    furi_check(rx_buffer);

    const uint8_t* data = bit_buffer_get_data(tx_buffer);
    const size_t total_len = bit_buffer_get_size_bytes(tx_buffer);

    if(total_len == 0) {
        return Iso14443_4aErrorNone;
    }

    /* FSC (Frame Size Card) includes the 1-byte PCB.
     * Maximum INF per I-block = FSC - 1. */
    const uint16_t fsc = iso14443_4a_get_frame_size_max(instance->data);
    const uint16_t max_inf = (fsc > 1) ? (fsc - 1) : 16;

    /* Payload fits in a single I-block — no chaining needed. */
    if(total_len <= max_inf) {
        return iso14443_4a_poller_send_block(instance, tx_buffer, rx_buffer);
    }

    /* Payload exceeds FSC — split across chained I-blocks.
     * send_block() internally resets instance->tx_buffer before reading
     * from the tx_buffer parameter, so we need a separate fragment buffer. */
    Iso14443_4aError error = Iso14443_4aErrorNone;
    BitBuffer* frag_buffer = bit_buffer_alloc(ISO14443_4A_POLLER_INTERNAL_BUF_SIZE);
    size_t offset = 0;

    while(offset < total_len) {
        const size_t chunk_size = MIN(total_len - offset, max_inf);
        const bool is_last = (offset + chunk_size >= total_len);

        bit_buffer_reset(frag_buffer);
        bit_buffer_append_bytes(frag_buffer, &data[offset], chunk_size);

        if(!is_last) {
            /* Send chained I-block with chaining bit set.
             * Card responds with R(ACK) — handled by decode_response. */
            error =
                iso14443_4a_poller_send_chain_block(instance, frag_buffer, instance->rx_buffer);
            if(error != Iso14443_4aErrorNone) break;
        } else {
            /* Send final (unchained) I-block — card responds with actual data. */
            error = iso14443_4a_poller_send_block(instance, frag_buffer, rx_buffer);
            if(error != Iso14443_4aErrorNone) break;
        }

        offset += chunk_size;
    }

    bit_buffer_free(frag_buffer);
    return error;
}

Iso14443_4aError iso14443_4a_poller_send_receive_ready_block(
    Iso14443_4aPoller* instance,
    bool acknowledged,
    const BitBuffer* tx_buffer,
    BitBuffer* rx_buffer) {
    bool CID_present = bit_buffer_get_size_bytes(tx_buffer) != 0;
    iso14443_4_layer_set_r_block(instance->iso14443_4_layer, acknowledged, CID_present);
    Iso14443_4aError error = iso14443_4a_poller_send_block(instance, tx_buffer, rx_buffer);
    return error;
}

Iso14443_4aError iso14443_4a_poller_send_supervisory_block(
    Iso14443_4aPoller* instance,
    bool deselect,
    const BitBuffer* tx_buffer,
    BitBuffer* rx_buffer) {
    bool CID_present = bit_buffer_get_size_bytes(tx_buffer) != 0;
    iso14443_4_layer_set_s_block(instance->iso14443_4_layer, deselect, CID_present);
    Iso14443_4aError error = iso14443_4a_poller_send_block(instance, tx_buffer, rx_buffer);
    return error;
}

Iso14443_4aError iso14443_4a_poller_send_block_pwt_ext(
    Iso14443_4aPoller* instance,
    const BitBuffer* tx_buffer,
    BitBuffer* rx_buffer) {
    furi_assert(instance);

    uint8_t attempts_left = ISO14443_4A_SEND_BLOCK_MAX_ATTEMPTS;
    bit_buffer_reset(instance->tx_buffer);
    iso14443_4_layer_encode_command(instance->iso14443_4_layer, tx_buffer, instance->tx_buffer);

    Iso14443_4aError error = Iso14443_4aErrorNone;

    do {
        bit_buffer_reset(instance->rx_buffer);
        Iso14443_3aError iso14443_3a_error = iso14443_3a_poller_send_standard_frame(
            instance->iso14443_3a_poller,
            instance->tx_buffer,
            instance->rx_buffer,
            iso14443_4a_get_fwt_fc_max(instance->data));

        if(iso14443_3a_error != Iso14443_3aErrorNone) {
            FURI_LOG_T(
                TAG, "Attempt: %u", ISO14443_4A_SEND_BLOCK_MAX_ATTEMPTS + 1 - attempts_left);
            FURI_LOG_RAW_T("RAW RX(%d):", bit_buffer_get_size_bytes(instance->rx_buffer));
            for(size_t x = 0; x < bit_buffer_get_size_bytes(instance->rx_buffer); x++) {
                FURI_LOG_RAW_T("%02X ", bit_buffer_get_byte(instance->rx_buffer, x));
            }
            FURI_LOG_RAW_T("\r\n");

            error = iso14443_4a_process_error(iso14443_3a_error);
            break;

        } else {
            error = iso14443_4_layer_decode_response_pwt_ext(
                instance->iso14443_4_layer, rx_buffer, instance->rx_buffer);
            if(error == Iso14443_4aErrorSendExtra) {
                if(--attempts_left == 0) break;
                // Send response for Control message
                if(bit_buffer_get_size_bytes(rx_buffer))
                    bit_buffer_copy(instance->tx_buffer, rx_buffer);
                continue;
            }
            break;
        }
    } while(true);

    return error;
}
