/**
 * @file ak_reset_netcfg.h
 * @brief Three-fast-boot trigger for clearing Tuya activation/network state.
 *
 * Three physical power-on/external resets within five seconds call
 * tuya_iot_reset(), matching FP-3 without counting software/crash restart loops.
 * The reset clears Wi-Fi/cloud binding data but does not erase UUID/AuthKey.
 */

#ifndef AK_RESET_NETCFG_H
#define AK_RESET_NETCFG_H

#include "tuya_cloud_types.h"

/** Count a physical boot and arm its five-second clear timer. */
OPERATE_RET ak_reset_netcfg_start(void);

/** Reset network/binding state when the persisted boot counter reached three. */
OPERATE_RET ak_reset_netcfg_check(void);

#endif /* AK_RESET_NETCFG_H */
