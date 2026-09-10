#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
coverage_root=${COVERAGE_DIR:-"$repo_root/coverage"}
bazel_test_config=${BAZEL_TEST_CONFIG:-coverage}
llvm_profdata=${LLVM_PROFDATA:-llvm-profdata}
llvm_cov=${LLVM_COV:-llvm-cov}

if [ "$(uname -s)" = "Darwin" ]; then
  llvm_profdata=${LLVM_PROFDATA:-"xcrun llvm-profdata"}
  llvm_cov=${LLVM_COV:-"xcrun llvm-cov"}
fi

rm -rf "$coverage_root/bin" "$coverage_root/profiles"
rm -f "$coverage_root/coverage.profdata" "$coverage_root/coverage-summary.txt" "$coverage_root/llvm-cov.report"
mkdir -p "$coverage_root/profiles"

coverage_ignore_regex='(^|/)(tests|hexedit)/|(^|/)(moc_|qrc_|ui_)|\.moc$|rep_.*_replica\.h|(^|/)Qt/[0-9][^/]*/|/Applications/|/opt/homebrew/|/Library/Developer/|bazel-out/|external/'

cd "$repo_root"

# BAZEL_TEST_STRACE_LOG (set only by the SonarCloud job while diagnosing a
# server crash) attaches strace to the Bazel server for the duration of the
# test run, scoped to process-lifecycle syscalls to keep overhead low. `bazel
# info server_pid` starts the server if needed and prints its PID without
# running a build; strace then attaches to that already-running process
# (not exec'd fresh), so this doesn't touch bazel test's own exit-code
# propagation below at all.
strace_pid=
if [ -n "${BAZEL_TEST_STRACE_LOG:-}" ]; then
  bazel_server_pid=$(bazel info server_pid)
  sudo strace -f -e trace=%process -tt -o "$BAZEL_TEST_STRACE_LOG" -p "$bazel_server_pid" &
  strace_pid=$!
  sleep 1
fi

# Let Bazel run every compatible test so target-specific environments, runfiles,
# framework paths, platform constraints, and failures retain their normal test
# semantics. LLVM's %m token gives each instrumented binary a unique profile
# name; %p prevents collisions between concurrent processes from that binary.
#
# BAZEL_TEST_CONFIG lets a caller select a different named config from
# .bazelrc; the SonarCloud build-wrapper step (.github/workflows/sonar.yml)
# passes `sonar`, which layers on :coverage with the disk cache disabled.
bazel test \
  --config="$bazel_test_config" \
  --nocache_test_results \
  --sandbox_writable_path="$coverage_root/profiles" \
  --test_env="LLVM_PROFILE_FILE=$coverage_root/profiles/%m-%p.profraw" \
  //...

if [ -n "$strace_pid" ]; then
  # SIGINT is ignored by background jobs in POSIX shells by default; SIGTERM
  # isn't, and strace treats it the same way (clean detach, flush, exit).
  sudo kill -TERM "$strace_pid" 2>/dev/null || true
  wait "$strace_pid" 2>/dev/null || true
fi

# Enumerate the instrumented test executables from the configured graph for
# llvm-cov's object list.
test_files=$(bazel cquery --config="$bazel_test_config" --output=files \
  'kind("cc_test", //...)')

primary=""
objects=""
for f in $test_files; do
  [ -x "$f" ] || continue
  case "$f" in *.dll|*.so|*.dylib) continue ;; esac
  if [ -z "$primary" ]; then
    primary="$f"
  else
    objects="$objects -object=$f"
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
