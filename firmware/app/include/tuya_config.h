/**
 * @file tuya_config.h
 * @brief IoT product + license configuration for the board-side cloud ASR.
 *
 * The PID comes from Kconfig (CONFIG_TUYA_PRODUCT_ID in the build profile).
 * UUID/AuthKey come from the device's authorization storage at runtime,
 * written over USB inside the pairing window or with `tos.py monitor` auth.
 *
 * Only debug profiles (CONFIG_AK_LICENSE_DEBUG_READ=y) may fall back to
 * compile-time values from the GITIGNORED ak_tuya_authkey.h (copy
 * ak_tuya_authkey.h.example and fill it). Production profiles never include
 * that header, so no UUID/AuthKey can end up in a release BIN (FW-07, Q3).
 */

#ifndef AK_TUYA_CONFIG_H_
#define AK_TUYA_CONFIG_H_

#if AK_ENABLE_LICENSE_DEBUG_READ
#include "ak_tuya_authkey.h"
#define TUYA_OPENSDK_UUID    AK_TUYA_UUID
#define TUYA_OPENSDK_AUTHKEY AK_TUYA_AUTHKEY
#ifndef TUYA_PRODUCT_ID
#define TUYA_PRODUCT_ID AK_TUYA_PRODUCT_ID
#endif
#endif

#ifndef TUYA_PRODUCT_ID
#define TUYA_PRODUCT_ID ""
#endif

#endif /* AK_TUYA_CONFIG_H_ */
