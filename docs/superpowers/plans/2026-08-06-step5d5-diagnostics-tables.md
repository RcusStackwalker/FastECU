# Step 5d-5 Diagnostics Table Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the five NRC/DTC lookup tables from `QHash` statics on the `FileActions` god object to portable `std::unordered_map` constants beside the parsers, delete the transitional Qt shim, and fix the category-bit lookup bug that has made 53% of the DTC descriptions unreachable.

**Architecture:** The portable `nrc_description()` / `dtc_description()` free functions in `//src/algorithms/diagnostics` already take their lookup tables as parameters — that parameterization exists only because the tables were stranded in Qt-typed statics in backend. This plan adds the tables as portable constants in the same package, adds a one-line overload on each parser that binds them, converts the twelve call sites to call the portable functions directly, and then deletes the shim, the `error_codes.h` header, and the `FileActions::parse_*` wrappers.

**Tech Stack:** C++23, Bazel 9.1.1, GoogleTest, Qt 6.8.3 (only at the call sites being converted).

## Global Constraints

- Every header needs `#pragma once` (enforced by prek).
- Backend/algorithms code uses `bytes::Byte` / `bytes::Bytes` / `bytes::ByteView` from `src/algorithms/protocol/bytes.h`; `QByteArray` is a boundary type only, converted explicitly via `src/algorithms/protocol/qt_bytes.h` (ADR 0004).
- Prefer `std::string_view` by value over `const char*` / `const std::string&` (ADR 0009).
- `//src/algorithms/diagnostics` is a **portable** package: no Qt, no threads, no filesystem. `//:portable_closure` enforces this and will fail the build if a Qt dep appears in its closure.
- The `:diagnostics` `cc_library` uses `glob(["*.cpp"], exclude = ["*_test.cpp", "qt_*.cpp"])` for `srcs` and the matching glob for `hdrs`. **New non-test, non-`qt_`-prefixed files in this package are picked up automatically — do not add them to `srcs`/`hdrs` by hand.**
- Commit directly to the working branch; do not open PRs unless asked.
- Full gate before the final commit:
  ```
  bazel build -k --config=release //:fastecu //tests/...
  bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
              //:serial_compat_allowlist //:portable_closure
  ```

## Reference: the bug being fixed in Task 6

All four DTC tables are keyed by the **14-bit code with the category bits masked off** (verified: max key across all four tables is `0x3000`; none reaches `0x4000`). But `dtc_description` looks the **full** value up, including the two category bits it just used to select the table:

| Category | `dtc >> 14` | Lookup key used | Table key | Entries | Reachable today |
|---|---|---|---|---|---|
| P | 0 | `dtc` | `code` | 1,733 | yes (`dtc == code`) |
| C | 1 | `0x4000\|code` | `code` | 487 | **no** |
| B | 2 | `0x8000\|code` | `code` | 1,147 | **no** |
| U | 3 | `0xC000\|code` | `code` | 299 | **no** |

The existing tests pass because they use synthetic tables keyed by the *full* value (`diagnostics_test.cpp:37` uses `{0x4001, "C0001 - Test chassis code"}`), the opposite convention from every real table. Task 6 fixes the lookup and corrects the synthetic tables so they stop encoding the wrong convention.

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `src/algorithms/diagnostics/dtc_tables.h` | Declares five accessors returning `const std::unordered_map<int, std::string>&`. Nothing else. |
| `src/algorithms/diagnostics/dtc_tables.cpp` | ~3,770 lines of generated table data. Machine-transcribed, never hand-edited. |
| `src/algorithms/diagnostics/dtc_tables_test.cpp` | Golden test: entry counts and sampled entries, proving the transcription is lossless. |
| `scripts/gen_dtc_tables.py` | The transcription script, kept so the move is reproducible and reviewable. |

**Modified:**

