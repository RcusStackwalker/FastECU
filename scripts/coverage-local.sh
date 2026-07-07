#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root=${BUILD_DIR:-"$repo_root/build/coverage"}
coverage_root=${COVERAGE_DIR:-"$repo_root/coverage"}
make_cmd=${MAKE:-make}
qmake_cmd=${QMAKE:-qmake}
llvm_profdata=${LLVM_PROFDATA:-llvm-profdata}
llvm_cov=${LLVM_COV:-llvm-cov}

if [ "$(uname -s)" = "Darwin" ]; then
  llvm_profdata=${LLVM_PROFDATA:-"xcrun llvm-profdata"}
  llvm_cov=${LLVM_COV:-"xcrun llvm-cov"}
fi

rm -rf "$coverage_root/bin" "$coverage_root/profiles"
rm -f "$coverage_root/coverage.profdata" "$coverage_root/coverage-summary.txt" "$coverage_root/llvm-cov.report"
mkdir -p "$build_root" "$coverage_root/bin" "$coverage_root/profiles"

coverage_ignore_regex='(^|/)(tests|hexedit)/|(^|/)(moc_|qrc_|ui_)|\.moc$|rep_.*_replica\.h|(^|/)Qt/[0-9][^/]*/|/Applications/|/opt/homebrew/|/Library/Developer/'

build_and_run() {
  project=$1
  binary=$2
  build_dir="$build_root/$binary"

  rm -rf "$build_dir"
  mkdir -p "$build_dir"
  (
    cd "$build_dir"
    "$qmake_cmd" CONFIG+=debug CONFIG-=release CONFIG-=debug_and_release \
      "QMAKE_CXXFLAGS+=-fprofile-instr-generate -fcoverage-mapping" \
      "QMAKE_LFLAGS+=-fprofile-instr-generate -fcoverage-mapping" \
      "$repo_root/$project"
    "$make_cmd"
    LLVM_PROFILE_FILE="$coverage_root/profiles/$binary-%p.profraw" "./$binary"
    cp "$binary" "$coverage_root/bin/$binary"
  )
}

build_and_run tests/tests.pro mut_dma_tests
build_and_run tests/serial_backend_tests.pro serial_backend_tests
build_and_run tests/serial_crash_tests.pro serial_crash_tests
build_and_run tests/mut_dma_integration_tests.pro mut_dma_integration_tests

set -- $llvm_profdata
"$@" merge -sparse "$coverage_root"/profiles/*.profraw -o "$coverage_root/coverage.profdata"

set -- $llvm_cov
"$@" report "$coverage_root/bin/mut_dma_tests" \
  -object="$coverage_root/bin/serial_backend_tests" \
  -object="$coverage_root/bin/serial_crash_tests" \
  -object="$coverage_root/bin/mut_dma_integration_tests" \
  -instr-profile="$coverage_root/coverage.profdata" \
  -ignore-filename-regex="$coverage_ignore_regex" \
  > "$coverage_root/coverage-summary.txt"

set -- $llvm_cov
"$@" show "$coverage_root/bin/mut_dma_tests" \
  -object="$coverage_root/bin/serial_backend_tests" \
  -object="$coverage_root/bin/serial_crash_tests" \
  -object="$coverage_root/bin/mut_dma_integration_tests" \
  -instr-profile="$coverage_root/coverage.profdata" \
  -ignore-filename-regex="$coverage_ignore_regex" \
  > "$coverage_root/llvm-cov.report"

cat "$coverage_root/coverage-summary.txt"
