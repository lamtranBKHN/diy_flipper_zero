#include <furi.h>
#include <furi_hal.h>

#include <furi_hal_nfc.h>
#include <furi_hal_pn532.h>
#include <furi_hal_nfc_pn532.h>

#include "../test.h"

#define TAG "NfcRobustnessTest"

MU_TEST(nfc_error_enum_values) {
    mu_assert(FuriHalPn532ErrorNone == 0, "FuriHalPn532ErrorNone != 0");
    mu_assert(FuriHalPn532ErrorTimeout == 1, "FuriHalPn532ErrorTimeout != 1");
    mu_assert(FuriHalPn532ErrorComm == 2, "FuriHalPn532ErrorComm != 2");
    mu_assert(FuriHalPn532ErrorInvalidAck == 3, "FuriHalPn532ErrorInvalidAck != 3");
    mu_assert(FuriHalPn532ErrorInvalidFrame == 4, "FuriHalPn532ErrorInvalidFrame != 4");
    mu_assert(FuriHalPn532ErrorBufferOverflow == 5, "FuriHalPn532ErrorBufferOverflow != 5");
}

MU_TEST(nfc_timeout_constants_defined) {
    mu_assert(PN532_TIMEOUT_ACK_MS == 150, "PN532_TIMEOUT_ACK_MS != 150");
    mu_assert(PN532_TIMEOUT_CMD_MS == 300, "PN532_TIMEOUT_CMD_MS != 300");
    mu_assert(PN532_TIMEOUT_POLL_MS == 400, "PN532_TIMEOUT_POLL_MS != 400");
    mu_assert(PN532_TIMEOUT_EXCHANGE_MS == 1200, "PN532_TIMEOUT_EXCHANGE_MS != 1200");
    mu_assert(PN532_TIMEOUT_EXCHANGE_4K_MS == 1800, "PN532_TIMEOUT_EXCHANGE_4K_MS != 1800");
    mu_assert(PN532_TIMEOUT_PRESENCE_MS == 150, "PN532_TIMEOUT_PRESENCE_MS != 150");
}

MU_TEST(nfc_i2c_retries_defined) {
    mu_assert(PN532_I2C_RETRIES == 3, "PN532_I2C_RETRIES != 3");
}

MU_TEST(nfc_error_str_mapping) {
    mu_assert(
        strcmp(furi_hal_nfc_pn532_last_error_str(), "None") == 0,
        "Default last_error_str != 'None'");
}

MU_TEST(nfc_error_buffer_overflow_exists) {
    mu_assert(
        FuriHalPn532ErrorBufferOverflow > FuriHalPn532ErrorInvalidFrame,
        "BufferOverflow should be after InvalidFrame");
}

MU_TEST(nfc_freshness_timeout_defined) {
    mu_assert(
        PN532_TARGET_FRESHNESS_TIMEOUT_MS == 5000, "PN532_TARGET_FRESHNESS_TIMEOUT_MS != 5000");
}

MU_TEST(nfc_max_frame_sizes) {
    mu_assert(PN532_MAX_FRAME_SIZE == 192, "PN532_MAX_FRAME_SIZE != 192");
    mu_assert(PN532_MAX_RX_FRAME == 270, "PN532_MAX_RX_FRAME != 270");
    mu_assert(PN532_MAX_TX_PAYLOAD == 255, "PN532_MAX_TX_PAYLOAD != 255");
}

#ifndef NFC_TEST_INCLUDED

MU_TEST_SUITE(nfc_robustness) {
    MU_RUN_TEST(nfc_error_enum_values);
    MU_RUN_TEST(nfc_timeout_constants_defined);
    MU_RUN_TEST(nfc_i2c_retries_defined);
    MU_RUN_TEST(nfc_error_str_mapping);
    MU_RUN_TEST(nfc_error_buffer_overflow_exists);
    MU_RUN_TEST(nfc_freshness_timeout_defined);
    MU_RUN_TEST(nfc_max_frame_sizes);
}

int run_minunit_test_nfc_robustness(void) {
    MU_RUN_SUITE(nfc_robustness);
    return MU_EXIT_CODE;
}

#endif /* NFC_TEST_INCLUDED */
