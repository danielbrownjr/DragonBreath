#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-pb-klipper-mqtt-test"

# db_klipper_mqtt_arm.h is pure (only libc), so no ESP stubs are needed — just its
# include dir for the header under test.
cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/db_klipper_mqtt/include" \
  "$root/tests/db_klipper_mqtt_host_test.c" \
  -o "$out"

"$out"
