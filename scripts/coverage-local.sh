#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
coverage_root=${COVERAGE_DIR:-"$repo_root/coverage"}
bazel_test_config=${BAZEL_TEST_CONFIG:-coverage}
llvm_profdata=${LLVM_PROFDATA:-llvm-profdata}
llvm_cov=${LLVM_COV:-llvm-cov}
# Floor for the post-rewrite import check below. The report currently carries
# ~400 file sections, ~350 of them inside the repo; this only has to be high
# enough that a wholesale path-matching failure cannot slip under it.
MIN_RESOLVABLE_FILES=100

if [ "$(uname -s)" = "Darwin" ]; then
  llvm_profdata=${LLVM_PROFDATA:-"xcrun llvm-profdata"}
  llvm_cov=${LLVM_COV:-"xcrun llvm-cov"}
fi

rm -rf "$coverage_root/bin" "$coverage_root/profiles"
rm -f "$coverage_root/coverage.profdata" "$coverage_root/coverage-summary.txt" \
  "$coverage_root/llvm-cov.report" "$coverage_root/llvm-cov.headers"
mkdir -p "$coverage_root/profiles"

coverage_ignore_regex='(^|/)(tests|hexedit)/|(^|/)(moc_|qrc_|ui_)|\.moc$|rep_.*_replica\.h|(^|/)Qt/[0-9][^/]*/|/Applications/|/opt/homebrew/|/Library/Developer/|bazel-out/|external/'

cd "$repo_root"

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

# `--config=coverage` pins `-ffile-compilation-dir=.` (see .bazelrc), so the
# coverage mapping embedded in the binaries carries workspace-relative source
# paths (e.g. "src/foo.cpp") instead of the execroot-absolute ones clang would
# otherwise record. `llvm-cov show` prints those relative paths verbatim as
# each file's section header. SonarCloud's llvm-cov sensor resolves those
# headers with PathResolver.relativePath(), which expects an absolute path to
# relativize against sonar.projectBaseDir; fed a relative one, it silently
# matches no indexed file, so every line reports as uncovered even though the
# section headers and hit counts are otherwise correct. Rewrite the headers to
# absolute paths so the sensor can match them.
#
# A header that is already absolute is left alone rather than prefixed a second
# time: it is either a system or toolchain header the ignore regex did not
# catch (harmless -- the sensor has no indexed file for it), or a sign that the
# compilation directory did not take effect, which the check below catches.
# Record each rewritten header in a side manifest so the import check below can
# verify them without re-parsing the (large) report.
: > "$coverage_root/llvm-cov.headers"

awk -v prefix="$repo_root/" -v manifest="$coverage_root/llvm-cov.headers" '
  /^[^[:space:]][^:]*\.(c|cc|cpp|cxx|h|hh|hpp):$/ {
    header = ($0 ~ /^\//) ? $0 : prefix $0
    print header
    print substr(header, 1, length(header) - 1) > manifest
    next
  }
  { print }
' "$coverage_root/llvm-cov.report.tmp" > "$coverage_root/llvm-cov.report"
rm -f "$coverage_root/llvm-cov.report.tmp"

# Every way this report can stop being importable -- a change in how llvm-cov
# spells its section headers, a compilation directory that stops being relative,
# a repo_root that is not what SonarCloud calls the project base dir -- looks
# the same from CI: the scan succeeds, the sensor logs that it parsed the file,
# and coverage silently reads 0.0%. That went unnoticed for days. So assert the
# property the sensor actually needs: headers that are absolute, inside the
# project, and naming files that exist.
headers_total=0
resolvable=0
while IFS= read -r header; do
  headers_total=$((headers_total + 1))
  case "$header" in
    "$repo_root"/*) [ -f "$header" ] && resolvable=$((resolvable + 1)) ;;
  esac
done < "$coverage_root/llvm-cov.headers"

echo "llvm-cov report: $headers_total file sections, $resolvable under $repo_root"

if [ "$resolvable" -lt "$MIN_RESOLVABLE_FILES" ]; then
  echo "only $resolvable of $headers_total section headers in" \
    "$coverage_root/llvm-cov.report name existing files under $repo_root" \
    "(expected at least $MIN_RESOLVABLE_FILES). SonarCloud matches coverage to" \
    "indexed files by that path, so it would import nothing and report 0.0%." \
    "First few headers:" >&2
  head -5 "$coverage_root/llvm-cov.headers" >&2
  exit 1
fi

cat "$coverage_root/coverage-summary.txt"
