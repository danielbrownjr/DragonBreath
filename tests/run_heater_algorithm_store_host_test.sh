#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="${TMPDIR:-/tmp}/dragonbreath-heater-algorithm-store-test"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/pb_heater/include" \
  -I"$root/tests/stubs" \
  "$root/components/pb_heater/pb_heater_algorithm_store.c" \
  "$root/tests/pb_heater_algorithm_store_host_test.c" \
  -lm -o "$out"

"$out"
