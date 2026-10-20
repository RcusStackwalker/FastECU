#!/usr/bin/env bash
# Sonar spike: wait for the just-uploaded analysis to be processed, then print
# the main branch's issue counts to the job summary. The free SonarCloud plan
# only serves the main branch, so each variant is scanned as `master` and read
# immediately, before the next variant overwrites it.
#
# Usage: sonar_spike_results.sh <label>   (needs SONAR_TOKEN, SPIKE_PROJECT_KEY)
set -euo pipefail

label=$1
api() { curl -fsS -u "${SONAR_TOKEN}:" "https://sonarcloud.io/api/$1"; }

ce_url=$(sed -n 's/^ceTaskUrl=//p' .scannerwork/report-task.txt)
for _ in $(seq 1 60); do
  status=$(curl -fsS -u "${SONAR_TOKEN}:" "$ce_url" | jq -r .task.status)
  case "$status" in
    SUCCESS) break ;;
    FAILED | CANCELED) echo "::error::Sonar background task $status"; exit 1 ;;
  esac
  sleep 5
done
[ "$status" = SUCCESS ] || { echo "::error::Sonar background task still $status"; exit 1; }

q="componentKeys=${SPIKE_PROJECT_KEY}&resolved=false&ps=1"
total=$(api "issues/search?${q}" | jq .total)
s1117=$(api "issues/search?${q}&rules=cpp:S1117" | jq .total)
s824=$(api "issues/search?${q}&rules=cpp:S824" | jq .total)
ncloc=$(api "measures/component?component=${SPIKE_PROJECT_KEY}&metricKeys=ncloc" |
  jq -r '.component.measures[0].value // "n/a"')

{
  echo "### ${label}"
  echo "| total issues | cpp:S1117 | cpp:S824 | ncloc |"
  echo "|---|---|---|---|"
  echo "| ${total} | ${s1117} | ${s824} | ${ncloc} |"
} | tee -a "$GITHUB_STEP_SUMMARY"
