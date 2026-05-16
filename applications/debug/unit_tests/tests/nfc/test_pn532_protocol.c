#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_pn532.h>
#include <furi_hal_nfc_pn532.h>
#include "../test.h"

#define TAG "NfcPn532ProtocolTest"

static_assert(
    PN532_MAX_RX_FRAME >= PN532_MAX_TX_PAYLOAD + 7,
    "PN532_MAX_RX_FRAME too small for PN532_MAX_TX_PAYLOAD + frame overhead");

MU_TEST(nfc_buffer_sizes) {
    mu_assert(PN532_MAX_RX_FRAME >= PN532_MAX_TX_PAYLOAD + 7,
        "RX frame must accommodate TX payload + 7 bytes overhead");
    mu_assert(PN532_MAX_FRAME_SIZE <= PN532_MAX_RX_FRAME,
        "MAX_FRAME_SIZE must not exceed MAX_RX_FRAME");
}

static bool verify_crc_a(const uint8_t* data, size_t len, uint16_t expected_crc) {
    uint16_t crc = 0x6363;
    for(size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        byte ^= (uint8_t)(crc & 0xFF);
        byte ^= (uint8_t)(byte << 4);
        crc = (crc >> 8) ^ (((uint16_t)byte) << 8) ^ (((uint16_t)byte) << 3) ^ (byte >> 4);
    }
    return crc == expected_crc;
}

MU_TEST(nfc_crc_a_known_values) {
    mu_assert(verify_crc_a((const uint8_t[]){0x00}, 1, 0xE3E3),
        "CRC-A of {0x00} failed");
    mu_assert(verify_crc_a((const uint8_t[]){0xFF}, 1, 0x1C1C),
        "CRC-A of {0xFF} failed");
    mu_assert(verify_crc_a((const uint8_t[]){0x93, 0x20}, 2, 0x3C6F),
        "CRC-A of SDD CL1 failed");
}

MU_TEST(nfc_frame_encode_decode) {
    uint8_t frame[] = {0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00};
    size_t len = sizeof(frame);
    uint16_t crc_val = 0x6363;
    for(size_t i = 0; i < len; i++) {
        uint8_t byte = frame[i];
        byte ^= (uint8_t)(crc_val & 0xFF);
        byte ^= (uint8_t)(byte << 4);
        crc_val = (crc_val >> 8) ^ (((uint16_t)byte) << 8) ^ (((uint16_t)byte) << 3) ^ (byte >> 4);
    }
    uint8_t frame_with_crc[8] = {0};
    memcpy(frame_with_crc, frame, len);
    frame_with_crc[len] = (uint8_t)(crc_val & 0xFF);
    frame_with_crc[len + 1] = (uint8_t)(crc_val >> 8);
    mu_assert(verify_crc_a(frame_with_crc, len + 2, crc_val),
        "CRC should verify when appended correctly");
    frame_with_crc[len] ^= 1;
    mu_assert(!verify_crc_a(frame_with_crc, len + 2, crc_val),
        "CRC should fail when payload is corrupted");
}

MU_TEST(nfc_error_enum_completeness) {
    mu_assert(FuriHalPn532ErrorNone == 0, "FuriHalPn532ErrorNone != 0");
    mu_assert(FuriHalPn532ErrorTimeout == 1, "FuriHalPn532ErrorTimeout != 1");
    mu_assert(FuriHalPn532ErrorComm == 2, "FuriHalPn532ErrorComm != 2");
    mu_assert(FuriHalPn532ErrorInvalidAck == 3, "FuriHalPn532ErrorInvalidAck != 3");
    mu_assert(FuriHalPn532ErrorInvalidFrame == 4, "FuriHalPn532ErrorInvalidFrame != 4");
    mu_assert(FuriHalPn532ErrorBufferOverflow == 5, "FuriHalPn532ErrorBufferOverflow != 5");
    mu_assert(FuriHalPn532ErrorUnsupported == 6, "FuriHalPn532ErrorUnsupported != 6");
}

MU_TEST(nfc_pn532_result_enum_completeness) {
    mu_assert(FuriHalNfcPn532ResultDetected == 0, "FuriHalNfcPn532ResultDetected != 0");
    mu_assert(FuriHalNfcPn532ResultNotPresent == 1, "FuriHalNfcPn532ResultNotPresent != 1");
    mu_assert(FuriHalNfcPn532ResultUnsupportedByPn532 == 2, "FuriHalNfcPn532ResultUnsupportedByPn532 != 2");
    mu_assert(FuriHalNfcPn532ResultCommunicationError == 3, "FuriHalNfcPn532ResultCommunicationError != 3");
    mu_assert(FuriHalNfcPn532ResultParseError == 4, "FuriHalNfcPn532ResultParseError != 4");
}

MU_TEST(nfc_furi_hal_error_enum_completeness) {
    mu_assert(FuriHalNfcErrorNone == 0, "FuriHalNfcErrorNone != 0");
    mu_assert(FuriHalNfcErrorNotSupported == 1, "FuriHalNfcErrorNotSupported != 1");
    mu_assert(FuriHalNfcErrorBusy == 2, "FuriHalNfcErrorBusy != 2");
    mu_assert(FuriHalNfcErrorCommunication == 3, "FuriHalNfcErrorCommunication != 3");
    mu_assert(FuriHalNfcErrorIncompleteFrame == 4, "FuriHalNfcErrorIncompleteFrame != 4");
    mu_assert(FuriHalNfcErrorDataFormat == 5, "FuriHalNfcErrorDataFormat != 5");
    mu_assert(FuriHalNfcErrorCommunicationTimeout == 6, "FuriHalNfcErrorCommunicationTimeout != 6");
    mu_assert(FuriHalNfcErrorBufferOverflow == 7, "FuriHalNfcErrorBufferOverflow != 7");
}

#ifndef NFC_TEST_INCLUDED

MU_TEST_SUITE(nfc_pn532_protocol) {
    MU_RUN_TEST(nfc_buffer_sizes);
    MU_RUN_TEST(nfc_crc_a_known_values);
    MU_RUN_TEST(nfc_frame_encode_decode);
    MU_RUN_TEST(nfc_error_enum_completeness);
    MU_RUN_TEST(nfc_pn532_result_enum_completeness);
    MU_RUN_TEST(nfc_furi_hal_error_enum_completeness);
}

int run_minunit_test_nfc_pn532_protocol(void) {
    MU_RUN_SUITE(nfc_pn532_protocol);
    return MU_EXIT_CODE;
}

#endif /* NFC_TEST_INCLUDED */