| File | Change |
|---|---|
| `src/algorithms/diagnostics/nrc_parser.h` / `.cpp` | Add a `nrc_description(bytes::ByteView)` overload binding the real table. |
| `src/algorithms/diagnostics/dtc_parser.h` / `.cpp` | Add a `dtc_description(std::uint16_t)` overload; Task 6 fixes the lookup mask. |
| `src/algorithms/diagnostics/diagnostics_test.cpp` | Task 6 corrects the synthetic tables' key convention. |
| `src/algorithms/diagnostics/BUILD.bazel` | Add the `dtc_tables_test` target; delete the `qt_compat` target and its test. |
| `src/ui/desktop/dtc_operations.cpp` | 6 call sites. |
| `src/ui/desktop/BUILD.bazel` | Add `//src/algorithms/diagnostics` to `:desktop` deps. |
| `src/platform/desktop/common/flash/legacy/tcu/flash_tcu_subaru_hitachi_m32r_can_operation.cpp` | 6 call sites; add two includes. |
| `src/platform/desktop/common/flash/legacy/BUILD.bazel` | Add `//src/algorithms/diagnostics` to `:legacy_flash_operations` deps. |
| `src/backend/definitions/file_actions.h` / `.cpp` | Delete the five `QHash` declarations and the two `parse_*` wrappers. |
| `src/backend/definitions/BUILD.bazel` | Drop the `//src/algorithms/diagnostics:qt_compat` dep. |

**Deleted:** `src/algorithms/diagnostics/qt_dtc_parser.{h,cpp}`, `qt_nrc_parser.{h,cpp}`, `diagnostic_parsers_qt_compat_test.cpp`, `src/backend/definitions/error_codes.h`.

---

### Task 1: Portable diagnostic tables

**Files:**
- Create: `scripts/gen_dtc_tables.py`
- Create: `src/algorithms/diagnostics/dtc_tables.h`
- Create: `src/algorithms/diagnostics/dtc_tables.cpp` (generated)
- Test: `src/algorithms/diagnostics/dtc_tables_test.cpp`
- Modify: `src/algorithms/diagnostics/BUILD.bazel`

**Interfaces:**
- Consumes: nothing.
- Produces: five free functions declared in `dtc_tables.h`, each returning `const std::unordered_map<int, std::string>&` — `nrc_codes()`, `dtc_p_codes()`, `dtc_b_codes()`, `dtc_c_codes()`, `dtc_u_codes()`. Task 2 calls all five.

- [ ] **Step 1: Write the header**

Create `src/algorithms/diagnostics/dtc_tables.h`:

```cpp
#pragma once

#include <string>
#include <unordered_map>

// Diagnostic lookup tables, formerly the FileActions::neg_rsp_codes /
// dtc_[PBCU]xxxx_codes QHash statics in src/backend/definitions/error_codes.h.
//
// The DTC tables are keyed by the 14-bit code with the two category bits
// masked off -- the category selects which table to consult, so it is not
// part of the key. nrc_codes() is keyed by the raw NRC byte.
//
// Generated data lives in dtc_tables.cpp; regenerate with
// scripts/gen_dtc_tables.py rather than editing it by hand.
const std::unordered_map<int, std::string>& nrc_codes();
const std::unordered_map<int, std::string>& dtc_p_codes();
const std::unordered_map<int, std::string>& dtc_b_codes();
const std::unordered_map<int, std::string>& dtc_c_codes();
const std::unordered_map<int, std::string>& dtc_u_codes();
```

- [ ] **Step 2: Write the transcription script**

The `QHash<int, QString>` and `std::unordered_map<int, std::string>` initializer-list entry syntax is identical, so entry lines are copied byte-for-byte and only the surrounding declaration changes. Create `scripts/gen_dtc_tables.py`:

```python
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

text = open(SRC, encoding="utf-8").read()


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

open(OUT, "w", encoding="utf-8").write("".join(parts))
print(f"wrote {OUT}")
for member, fn in ACCESSORS:
    print(f"  {fn}(): {count(body(member))} entries")
```

- [ ] **Step 3: Run the script**

Run: `python3 scripts/gen_dtc_tables.py`

Expected output, exactly:

```
wrote src/algorithms/diagnostics/dtc_tables.cpp
  nrc_codes(): 59 entries
  dtc_p_codes(): 1733 entries
  dtc_b_codes(): 1147 entries
  dtc_c_codes(): 487 entries
  dtc_u_codes(): 299 entries
```

If any count differs, stop — `error_codes.h` is not what this plan was written against.

- [ ] **Step 4: Write the failing golden test**

The point of this test is that the transcription is lossless. Counts catch a truncated table; sampled entries catch a mangled one; the first/last key of each table catches an off-by-one at a block boundary.

Create `src/algorithms/diagnostics/dtc_tables_test.cpp`:

