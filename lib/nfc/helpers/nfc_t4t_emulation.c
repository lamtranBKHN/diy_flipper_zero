#include "nfc_t4t_emulation.h"
#include <stdlib.h>
#include <string.h>
#include <furi.h>

#define TAG "NfcT4tEmu"

/* APDU instruction bytes */
#define INS_SELECT     0xA4
#define INS_READ_BIN   0xB0
#define INS_UPDATE_BIN 0xD6

/* P1 values for SELECT */
#define P1_SELECT_BY_ID   0x00
#define P1_SELECT_BY_NAME 0x04

/* Status words */
#define SW_OK_HI        0x90
#define SW_OK_LO        0x00
#define SW_NOT_FOUND_HI 0x6A
#define SW_NOT_FOUND_LO 0x82
#define SW_NOT_SUPP_HI  0x6A
#define SW_NOT_SUPP_LO  0x81
#define SW_EOF_HI       0x62
#define SW_EOF_LO       0x82
#define SW_WRONG_LEN_HI 0x67
#define SW_WRONG_LEN_LO 0x00

/* File IDs */
#define FID_CC_HI   0xE1
#define FID_CC_LO   0x03
#define FID_NDEF_HI 0xE1
#define FID_NDEF_LO 0x04

/* NDEF AID (NFC Forum Type 4 Tag v2.0) */
static const uint8_t NDEF_AID[] = {
    0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01};

typedef enum {
    FileNone = 0,
    FileCC,
    FileNDEF,
} SelectedFile;

struct NfcT4tEmulation {
    SelectedFile selected;
    uint8_t ndef_file[NFC_T4T_NDEF_MAX_LEN + 2]; /* 2-byte length prefix */
    size_t ndef_file_len;
};

/* Capability Container — fixed 15-byte structure per NFC Forum T4T spec */
static const uint8_t CC_FILE[15] = {
    0x00, 0x0F,       /* CCLEN = 15 */
    0x20,             /* Mapping version 2.0 */
    0x00, 0x3B,       /* MLe: max R-APDU data = 59 bytes */
    0x00, 0x34,       /* MLc: max C-APDU data = 52 bytes */
    0x04,             /* NDEF File Control TLV tag */
    0x06,             /* TLV length */
    0xE1, 0x04,       /* NDEF File ID */
    0x00, 0x80,       /* Max NDEF file size = 128 bytes */
    0x00,             /* Read access: open */
    0xFF,             /* Write access: proprietary (read-only) */
};

static void sw_append(uint8_t* buf, size_t* len, uint8_t hi, uint8_t lo) {
    buf[(*len)++] = hi;
    buf[(*len)++] = lo;
}

NfcT4tEmulation* nfc_t4t_emulation_alloc(void) {
    NfcT4tEmulation* ctx = malloc(sizeof(NfcT4tEmulation));
    if(ctx) nfc_t4t_emulation_reset(ctx);
    return ctx;
}

void nfc_t4t_emulation_free(NfcT4tEmulation* ctx) {
    if(ctx) free(ctx);
}

void nfc_t4t_emulation_reset(NfcT4tEmulation* ctx) {
    furi_assert(ctx);
    ctx->selected = FileNone;
    ctx->ndef_file_len = 2; /* 2-byte length = 0x0000 (empty) */
    memset(ctx->ndef_file, 0, sizeof(ctx->ndef_file));
}

bool nfc_t4t_emulation_set_ndef(
    NfcT4tEmulation* ctx,
    const uint8_t* ndef_data,
    size_t ndef_len) {
    furi_assert(ctx);
    if(!ndef_data || ndef_len > NFC_T4T_NDEF_MAX_LEN) return false;

    /* NDEF file format: [2-byte big-endian length] [NDEF message bytes] */
    ctx->ndef_file[0] = (uint8_t)(ndef_len >> 8);
    ctx->ndef_file[1] = (uint8_t)(ndef_len & 0xFF);
    memcpy(&ctx->ndef_file[2], ndef_data, ndef_len);
    ctx->ndef_file_len = 2 + ndef_len;
    return true;
}

