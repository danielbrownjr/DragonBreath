#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR=${1:-.}
TARGET=${2:-esp32c3}
BUILD_DIR=${3:-build}
if (( $# > 3 )); then
  IDF_ARGS=("${@:4}")
else
  IDF_ARGS=(build)
fi
IDF_PY=${IDF_PY:-idf.py}
PROJECT_DIR=$(cd "$PROJECT_DIR" && pwd)
[[ "$BUILD_DIR" = /* ]] || BUILD_DIR="$PROJECT_DIR/$BUILD_DIR"

IDF_ROOT=${IDF_PATH:-"${HOME}/esp/esp-idf"}
IDF_TOOLS_ROOT=${IDF_TOOLS_PATH:-"${HOME}/.espressif"}
if [[ ! -f "$IDF_ROOT/export.sh" ]]; then
  echo "error: ESP-IDF export.sh not found at $IDF_ROOT/export.sh" >&2
  exit 2
fi

for family in xtensa-esp-elf riscv32-esp-elf; do
  family_root="$IDF_TOOLS_ROOT/tools/$family"
  if [[ -d "$family_root" ]]; then
    while IFS= read -r bin_dir; do PATH="$bin_dir:$PATH"; done < <(find "$family_root" -type d -name bin | sort)
  fi
done
export PATH
# shellcheck disable=SC1090
source "$IDF_ROOT/export.sh" >/dev/null

case "$TARGET" in
  esp32) compiler=xtensa-esp32-elf-gcc ;;
  esp32c2|esp32c3|esp32c5|esp32c6|esp32h2|esp32p4) compiler=riscv32-esp-elf-gcc ;;
  esp32s2) compiler=xtensa-esp32s2-elf-gcc ;;
  esp32s3) compiler=xtensa-esp32s3-elf-gcc ;;
  *) echo "error: unsupported target: $TARGET" >&2; exit 2 ;;
esac
if ! command -v "$compiler" >/dev/null 2>&1; then
  echo "error: $compiler is unavailable; run $IDF_ROOT/install.sh $TARGET" >&2
  exit 2
fi
echo "ESP-IDF: $IDF_ROOT"
echo "Python:  $(command -v python)"
echo "Compiler: $(command -v "$compiler")"

MANIFEST="$PROJECT_DIR/main/idf_component.yml"
LOCK="$PROJECT_DIR/dependencies.lock"
MAPPING_TMP=$(mktemp)
MISMATCH_TMP=$(mktemp)
RESOLVED_TMP=$(mktemp)
trap 'rm -f "$MAPPING_TMP" "$MISMATCH_TMP" "$RESOLVED_TMP"' EXIT
extract_manifest_git_refs() {
  awk '
    function emit() {
      if (name != "" && repo != "" && value != "") print name, repo, value
    }
    /^  [A-Za-z0-9_.\/-]+:$/ {
      emit()
      name=$1; sub(/:$/, "", name)
      repo=""; value=""
      next
    }
    name != "" && /^    git:/ {
      repo=$2; gsub(/["'\'' ]/, "", repo)
      next
    }
    name != "" && /^    version:/ {
      value=$2; gsub(/["'\'' ]/, "", value)
    }
    END { emit() }
  ' "$1"
}
extract_lock_git_refs() {
  awk '
    function emit() {
      if (name != "" && is_git && value != "") print name, value
    }
    /^  [A-Za-z0-9_.\/-]+:$/ {
      emit()
      name=$1; sub(/:$/, "", name)
      is_git=0; value=""
      next
    }
    name != "" && /^[[:space:]]+git:/ { is_git=1; next }
    name != "" && /^    version:/ {
      value=$2; gsub(/["'\'' ]/, "", value)
    }
    END { emit() }
  ' "$1"
}
resolve_git_ref() {
  repo=$1
  ref=$2
  if [[ "$ref" =~ ^[0-9a-f]{40}$ ]]; then
    printf '%s\n' "$ref"
    return 0
  fi
  cached=$(awk -v repo="$repo" -v ref="$ref" '$1 == repo && $2 == ref { print $3; exit }' "$RESOLVED_TMP")
  if [[ -n "$cached" ]]; then
    printf '%s\n' "$cached"
    return 0
  fi
  resolved=$(git ls-remote "$repo" "refs/tags/$ref" "refs/heads/$ref" | awk -v ref="$ref" '
    $2 == "refs/tags/" ref { print $1; exit }
    $2 == "refs/heads/" ref { print $1; exit }
  ')
  [[ -n "$resolved" ]] || return 1
  printf '%s %s %s\n' "$repo" "$ref" "$resolved" >> "$RESOLVED_TMP"
  printf '%s\n' "$resolved"
}
check_lock() {
  : > "$MISMATCH_TMP"
  [[ -f "$MANIFEST" && -f "$LOCK" ]] || return 0
  extract_lock_git_refs "$LOCK" > "$MAPPING_TMP"
  while read -r component repo requested; do
    if ! expected=$(resolve_git_ref "$repo" "$requested"); then
      printf '%s could not resolve %s at %s\n' "$component" "$requested" "$repo" >> "$MISMATCH_TMP"
      continue
    fi
    actual=$(awk -v name="$component" '$1 == name { print $2; exit }' "$MAPPING_TMP")
    [[ "$actual" == "$expected" ]] || printf '%s expected %s (%s), lock has %s\n' "$component" "$requested" "$expected" "${actual:-missing}" >> "$MISMATCH_TMP"
  done < <(extract_manifest_git_refs "$MANIFEST")
  [[ ! -s "$MISMATCH_TMP" ]]
}

mkdir -p "$BUILD_DIR"
if ! check_lock; then
  stamp=$(date +%Y%m%d-%H%M%S)
  backup="$BUILD_DIR/dependency-cache-$stamp"
  mkdir -p "$backup"
  echo "Stale ESP-IDF dependency cache detected:"
  sed 's/^/  /' "$MISMATCH_TMP"
  mv "$LOCK" "$backup/dependencies.lock"
  [[ ! -d "$PROJECT_DIR/managed_components" ]] || mv "$PROJECT_DIR/managed_components" "$backup/managed_components"
  echo "Quarantined stale dependency state under $backup"
fi

if [[ "${IDF_PREFLIGHT_ONLY:-0}" == "1" ]]; then
  echo "Toolchain and dependency-cache preflight passed."
  exit 0
fi
"$IDF_PY" -C "$PROJECT_DIR" -B "$BUILD_DIR" -D "IDF_TARGET=$TARGET" "${IDF_ARGS[@]}"
check_lock || { echo "error: dependency lock differs from manifest git refs" >&2; cat "$MISMATCH_TMP" >&2; exit 1; }
echo "Dependency lock matches every manifest git ref."
