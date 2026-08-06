#!/usr/bin/env python3
"""One-shot transcription tool: copied error_codes.h's QHash tables into the
portable std::unordered_map accessors now in dtc_tables.cpp. Entry lines were
copied byte-for-byte; only the surrounding declaration changed.

Its input, error_codes.h, was deleted in commit 03f202e, so this script no
longer runs (it fails with FileNotFoundError). It is retained as the
reviewable record of how dtc_tables.cpp was produced, not as a live
regeneration path -- see dtc_tables.h for how to add new entries now.

When this script was last run, its output still needed a pass of
clang-format (via prek) before committing: the script emits 4-space
indentation, while the committed file follows the project's formatting.
"""

import re
import sys

SRC = "src/backend/definitions/error_codes.h"
OUT = "src/algorithms/diagnostics/dtc_tables.cpp"

ACCESSORS = [
    ("neg_rsp_codes", "nrc_codes"),
    ("dtc_Pxxxx_codes", "dtc_p_codes"),
    ("dtc_Bxxxx_codes", "dtc_b_codes"),
    ("dtc_Cxxxx_codes", "dtc_c_codes"),
    ("dtc_Uxxxx_codes", "dtc_u_codes"),
]

with open(SRC, encoding="utf-8") as f:
    text = f.read()


def body(member):
    match = re.search(
        r"^const QHash<int, QString> FileActions::" + member + r"\{\n(.*?)^\};$",
        text,
        re.M | re.S,
    )
    if not match:
        sys.exit(f"table {member} not found in {SRC}")
    return match.group(1)


def count(entries):
    return len(re.findall(r"^\s*\{0x", entries, re.M))


parts = [
    '#include "src/algorithms/diagnostics/dtc_tables.h"\n',
    "\nnamespace\n{\nusing Table = std::unordered_map<int, std::string>;\n} // namespace\n",
]
for member, fn in ACCESSORS:
    entries = body(member)
    parts.append(
        f"\n// {count(entries)} entries, transcribed verbatim from the former "
        f"FileActions::{member}.\n"
        f"const std::unordered_map<int, std::string>& {fn}()\n{{\n"
        f"    static const Table table{{\n{entries}    }};\n"
        f"    return table;\n}}\n"
    )

with open(OUT, "w", encoding="utf-8") as f:
    f.write("".join(parts))
print(f"wrote {OUT}")
for member, fn in ACCESSORS:
    print(f"  {fn}(): {count(body(member))} entries")
