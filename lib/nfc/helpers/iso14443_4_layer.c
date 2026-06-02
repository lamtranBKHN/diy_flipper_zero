#include "iso14443_4_layer.h"

#include <furi.h>

// EMV Specific masks
#define ISO14443_4_BLOCK_PCB_I_        (0U << 6)
#define ISO14443_4_BLOCK_PCB_R_        (2U << 6)
#define ISO14443_4_BLOCK_PCB_TYPE_MASK (3U << 6)
#define ISO14443_4_BLOCK_PCB_S_WTX     (3U << 4)
#define ISO14443_4_BLOCK_PCB_S         (3U << 6)
//

#define ISO14443_4_BLOCK_PCB      (1U << 1)
#define ISO14443_4_BLOCK_PCB_MASK (0x03)

#define ISO14443_4_BLOCK_PCB_I              (0U)
#define ISO14443_4_BLOCK_PCB_I_MASK         (1U << 1)
#define ISO14443_4_BLOCK_PCB_I_ZERO_MASK    (7U << 5)
#define ISO14443_4_BLOCK_PCB_I_NAD_OFFSET   (2)
#define ISO14443_4_BLOCK_PCB_I_CID_OFFSET   (3)
#define ISO14443_4_BLOCK_PCB_I_CHAIN_OFFSET (4)
#define ISO14443_4_BLOCK_PCB_I_NAD_MASK     (1U << ISO14443_4_BLOCK_PCB_I_NAD_OFFSET)
#define ISO14443_4_BLOCK_PCB_I_CID_MASK     (1U << ISO14443_4_BLOCK_PCB_I_CID_OFFSET)
#define ISO14443_4_BLOCK_PCB_I_CHAIN_MASK   (1U << ISO14443_4_BLOCK_PCB_I_CHAIN_OFFSET)

#define ISO14443_4_BLOCK_PCB_R_MASK        (5U << 5)
#define ISO14443_4_BLOCK_PCB_R_NACK_OFFSET (4)
#define ISO14443_4_BLOCK_PCB_R_CID_OFFSET  (3)
#define ISO14443_4_BLOCK_PCB_R_CID_MASK    (1U << ISO14443_4_BLOCK_PCB_R_CID_OFFSET)
#define ISO14443_4_BLOCK_PCB_R_NACK_MASK   (1U << ISO14443_4_BLOCK_PCB_R_NACK_OFFSET)

#define ISO14443_4_BLOCK_PCB_S_MASK                (3U << 6)
#define ISO14443_4_BLOCK_PCB_S_CID_OFFSET          (3)
#define ISO14443_4_BLOCK_PCB_S_WTX_DESELECT_OFFSET (4)
#define ISO14443_4_BLOCK_PCB_S_CID_MASK            (1U << ISO14443_4_BLOCK_PCB_R_CID_OFFSET)
#define ISO14443_4_BLOCK_PCB_S_WTX_DESELECT_MASK   (3U << ISO14443_4_BLOCK_PCB_S_WTX_DESELECT_OFFSET)

#define ISO14443_4_BLOCK_CID_MASK (0x0F)

#define ISO14443_4_BLOCK_PCB_BITS_ACTIVE(pcb, mask) (((pcb) & (mask)) == (mask))

#define ISO14443_4_BLOCK_PCB_IS_I_BLOCK(pcb)                               \
    (ISO14443_4_BLOCK_PCB_BITS_ACTIVE(pcb, ISO14443_4_BLOCK_PCB_I_MASK) && \
     (((pcb) & ISO14443_4_BLOCK_PCB_I_ZERO_MASK) == 0))

#define ISO14443_4_BLOCK_PCB_IS_R_BLOCK(pcb) \
    ISO14443_4_BLOCK_PCB_BITS_ACTIVE(pcb, ISO14443_4_BLOCK_PCB_R_MASK)

#define ISO14443_4_BLOCK_PCB_IS_S_BLOCK(pcb) \
    ISO14443_4_BLOCK_PCB_BITS_ACTIVE(pcb, ISO14443_4_BLOCK_PCB_S_MASK)

#define ISO14443_4_BLOCK_PCB_IS_CHAIN_ACTIVE(pcb) \
    ISO14443_4_BLOCK_PCB_BITS_ACTIVE(pcb, ISO14443_4_BLOCK_PCB_I_CHAIN_MASK)