```cpp
#include "src/algorithms/diagnostics/dtc_tables.h"

#include <gtest/gtest.h>

#include <ios>

TEST(DiagnosticTables, EntryCountsMatchTheLegacyQHashTables)
{
    EXPECT_EQ(nrc_codes().size(), 59u);
    EXPECT_EQ(dtc_p_codes().size(), 1733u);
    EXPECT_EQ(dtc_b_codes().size(), 1147u);
    EXPECT_EQ(dtc_c_codes().size(), 487u);
    EXPECT_EQ(dtc_u_codes().size(), 299u);
}

TEST(DiagnosticTables, SampledNrcEntriesSurvivedTranscription)
{
    EXPECT_EQ(nrc_codes().at(0x10), "General reject");
    EXPECT_EQ(nrc_codes().at(0x11), "Service not supported");
    EXPECT_EQ(nrc_codes().at(0x33), "Security access denied");
    EXPECT_EQ(nrc_codes().at(0x94), "Resource temporary unavailable");
}

TEST(DiagnosticTables, FirstAndLastEntryOfEachDtcTableSurvived)
{
    EXPECT_EQ(dtc_p_codes().at(0x0000), "P0000 - No trouble code");
    EXPECT_EQ(dtc_b_codes().at(0x1200),
              "B1200 - Climate Control Pushbutton Circuit Failure");
    EXPECT_EQ(dtc_c_codes().at(0x1091),
              "C1091 - Speed Wheel Sensor All Coherency Failure");
    EXPECT_EQ(dtc_u_codes().at(0x1000),
              "U1000 - SCP (J1850) Invalid or Missing Data for Primary Id");
    EXPECT_EQ(dtc_u_codes().at(0x2500),
              "U2500 - (CAN) Lack of Acknowledgement From Engine Management");
}

// Every key is below 0x4000, i.e. the category bits are masked off. This is
// the invariant dtc_description's lookup depends on; if a future table edit
// breaks it, the mask in dtc_parser.cpp silently starts mismatching.
TEST(DiagnosticTables, DtcKeysCarryNoCategoryBits)
{
    for (const auto *table : {&dtc_p_codes(), &dtc_b_codes(), &dtc_c_codes(), &dtc_u_codes()})
    {
        for (const auto& entry : *table)
        {
            EXPECT_LT(entry.first, 0x4000)
                << "key 0x" << std::hex << entry.first << " has category bits set";
        }
    }
}
```

- [ ] **Step 5: Register the test target**

In `src/algorithms/diagnostics/BUILD.bazel`, after the existing `diagnostics_test` target, add:

```python
cc_test(
    name = "dtc_tables_test",
    size = "small",
    srcs = ["dtc_tables_test.cpp"],
    deps = [
        ":diagnostics",
        "@googletest//:gtest_main",
    ],
)
```

Do **not** add `dtc_tables.cpp`/`.h` to the `:diagnostics` target — its globs pick them up automatically.

- [ ] **Step 6: Run the test**

Run: `bazel test --config=release //src/algorithms/diagnostics:dtc_tables_test`
Expected: PASS. (The tables were generated in Step 3, so this passes on first run — the golden test is a transcription check, not a TDD driver. If it fails, the transcription is wrong; re-run Step 3 and diff.)

- [ ] **Step 7: Verify the portable closure still holds**

Run: `bazel test --config=release //:portable_closure`
Expected: PASS. `dtc_tables.cpp` pulls in only `<string>` and `<unordered_map>`.

- [ ] **Step 8: Commit**

```bash
git add scripts/gen_dtc_tables.py src/algorithms/diagnostics/dtc_tables.h \
        src/algorithms/diagnostics/dtc_tables.cpp \
        src/algorithms/diagnostics/dtc_tables_test.cpp \
        src/algorithms/diagnostics/BUILD.bazel
git commit -m "refactor: add portable NRC/DTC lookup tables

Transcribes the five QHash statics from error_codes.h into portable
std::unordered_map accessors beside the parsers that consume them.
Entry lines are copied byte-for-byte by scripts/gen_dtc_tables.py; the
golden test pins entry counts, sampled entries, and the first/last key
of each table.

No caller changes yet -- FileActions still owns the QHash tables."
```

---

### Task 2: Default-table overloads

**Files:**
- Modify: `src/algorithms/diagnostics/nrc_parser.h`, `src/algorithms/diagnostics/nrc_parser.cpp`
- Modify: `src/algorithms/diagnostics/dtc_parser.h`, `src/algorithms/diagnostics/dtc_parser.cpp`
- Test: `src/algorithms/diagnostics/dtc_tables_test.cpp` (extend)

