#pragma once

#include "srix.h"

#ifdef __cplusplus
extern "C" {
#endif

bool srix_device_save(const SrixData* data, const char* path);

bool srix_device_load(SrixData* data, const char* path);

#ifdef __cplusplus
}
#endif
