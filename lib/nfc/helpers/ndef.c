#include "ndef.h"
#include <string.h>
#include <furi.h>

#define TAG "Ndef"

// URI prefix strings indexed by code value (0x00-0x23)
static const char* ndef_uri_prefixes[] = {
    "", // 0x00: No prefix
    "http://www.", // 0x01
    "https://www.", // 0x02
    "http://", // 0x03
    "https://", // 0x04
    "tel:", // 0x05
    "mailto:", // 0x06
    "ftp://anonymous:anonymous@", // 0x07
    "ftp://ftp.", // 0x08
    "ftps://", // 0x09
    "sftp://", // 0x0A
    "smb://", // 0x0B
    "nfs://", // 0x0C
    "ftp://", // 0x0D
    "dav://", // 0x0E
    "news:", // 0x0F
    "telnet://", // 0x10
    "imap:", // 0x11
    "rtsp://", // 0x12
    "urn:", // 0x13
    "pop:", // 0x14
    "sip:", // 0x15
    "sips:", // 0x16
    "tftp:", // 0x17
    "btspp://", // 0x18
    "btl2cap://", // 0x19
    "btgoep://", // 0x1A
    "tcpobex://", // 0x1B
    "irdaobex://", // 0x1C
    "file://", // 0x1D
    "urn:epc:id:", // 0x1E
    "urn:epc:tag:", // 0x1F
    "urn:epc:pat:", // 0x20
    "urn:epc:raw:", // 0x21
    "urn:epc:", // 0x22
    "urn:nfc:", // 0x23
};

bool ndef_find_tlv(const uint8_t* data, size_t data_len, size_t* ndef_offset, size_t* ndef_length) {
    if(!data || !ndef_offset || !ndef_length) return false;

    size_t offset = 0;
    while(offset < data_len) {
        uint8_t type = data[offset++];
        if(type == NDEF_TLV_TERMINATOR) break;
        if(type == NDEF_TLV_NULL) continue;

        size_t len = 0;
        if(offset >= data_len) break;
        uint8_t len_byte = data[offset++];
        if(len_byte < 0xFF) {
            len = len_byte;
        } else {
            // 3-byte extended length (0xFF followed by 2-byte big-endian length)
            if(offset + 2 > data_len) break;
            len = ((size_t)data[offset] << 8) | data[offset + 1];
            offset += 2;
        }

        if(type == NDEF_TLV_NDEF) {
            if(offset + len > data_len) return false;
            *ndef_offset = offset;
            *ndef_length = len;
            return true;
        }

        offset += len;
    }

    return false;
}

bool ndef_parse(const uint8_t* data, size_t data_len, NdefMessage* msg) {
    if(!data || !msg) return false;

    memset(msg, 0, sizeof(NdefMessage));

    // Find NDEF TLV
    size_t ndef_off = 0, ndef_len = 0;
    if(!ndef_find_tlv(data, data_len, &ndef_off, &ndef_len)) {
        return false;
    }

    const uint8_t* ndef_data = data + ndef_off;
    size_t remaining = ndef_len;
    size_t record_idx = 0;

    while(remaining > 0 && record_idx < NDEF_MAX_RECORDS) {
        // First byte: flags + TNF
        uint8_t flags_tnf = ndef_data[0];
        //uint8_t mb = (flags_tnf >> 7) & 1;  // Message Begin
        //uint8_t me = (flags_tnf >> 6) & 1;  // Message End
        //uint8_t cf = (flags_tnf >> 5) & 1;  // Chunk Flag
        //uint8_t sr = (flags_tnf >> 4) & 1;  // Short Record (3-byte header)
        //uint8_t il = (flags_tnf >> 3) & 1;  // ID Length present
        uint8_t tnf = flags_tnf & 0x07;

        remaining--;
        if(remaining < 1) break;

        size_t pos = 1;
        size_t id_len = 0;

        // Type length
        uint8_t type_len = ndef_data[pos++];
        remaining--;

        // Payload length
        size_t payload_len = 0;
        if(flags_tnf & 0x10) {
            // Short Record: 1 byte payload length
            if(remaining < 1) break;
            payload_len = ndef_data[pos++];
            remaining--;
        } else {
            // Normal: 4 byte payload length
            if(remaining < 4) break;
            payload_len = ((size_t)ndef_data[pos] << 24) | ((size_t)ndef_data[pos + 1] << 16) |
                          ((size_t)ndef_data[pos + 2] << 8) | ndef_data[pos + 3];
            pos += 4;
            remaining -= 4;
        }

        // ID length (optional)
        if(flags_tnf & 0x08) {
            if(remaining < 1) break;
            id_len = ndef_data[pos++];
            remaining--;
        }

        // Type
        if(remaining < type_len) break;
        uint8_t* type = (uint8_t*)ndef_data + pos;
        pos += type_len;
        remaining -= type_len;

        // ID
        uint8_t* id = NULL;
        if(id_len > 0) {
            if(remaining < id_len) break;
            id = (uint8_t*)ndef_data + pos;
            pos += id_len;
            remaining -= id_len;
        }

        // Payload
        if(remaining < payload_len) break;
        uint8_t* payload = (uint8_t*)ndef_data + pos;
        pos += payload_len;
        remaining -= payload_len;

        // Fill record
        NdefRecord* rec = &msg->records[record_idx++];
        rec->tnf = tnf;
        rec->type = type;
        rec->type_len = type_len;
        rec->id = id;
        rec->id_len = id_len;
        rec->payload = payload;
        rec->payload_len = payload_len;

        ndef_data = ndef_data + pos;
    }

    msg->record_count = record_idx;
    return (record_idx > 0);
}

