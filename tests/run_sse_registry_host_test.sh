#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="${TMPDIR:-/tmp}/dragonbreath-sse-registry-test"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/pb_httpd" \
  "$root/components/pb_httpd/pb_sse_registry.c" \
  "$root/tests/pb_sse_registry_host_test.c" \
  -o "$out"

"$out"
