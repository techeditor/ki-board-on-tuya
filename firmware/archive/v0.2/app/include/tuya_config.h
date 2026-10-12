/**
 * @file tuya_config.h
 * @brief IoT product + license configuration for the board-side cloud ASR.
 *
 * The real UUID/AuthKey/PID live in the GITIGNORED header ak_tuya_authkey.h
 * (copy ak_tuya_authkey.h.example -> ak_tuya_authkey.h and fill from your Tuya
 * license, or flash a License and let tuya_authorize_read() supply it at
 * runtime — these macros are the compile-time fallback). This tracked file only
 * wires those macros into the names the SDK expects. Same scheme as FP-3.
 */

#ifndef AK_TUYA_CONFIG_H_
#define AK_TUYA_CONFIG_H_

#include "ak_tuya_authkey.h"

#ifndef TUYA_PRODUCT_ID
#define TUYA_PRODUCT_ID AK_TUYA_PRODUCT_ID
#endif

#define TUYA_OPENSDK_UUID    AK_TUYA_UUID
#define TUYA_OPENSDK_AUTHKEY AK_TUYA_AUTHKEY

#endif /* AK_TUYA_CONFIG_H_ */
