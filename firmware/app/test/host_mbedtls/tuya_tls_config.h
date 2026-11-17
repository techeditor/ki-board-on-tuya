/* Host-test mbedTLS configuration: only the modules ak_ble_sec uses, from
 * the same TuyaOpen mbedtls-3.1.0 sources the firmware links. mbedTLS's
 * build_info.h includes "tuya_tls_config.h" unconditionally; this file
 * shadows the SDK's on the host include path. */
#ifndef AK_HOST_TUYA_TLS_CONFIG_H
#define AK_HOST_TUYA_TLS_CONFIG_H

#define MBEDTLS_AES_C
#define MBEDTLS_CCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_HKDF_C
#define MBEDTLS_BASE64_C
/* ak_ota: ECDSA P-256 verification. */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C

#endif
