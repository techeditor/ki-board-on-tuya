#!/usr/bin/env bash
# Compile and run host unit tests for the pure FP-lcd-showcase modules
# (showcase_map + showcase_state). No SDK or hardware required.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
app="$(cd "${here}/.." && pwd)"
app_common="$(cd "${app}/../../app/common" && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "${out}"' EXIT

gcc -std=c11 -Wall -Wextra -Werror \
    -I "${app}/include" \
    -I "${app_common}/include" \
    "${app}/src/showcase_map.c" \
    "${app}/src/showcase_state.c" \
    "${here}/test_lcd_showcase.c" \
    -o "${out}/test_lcd_showcase"

"${out}/test_lcd_showcase"
