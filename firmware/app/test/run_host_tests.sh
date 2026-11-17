#!/usr/bin/env bash
# Compile and run the host unit tests for the pure firmware modules
# (input FSM, keymap, settings, protocol, RX parser, field checks, BLE secure
# channel). No hardware required. The BLE channel links the TuyaOpen
# mbedTLS sources (the same ones the firmware uses): set TUYAOPEN_ROOT (or
# OPEN_SDK_ROOT) when the SDK is not at $HOME/TuyaOpen.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
app="$(cd "${here}/.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "${out}"' EXIT

sdk="${TUYAOPEN_ROOT:-${OPEN_SDK_ROOT:-$HOME/TuyaOpen}}"
mbed="${sdk}/src/libtls/mbedtls-3.1.0"
if [[ ! -f "${mbed}/library/ccm.c" ]]; then
    echo "mbedTLS sources not found under ${mbed}; set TUYAOPEN_ROOT" >&2
    exit 2
fi

python3 "${app}/tools/version.py" check >/dev/null
python3 "${app}/tools/check_boot_order.py"
if python3 -c "import cryptography" 2>/dev/null; then
    python3 "${app}/tools/test_ota_pack.py" -q
else
    echo "test_ota_pack: SKIPPED (pip install cryptography)" >&2
fi

# Host mbedTLS: only the modules ak_ble_sec needs (see host_mbedtls/).
mbed_inc=(-I "${here}/host_mbedtls" -I "${mbed}/include" -I "${mbed}/library"
          "-DMBEDTLS_CONFIG_FILE=\"empty_config.h\"")
for src in aes ccm cipher cipher_wrap md sha256 hkdf base64 platform_util constant_time \
           bignum ecp ecp_curves ecdsa asn1parse asn1write; do
    gcc -std=c11 -O1 -w "${mbed_inc[@]}" -c "${mbed}/library/${src}.c" -o "${out}/mbed_${src}.o"
done

gcc -std=c11 -Wall -Wextra -Werror \
    -I "${app}/common/include" "${mbed_inc[@]}" \
    "${app}/common/src/ak_input_fsm.c" \
    "${app}/common/src/ak_keymap.c" \
    "${app}/common/src/ak_voice_route.c" \
    "${app}/common/src/ak_settings.c" \
    "${app}/common/src/ak_protocol.c" \
    "${app}/common/src/ak_rx.c" \
    "${app}/common/src/ak_validate.c" \
    "${app}/common/src/ak_font.c" \
    "${app}/common/src/ak_font_data.c" \
    "${app}/common/src/ak_font_ui_data.c" \
    "${app}/common/src/ak_text.c" \
    "${app}/common/src/ak_i18n.c" \
    "${app}/common/src/ak_provision.c" \
    "${app}/common/src/ak_cred.c" \
    "${app}/common/src/ak_voice_mode.c" \
    "${app}/common/src/ak_byte_ring.c" \
    "${app}/common/src/ak_led_power.c" \
    "${app}/common/src/ak_bat_gauge.c" \
    "${app}/common/src/ak_led_policy.c" \
    "${app}/common/src/ak_ble_sec.c" \
    "${app}/common/src/ak_window_marker.c" \
    "${app}/common/src/ak_menu.c" \
    "${app}/common/src/ak_ui.c" \
    "${here}/test_ak.c" \
    "${out}"/mbed_*.o \
    -o "${out}/test_ak"

"${out}/test_ak"

gcc -std=c11 -Wall -Wextra -Werror \
    -I "${app}/common/include" -I "${here}" "${mbed_inc[@]}" \
    "${app}/common/src/ak_ota.c" \
    "${app}/common/src/ak_rx.c" \
    "${app}/common/src/ak_validate.c" \
    "${here}/test_ota.c" \
    "${out}"/mbed_*.o \
    -o "${out}/test_ota"

"${out}/test_ota"