**Interfaces:**
- Consumes: `nrc_codes()`, `dtc_p_codes()`, `dtc_b_codes()`, `dtc_c_codes()`, `dtc_u_codes()` from Task 1.
- Produces: `std::string nrc_description(bytes::ByteView)` and `std::string dtc_description(std::uint16_t)`. Tasks 3 and 4 call these. The existing parameterized overloads are unchanged and stay public.

- [ ] **Step 1: Write the failing test**

Append to `src/algorithms/diagnostics/dtc_tables_test.cpp`:

```cpp
#include "src/algorithms/diagnostics/dtc_parser.h"
#include "src/algorithms/diagnostics/nrc_parser.h"

TEST(DiagnosticDefaults, NrcOverloadUsesTheRealTable)
{
    const bytes::Bytes frame{0x7f, 0x22, 0x33};
    EXPECT_EQ(nrc_description(frame), "Security access denied");
}

TEST(DiagnosticDefaults, NrcOverloadKeepsNonNegativeResponseHandling)
{
    const bytes::Bytes not_negative{0x62, 0x00, 0x01};
    EXPECT_EQ(nrc_description(not_negative), "Not a valid answer");
}

TEST(DiagnosticDefaults, DtcOverloadUsesTheRealPowertrainTable)
{
    EXPECT_EQ(dtc_description(0x0000), "P0000 - No trouble code");
}
```

Put the two new `#include` lines with the existing one at the top of the file, not mid-file.

- [ ] **Step 2: Run test to verify it fails**

Run: `bazel test --config=release //src/algorithms/diagnostics:dtc_tables_test`
Expected: FAIL — compile error, "no matching function for call to `nrc_description`" (only the two-argument form exists).

- [ ] **Step 3: Add the NRC overload**

In `src/algorithms/diagnostics/nrc_parser.h`, after the existing declaration:

```cpp
// Same decode against the standard NRC table in dtc_tables.h. Prefer this
// over the table-taking overload; that one exists so tests can exercise the
// frame handling against a synthetic table.
std::string nrc_description(bytes::ByteView nrc);
```

In `src/algorithms/diagnostics/nrc_parser.cpp`, add the include and the definition:

```cpp
#include "src/algorithms/diagnostics/nrc_parser.h"

#include "src/algorithms/diagnostics/dtc_tables.h"

// ... existing nrc_description(ByteView, const std::unordered_map<...>&) ...

std::string nrc_description(bytes::ByteView nrc)
{
    return nrc_description(nrc, nrc_codes());
}
```

- [ ] **Step 4: Add the DTC overload**

In `src/algorithms/diagnostics/dtc_parser.h`, after the existing declaration:

```cpp
// Same decode against the standard P/C/B/U tables in dtc_tables.h. Prefer
// this over the table-taking overload; that one exists so tests can exercise
// the category selection against synthetic tables.
std::string dtc_description(std::uint16_t dtc);
```

In `src/algorithms/diagnostics/dtc_parser.cpp`, add the include and the definition after the existing function:

```cpp
#include "src/algorithms/diagnostics/dtc_tables.h"
```

```cpp
std::string dtc_description(std::uint16_t dtc)
{
    return dtc_description(dtc, dtc_p_codes(), dtc_c_codes(), dtc_b_codes(), dtc_u_codes());
}
```

Note the argument order: the parameterized function's signature is `(dtc, pCodes, cCodes, bCodes, uCodes)` — **C before B**, which is not the order the tables are declared in `dtc_tables.h`. Getting this wrong swaps chassis and body descriptions, and the tests in Task 6 are what catch it.

- [ ] **Step 5: Run test to verify it passes**

Run: `bazel test --config=release //src/algorithms/diagnostics:dtc_tables_test //src/algorithms/diagnostics:diagnostics_test`
Expected: PASS, both targets. The pre-existing `diagnostics_test` must still pass — the parameterized overloads were not touched.

- [ ] **Step 6: Commit**

```bash
git add src/algorithms/diagnostics/
git commit -m "refactor: add default-table overloads to the NRC/DTC parsers

nrc_description(ByteView) and dtc_description(uint16_t) bind the real
tables so callers no longer have to supply them. The table-taking
overloads stay -- synthetic tables are what make the frame handling and
category selection cheap to assert without coupling to table contents."
```

---

