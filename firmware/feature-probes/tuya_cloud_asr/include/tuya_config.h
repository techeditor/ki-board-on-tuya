/**
 * @file tuya_config.h
 * @brief IoT product + license configuration for FP-3 tuya-cloud-asr.
 *
 * The real UUID/AuthKey/PID live in the GITIGNORED header ak_tuya_authkey.h
 * (copy it from ak_tuya_authkey.h.example and fill from docs.local/tuya_keys.txt).
 * This tracked file only wires those macros into the names the SDK expects.
 */

#ifndef TUYA_CONFIG_H_
#define TUYA_CONFIG_H_

#include "ak_tuya_authkey.h"

#ifndef TUYA_PRODUCT_ID
#define TUYA_PRODUCT_ID AK_TUYA_PRODUCT_ID
#endif

#define TUYA_OPENSDK_UUID    AK_TUYA_UUID
#define TUYA_OPENSDK_AUTHKEY AK_TUYA_AUTHKEY

#endif /* TUYA_CONFIG_H_ */
