#!/usr/bin/env sh
set -eu

if [ "$#" -ne 2 ]; then
  echo "Usage: $0 <coverage-summary.txt> <baseline-summary.txt>" >&2
  exit 2
fi

summary_file=$1
baseline_file=$2

extract_line_percent() {
  awk '
    /^TOTAL/ {
      value = $10
    }
    END {
      if (value == "") {
        exit 1
      }
      sub(/%$/, "", value)
      print value
    }
  ' "$1"
}

current=$(extract_line_percent "$summary_file")
baseline=$(extract_line_percent "$baseline_file")

awk -v current="$current" -v baseline="$baseline" '
  BEGIN {
    if (current + 0.0001 < baseline) {
      printf("Coverage decreased: current %.2f%%, baseline %.2f%%\n", current, baseline) > "/dev/stderr"
      exit 1
    }
    printf("Coverage ratchet passed: current %.2f%%, baseline %.2f%%\n", current, baseline)
  }
'