#define ISO14443_4_BLOCK_PCB_R_NACK_ACTIVE(pcb) \
    ISO14443_4_BLOCK_PCB_BITS_ACTIVE(pcb, ISO14443_4_BLOCK_PCB_R_NACK_MASK)

#define ISO14443_4_LAYER_NAD_NOT_SUPPORTED ((uint8_t) - 1)
#define ISO14443_4_LAYER_NAD_NOT_SET       ((uint8_t) - 2)

/* Maximum chained payload accumulation buffer.
 * Uses ISO14443_4_MAX_APDU_SIZE from iso14443_4_layer.h.
 * Covers large ISO14443-4 chained APDUs (EMV, DESFire, large NDEF).
 * Fits comfortably in STM32WB55's 256 KB RAM. */
#define ISO14443_4_CHAIN_BUF_SIZE ISO14443_4_MAX_APDU_SIZE

struct Iso14443_4Layer {
    uint8_t pcb;
    uint8_t pcb_prev;

    // Listener specific
    uint8_t cid;
    uint8_t nad;
    bool listener_chaining;

    // Receiver chaining: accumulates fragmented I-block responses from card
    uint8_t chain_buf[ISO14443_4_CHAIN_BUF_SIZE];
    size_t chain_len;
    bool chain_active;

    // Listener chaining: accumulates fragmented I-block commands from reader
    BitBuffer* chaining_buffer;
    BitBuffer* last_tx_data;
};

static inline void iso14443_4_layer_update_pcb(Iso14443_4Layer* instance, bool toggle_num) {
    instance->pcb_prev = instance->pcb;
    if(toggle_num) {
        instance->pcb ^= (uint8_t)0x01;
    }
}

Iso14443_4Layer* iso14443_4_layer_alloc(void) {
    Iso14443_4Layer* instance = malloc(sizeof(Iso14443_4Layer));
    furi_check(instance);
    instance->chaining_buffer = NULL;
    instance->last_tx_data = NULL;

    iso14443_4_layer_reset(instance);
    return instance;
}

void iso14443_4_layer_free(Iso14443_4Layer* instance) {
    furi_assert(instance);
    if(instance->chaining_buffer) {
        bit_buffer_free(instance->chaining_buffer);
    }
    if(instance->last_tx_data) {
        bit_buffer_free(instance->last_tx_data);
    }
    free(instance);
}

void iso14443_4_layer_reset(Iso14443_4Layer* instance) {
    furi_assert(instance);
    instance->pcb_prev = 0;
    instance->pcb = ISO14443_4_BLOCK_PCB_I | ISO14443_4_BLOCK_PCB;

    instance->cid = ISO14443_4_LAYER_CID_NOT_SUPPORTED;
    instance->nad = ISO14443_4_LAYER_NAD_NOT_SUPPORTED;
    instance->listener_chaining = false;

    instance->chain_len = 0;
    instance->chain_active = false;

    if(instance->chaining_buffer) {
        bit_buffer_free(instance->chaining_buffer);
        instance->chaining_buffer = NULL;
    }
    if(instance->last_tx_data) {
        bit_buffer_free(instance->last_tx_data);
        instance->last_tx_data = NULL;
    }
}

void iso14443_4_layer_set_i_block(Iso14443_4Layer* instance, bool chaining, bool CID_present) {
    uint8_t block_pcb = instance->pcb & ISO14443_4_BLOCK_PCB_MASK;
    instance->pcb = ISO14443_4_BLOCK_PCB_I | (chaining << ISO14443_4_BLOCK_PCB_I_CHAIN_OFFSET) |
                    (CID_present << ISO14443_4_BLOCK_PCB_I_CID_OFFSET) | block_pcb;
}

void iso14443_4_layer_set_r_block(Iso14443_4Layer* instance, bool acknowledged, bool CID_present) {
    furi_assert(instance);
    uint8_t block_pcb = instance->pcb & ISO14443_4_BLOCK_PCB_MASK;
    instance->pcb = ISO14443_4_BLOCK_PCB_R_MASK |
                    (!acknowledged << ISO14443_4_BLOCK_PCB_R_NACK_OFFSET) |
                    (CID_present << ISO14443_4_BLOCK_PCB_R_CID_OFFSET) | block_pcb;
}

