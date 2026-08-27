#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-pid-backend-test"

# Prove the retained product-local fallback still builds and obeys heater policy.
cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/pb_heater/include" \
  -I"$root/components/db_portal/include" \
  -I"$root/tests/stubs" \
  "$root/tests/pb_pid_backend_host_test.c" \
  -lm -o "$out-legacy"
"$out-legacy"

# Host tests run before ESP-IDF resolves managed_components on a clean CI checkout.
# Fetch the same temporary dc_pid pin declared in main/idf_component.yml so this
# test is self-contained instead of depending on a prior firmware-build side effect.
dc_pid_repo="https://github.com/danielbrownjr/dragon-core.git"
dc_pid_ref="c8f226cb5bcdc35ac5ca529497f692f174497859"
dc_pid_checkout="${TMPDIR:-/tmp}/dragonbreath-dc-pid-$$"
trap 'rm -rf "$dc_pid_checkout"' EXIT HUP INT TERM

git init -q "$dc_pid_checkout"
git -C "$dc_pid_checkout" remote add origin "$dc_pid_repo"
git -C "$dc_pid_checkout" fetch -q --depth=1 origin "$dc_pid_ref"
git -C "$dc_pid_checkout" checkout -q --detach FETCH_HEAD

# Prove the default dragon-core backend through the same product adapter contract.
cc -std=c11 -Wall -Wextra -Werror \
  -DCONFIG_PB_HEATER_PID_BACKEND_DC_PID=1 \
  -I"$root/components/pb_heater/include" \
  -I"$root/components/db_portal/include" \
  -I"$root/tests/stubs" \
  -I"$dc_pid_checkout/components/dc_pid/include" \
  "$root/tests/pb_pid_backend_host_test.c" \
  "$dc_pid_checkout/components/dc_pid/dc_pid.c" \
  -lm -o "$out-dc"
"$out-dc"
