#!/usr/bin/env bash
# Compile and run the host unit tests for the pure firmware modules
# (ak_input_fsm + ak_protocol). No SDK or hardware required.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
app="$(cd "${here}/.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "${out}"' EXIT

python3 "${app}/tools/version.py" check >/dev/null

gcc -std=c11 -Wall -Wextra -Werror \
    -I "${app}/common/include" \
    "${app}/common/src/ak_input_fsm.c" \
    "${app}/common/src/ak_keymap.c" \
    "${app}/common/src/ak_voice_route.c" \
    "${app}/common/src/ak_settings.c" \
    "${app}/common/src/ak_protocol.c" \
    "${app}/common/src/ak_rx.c" \
    "${here}/test_ak.c" \
    -o "${out}/test_ak"

"${out}/test_ak"
