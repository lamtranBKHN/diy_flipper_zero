#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum NDEF payload size for emulation.
 * 126 bytes keeps total RAM footprint under 200 bytes per instance. */
#define NFC_T4T_NDEF_MAX_LEN 126

typedef struct NfcT4tEmulation NfcT4tEmulation;

/** Allocate a Type 4 Tag emulation context. */
NfcT4tEmulation* nfc_t4t_emulation_alloc(void);

/** Free a Type 4 Tag emulation context. */
void nfc_t4t_emulation_free(NfcT4tEmulation* ctx);

/** Reset state machine (call before each emulation session). */
void nfc_t4t_emulation_reset(NfcT4tEmulation* ctx);

/** Set the NDEF message to serve.
 *  @param ndef_data  Raw NDEF message bytes (not TLV-wrapped).
 *  @param ndef_len   Length in bytes. Must be <= NFC_T4T_NDEF_MAX_LEN.
 *  @return true on success. */
bool nfc_t4t_emulation_set_ndef(NfcT4tEmulation* ctx, const uint8_t* ndef_data, size_t ndef_len);

/** Process one APDU command received from the reader.
 *  @param cmd      Raw APDU bytes (no I-block header, no CRC).
 *  @param cmd_len  Length of cmd.
 *  @param resp     Output buffer for response (min 260 bytes).
 *  @param resp_len Set to number of bytes written to resp.
 *  @return true if a response was generated. */
bool nfc_t4t_emulation_process_apdu(
    NfcT4tEmulation* ctx,
    const uint8_t* cmd,
    size_t cmd_len,
    uint8_t* resp,
    size_t* resp_len);

#ifdef __cplusplus
}
#endif
