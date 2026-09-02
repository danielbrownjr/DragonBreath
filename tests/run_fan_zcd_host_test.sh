#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-pb-fan-zcd-test"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/pb_fan/include" \
  "$root/tests/pb_fan_zcd_host_test.c" \
  -o "$out"

"$out"
