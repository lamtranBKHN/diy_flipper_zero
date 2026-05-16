#pragma once

#include <nfc/protocols/nfc_device_base_i.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SRIX_PROTOCOL_NAME "SRIX"
#define SRIX_UID_SIZE      (8U)
#define SRIX_BLOCK_SIZE    (4U)
#define SRIX_BLOCKS_TOTAL  (128U)
#define SRIX_DATA_SIZE     (SRIX_BLOCK_SIZE * SRIX_BLOCKS_TOTAL) // 512

typedef enum {
    SrixErrorNone,
    SrixErrorNotPresent,
    SrixErrorTimeout,
    SrixErrorCommunication,
    SrixErrorWriteFailed,
} SrixError;

typedef enum {
    SrixTypeUnknown,
    SrixType512,
    SrixType4K,
} SrixType;

typedef struct {
    uint8_t uid[SRIX_UID_SIZE];
    SrixType type;
    uint8_t data[SRIX_DATA_SIZE];
} SrixData;

extern const NfcDeviceBase nfc_device_srix;

SrixData* srix_alloc(void);

void srix_free(SrixData* data);

void srix_reset(SrixData* data);

void srix_copy(SrixData* data, const SrixData* other);

bool srix_verify(SrixData* data, const FuriString* device_type);

bool srix_load(SrixData* data, FlipperFormat* ff, uint32_t version);

bool srix_save(const SrixData* data, FlipperFormat* ff);

bool srix_is_equal(const SrixData* data, const SrixData* other);

const char* srix_get_device_name(const SrixData* data, NfcDeviceNameType name_type);

const uint8_t* srix_get_uid(const SrixData* data, size_t* uid_len);

bool srix_set_uid(SrixData* data, const uint8_t* uid, size_t uid_len);

SrixData* srix_get_base_data(const SrixData* data);

#ifdef __cplusplus
}
#endif
