#include "ndef_write.h"
#include "ndef.h"
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller_sync.h>
#include <nfc/protocols/mf_classic/mf_classic_poller_sync.h>
#include <furi.h>
#include <string.h>

#define TAG "NdefWrite"

static const uint8_t ndef_mad_key[6] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
static const uint8_t ndef_aid[2] = {0x03, 0xE1};
#define NDEF_TLV_MAX_SIZE 256

static const uint8_t ntag213_cc[4] = {0xE1, 0x10, 0x12, 0x00};
static const uint8_t ntag215_cc[4] = {0xE1, 0x10, 0x3E, 0x00};
static const uint8_t ntag216_cc[4] = {0xE1, 0x10, 0x6D, 0x00};

static MfUltralightError ndef_write_ultralight_pages(
    Nfc* nfc,
    const uint8_t* tlv_data,
    size_t tlv_len,
    uint8_t start_page) {
    size_t pages = (tlv_len + 3) / 4;
    MfUltralightPage page;

    for(size_t i = 0; i < pages; i++) {
        size_t offset = i * 4;
        size_t chunk_len = (offset + 4 <= tlv_len) ? 4 : (tlv_len - offset);

        memset(page.data, 0, sizeof(page.data));
        memcpy(page.data, tlv_data + offset, chunk_len);

        MfUltralightError err =
            mf_ultralight_poller_sync_write_page(nfc, start_page + (uint8_t)i, &page);
        if(err != MfUltralightErrorNone) return err;
    }
    return MfUltralightErrorNone;
}

static bool
    ndef_build_tlv(const uint8_t* payload, size_t payload_len, uint8_t* tlv_buf, size_t* tlv_len) {
    if(payload_len + 5 > *tlv_len) return false;
    size_t pos = 0;
    tlv_buf[pos++] = 0x03; // NDEF Message TLV
    if(payload_len < 255) {
        tlv_buf[pos++] = (uint8_t)payload_len;
    } else {
        tlv_buf[pos++] = 0xFF;
        tlv_buf[pos++] = (uint8_t)(payload_len >> 8);
        tlv_buf[pos++] = (uint8_t)(payload_len & 0xFF);
    }
    memcpy(&tlv_buf[pos], payload, payload_len);
    pos += payload_len;
    tlv_buf[pos++] = 0xFE; // Terminator TLV
    *tlv_len = pos;
    return true;
}

static MfUltralightError ndef_build_and_write(
    Nfc* nfc,
    const NdefWriteContext* ctx,
    uint8_t* tlv_buf,
    size_t tlv_buf_size) {
    uint8_t payload[200];
    size_t payload_len = sizeof(payload);
    bool ok = false;

    switch(ctx->type) {
    case NdefWriteTypeUri:
        ok = ndef_build_uri(
            ctx->data.uri.uri,
            ctx->data.uri.uri_len,
            (NdefUriPrefix)ctx->data.uri.prefix,
            payload,
            &payload_len);
        break;
    case NdefWriteTypeText:
        ok = ndef_build_text(
            ctx->data.text.text,
            ctx->data.text.text_len,
            ctx->data.text.lang,
            payload,
            &payload_len);
        break;
    default:
        // Other types not implemented yet
        return MfUltralightErrorProtocol;
    }

    if(!ok) return MfUltralightErrorProtocol;

    size_t tlv_len = tlv_buf_size;
    if(!ndef_build_tlv(payload, payload_len, tlv_buf, &tlv_len)) {
        return MfUltralightErrorProtocol;
    }

    return ndef_write_ultralight_pages(nfc, tlv_buf, tlv_len, 4);
}

MfUltralightError ndef_write_ultralight(Nfc* nfc, const NdefWriteContext* ctx) {
    if(!nfc || !ctx) return MfUltralightErrorProtocol;

    uint8_t tlv_buf[NDEF_TLV_MAX_SIZE];
    MfUltralightVersion version;
    MfUltralightError err = mf_ultralight_poller_sync_read_version(nfc, &version);

    const uint8_t* cc = ntag213_cc;
    if(err == MfUltralightErrorNone) {
        if(version.storage_size == 0x11)
            cc = ntag215_cc;
        else if(version.storage_size == 0x13)
            cc = ntag216_cc;
    }

    MfUltralightPage cc_page;
    memcpy(cc_page.data, cc, 4);
    err = mf_ultralight_poller_sync_write_page(nfc, 3, &cc_page);
    if(err != MfUltralightErrorNone) return err;

    return ndef_build_and_write(nfc, ctx, tlv_buf, sizeof(tlv_buf));
}

MfClassicError ndef_write_classic(
    Nfc* nfc,
    const NdefWriteContext* ctx,
    const uint8_t key[6],
    MfClassicKeyType key_type) {
    if(!nfc || !ctx || !key) return MfClassicErrorNone;

    uint8_t payload[200];
    size_t payload_len = sizeof(payload);
    bool ok = false;

    switch(ctx->type) {
    case NdefWriteTypeUri:
        ok = ndef_build_uri(
            ctx->data.uri.uri,
            ctx->data.uri.uri_len,
            (NdefUriPrefix)ctx->data.uri.prefix,
            payload,
            &payload_len);
        break;
    case NdefWriteTypeText:
        ok = ndef_build_text(
            ctx->data.text.text,
            ctx->data.text.text_len,
            ctx->data.text.lang,
            payload,
            &payload_len);
        break;
    default:
        return MfClassicErrorProtocol;
    }

    if(!ok) return MfClassicErrorProtocol;

    uint8_t tlv_buf[NDEF_TLV_MAX_SIZE];
    size_t tlv_len = sizeof(tlv_buf);
    if(!ndef_build_tlv(payload, payload_len, tlv_buf, &tlv_len)) {
        return MfClassicErrorProtocol;
    }

    MfClassicBlock mad_block;
    MfClassicError err = mf_classic_poller_sync_read_block(
        nfc, 1, (MfClassicKey*)ndef_mad_key, MfClassicKeyTypeA, &mad_block);
    if(err == MfClassicErrorNone) {
        if(mad_block.data[2] != ndef_aid[0] || mad_block.data[3] != ndef_aid[1]) {
            mad_block.data[2] = ndef_aid[0];
            mad_block.data[3] = ndef_aid[1];
            err = mf_classic_poller_sync_write_block(
                nfc, 1, (MfClassicKey*)ndef_mad_key, MfClassicKeyTypeA, &mad_block);
            if(err != MfClassicErrorNone) return err;
        }
    }

    size_t blocks = (tlv_len + 15) / 16;
    if(blocks > 3) blocks = 3;

    MfClassicBlock block;
    for(size_t i = 0; i < blocks; i++) {
        size_t offset = i * 16;
        size_t chunk_len = (offset + 16 <= tlv_len) ? 16 : (tlv_len - offset);

        memset(block.data, 0, sizeof(block.data));
        memcpy(block.data, tlv_buf + offset, chunk_len);

        err = mf_classic_poller_sync_write_block(
            nfc, 4 + (uint8_t)i, (MfClassicKey*)key, key_type, &block);
        if(err != MfClassicErrorNone) return err;
    }

    return MfClassicErrorNone;
}
