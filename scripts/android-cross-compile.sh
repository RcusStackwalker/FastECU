#!/usr/bin/env bash
# The portable-core gate (docs/modularization-plan.md, step 7). Everything under
# //src/backend and //src/algorithms is portable by definition, so:
#
#   1. no //src/platform label may be reachable from it, however many deps away
#      (a query, so it needs no NDK), and
#   2. all of it must build for Android arm64 with the NDK (compile/link only;
#      nothing is run). Qt-bearing code has no Android configuration, so it
#      fails here even when it is not a //src/platform label.
#
# Requires ANDROID_NDK_HOME. Extra arguments go to the `bazel build`.
set -euo pipefail

: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to an Android NDK (r27 or newer)}"

cd "$(dirname "$0")/.."

portable='//src/backend/... + //src/algorithms/...'

violations="$(bazel query "filter('^//src/platform/', deps($portable))")"
if [[ -n "$violations" ]]; then
    echo "FAIL: portable targets reach platform code:" >&2
    sed 's/^/  /' <<<"$violations" >&2
    echo "" >&2
    echo "Portable targets must not depend on //src/platform, directly or through" >&2
    echo "several layers of deps. Move the platform-facing code to src/platform or" >&2
    echo "src/ui, or reach it through a backend port." >&2
    exit 1
fi

bazel build -k --config=android --config=release "$@" //src/backend/... //src/algorithms/...
