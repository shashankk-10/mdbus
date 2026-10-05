#!/usr/bin/env bash
# Builds and tests the repo three ways, each in its own directory: as is, under ASan + UBSan,
# and under TSan. Any compiler warning or failing test stops it.
#   scripts/check.sh    BUILD_ROOT (default /tmp/mdbus-check) and JOBS override.
# JOBS defaults to 4, one per P-core.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
build_root="${BUILD_ROOT:-/tmp/mdbus-check}"
# ASan: stop at the first error, and abort so ctest records a crash, not a clean exit.
export ASAN_OPTIONS=halt_on_error=1:abort_on_error=1
# UBSan: stop at the first error and print where it happened.
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
# TSan: judge the shm accesses made through interceptors (memcpy, memset) too, and stop at the
# first race.
export TSAN_OPTIONS=ignore_interceptors_accesses=0:halt_on_error=1

# build_and_test <dir name> [cmake options...]: clean build, fail on any warning, then ctest.
build_and_test() {
  local build_dir="$build_root/$1"
  shift
  echo "== $(basename "$build_dir")"
  cmake -S "$repo_root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release "$@" >/dev/null
  cmake --build "$build_dir" --clean-first -j "${JOBS:-4}" >"$build_dir/build.log" 2>&1 ||
    { tail -30 "$build_dir/build.log"; exit 1; }
  if grep 'warning:' "$build_dir/build.log"; then exit 1; fi
  ctest --test-dir "$build_dir" -j "${JOBS:-4}" --output-on-failure
}

build_and_test default
build_and_test address -DMDBUS_SANITIZE=address
build_and_test thread -DMDBUS_SANITIZE=thread
