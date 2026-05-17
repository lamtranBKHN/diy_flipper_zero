#pragma once

#include <nfc/nfc.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight.h>
#include <nfc/protocols/mf_classic/mf_classic.h>
#include <nfc/helpers/ndef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NdefWriteTypeUri,
    NdefWriteTypeText,
    NdefWriteTypeVcard,
    NdefWriteTypeWifi,
} NdefWriteType;

typedef struct {
    NdefWriteType type;
    union {
        struct {
            const char* uri;
            size_t uri_len;
            uint8_t prefix;
        } uri;
        struct {
            const char* text;
            size_t text_len;
            const char* lang;
        } text;
        struct {
            const char* vcard;
            size_t vcard_len;
        } vcard;
        struct {
            const char* ssid;
            size_t ssid_len;
            const char* password;
            size_t password_len;
            uint8_t auth_type;
        } wifi;
    } data;
} NdefWriteContext;

MfUltralightError ndef_write_ultralight(Nfc* nfc, const NdefWriteContext* ctx);

MfClassicError ndef_write_classic(
    Nfc* nfc,
    const NdefWriteContext* ctx,
    const uint8_t key[6],
    MfClassicKeyType key_type);

#ifdef __cplusplus
}
#endif
