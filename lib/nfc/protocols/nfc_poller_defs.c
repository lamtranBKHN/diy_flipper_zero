#include "nfc_poller_defs.h"

#include <nfc/protocols/iso14443_3a/iso14443_3a_poller_defs.h>
#include <nfc/protocols/iso14443_3b/iso14443_3b_poller_defs.h>
#include <nfc/protocols/iso14443_4a/iso14443_4a_poller_defs.h>
#include <nfc/protocols/iso14443_4b/iso14443_4b_poller_defs.h>
#include <nfc/protocols/iso15693_3/iso15693_3_poller_defs.h>
#include <nfc/protocols/felica/felica_poller_defs.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller_defs.h>
#include <nfc/protocols/mf_classic/mf_classic_poller_defs.h>
#include <nfc/protocols/mf_plus/mf_plus_poller_defs.h>
#include <nfc/protocols/mf_desfire/mf_desfire_poller_defs.h>
#include <nfc/protocols/slix/slix_poller_defs.h>
#include <nfc/protocols/ntag4xx/ntag4xx_poller_defs.h>
#include <nfc/protocols/type_4_tag/type_4_tag_poller_defs.h>

/* ST25TB, EMV, and SRIX pollers are excluded on PN532-only builds.
 * Their source files are omitted by the SConscript exclusion filter, so
 * we must not reference their symbols when FURI_HAL_NFC_PN532_ONLY is set. */
#ifndef FURI_HAL_NFC_PN532_ONLY
#include <nfc/protocols/st25tb/st25tb_poller_defs.h>
#include <nfc/protocols/emv/emv_poller_defs.h>
#include <nfc/protocols/srix/srix_poller_defs.h>
#endif

const NfcPollerBase* const nfc_pollers_api[NfcProtocolNum] = {
    [NfcProtocolIso14443_3a] = &nfc_poller_iso14443_3a,
    [NfcProtocolIso14443_3b] = &nfc_poller_iso14443_3b,
    [NfcProtocolIso14443_4a] = &nfc_poller_iso14443_4a,
    [NfcProtocolIso14443_4b] = &nfc_poller_iso14443_4b,
    [NfcProtocolIso15693_3] = &nfc_poller_iso15693_3,
    [NfcProtocolFelica] = &nfc_poller_felica,
    [NfcProtocolMfUltralight] = &mf_ultralight_poller,
    [NfcProtocolMfClassic] = &mf_classic_poller,
    [NfcProtocolMfPlus] = &mf_plus_poller,
    [NfcProtocolMfDesfire] = &mf_desfire_poller,
    [NfcProtocolSlix] = &nfc_poller_slix,
#ifndef FURI_HAL_NFC_PN532_ONLY
    [NfcProtocolSt25tb] = &nfc_poller_st25tb,
#else
    [NfcProtocolSt25tb] = NULL,
#endif
    [NfcProtocolNtag4xx] = &ntag4xx_poller,
    [NfcProtocolType4Tag] = &type_4_tag_poller,
#ifndef FURI_HAL_NFC_PN532_ONLY
    [NfcProtocolEmv] = &emv_poller,
    [NfcProtocolSrix] = &nfc_poller_srix,
#else
    [NfcProtocolEmv] = NULL,
    [NfcProtocolSrix] = NULL,
#endif
    /* Add new pollers here */
};
