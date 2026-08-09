#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="${TMPDIR:-/tmp}/dragonbreath-db-portal-config-test"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/tests/stubs" \
  -I"$root/components/db_portal/include" \
  -I"$root/components/db_klipper_mqtt/include" \
  -I"$root/components/pb_ha/include" \
  "$root/components/db_portal/db_portal_config.c" \
  "$root/tests/db_portal_config_host_test.c" \
  -o "$out"

"$out"
