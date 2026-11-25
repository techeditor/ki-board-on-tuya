/* Release OTA public key (ECDSA P-256) -- PLACEHOLDER.
 *
 * No release key exists yet. While AK_OTA_KEY_PLACEHOLDER is 1:
 *   - release firmware compiles with a loud warning and rejects every OTA
 *     package (ota_status reason "no_key");
 *   - a build with AK_RELEASE_BUILD=1 in the environment fails (CMakeLists.txt).
 * An official release replaces this file with the release public key (same
 * layout as ak_ota_key_test.h). Only the PUBLIC key goes in the repository. */
#ifndef AK_OTA_KEY_RELEASE_H
#define AK_OTA_KEY_RELEASE_H

#define AK_OTA_KEY_PLACEHOLDER 1
#define AK_OTA_KEY_POINT { 0 }

#endif