### Task 3: Convert the DTC dialog call sites

**Files:**
- Modify: `src/ui/desktop/dtc_operations.cpp` (lines 312, 382, 533, 670 for NRC; 591, 617 for DTC)
- Modify: `src/ui/desktop/BUILD.bazel`

**Interfaces:**
- Consumes: `nrc_description(bytes::ByteView)` and `dtc_description(std::uint16_t)` from Task 2.
- Produces: nothing new. After this task `dtc_operations.cpp` no longer calls `FileActions::parse_nrc_message` / `parse_dtc_message`.

- [ ] **Step 1: Add the includes**

`src/ui/desktop/dtc_operations.cpp` already includes `src/algorithms/protocol/qt_bytes.h` (line 7). Add beside it:

```cpp
#include "src/algorithms/diagnostics/dtc_parser.h"
#include "src/algorithms/diagnostics/nrc_parser.h"
```

- [ ] **Step 2: Convert the four NRC sites**

Lines 312, 382, 533 and 670 are the same shape. At line 312 the argument is `received.mid(3, ...)`; at 382, 533 and 670 it is `received.mid(cmd_index, ...)`. Convert each to bind the temporary to a named local first — `received.mid(...)` returns a temporary `QByteArray`, and a named local makes the view's lifetime obvious on a hardware-facing path rather than relying on full-expression lifetime extension.

Line 312, before:

```cpp
            emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(3, received.length() - 1)), true, true);
```

after:

```cpp
            const QByteArray nrcFrame = received.mid(3, received.length() - 1);
            emit LOG_E("Wrong response from ECU: " +
                           QString::fromStdString(nrc_description(bytes::view(nrcFrame))),
                       true, true);
```

Lines 382, 533 and 670, before:

```cpp
                emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(cmd_index, received.length() - 1)), true, true);
```

after:

```cpp
                const QByteArray nrcFrame = received.mid(cmd_index, received.length() - 1);
                emit LOG_E("Wrong response from ECU: " +
                               QString::fromStdString(nrc_description(bytes::view(nrcFrame))),
                           true, true);
```

- [ ] **Step 3: Convert the two DTC sites**

Lines 591 and 617 are identical. Before:

```cpp
        emit LOG_I("DTC: " + FileActions::parse_dtc_message(dtc_list.at(i).toUInt(&ok, 16)), true, true);
```

after:

```cpp
        emit LOG_I("DTC: " + QString::fromStdString(
                                 dtc_description(static_cast<std::uint16_t>(dtc_list.at(i).toUInt(&ok, 16)))),
                   true, true);
```

The `static_cast` is required: `QString::toUInt` returns `uint`, and the overload takes `std::uint16_t`. Without it the narrowing is implicit and `-Wconversion` may reject it.

Leave the two commented-out `// emit LOG_I("DTC: " + FileActions::parse_dtc_message(dtc), true, true);` lines at 585 and 611 alone — Task 5 deletes them along with the method.

- [ ] **Step 4: Add the build dependency**

`dtc_operations.cpp` reached the parsers transitively through `//src/backend/definitions`. It now needs them directly. In `src/ui/desktop/BUILD.bazel`, in the `:desktop` target's `deps`, add `"//src/algorithms/diagnostics",` in alphabetical order — it sorts before the existing `"//src/algorithms/crypto:qt_compat",` at line 73.

- [ ] **Step 5: Build and verify**

Run: `bazel build --config=release //:fastecu`
Expected: success.

Run: `grep -n "parse_nrc_message\|parse_dtc_message" src/ui/desktop/dtc_operations.cpp`
Expected: only the two commented-out lines at 585 and 611.

- [ ] **Step 6: Commit**

```bash
git add src/ui/desktop/dtc_operations.cpp src/ui/desktop/BUILD.bazel
git commit -m "refactor: call the portable NRC/DTC parsers from the DTC dialog

Six sites in dtc_operations.cpp now call nrc_description/dtc_description
directly instead of routing through FileActions' QHash-backed wrappers.
No behavior change: the wrappers already delegated to these functions."
```

---

### Task 4: Convert the TCU flash call sites

**Files:**
- Modify: `src/platform/desktop/common/flash/legacy/tcu/flash_tcu_subaru_hitachi_m32r_can_operation.cpp` (lines 122, 163, 198, 225, 254, 293)
- Modify: `src/platform/desktop/common/flash/legacy/BUILD.bazel`

