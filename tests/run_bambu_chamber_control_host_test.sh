#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="${TMPDIR:-/tmp}/dragonbreath-bambu-chamber-control-test"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/db_portal/include" \
  -I"$root/components/pb_heater/include" \
  -I"$root/tests/stubs" \
  "$root/components/db_portal/db_bambu_chamber_control.c" \
  "$root/tests/bambu_chamber_control_host_test.c" \
  -lm -o "$out"

"$out"
