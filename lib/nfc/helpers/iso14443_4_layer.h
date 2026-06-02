#pragma once

#include "protocols/iso14443_4a/iso14443_4a.h"
#include <toolbox/bit_buffer.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum ISO-DEP APDU payload size (reassembled from chained I-blocks).
 * 1024 bytes covers EMV, DESFire, and large NDEF operations.
 * Fits comfortably in STM32WB55's 256 KB RAM. */
#define ISO14443_4_MAX_APDU_SIZE 1024

typedef struct Iso14443_4Layer Iso14443_4Layer;

Iso14443_4Layer* iso14443_4_layer_alloc(void);

void iso14443_4_layer_free(Iso14443_4Layer* instance);

void iso14443_4_layer_reset(Iso14443_4Layer* instance);

void iso14443_4_layer_set_i_block(Iso14443_4Layer* instance, bool chaining, bool CID_present);
void iso14443_4_layer_set_r_block(Iso14443_4Layer* instance, bool acknowledged, bool CID_present);
void iso14443_4_layer_set_s_block(Iso14443_4Layer* instance, bool deselect, bool CID_present);

void iso14443_4_layer_set_listener_chaining(Iso14443_4Layer* instance, bool chaining);

// Poller mode

void iso14443_4_layer_encode_command(
    Iso14443_4Layer* instance,
    const BitBuffer* input_data,
    BitBuffer* block_data);

bool iso14443_4_layer_decode_response(
    Iso14443_4Layer* instance,
    BitBuffer* output_data,
    const BitBuffer* block_data);

Iso14443_4aError iso14443_4_layer_decode_response_pwt_ext(
    Iso14443_4Layer* instance,
    BitBuffer* output_data,
    const BitBuffer* block_data);

// Listener mode

typedef enum {
    Iso14443_4LayerResultSkip = (0),
    Iso14443_4LayerResultData = (1 << 1),
    Iso14443_4LayerResultSend = (1 << 2),
    Iso14443_4LayerResultHalt = (1 << 3),
    Iso14443_4LayerResultError = (1 << 4), /**< protocol-level error (e.g. overflow); listener must reset. */
} Iso14443_4LayerResult;

Iso14443_4LayerResult iso14443_4_layer_decode_command(
    Iso14443_4Layer* instance,
    const BitBuffer* input_data,
    BitBuffer* block_data);

bool iso14443_4_layer_encode_response(
    Iso14443_4Layer* instance,
    const BitBuffer* input_data,
    BitBuffer* block_data);

#define ISO14443_4_LAYER_CID_NOT_SUPPORTED ((uint8_t) - 1)
void iso14443_4_layer_set_cid(Iso14443_4Layer* instance, uint8_t cid);

void iso14443_4_layer_set_nad_supported(Iso14443_4Layer* instance, bool nad);

/** Returns true if the layer is currently accumulating a chained response from the card. */
bool iso14443_4_layer_is_chaining(const Iso14443_4Layer* instance);

/** Encode an R(ACK) block acknowledging receipt of a chained I-block fragment.
 *
 * @param      instance    ISO14443-4 layer instance.
 * @param      rx_pcb      PCB byte from the received I-block (used to extract sequence bit).
 * @param[out] block_data  Buffer to write the R(ACK) block into (1 byte).
 */
void iso14443_4_layer_encode_r_ack(
    Iso14443_4Layer* instance,
    uint8_t rx_pcb,
    BitBuffer* block_data);

/** Encode an R(NAK) block requesting retransmission of the last chained I-block fragment.
 *
 * @param      instance    ISO14443-4 layer instance.
 * @param      rx_pcb      PCB byte from the received I-block (used to extract sequence bit).
 * @param[out] block_data  Buffer to write the R(NAK) block into (1 byte).
 */
void iso14443_4_layer_encode_r_nack(
    Iso14443_4Layer* instance,
    uint8_t rx_pcb,
    BitBuffer* block_data);

#ifdef __cplusplus
}
#endif