**Interfaces:**
- Consumes: `nrc_description(bytes::ByteView)` from Task 2.
- Produces: nothing new.

- [ ] **Step 1: Add the includes**

This file does **not** currently include `qt_bytes.h`. Add both includes after the existing `src/algorithms/protocol/ssm/ssm_protocol.h` include at line 3:

```cpp
#include "src/algorithms/diagnostics/nrc_parser.h"
#include "src/algorithms/protocol/qt_bytes.h"
```

- [ ] **Step 2: Convert all six sites**

All six are byte-identical. Before:

```cpp
            emit LOG_E("Wrong response from TCU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
```

after:

```cpp
            const QByteArray nrcFrame = received.mid(4, received.length() - 1);
            emit LOG_E("Wrong response from TCU: " +
                           QString::fromStdString(nrc_description(bytes::view(nrcFrame))),
                       true, true);
```

Apply at lines 122, 163, 198, 225, 254 and 293. Indentation differs between sites — match the surrounding block rather than copying the indentation above verbatim. Each site is inside its own braced block, so the repeated `nrcFrame` name does not collide.

- [ ] **Step 3: Add the build dependency**

In `src/platform/desktop/common/flash/legacy/BUILD.bazel`, in the `:legacy_flash_operations` target's `deps` list, add `"//src/algorithms/diagnostics",` before the existing `"//src/algorithms/protocol/colt:qt_compat",`.

- [ ] **Step 4: Build and verify**

Run: `bazel build --config=release //:fastecu`
Expected: success.

Run: `grep -rn "parse_nrc_message\|parse_dtc_message" src/ | grep -v "^src/backend/definitions/"`
Expected: only the two commented-out lines in `src/ui/desktop/dtc_operations.cpp`. Every live caller is now converted.

- [ ] **Step 5: Commit**

```bash
git add src/platform/desktop/common/flash/legacy/tcu/flash_tcu_subaru_hitachi_m32r_can_operation.cpp \
        src/platform/desktop/common/flash/legacy/BUILD.bazel
git commit -m "refactor: call the portable NRC parser from the TCU flash path

Six sites in flash_tcu_subaru_hitachi_m32r_can_operation.cpp. This was
the last live caller of FileActions::parse_nrc_message."
```

---

### Task 5: Delete the Qt shim and the FileActions tables

**Files:**
- Delete: `src/algorithms/diagnostics/qt_dtc_parser.h`, `qt_dtc_parser.cpp`, `qt_nrc_parser.h`, `qt_nrc_parser.cpp`, `diagnostic_parsers_qt_compat_test.cpp`
- Delete: `src/backend/definitions/error_codes.h`
- Modify: `src/algorithms/diagnostics/BUILD.bazel`, `src/backend/definitions/BUILD.bazel`
- Modify: `src/backend/definitions/file_actions.h` (lines 169-175, 296-297), `file_actions.cpp` (line 8, lines 2130-2140)
- Modify: `src/ui/desktop/dtc_operations.cpp` (lines 585, 611)

**Interfaces:**
- Consumes: nothing.
- Produces: nothing. This task is pure deletion.

- [ ] **Step 1: Delete the shim files and its test**

```bash
git rm src/algorithms/diagnostics/qt_dtc_parser.h \
       src/algorithms/diagnostics/qt_dtc_parser.cpp \
       src/algorithms/diagnostics/qt_nrc_parser.h \
       src/algorithms/diagnostics/qt_nrc_parser.cpp \
       src/algorithms/diagnostics/diagnostic_parsers_qt_compat_test.cpp
```

- [ ] **Step 2: Remove the shim's Bazel targets**

In `src/algorithms/diagnostics/BUILD.bazel`, delete the whole `cc_library(name = "qt_compat", ...)` block together with its `# TRANSITIONAL Qt shim:` comment, and the whole `fastecu_qttest(name = "test_diagnostic_parsers", ...)` block.

The `load(...)` line at the top now over-imports. Change:

```python
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "QT_DEPS", "fastecu_qttest")
```

to remove it entirely — no target in this file uses `COMMON_COPTS`, `QT_DEPS` or `fastecu_qttest` any more. Deleting the whole `load` line is correct; buildifier flags unused loads.

- [ ] **Step 3: Remove the backend dependency on the shim**

