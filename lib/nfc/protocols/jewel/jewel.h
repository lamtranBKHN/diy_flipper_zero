#pragma once

#include <nfc/protocols/nfc_device_base_i.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JEWEL_UID_SIZE     (6U)
#define JEWEL_BLOCK_SIZE   (8U)
#define JEWEL_BLOCKS_TOTAL (16U)
#define JEWEL_DUMP_SIZE    (JEWEL_BLOCK_SIZE * JEWEL_BLOCKS_TOTAL)

typedef enum {
    JewelErrorNone,
    JewelErrorNotPresent,
    JewelErrorCommunication,
    JewelErrorTimeout,
    JewelErrorProtocol,
} JewelError;

typedef struct {
    uint8_t hr0;
    uint8_t hr1;
    uint8_t uid[4];
    uint8_t atqa[2];
    uint8_t dump[JEWEL_DUMP_SIZE]; /**< Full 128-byte memory dump (READ_ALL) */
    bool dump_valid; /**< True when dump contains valid READ_ALL data */
} JewelData;

extern const NfcDeviceBase nfc_device_jewel;

JewelData* jewel_alloc(void);

void jewel_free(JewelData* data);

void jewel_reset(JewelData* data);

void jewel_copy(JewelData* data, const JewelData* other);

bool jewel_verify(JewelData* data, const FuriString* device_type);

bool jewel_load(JewelData* data, FlipperFormat* ff, uint32_t version);

bool jewel_save(const JewelData* data, FlipperFormat* ff);

bool jewel_is_equal(const JewelData* data, const JewelData* other);

const char* jewel_get_device_name(const JewelData* data, NfcDeviceNameType name_type);

const uint8_t* jewel_get_uid(const JewelData* data, size_t* uid_len);

bool jewel_set_uid(JewelData* data, const uint8_t* uid, size_t uid_len);

JewelData* jewel_get_base_data(const JewelData* data);

#ifdef __cplusplus
}
#endif