void iso14443_4_layer_set_s_block(Iso14443_4Layer* instance, bool deselect, bool CID_present) {
    furi_assert(instance);
    uint8_t des_wtx = !deselect ? (ISO14443_4_BLOCK_PCB_S_WTX_DESELECT_MASK) : 0;
    instance->pcb = ISO14443_4_BLOCK_PCB_S_MASK | des_wtx |
                    (CID_present << ISO14443_4_BLOCK_PCB_S_CID_OFFSET) | ISO14443_4_BLOCK_PCB;
}

void iso14443_4_layer_set_listener_chaining(Iso14443_4Layer* instance, bool chaining) {
    furi_assert(instance);
    instance->listener_chaining = chaining;
}

void iso14443_4_layer_encode_command(
    Iso14443_4Layer* instance,
    const BitBuffer* input_data,
    BitBuffer* block_data) {
    furi_assert(instance);

    bit_buffer_append_byte(block_data, instance->pcb);
    bit_buffer_append(block_data, input_data);

    iso14443_4_layer_update_pcb(instance, true);
}

static inline uint8_t iso14443_4_layer_get_response_pcb(const BitBuffer* block_data) {
    const uint8_t* data = bit_buffer_get_data(block_data);
    return data[0];
}

bool iso14443_4_layer_decode_response(
    Iso14443_4Layer* instance,
    BitBuffer* output_data,
    const BitBuffer* block_data) {
    furi_assert(instance);

    bool ret = false;

    do {
        if(ISO14443_4_BLOCK_PCB_IS_R_BLOCK(instance->pcb_prev)) {
            const uint8_t response_pcb = iso14443_4_layer_get_response_pcb(block_data);
            ret = (ISO14443_4_BLOCK_PCB_IS_R_BLOCK(response_pcb)) &&
                  (!ISO14443_4_BLOCK_PCB_R_NACK_ACTIVE(response_pcb));
            instance->pcb &= ISO14443_4_BLOCK_PCB_MASK;
            iso14443_4_layer_update_pcb(instance, true);
        } else if(ISO14443_4_BLOCK_PCB_IS_CHAIN_ACTIVE(instance->pcb_prev)) {
            const uint8_t response_pcb = iso14443_4_layer_get_response_pcb(block_data);
            ret = (ISO14443_4_BLOCK_PCB_IS_R_BLOCK(response_pcb)) &&
                  (!ISO14443_4_BLOCK_PCB_R_NACK_ACTIVE(response_pcb));
            instance->pcb &= ~(ISO14443_4_BLOCK_PCB_I_CHAIN_MASK);
        } else if(ISO14443_4_BLOCK_PCB_IS_S_BLOCK(instance->pcb_prev)) {
            ret = bit_buffer_starts_with_byte(block_data, instance->pcb_prev);
            if(bit_buffer_get_size_bytes(block_data) > 1)
                bit_buffer_copy_right(output_data, block_data, 1);
        } else {
            const uint8_t response_pcb = iso14443_4_layer_get_response_pcb(block_data);

            /* When already chaining, accept any valid I-block (PCB bit toggles between fragments).
             * For initial response, mask out CHAIN bit when comparing against pcb_prev. */
            bool pcb_match;
            if(instance->chain_active) {
                pcb_match = ISO14443_4_BLOCK_PCB_IS_I_BLOCK(response_pcb);
            } else {
                pcb_match = (response_pcb & ~ISO14443_4_BLOCK_PCB_I_CHAIN_MASK) ==
                            (instance->pcb_prev & ~ISO14443_4_BLOCK_PCB_I_CHAIN_MASK);
            }
            if(!pcb_match) break;

            /* Check if card is chaining (CHAIN bit set = more I-blocks to come) */
            if(ISO14443_4_BLOCK_PCB_IS_CHAIN_ACTIVE(response_pcb)) {
                /* Accumulate fragment into chain_buf */
                const size_t frag_bytes = bit_buffer_get_size_bytes(block_data);
                const size_t frag_len = (frag_bytes > 1) ? (frag_bytes - 1) : 0;
                if(frag_len > 0) {
                    if((instance->chain_len + frag_len) <= ISO14443_4_CHAIN_BUF_SIZE) {
                        const uint8_t* frag_data = bit_buffer_get_data(block_data);
                        memcpy(
                            &instance->chain_buf[instance->chain_len],
                            &frag_data[1], /* skip PCB byte */
                            frag_len);
                        instance->chain_len += frag_len;
                    } else {
                        FURI_LOG_E(
                            "Iso14443_4",
                            "Chaining buffer overflow (>%d)",
                            ISO14443_4_CHAIN_BUF_SIZE);
                        break; /* Fail cleanly instead of silently truncating */
                    }
                }
                instance->chain_active = true;
                /* Caller must send R(ACK) and call decode_response again */
                ret = false;
            } else {
                /* Final block (or non-chained response) */
                if(instance->chain_active && instance->chain_len > 0) {
                    /* Prepend accumulated chain data before this final fragment */
                    bit_buffer_reset(output_data);
                    bit_buffer_append_bytes(output_data, instance->chain_buf, instance->chain_len);
                    /* Append final fragment (skip PCB byte) */
                    const size_t final_bytes = bit_buffer_get_size_bytes(block_data);
                    if(final_bytes > 1) {
                        const uint8_t* final_data = bit_buffer_get_data(block_data);
                        bit_buffer_append_bytes(output_data, &final_data[1], final_bytes - 1);
                    }
                    instance->chain_len = 0;
                    instance->chain_active = false;
                } else {
                    bit_buffer_copy_right(output_data, block_data, 1);
                }
                ret = true;
            }
        }
    } while(false);

    return ret;
}

