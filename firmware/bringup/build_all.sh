#!/usr/bin/env bash
set -euo pipefail

bringup_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${bringup_root}/../.." && pwd)"
tuyaopen_root="${TUYAOPEN_ROOT:-/data/projects/TuyaOpen}"

apps=(
    uart_loopback
    key1_uart
    input_scan
    lcd_st7789
    ws2812
    wireless_selftest
    full_selftest
)

if (($# > 0)); then
    apps=("$@")
fi

if [[ ! -f "${tuyaopen_root}/export.sh" ]]; then
    echo "TuyaOpen export.sh not found: ${tuyaopen_root}/export.sh" >&2
    exit 1
fi

source "${tuyaopen_root}/export.sh"

python3 "${repo_root}/hardware/pinmap/check_pins.py"

mapfile -t format_sources < <(
    find "${bringup_root}" \
        \( -path '*/.build' -o -path '*/dist' \) -prune \
        -o -type f \( -name '*.c' -o -name '*.h' \) -print \
        | sort
)
clang-format \
    --dry-run \
    --Werror \
    "--style=file:${tuyaopen_root}/.clang-format" \
    "${format_sources[@]}"

for app in "${apps[@]}"; do
    app_dir="${bringup_root}/${app}"
    echo
    echo "==> Building ${app}"
    (
        cd "${app_dir}"
        cmp app_default.config config/TUYA_T5AI_CORE.config
        tos.py config choice -c TUYA_T5AI_CORE.config
        tos.py build
    )

    artifact="$(
        find "${app_dir}/dist" -type f -name "${app}_QIO_*.bin" -print \
            | sort \
            | tail -1
    )"
    if [[ -z "${artifact}" ]]; then
        echo "QIO artifact not found for ${app}" >&2
        exit 1
    fi
    echo "    ${artifact}"
done

echo
echo "All bring-up applications built successfully."