In `src/backend/definitions/BUILD.bazel`, delete the `"//src/algorithms/diagnostics:qt_compat",` entry at line 141 **and** the five-line comment above it that begins `# file_actions.cpp includes qt_dtc_parser.h/qt_nrc_parser.h directly`.

- [ ] **Step 4: Remove the FileActions members**

In `src/backend/definitions/file_actions.h`, delete lines 169-175 — the comment block and all five declarations:

```cpp
    /***********************************
     * Negative response codes (NRC)
     * ********************************/
    static const QHash<int, QString> neg_rsp_codes;   // Inited at error_codes.h
    static const QHash<int, QString> dtc_Pxxxx_codes; // Inited at error_codes.h
    static const QHash<int, QString> dtc_Bxxxx_codes; // Inited at error_codes.h
    static const QHash<int, QString> dtc_Cxxxx_codes; // Inited at error_codes.h
    static const QHash<int, QString> dtc_Uxxxx_codes; // Inited at error_codes.h
```

and the two declarations near line 296:

```cpp
    static QString parse_nrc_message(const QByteArray& nrc);
    static QString parse_dtc_message(uint16_t dtc);
```

In `src/backend/definitions/file_actions.cpp`, delete the `#include "src/backend/definitions/error_codes.h"` at line 8, and the two definitions at lines 2130-2140:

```cpp
QString FileActions::parse_nrc_message(const QByteArray& nrc)
{
    return NrcParser::parse(nrc, neg_rsp_codes);
}

QString FileActions::parse_dtc_message(uint16_t dtc)
{
    return DtcParser::parse(dtc, dtc_Pxxxx_codes, dtc_Cxxxx_codes,
                            dtc_Bxxxx_codes, dtc_Uxxxx_codes);
}
```

Also delete any now-unused `#include` of `qt_dtc_parser.h` / `qt_nrc_parser.h` in `file_actions.cpp`.

- [ ] **Step 5: Delete the data header**

```bash
git rm src/backend/definitions/error_codes.h
```

`scripts/gen_dtc_tables.py` will no longer run after this — that is expected and correct. It is kept in the tree as the reviewable record of how `dtc_tables.cpp` was produced, and its docstring already says it must run before this deletion.

- [ ] **Step 6: Delete the two dead commented-out call sites**

In `src/ui/desktop/dtc_operations.cpp`, delete line 585 and line 611, both of which read:

```cpp
            // emit LOG_I("DTC: " + FileActions::parse_dtc_message(dtc), true, true);
```

They reference a method that no longer exists.

- [ ] **Step 7: Verify nothing references the deleted symbols**

Run:

```bash
grep -rn "parse_nrc_message\|parse_dtc_message\|neg_rsp_codes\|dtc_Pxxxx_codes\|error_codes.h\|NrcParser\|DtcParser\|qt_compat.*diagnostics" src/ apps/ tests/
```

Expected: no output at all.

- [ ] **Step 8: Full build and test**

Run:

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

Expected: all pass.

Run: `prek run --all-files`
Expected: pass (buildifier confirms the `load` line removal in Step 2 was complete).

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "refactor: delete the NRC/DTC Qt shim and error_codes.h

Drains the last transitional shim in //src/algorithms/diagnostics. The
five QHash statics, the two FileActions wrappers, the 3,744-line
error_codes.h and the qt_compat target are all gone; every caller now
uses the portable functions directly.

Closes the 5d-5 diagnostics sub-slice."
```

---

### Task 6: Fix the DTC category-bit lookup

This is a **user-visible behavior change**, deliberately kept as its own commit so it can be reverted independently of the migration. See the reference table near the top of this plan for why 1,933 of the 3,666 DTC descriptions have never been reachable.

**Files:**
- Modify: `src/algorithms/diagnostics/dtc_parser.cpp` (line 45)
- Modify: `src/algorithms/diagnostics/diagnostics_test.cpp` (lines 36-39)
- Test: `src/algorithms/diagnostics/dtc_tables_test.cpp` (extend)

**Interfaces:**
- Consumes: `dtc_description(std::uint16_t)` from Task 2, the tables from Task 1.
- Produces: no signature change. Only the lookup key changes.

- [ ] **Step 1: Write the failing test**

Append to `src/algorithms/diagnostics/dtc_tables_test.cpp`:

```cpp
// Regression: the tables are keyed by the 14-bit code, but dtc_description
// used to look up the full value including the two category bits it had
// already consumed to pick the table. P codes worked by coincidence
// (category 0 leaves the value unchanged); C, B and U never matched, so
// 1,933 of the 3,666 descriptions in the tree were unreachable.
TEST(DiagnosticDefaults, NonPowertrainCategoriesResolveRealDescriptions)
{
    EXPECT_EQ(dtc_description(0x4000 | 0x1091),
              "C1091 - Speed Wheel Sensor All Coherency Failure");
    EXPECT_EQ(dtc_description(0x8000 | 0x1200),
              "B1200 - Climate Control Pushbutton Circuit Failure");
    EXPECT_EQ(dtc_description(0xc000 | 0x1000),
              "U1000 - SCP (J1850) Invalid or Missing Data for Primary Id");
}