Iso14443_4aError iso14443_4_layer_decode_response_pwt_ext(
    Iso14443_4Layer* instance,
    BitBuffer* output_data,
    const BitBuffer* block_data) {
    furi_assert(instance);

    Iso14443_4aError ret = Iso14443_4aErrorProtocol;
    bit_buffer_reset(output_data);

    do {
        const uint8_t pcb_field = bit_buffer_get_byte(block_data, 0);
        const uint8_t block_type = pcb_field & ISO14443_4_BLOCK_PCB_TYPE_MASK;
        switch(block_type) {
        case ISO14443_4_BLOCK_PCB_I_:
            if(pcb_field == instance->pcb_prev) {
                bit_buffer_copy_right(output_data, block_data, 1);
                ret = Iso14443_4aErrorNone;
            } else {
                // send original request again
                ret = Iso14443_4aErrorSendExtra;
            }
            break;
        case ISO14443_4_BLOCK_PCB_R_:
            // R-block during PWT extension: ACK = continue, NACK = retransmit
            if(pcb_field & ISO14443_4_BLOCK_PCB_R_NACK_MASK) {
                ret = Iso14443_4aErrorSendExtra;
            } else {
                ret = Iso14443_4aErrorNone;
            }
            break;
        case ISO14443_4_BLOCK_PCB_S:
            if((pcb_field & ISO14443_4_BLOCK_PCB_S_WTX) == ISO14443_4_BLOCK_PCB_S_WTX) {
                const uint8_t inf_field = bit_buffer_get_byte(block_data, 1);
                //const uint8_t power_level = inf_field >> 6;
                const uint8_t wtxm = inf_field & 0b111111;
                //uint32_t fwt_temp = MIN((fwt * wtxm), fwt_max);

                bit_buffer_append_byte(
                    output_data,
                    ISO14443_4_BLOCK_PCB_S | ISO14443_4_BLOCK_PCB_S_WTX | ISO14443_4_BLOCK_PCB);
                bit_buffer_append_byte(output_data, wtxm);
                ret = Iso14443_4aErrorSendExtra;
            }
            break;
        }
    } while(false);

    if(ret != Iso14443_4aErrorNone) {
        FURI_LOG_RAW_T("RAW RX:");
        for(size_t x = 0; x < bit_buffer_get_size_bytes(block_data); x++) {
            FURI_LOG_RAW_T("%02X ", bit_buffer_get_byte(block_data, x));
        }
        FURI_LOG_RAW_T("\r\n");
    }

    return ret;
}

void iso14443_4_layer_set_cid(Iso14443_4Layer* instance, uint8_t cid) {
    instance->cid = cid;
}

void iso14443_4_layer_set_nad_supported(Iso14443_4Layer* instance, bool nad) {
    instance->nad = nad ? 0 : ISO14443_4_LAYER_NAD_NOT_SUPPORTED;
}

