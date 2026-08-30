#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
tmp="${TMPDIR:-/tmp}/dragonbreath-dc-pid"
out="${TMPDIR:-/tmp}/dragonbreath-pb-heater-pid-test"

# Keep host coverage on the exact temporary dependency used by the firmware
# manifest. Delete this fetch/pin once dc_pid is consumed from a dragon-core tag.
dc_pid_commit=3447fd4d7d76235c8d14b10aed18a7c169e3fc80
base="https://raw.githubusercontent.com/danielbrownjr/dragon-core/$dc_pid_commit/components/dc_pid"
mkdir -p "$tmp/include"
curl -fsSL "$base/include/dc_pid.h" -o "$tmp/include/dc_pid.h"
curl -fsSL "$base/dc_pid.c" -o "$tmp/dc_pid.c"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/pb_heater/include" \
  -I"$root/tests/stubs" \
  -I"$tmp/include" \
  "$root/tests/pb_heater_pid_host_test.c" \
  "$tmp/dc_pid.c" \
  -lm -o "$out"

"$out"
