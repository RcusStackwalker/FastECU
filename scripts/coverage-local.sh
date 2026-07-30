#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
coverage_root=${COVERAGE_DIR:-"$repo_root/coverage"}
llvm_profdata=${LLVM_PROFDATA:-llvm-profdata}
llvm_cov=${LLVM_COV:-llvm-cov}

if [ "$(uname -s)" = "Darwin" ]; then
  llvm_profdata=${LLVM_PROFDATA:-"xcrun llvm-profdata"}
  llvm_cov=${LLVM_COV:-"xcrun llvm-cov"}
fi

rm -rf "$coverage_root/bin" "$coverage_root/profiles"
rm -f "$coverage_root/coverage.profdata" "$coverage_root/coverage-summary.txt" "$coverage_root/llvm-cov.report"
mkdir -p "$coverage_root/bin" "$coverage_root/profiles"

coverage_ignore_regex='(^|/)(tests|hexedit)/|(^|/)(moc_|qrc_|ui_)|\.moc$|rep_.*_replica\.h|(^|/)Qt/[0-9][^/]*/|/Applications/|/opt/homebrew/|/Library/Developer/|bazel-out/|external/'

cd "$repo_root"

# rules_qt's macOS test binaries locate Qt frameworks via a bazel-internal
# relative rpath that only resolves inside `bazel test`'s sandbox. To run them
# standalone (required to collect llvm profiles) point the dynamic loader at the
# Qt framework directory explicitly. Harmless/empty on non-Darwin.
qt_framework_path=""
if [ "$(uname -s)" = "Darwin" ]; then
  qtcore=$(find "$(bazel info output_base 2>/dev/null)/external" -maxdepth 3 -name QtCore.framework -type d 2>/dev/null | head -n1)
  [ -n "$qtcore" ] && qt_framework_path=$(dirname "$qtcore")
fi

# Build every macOS-compatible cc_test under //tests and the co-located
# //src/**/*_test targets with coverage instrumentation. Incompatible
# (Windows-only) targets are skipped by Bazel.
bazel build --config=coverage -k //tests/... //src/... || true

# Enumerate the instrumented test executables from the configured graph,
# including co-located src tests so they contribute to the coverage report.
test_files=$(bazel cquery --config=coverage --output=files \
  'kind("cc_test", //tests/... + //src/...)' 2>/dev/null || true)

primary=""
objects=""
for f in $test_files; do
  [ -x "$f" ] || continue
  case "$f" in *.dll|*.so|*.dylib) continue ;; esac
  name=$(basename "$f")
  dest="$coverage_root/bin/$name"
  cp "$f" "$dest"
  DYLD_FRAMEWORK_PATH="$qt_framework_path" LLVM_PROFILE_FILE="$coverage_root/profiles/$name-%p.profraw" "$dest" || true
  if [ -z "$primary" ]; then
    primary="$dest"
  else
    objects="$objects -object=$dest"
  fi
done

if [ -z "$primary" ]; then
  echo "no instrumented test binaries were produced" >&2
  exit 1
fi

set -- $llvm_profdata
"$@" merge -sparse "$coverage_root"/profiles/*.profraw -o "$coverage_root/coverage.profdata"

set -- $llvm_cov
# shellcheck disable=SC2086
"$@" report "$primary" $objects \
  -instr-profile="$coverage_root/coverage.profdata" \
  -ignore-filename-regex="$coverage_ignore_regex" \
  > "$coverage_root/coverage-summary.txt"

set -- $llvm_cov
# shellcheck disable=SC2086
"$@" show "$primary" $objects \
  -instr-profile="$coverage_root/coverage.profdata" \
  -ignore-filename-regex="$coverage_ignore_regex" \
  > "$coverage_root/llvm-cov.report.tmp"

# Bazel compiles with `-ffile-compilation-dir=.` for reproducible builds, so
# the coverage mapping embedded in the binaries carries workspace-relative
# source paths (e.g. "src/foo.cpp") instead of absolute ones. `llvm-cov show`
# then prints those relative paths verbatim as each file's section header.
# SonarCloud's llvm-cov sensor resolves those headers with
# PathResolver.relativePath(), which expects an absolute path to relativize
# against sonar.projectBaseDir; fed a relative one, it silently matches no
# indexed file, so every line reports as uncovered even though the section
# headers and hit counts are otherwise correct. Rewrite the headers to
# absolute paths so the sensor can match them.
awk -v prefix="$repo_root/" '
  /^[^[:space:]][^:]*\.(c|cc|cpp|cxx|h|hh|hpp):$/ { print prefix $0; next }
  { print }
' "$coverage_root/llvm-cov.report.tmp" > "$coverage_root/llvm-cov.report"
rm -f "$coverage_root/llvm-cov.report.tmp"

cat "$coverage_root/coverage-summary.txt"
