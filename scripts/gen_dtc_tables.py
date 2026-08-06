#!/usr/bin/env python3
"""Transcribe error_codes.h's QHash tables into portable std::unordered_map
accessors. Entry lines are copied byte-for-byte; only the surrounding
declaration changes.

Run from the repository root, before error_codes.h is deleted in Task 5:
    python3 scripts/gen_dtc_tables.py
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