Iso14443_4LayerResult iso14443_4_layer_decode_command(
    Iso14443_4Layer* instance,
    const BitBuffer* input_data,
    BitBuffer* block_data) {
    furi_assert(instance);

    uint8_t prologue_len = 0;
    instance->pcb = bit_buffer_get_byte(input_data, prologue_len++);

    if(ISO14443_4_BLOCK_PCB_IS_I_BLOCK(instance->pcb)) {
        if(instance->pcb & ISO14443_4_BLOCK_PCB_I_CID_MASK) {
            const uint8_t cid = bit_buffer_get_byte(input_data, prologue_len++) &
                                ISO14443_4_BLOCK_CID_MASK;
            if(instance->cid == ISO14443_4_LAYER_CID_NOT_SUPPORTED || cid != instance->cid) {
                return Iso14443_4LayerResultSkip;
            }
        } else if(instance->cid != ISO14443_4_LAYER_CID_NOT_SUPPORTED && instance->cid != 0) {
            return Iso14443_4LayerResultSkip;
        }
        if(instance->pcb & ISO14443_4_BLOCK_PCB_I_NAD_MASK) {
            if(instance->nad == ISO14443_4_LAYER_NAD_NOT_SUPPORTED) {
                return Iso14443_4LayerResultSkip;
            }
            instance->nad = bit_buffer_get_byte(input_data, prologue_len++);
        }

        if(instance->pcb & ISO14443_4_BLOCK_PCB_I_CHAIN_MASK) {
            if(!instance->chaining_buffer) {
                instance->chaining_buffer = bit_buffer_alloc(ISO14443_4_CHAIN_BUF_SIZE * 4);
            }
            bit_buffer_append_bytes(
                instance->chaining_buffer,
                bit_buffer_get_data(input_data) + prologue_len,
                bit_buffer_get_size_bytes(input_data) - prologue_len);

            bit_buffer_reset(block_data);
            uint8_t r_pcb = ISO14443_4_BLOCK_PCB_R_MASK | ISO14443_4_BLOCK_PCB;
            if(instance->cid != ISO14443_4_LAYER_CID_NOT_SUPPORTED) {
                r_pcb |= ISO14443_4_BLOCK_PCB_R_CID_MASK;
            }
            r_pcb |= (instance->pcb & 0x01);
            bit_buffer_append_byte(block_data, r_pcb);
            if(instance->cid != ISO14443_4_LAYER_CID_NOT_SUPPORTED) {
                bit_buffer_append_byte(block_data, instance->cid);
            }

            iso14443_4_layer_update_pcb(instance, false);
            return Iso14443_4LayerResultSend;
        }

        if(instance->chaining_buffer) {
            bit_buffer_copy_right(block_data, instance->chaining_buffer, 0);
            bit_buffer_append_bytes(
                block_data,
                bit_buffer_get_data(input_data) + prologue_len,
                bit_buffer_get_size_bytes(input_data) - prologue_len);
            bit_buffer_free(instance->chaining_buffer);
            instance->chaining_buffer = NULL;
        } else {
            bit_buffer_copy_right(block_data, input_data, prologue_len);
        }
        iso14443_4_layer_update_pcb(instance, false);
        return Iso14443_4LayerResultData;

    } else if(ISO14443_4_BLOCK_PCB_IS_S_BLOCK(instance->pcb)) {
        if(instance->pcb & ISO14443_4_BLOCK_PCB_S_CID_MASK) {
            const uint8_t cid = bit_buffer_get_byte(input_data, prologue_len++) &
                                ISO14443_4_BLOCK_CID_MASK;
            if(instance->cid == ISO14443_4_LAYER_CID_NOT_SUPPORTED || cid != instance->cid) {
                return Iso14443_4LayerResultSkip;
            }
        } else if(instance->cid != ISO14443_4_LAYER_CID_NOT_SUPPORTED && instance->cid != 0) {
            return Iso14443_4LayerResultSkip;
        }
        if((instance->pcb & ISO14443_4_BLOCK_PCB_S_WTX_DESELECT_MASK) == 0) {
            // DESELECT
            bit_buffer_copy(block_data, input_data);
            return Iso14443_4LayerResultSend | Iso14443_4LayerResultHalt;
        } else {
            // WTX ACK or wrong value
            return Iso14443_4LayerResultSkip;
        }

    } else if(ISO14443_4_BLOCK_PCB_IS_R_BLOCK(instance->pcb)) {
        bool is_nack = (instance->pcb & ISO14443_4_BLOCK_PCB_R_NACK_MASK) != 0;
        iso14443_4_layer_update_pcb(instance, true);
        if(is_nack) {
            if(instance->last_tx_data) {
                bit_buffer_copy(block_data, instance->last_tx_data);
                iso14443_4_layer_update_pcb(instance, false);
                return Iso14443_4LayerResultSend;
            }
            instance->pcb |= ISO14443_4_BLOCK_PCB_R_NACK_MASK;
        } else {
            instance->pcb &= ~ISO14443_4_BLOCK_PCB_R_NACK_MASK;
        }
        bit_buffer_reset(block_data);
        bit_buffer_append_byte(block_data, instance->pcb);
        iso14443_4_layer_update_pcb(instance, false);
        return Iso14443_4LayerResultSend;
    }
    return Iso14443_4LayerResultSkip;
}