// The category bits must still select the right table -- a mask alone would
// let a B code resolve against the C table.
TEST(DiagnosticDefaults, CategoryStillSelectsTheTable)
{
    // 0x1200 is a real B key and absent from the C table.
    EXPECT_EQ(dtc_description(0x4000 | 0x1200), "C1200 - Unknown error code");
}

TEST(DiagnosticDefaults, UnknownCodesStillFallBack)
{
    EXPECT_EQ(dtc_description(0x8000 | 0x3fff), "B3fff - Unknown error code");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bazel test --config=release //src/algorithms/diagnostics:dtc_tables_test`
Expected: FAIL. `NonPowertrainCategoriesResolveRealDescriptions` reports actual `"C1091 - Unknown error code"` against expected `"C1091 - Speed Wheel Sensor All Coherency Failure"`, and similarly for B and U. The other two new tests already pass.

- [ ] **Step 3: Fix the lookup**

In `src/algorithms/diagnostics/dtc_parser.cpp`, at line 45:

```cpp
    const auto it = table->find(dtc);
```

becomes:

```cpp
    // Tables are keyed by the 14-bit code, not the full value -- the top two
    // bits already selected which table to consult above.
    const auto it = table->find(dtc & 0x3fff);
```

- [ ] **Step 4: Correct the misleading synthetic tables**

`diagnostics_test.cpp` is why this was never caught: its synthetic tables use the *full*-value convention, the opposite of every real table. At lines 36-39, change:

```cpp
    const std::unordered_map<int, std::string> cCodes = {{0x4001, "C0001 - Test chassis code"}};

    EXPECT_EQ(dtc_description(0x4001, {}, cCodes, {}, {}), "C0001 - Test chassis code");
```

to:

```cpp
    // Keyed by the 14-bit code, matching the real tables in dtc_tables.h --
    // dtc_description masks the category bits off before the lookup.
    const std::unordered_map<int, std::string> cCodes = {{0x0001, "C0001 - Test chassis code"}};

    EXPECT_EQ(dtc_description(0x4001, {}, cCodes, {}, {}), "C0001 - Test chassis code");
```

The call argument stays `0x4001` — the caller still passes the full value; only the table's key convention changes.

- [ ] **Step 5: Run tests to verify they pass**

Run: `bazel test --config=release //src/algorithms/diagnostics:all`
Expected: PASS, both `diagnostics_test` and `dtc_tables_test`.

- [ ] **Step 6: Full gate**

Run:

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add src/algorithms/diagnostics/
git commit -m "fix: look DTCs up by the 14-bit code, not the full value

All four DTC tables are keyed by the code with the category bits masked
off -- the category selects which table to consult. dtc_description
looked up the full value instead, so only P codes (category 0, where the
two are equal) ever matched. C, B and U always fell through to
'Unknown error code', making 1,933 of the 3,666 descriptions in the tree
unreachable.

The existing tests missed this because their synthetic tables used the
full-value convention, the opposite of every real table; those are
corrected here too."
```

---

## Definition of done

- `grep -rn "parse_nrc_message\|parse_dtc_message\|neg_rsp_codes\|error_codes.h\|NrcParser\|DtcParser" src/ apps/ tests/` returns nothing.
- `//src/algorithms/diagnostics` has no `qt_compat` target and no Qt in its closure.
- `bazel test -k --config=release //... ` passes.
- `prek run --all-files` passes.
- A B/C/U trouble code read from a vehicle prints its real description rather than "Unknown error code".

## Follow-on

5d-5b (logger-definition glue) is the remaining sub-slice; it is specified in the [5d-5 design](../specs/2026-08-06-step5d5-diagnostics-and-logger-glue-design.md) and gets its own plan. It does not depend on anything in this plan.
