#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-pid-backend-test"

# Prove the retained product-local fallback still builds and obeys heater policy.
cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/pb_heater/include" \
  "$root/tests/pb_pid_backend_host_test.c" \
  -lm -o "$out-legacy"
"$out-legacy"

# Prove the default dragon-core backend through the same product adapter contract.
cc -std=c11 -Wall -Wextra -Werror \
  -DCONFIG_PB_HEATER_PID_BACKEND_DC_PID=1 \
  -I"$root/components/pb_heater/include" \
  -I"$root/managed_components/dc_pid/include" \
  "$root/tests/pb_pid_backend_host_test.c" \
  "$root/managed_components/dc_pid/dc_pid.c" \
  -lm -o "$out-dc"
"$out-dc"
