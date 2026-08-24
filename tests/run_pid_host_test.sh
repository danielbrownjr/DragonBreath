#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-pb-pid-test"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/pb_heater/include" \
  "$root/tests/pb_pid_host_test.c" \
  -lm -o "$out"

"$out"
