#!/usr/bin/env bash
# Compile and run host unit tests for the pure P2-1 modules
# (app_notification + input_map). No SDK or hardware required.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
app="$(cd "${here}/.." && pwd)"
app_common="$(cd "${app}/../../app/common" && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "${out}"' EXIT

gcc -std=c11 -Wall -Wextra -Werror \
    -I "${app}/include" \
    -I "${app_common}/include" \
    "${app}/src/app_notification.c" \
    "${app}/src/input_map.c" \
    "${app_common}/src/ak_input_fsm.c" \
    "${here}/test_phase2.c" \
    -o "${out}/test_phase2"

"${out}/test_phase2"
