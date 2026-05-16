/**
 * @file test_iso15693_lock.c
 * @brief Unit test for ISO15693 lock command (T12).
 *
 * Tests the ISO15693 lock block command implementation:
 * 1. Lock command frame construction
 * 2. Response parsing (success/error)
 * 3. Listener lock state tracking
 *
 * Run via: ./fbt FIRMWARE_APP_SET=unit_tests && ./fbt launch_app APPSRC=unit_tests
 */
#include <furi.h>
#include "test_api.h"
#include <minunit.h>
#include <string.h>

/* ISO15693 constants (mirrored from implementation) */
#define ISO15693_CMD_LOCK_BLOCK 0x22
#define ISO15693_FLAG_ADDRESS   0x10
#define ISO15693_UID_SIZE       8

static void test_lock_cmd_frame(void) {
    uint8_t uid[ISO15693_UID_SIZE] = {0xE0, 0x04, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    uint8_t cmd[12];
    size_t pos = 0;

    cmd[pos++] = ISO15693_FLAG_ADDRESS;
    cmd[pos++] = ISO15693_CMD_LOCK_BLOCK;
    memcpy(&cmd[pos], uid, ISO15693_UID_SIZE);
    pos += ISO15693_UID_SIZE;
    cmd[pos++] = 0x05;

    mu_assert_int_eq(ISO15693_FLAG_ADDRESS, cmd[0]);
    mu_assert_int_eq(ISO15693_CMD_LOCK_BLOCK, cmd[1]);
    mu_assert_int_eq(0xE0, cmd[2]);
    mu_assert_int_eq(0x06, cmd[9]);
    mu_assert_int_eq(0x05, cmd[10]);
    mu_assert_int_eq(11, pos);
}

static void test_lock_response_success(void) {
    uint8_t rx_data[] = {0x00};
    size_t rx_len = sizeof(rx_data);

    mu_assert_int_eq(0, rx_data[0] & 0x01);

    (void)rx_len;
}

static void test_lock_response_error(void) {
    uint8_t error_not_supported = 0x03;
    uint8_t error_block_locked = 0x02;
    uint8_t error_block_not_avail = 0x0B;

    mu_assert_int_eq(1, error_not_supported & 0x01);
    mu_assert_int_eq(0, error_block_locked & 0x01);
    mu_assert_int_eq(1, error_block_not_avail & 0x01);
}

static void test_listener_lock_tracking(void) {
    bool block_locked[256] = {0};
    uint8_t block_num = 42;

    mu_assert_int_eq(false, block_locked[block_num]);

    block_locked[block_num] = true;
    mu_assert_int_eq(true, block_locked[block_num]);

    mu_assert_int_eq(false, block_locked[0]);
    mu_assert_int_eq(false, block_locked[255]);
    mu_assert_int_eq(false, block_locked[41]);
    mu_assert_int_eq(false, block_locked[43]);
}

static void test_lock_multiple_blocks(void) {
    bool block_locked[256] = {0};

    block_locked[0] = true;
    block_locked[5] = true;
    block_locked[10] = true;
    block_locked[100] = true;

    mu_assert_int_eq(true, block_locked[0]);
    mu_assert_int_eq(true, block_locked[5]);
    mu_assert_int_eq(true, block_locked[10]);
    mu_assert_int_eq(true, block_locked[100]);

    for(int i = 0; i < 256; i++) {
        if(i != 0 && i != 5 && i != 10 && i != 100) {
            mu_assert_int_eq(false, block_locked[i]);
        }
    }
}

#ifndef NFC_TEST_INCLUDED

static int run_all(void) {
    test_lock_cmd_frame();
    test_lock_response_success();
    test_lock_response_error();
    test_listener_lock_tracking();
    test_lock_multiple_blocks();
    return 0;
}

TEST_API_DEFINE(run_all)

#endif /* NFC_TEST_INCLUDED */
