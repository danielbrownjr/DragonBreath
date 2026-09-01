#!/bin/sh
set -eu

build_dir="${1:-build-hil-devboard}"
config="$build_dir/config/sdkconfig.h"
if [ ! -f "$config" ]; then
    echo "missing HIL build config: $config" >&2
    exit 1
fi

grep -q '^#define CONFIG_PB_HIL_CONSOLE 1$' "$config"
grep -q '^#define CONFIG_PB_HIL_DEVBOARD 1$' "$config"
sh "$(dirname "$0")/check_devboard_compileout.sh" "$build_dir"

echo "HIL compile-out check: PASS"
