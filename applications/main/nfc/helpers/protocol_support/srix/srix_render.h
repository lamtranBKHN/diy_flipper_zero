#pragma once

#include "../nfc_protocol_support_render_common.h"

#include <nfc/protocols/srix/srix.h>

void nfc_render_srix_info(
    const SrixData* data,
    NfcProtocolFormatType format_type,
    FuriString* str);