bool nfc_t4t_emulation_process_apdu(
    NfcT4tEmulation* ctx,
    const uint8_t* cmd,
    size_t cmd_len,
    uint8_t* resp,
    size_t* resp_len) {
    furi_assert(ctx && cmd && resp && resp_len);
    *resp_len = 0;

    if(cmd_len < 4) {
        sw_append(resp, resp_len, SW_WRONG_LEN_HI, SW_WRONG_LEN_LO);
        return true;
    }

    const uint8_t ins = cmd[1];
    const uint8_t p1 = cmd[2];
    const uint8_t p2 = cmd[3];
    const uint8_t lc = (cmd_len >= 5) ? cmd[4] : 0;

    if(ins == INS_SELECT) {
        if(p1 == P1_SELECT_BY_NAME) {
            /* SELECT by AID — accept NDEF AID */
            if(lc == sizeof(NDEF_AID) && cmd_len >= (size_t)(5 + lc) &&
               memcmp(&cmd[5], NDEF_AID, sizeof(NDEF_AID)) == 0) {
                ctx->selected = FileNDEF;
                sw_append(resp, resp_len, SW_OK_HI, SW_OK_LO);
            } else {
                sw_append(resp, resp_len, SW_NOT_SUPP_HI, SW_NOT_SUPP_LO);
            }
        } else if(p1 == P1_SELECT_BY_ID) {
            if(lc == 2 && cmd_len >= 7) {
                uint8_t fid_hi = cmd[5];
                uint8_t fid_lo = cmd[6];
                if(fid_hi == FID_CC_HI && fid_lo == FID_CC_LO) {
                    ctx->selected = FileCC;
                    sw_append(resp, resp_len, SW_OK_HI, SW_OK_LO);
                } else if(fid_hi == FID_NDEF_HI && fid_lo == FID_NDEF_LO) {
                    ctx->selected = FileNDEF;
                    sw_append(resp, resp_len, SW_OK_HI, SW_OK_LO);
                } else {
                    sw_append(resp, resp_len, SW_NOT_FOUND_HI, SW_NOT_FOUND_LO);
                }
            } else {
                /* SELECT without FID — application select, accept */
                sw_append(resp, resp_len, SW_OK_HI, SW_OK_LO);
            }
        } else {
            sw_append(resp, resp_len, SW_NOT_SUPP_HI, SW_NOT_SUPP_LO);
        }
        return true;
    }

    if(ins == INS_READ_BIN) {
        if(ctx->selected == FileNone) {
            sw_append(resp, resp_len, SW_NOT_FOUND_HI, SW_NOT_FOUND_LO);
            return true;
        }
        uint16_t offset = ((uint16_t)p1 << 8) | p2;
        uint8_t le = (cmd_len >= 5) ? cmd[4] : 0;
        if(le == 0) le = 59; /* default: max MLe from CC */

        const uint8_t* src;
        size_t src_len;
        if(ctx->selected == FileCC) {
            src = CC_FILE;
            src_len = sizeof(CC_FILE);
        } else {
            src = ctx->ndef_file;
            src_len = ctx->ndef_file_len;
        }

        if(offset >= src_len) {
            sw_append(resp, resp_len, SW_EOF_HI, SW_EOF_LO);
            return true;
        }
        size_t avail = src_len - offset;
        size_t copy = (avail < le) ? avail : le;
        memcpy(resp, src + offset, copy);
        *resp_len = copy;
        sw_append(resp, resp_len, SW_OK_HI, SW_OK_LO);
        return true;
    }

    if(ins == INS_UPDATE_BIN) {
        /* Read-only emulation — reject writes */
        sw_append(resp, resp_len, SW_NOT_SUPP_HI, SW_NOT_SUPP_LO);
        return true;
    }

    /* Unknown instruction */
    sw_append(resp, resp_len, SW_NOT_SUPP_HI, SW_NOT_SUPP_LO);
    return true;
}
