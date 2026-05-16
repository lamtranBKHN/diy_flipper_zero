#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// NFC Forum NDEF constants
#define NDEF_TNF_EMPTY        (0x00)
#define NDEF_TNF_WELL_KNOWN   (0x01)
#define NDEF_TNF_MIME_MEDIA   (0x02)
#define NDEF_TNF_ABSOLUTE_URI (0x03)
#define NDEF_TNF_EXTERNAL     (0x04)
#define NDEF_TNF_UNKNOWN      (0x05)
#define NDEF_TNF_UNCHANGED    (0x06)
#define NDEF_TNF_RESERVED     (0x07)

// Well-known record type constants
#define NDEF_RTD_TEXT         'T' // NFC Forum Text Record
#define NDEF_RTD_URI          'U' // NFC Forum URI Record
#define NDEF_RTD_SMART_POSTER 'S' // (first byte of "Sp")

// TLV block types
#define NDEF_TLV_NULL           (0x00)
#define NDEF_TLV_LOCK_CONTROL   (0x01)
#define NDEF_TLV_MEMORY_CONTROL (0x02)
#define NDEF_TLV_NDEF           (0x03)
#define NDEF_TLV_PROPRIETARY    (0x05)
#define NDEF_TLV_TERMINATOR     (0xFE)

// URI prefix codes (NFC Forum URI Record Type Definition)
typedef enum {
    NdefUriPrefixNoPrefix = 0x00,
    NdefUriPrefixHttpWww = 0x01, // "http://www."
    NdefUriPrefixHttpsWww = 0x02, // "https://www."
    NdefUriPrefixHttp = 0x03, // "http://"
    NdefUriPrefixHttps = 0x04, // "https://"
    NdefUriPrefixTel = 0x05, // "tel:"
    NdefUriPrefixMailto = 0x06, // "mailto:"
    NdefUriPrefixFtp = 0x0D, // "ftp://"
} NdefUriPrefix;

#define NDEF_MAX_RECORDS      (4)
#define NDEF_MAX_PAYLOAD_SIZE (256)

typedef struct {
    uint8_t tnf; // Type Name Format
    uint8_t* type; // Record type (e.g., "U", "T", "Sp")
    size_t type_len;
    uint8_t* id; // Record ID (optional)
    size_t id_len;
    uint8_t* payload; // Record payload
    size_t payload_len;
} NdefRecord;

typedef struct {
    NdefRecord records[NDEF_MAX_RECORDS];
    size_t record_count;
} NdefMessage;

// Parse NDEF from raw tag data
// data: raw bytes from tag (starting at NDEF TLV payload)
// data_len: length of data
// msg: output message structure
// Returns: true if NDEF message was parsed successfully
bool ndef_parse(const uint8_t* data, size_t data_len, NdefMessage* msg);

// Build NDEF URI message
// uri: URI string (e.g., "flipperzero.one")
// uri_len: length of URI string
// prefix: URI prefix code (see NdefUriPrefix)
// out_buf: buffer for output
// out_len: output buffer size, updated with actual written length
// Returns: true if message was built
bool ndef_build_uri(
    const char* uri,
    size_t uri_len,
    NdefUriPrefix prefix,
    uint8_t* out_buf,
    size_t* out_len);

// Build NDEF text message
// text: text string
// text_len: length of text
// lang: language code (e.g., "en")
// out_buf: buffer for output
// out_len: output buffer size, updated with actual written length
bool ndef_build_text(
    const char* text,
    size_t text_len,
    const char* lang,
    uint8_t* out_buf,
    size_t* out_len);

// Extract URL from parsed NDEF message
// msg: parsed NDEF message
// out_url: buffer for URL string
// out_size: buffer size
// Returns: true if URL was extracted
bool ndef_extract_url(const NdefMessage* msg, char* out_url, size_t out_size);

// Extract text from parsed NDEF message
// msg: parsed NDEF message
// out_text: buffer for text string
// out_size: buffer size
// Returns: true if text was extracted
bool ndef_extract_text(const NdefMessage* msg, char* out_text, size_t out_size);

// Find NDEF TLV in raw tag data
// data: raw bytes from tag
// data_len: length of data
// ndef_offset: output - offset of NDEF TLV content
// ndef_length: output - length of NDEF TLV content
// Returns: true if NDEF TLV found
bool ndef_find_tlv(const uint8_t* data, size_t data_len, size_t* ndef_offset, size_t* ndef_length);

// Free NDEF message internal allocations
void ndef_message_clear(NdefMessage* msg);

#ifdef __cplusplus
}
#endif
