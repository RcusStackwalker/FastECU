"""Count unique warning sites per diagnostic flag in a Bazel build log (throwaway, #470)."""

import collections
import re
import sys

GNU = re.compile(r"^(?P<file>[^\s:()]+):(?P<line>\d+):\d+: warning: .*\[(?P<flag>-W[^\]]+)\]\s*$")
MSVC = re.compile(r"^(?P<file>.+?)\((?P<line>\d+)(?:,\d+)?\): warning (?P<flag>C\d+):")

sites: dict[str, set[tuple[str, str]]] = collections.defaultdict(set)
with open(sys.argv[1], errors="replace") as log:
    for raw in log:
        line = raw.rstrip()
        m = GNU.match(line) or MSVC.match(line)
        if m:
            sites[m["flag"]].add((m["file"].replace("\\", "/"), m["line"]))

total = sum(len(v) for v in sites.values())
print("| Flag | Unique sites |\n| --- | --- |")
for flag, s in sorted(sites.items(), key=lambda kv: -len(kv[1])):
    print(f"| `{flag}` | {len(s)} |")
print(f"| **total** | **{total}** |")
