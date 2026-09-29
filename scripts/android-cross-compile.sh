#!/usr/bin/env bash
# Builds every registered portable target (bazel/portable_targets.bzl) for
# Android arm64 with the NDK -- the step-7 cross-compile gate from
# docs/modularization-plan.md. Compile/link only; nothing is run.
#
# Requires ANDROID_NDK_HOME. Extra arguments go to `bazel build`.
set -euo pipefail

: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to an Android NDK (r27 or newer)}"

cd "$(dirname "$0")/.."

bazel build //:portable_roots
patterns="$(mktemp)"
trap 'rm -f "$patterns"' EXIT
# Roots without a compilable artifact (shared resource files) are not built.
grep -v '^//resources/' bazel-bin/portable_roots >"$patterns"

bazel build -k --config=android --config=release \
    --target_pattern_file="$patterns" "$@"
