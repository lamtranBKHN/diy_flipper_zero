#pragma once

#include <nfc/protocols/jewel/jewel.h>

#include "../nfc_protocol_support_render_common.h"

void nfc_render_jewel_info(
    const JewelData* data,
    NfcProtocolFormatType format_type,
    FuriString* str);

void nfc_render_jewel_dump(const JewelData* data, FuriString* str);