bool ndef_build_uri(
    const char* uri,
    size_t uri_len,
    NdefUriPrefix prefix,
    uint8_t* out_buf,
    size_t* out_len) {
    if(!uri || !out_buf || !out_len) return false;

    // NDEF record header:
    // flags (MB|ME|SR|IL=0|TNF=1) = 0xD1
    // type_len = 1 ('U')
    // payload_len = 1 (prefix) + uri_len (short record = 1 byte)
    // type = 'U' (0x55)
    // payload = [prefix_code] [uri_bytes]

    size_t payload_total = 1 + uri_len;
    if(payload_total > 255) return false; // Short Record limit

    size_t total = 3 + 1 + payload_total; // header(3) + type(1) + payload
    if(total > *out_len) return false;

    size_t pos = 0;
    out_buf[pos++] = 0xD1; // MB|ME|SR|TNF=WellKnown
    out_buf[pos++] = 1; // type length ('U')
    out_buf[pos++] = (uint8_t)payload_total; // payload length (short record)
    out_buf[pos++] = NDEF_RTD_URI; // 'U'
    out_buf[pos++] = (uint8_t)prefix; // URI prefix code
    memcpy(&out_buf[pos], uri, uri_len);
    pos += uri_len;

    *out_len = pos;
    return true;
}

bool ndef_build_text(
    const char* text,
    size_t text_len,
    const char* lang,
    uint8_t* out_buf,
    size_t* out_len) {
    if(!text || !lang || !out_buf || !out_len) return false;

    size_t lang_len = strlen(lang);
    if(lang_len > 31) lang_len = 31; // Language code max 31 bytes (5 bits in status byte)

    size_t payload_total = 1 + lang_len + text_len; // status byte + language + text
    if(payload_total > 255) return false; // Short Record limit

    size_t total = 3 + 1 + payload_total; // header(3) + type(1) + payload
    if(total > *out_len) return false;

    size_t pos = 0;
    out_buf[pos++] = 0xD1; // MB|ME|SR|TNF=WellKnown
    out_buf[pos++] = 1; // type length ('T')
    out_buf[pos++] = (uint8_t)payload_total; // payload length
    out_buf[pos++] = NDEF_RTD_TEXT; // 'T'
    out_buf[pos++] = (uint8_t)(lang_len & 0x1F); // status byte: bit 7=0 (UTF-8), bits 5-0=lang_len
    memcpy(&out_buf[pos], lang, lang_len);
    pos += lang_len;
    memcpy(&out_buf[pos], text, text_len);
    pos += text_len;

    *out_len = pos;
    return true;
}

bool ndef_extract_url(const NdefMessage* msg, char* out_url, size_t out_size) {
    if(!msg || !out_url || out_size == 0) return false;

    for(size_t i = 0; i < msg->record_count; i++) {
        const NdefRecord* rec = &msg->records[i];
        if(rec->tnf == NDEF_TNF_WELL_KNOWN && rec->type_len == 1 && rec->type[0] == NDEF_RTD_URI &&
           rec->payload_len > 0) {
            uint8_t prefix_code = rec->payload[0];
            const char* prefix = "";
            if(prefix_code <= 0x23) {
                prefix = ndef_uri_prefixes[prefix_code];
            }

            size_t prefix_len = strlen(prefix);
            size_t uri_body_len = rec->payload_len - 1;

            if(prefix_len + uri_body_len + 1 > out_size) return false;

            memcpy(out_url, prefix, prefix_len);
            memcpy(out_url + prefix_len, rec->payload + 1, uri_body_len);
            out_url[prefix_len + uri_body_len] = '\0';
            return true;
        }
    }

    return false;
}

bool ndef_extract_text(const NdefMessage* msg, char* out_text, size_t out_size) {
    if(!msg || !out_text || out_size == 0) return false;

    for(size_t i = 0; i < msg->record_count; i++) {
        const NdefRecord* rec = &msg->records[i];
        if(rec->tnf == NDEF_TNF_WELL_KNOWN && rec->type_len == 1 &&
           rec->type[0] == NDEF_RTD_TEXT && rec->payload_len > 1) {
            uint8_t status = rec->payload[0];
            size_t lang_len = status & 0x1F;

            if(1 + lang_len >= rec->payload_len) return false;

            size_t text_body_len = rec->payload_len - 1 - lang_len;
            if(text_body_len + 1 > out_size) return false;

            memcpy(out_text, rec->payload + 1 + lang_len, text_body_len);
            out_text[text_body_len] = '\0';
            return true;
        }
    }

    return false;
}

void ndef_message_clear(NdefMessage* msg) {
    if(msg) {
        memset(msg, 0, sizeof(NdefMessage));
    }
}
