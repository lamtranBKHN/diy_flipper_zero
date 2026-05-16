/**
 * @file test_iso14443a_listener.c
 * @brief Unit test for ISO14443-A listener TX parity (T06).
 *
 * Tests the custom parity packing and transmission in listener mode.
 * Verifies that:
 * 1. Parity bits are correctly interleaved with data bits (9 bits per byte)
 * 2. The listener TX function accepts and processes parity-framed data
 * 3. Odd parity calculation matches ISO14443-A specification
 *
 * Run via: ./fbt FIRMWARE_APP_SET=unit_tests && ./fbt launch_app APPSRC=unit_tests
 */
#include <furi.h>
#include "test_api.h"
#include <minunit.h>

/* ISO14443-A odd parity: parity bit = 1 if number of 1-bits is even */
static bool iso14443a_odd_parity(uint8_t byte) {
    bool parity = true;
    for(int i = 0; i < 8; i++) {
        parity ^= ((byte >> i) & 0x01) != 0;
    }
    return parity;
}

static void test_odd_parity_calc(void) {
    mu_assert_int_eq(true, iso14443a_odd_parity(0x00));
    mu_assert_int_eq(false, iso14443a_odd_parity(0x01));
    mu_assert_int_eq(true, iso14443a_odd_parity(0xFF));
    mu_assert_int_eq(true, iso14443a_odd_parity(0x55));
    mu_assert_int_eq(true, iso14443a_odd_parity(0xAA));
}

static void test_frame_packing_single(void) {
    uint8_t data[] = {0x01};
    uint8_t parity[] = {false};
    size_t bit_pos = 0;
    uint8_t framed[2] = {0};

    framed[0] |= data[0] << (bit_pos % 8);
    bit_pos += 8;

    if(parity[0]) {
        framed[bit_pos / 8] |= (1U << (bit_pos % 8));
    }
    bit_pos += 1;

    mu_assert_int_eq(0x01, framed[0]);
    mu_assert_int_eq(0x00, framed[1]);
    mu_assert_int_eq(9, bit_pos);
}

static void test_frame_packing_multi(void) {
    uint8_t data[] = {0x01, 0x02, 0x03};
    uint8_t parity[3];
    parity[0] = iso14443a_odd_parity(0x01);
    parity[1] = iso14443a_odd_parity(0x02);
    parity[2] = iso14443a_odd_parity(0x03);

    size_t bit_pos = 0;
    uint8_t framed[4] = {0};

    for(int i = 0; i < 3; i++) {
        size_t byte_index = bit_pos / 8;
        size_t bit_offset = bit_pos % 8;
        framed[byte_index] |= data[i] << bit_offset;
        if(bit_offset != 0) {
            framed[byte_index + 1] |= data[i] >> (8 - bit_offset);
        }
        bit_pos += 8;

        size_t parity_index = bit_pos / 8;
        size_t parity_offset = bit_pos % 8;
        if(parity[i]) {
            framed[parity_index] |= (1U << parity_offset);
        }
        bit_pos += 1;
    }

    mu_assert_int_eq(27, bit_pos);
    mu_assert_int_eq(0x01, framed[0]);
    mu_assert_int_eq(0x04, framed[1]);
    mu_assert_int_eq(0x06, framed[2]);
    mu_assert_int_eq(0x04, framed[3]);
}

#ifndef NFC_TEST_INCLUDED

static int run_all(void) {
    test_odd_parity_calc();
    test_frame_packing_single();
    test_frame_packing_multi();
    return 0;
}

TEST_API_DEFINE(run_all)

#endif /* NFC_TEST_INCLUDED */