bool iso14443_4_layer_encode_response(
    Iso14443_4Layer* instance,
    const BitBuffer* input_data,
    BitBuffer* block_data) {
    furi_assert(instance);

    if(ISO14443_4_BLOCK_PCB_IS_I_BLOCK(instance->pcb_prev)) {
        bit_buffer_append_byte(block_data, 0x00);
        if(instance->pcb_prev & ISO14443_4_BLOCK_PCB_I_CID_MASK) {
            bit_buffer_append_byte(block_data, instance->cid);
        }
        if(instance->pcb_prev & ISO14443_4_BLOCK_PCB_I_NAD_MASK &&
           instance->nad != ISO14443_4_LAYER_NAD_NOT_SET) {
            bit_buffer_append_byte(block_data, instance->nad);
            instance->nad = ISO14443_4_LAYER_NAD_NOT_SET;
        } else {
            instance->pcb &= ~ISO14443_4_BLOCK_PCB_I_NAD_MASK;
        }
        if(!instance->listener_chaining) {
            instance->pcb &= ~ISO14443_4_BLOCK_PCB_I_CHAIN_MASK;
        }
        bit_buffer_set_byte(block_data, 0, instance->pcb);
        bit_buffer_append(block_data, input_data);
        iso14443_4_layer_update_pcb(instance, false);

        if(!instance->last_tx_data) {
            instance->last_tx_data = bit_buffer_alloc(ISO14443_4_CHAIN_BUF_SIZE * 4);
        }
        bit_buffer_copy(instance->last_tx_data, block_data);

        return true;
    }

    if(ISO14443_4_BLOCK_PCB_IS_R_BLOCK(instance->pcb_prev)) {
        // R-block response: send ACK with current PCB
        bit_buffer_reset(block_data);
        bit_buffer_append_byte(block_data, instance->pcb & ~ISO14443_4_BLOCK_PCB_R_NACK_MASK);
        iso14443_4_layer_update_pcb(instance, false);
        return true;
    }

    return false;
}

void iso14443_4_layer_encode_r_ack(
    Iso14443_4Layer* instance,
    uint8_t rx_pcb,
    BitBuffer* block_data) {
    furi_assert(instance);
    furi_assert(block_data);

    /* Construct R(ACK): R-block type, ACK set, PCB bit from received I-block */
    uint8_t r_pcb = ISO14443_4_BLOCK_PCB_R_MASK | ISO14443_4_BLOCK_PCB | (rx_pcb & 0x01);
    UNUSED(instance); /* instance reserved for future CID support */
    bit_buffer_reset(block_data);
    bit_buffer_append_byte(block_data, r_pcb);
}

void iso14443_4_layer_encode_r_nack(
    Iso14443_4Layer* instance,
    uint8_t rx_pcb,
    BitBuffer* block_data) {
    furi_assert(instance);
    furi_assert(block_data);

    /* Construct R(NAK): R-block type, NACK set, PCB bit from received I-block
     * Per ISO/IEC 14443-4:2016 §7.1.6.2, R(NAK) requests card to retransmit last block. */
    uint8_t r_pcb = ISO14443_4_BLOCK_PCB_R_MASK | ISO14443_4_BLOCK_PCB |
                    ISO14443_4_BLOCK_PCB_R_NACK_MASK | (rx_pcb & 0x01);
    UNUSED(instance); /* instance reserved for future CID support */
    bit_buffer_reset(block_data);
    bit_buffer_append_byte(block_data, r_pcb);
}

bool iso14443_4_layer_is_chaining(const Iso14443_4Layer* instance) {
    furi_assert(instance);
    return instance->chain_active;
}
