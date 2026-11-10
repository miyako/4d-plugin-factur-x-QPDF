#!/bin/bash
set -euo pipefail
# SOURCE may point to an immutable git-archive checkout; SDK and libraries
# always come from the inspected worktree's pinned, unchanged dependencies.
root="$(cd "$(dirname "$0")/.." && pwd)"
source_root="${1:-$root}"
build="${2:?usage: run_native_ownership.sh [source-root] build-directory}"
mkdir -p "$build"
sdk="$root/4D-Plugin-SDK/4D Plugin API"
cmake -S "$root/facturx" -B "$build/dependencies" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFACTURX_PLUGIN_OUTPUT_DIR="$build/plugin" > "$build/configure.log" 2>&1
cmake --build "$build/dependencies" --target fx_qpdf --parallel 4 > "$build/build.log" 2>&1
clang++ -std=c++20 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Wno-int-conversion -Wno-multichar \
  -I"$source_root/facturx" -I"$sdk" -I"$root/qpdf/include" \
  "$root/tests/native_ownership.cpp" -x c++ "$sdk/4DPluginAPI.c" -x none \
  "$build/dependencies/libfx_qpdf.a" "$build/dependencies/libfx_zlib.a" \
  "$build/dependencies/jpeg/install/lib/libjpeg.a" \
  -framework CoreGraphics -framework CoreFoundation -o "$build/ownership"
failed=0
for test in failed-property-output failed-element-output successful-ownership; do
  if ! ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$build/ownership" "$test"; then
    failed=1
  fi
done
exit "$failed"
