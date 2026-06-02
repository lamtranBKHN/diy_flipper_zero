#include "nfc_poller_defs.h"

#include <nfc/protocols/iso14443_3a/iso14443_3a_poller_defs.h>
#include <nfc/protocols/iso14443_3b/iso14443_3b_poller_defs.h>
#include <nfc/protocols/iso14443_4a/iso14443_4a_poller_defs.h>
#include <nfc/protocols/iso14443_4b/iso14443_4b_poller_defs.h>
#include <nfc/protocols/felica/felica_poller_defs.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller_defs.h>
#include <nfc/protocols/mf_classic/mf_classic_poller_defs.h>
#include <nfc/protocols/mf_plus/mf_plus_poller_defs.h>
#include <nfc/protocols/mf_desfire/mf_desfire_poller_defs.h>
#include <nfc/protocols/ntag4xx/ntag4xx_poller_defs.h>
#include <nfc/protocols/type_4_tag/type_4_tag_poller_defs.h>

#include <nfc/protocols/srix/srix_poller_defs.h>
#include <nfc/protocols/emv/emv_poller_defs.h>
#include <nfc/protocols/jewel/jewel_poller_defs.h>

const NfcPollerBase* const nfc_pollers_api[NfcProtocolNum] = {
    [NfcProtocolIso14443_3a] = &nfc_poller_iso14443_3a,
    [NfcProtocolIso14443_3b] = &nfc_poller_iso14443_3b,
    [NfcProtocolIso14443_4a] = &nfc_poller_iso14443_4a,
    [NfcProtocolIso14443_4b] = &nfc_poller_iso14443_4b,
    [NfcProtocolFelica] = &nfc_poller_felica,
    [NfcProtocolMfUltralight] = &mf_ultralight_poller,
    [NfcProtocolMfClassic] = &mf_classic_poller,
    [NfcProtocolMfPlus] = &mf_plus_poller,
    [NfcProtocolMfDesfire] = &mf_desfire_poller,
    [NfcProtocolIso15693_3] = NULL,
    [NfcProtocolSlix] = NULL,
    [NfcProtocolSt25tb] = NULL,
    [NfcProtocolNtag4xx] = &ntag4xx_poller,
    [NfcProtocolType4Tag] = &type_4_tag_poller,
    [NfcProtocolEmv] = &emv_poller,
    [NfcProtocolSrix] = &nfc_poller_srix,
    [NfcProtocolJewel] = &nfc_poller_jewel,
    /* Add new pollers here */
};
