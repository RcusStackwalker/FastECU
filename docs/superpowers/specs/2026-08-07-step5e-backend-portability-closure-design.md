# Step 5e — Backend Portability Closure — Design

**Status:** Approved 2026-08-07. Fifth and final backend sub-project of step 5
(see the [step-5 umbrella design](2026-07-22-step5-backend-portable-design.md),
whose goal statement this document discharges). Depends on 5c (merged, PR #79)
and 5d (complete: 5d-5 merged as PR #153, 5d-5b as PR #154).

**Goal:** make the umbrella's headline claim — "`src/backend` becomes portable"
— provable rather than asserted, and remove the last backend dependencies on
`src/platform` and on Qt outside the transitional legacy adapters.

## Completion criterion

Exact and machine-checked:

- `scripts/check-serial-compat-allowlist.py`'s `FROZEN` set contains **zero**
  `//src/backend/...` entries.
- `//src/backend/flash` and `//src/backend/checksum` carry no `QT_DEPS`.

When both hold, backend has no dependency on `src/platform` and no Qt in any
non-adapter target. The remaining Qt in backend is confined to the
`Legacy*Adapter` targets and `src/backend/definitions`, all of which step 6
deletes by construction.

## What 5e is not

5e migrates **none** of the 27 legacy flash families. They live in
`src/platform/desktop/common/flash/legacy/`, not in backend, so backend
portability never depended on them. They are the umbrella's `tail` row, which
5e unblocks and — by moving the allowlist entry onto the package that actually
owns the debt — makes measurable for the first time.

## Findings that shaped the scope

Two facts found during design materially changed the slice.

- **`//src/backend/flash` is a visibility laundering channel, not just a code
  location.** All 27 legacy operation classes `#include "serial_port_actions.h"`
  directly, for far more than `configureIso15765Can`. They reach it
  *transitively through* `//src/backend/flash:flash`, and
  `src/platform/desktop/common/flash/legacy/BUILD.bazel` states this is
  deliberate: the package "does NOT depend on
  `//src/platform/desktop/common/serial:serial_qt_compat` directly -- that
  target's visibility is a frozen allowlist ... that Task 15 must not grow."
  Deleting the backend entry therefore strands the whole legacy flash package
  unless the allowlist is amended. See [The allowlist amendment](#the-allowlist-amendment).

- **`//src/backend/logging/protocols:__pkg__` is a stale allowlist entry.**
  That package's `BUILD.bazel` has no `serial_qt_compat` dependency and no
  `QT_DEPS`; it is already registered in `PORTABLE_ROOTS`. Its allowlist entry
  can be removed with no code change at all.

A third finding shrank a component rather than the slice: `//src/backend/checksum:flash_device_lookup`
is already a portable `string_view` reimplementation of
`FlashUtils::findFlashDeviceIndex`, and its own header admits it was cloned
rather than shared — "scoped to this package rather than changing that function
or its other Qt-linked callers." 5e collapses the two.

## Work items

Three independently landable items.

| ID | Item | Removes |
|----|------|---------|
| **5e-3** | Stale allowlist entry | `//src/backend/logging/protocols:__pkg__` |
| **5e-1** | `flash_utils` decomposition | `//src/backend/flash:__pkg__` from the allowlist; the `//src/backend/flash:flash` Qt target entirely |
| **5e-2** | Checksum dialogs hoisted to the UI | `QT_DEPS` from `//src/backend/checksum` |

**5e-3 lands first, alone.** It is a two-line change with no code behind it, and
landing it standalone exercises the freeze-test behavior before 5e-1 depends on
the same file. 5e-1 and 5e-2 are independent of each other.

## 5e-1: `flash_utils` decomposition

`src/backend/flash/flash_utils.{h,cpp}` holds four unrelated functions behind
one Qt target. Each goes to a different home, and the target dies.

| Function | Disposition |
|---|---|
| `configureIso15765Can(SerialPortActions*, ...)` | **Moves to platform** verbatim, into a new `src/platform/desktop/common/flash/legacy/legacy_flash_utils.{h,cpp}`. Its 16 call sites are all already in that package. |
| `findFlashDeviceIndex(const QString&)` | **Qt shim in the same new platform file**, forwarding to the portable index-returning lookup. Its 30 call sites change nothing but their `#include`. |
| `findFlashDevice(const QString&)` | **Deleted.** No callers outside `flash_utils.cpp` itself. |
| `buildIso15765Request(quint32, QByteArray)` | **Inlined at its single call site and deleted** (`flash_ecu_mitsu_m32r_can_operation.cpp:59`). Three lines existing only as a Qt-typed helper; it does not earn a home. |
| `cks_add8(std::span)` | **Moves to `//src/algorithms/checksum`.** Already portable; a checksum primitive sitting in the wrong layer. Its 2 callers are in `ui/desktop/ecu_operations.cpp`, and `ui -> algorithms` is a permitted direction. |

### Keeping the Qt signature (deliberate)

`findFlashDeviceIndex` keeps its `const QString&` signature behind the platform
shim rather than converting the 30 call sites to `std::string_view`. Both
options touch the same 30 files — the `#include` changes either way — so the
difference is only whether 5e also edits a live expression in each.

The 27 legacy classes are the least-covered, highest-consequence code in the
tree, with no bench access to verify against. 5e's job is backend cleanliness,
and an include-only diff there is reviewable at a glance. The shim dies
family-by-family in the tail, which rewrites those files anyway.

The index-returning form must survive conversion: call sites do not merely want
a pointer, they index the global table throughout each file
(`flashdevices[mcu_type_index].fblocks[blockno].len` and similar).

### De-duplicating the lookup

The portable lookup **moves from `//src/backend/checksum:flash_device_lookup`
to `//src/backend/flash:flash_device_lookup`** and gains the index-returning
form the flash callers need. `//src/backend/checksum:dispatch` re-points at it.

The table is flash-device geometry (`flashdevices[]`, `flashblock`, `romsize`),
consumed by 30 flash operations and one checksum dispatcher; `backend/checksum`
is the wrong owner. The alternative — folding it into
`//src/backend/definitions:models` alongside the table itself — is defensible,
but `models` is a header-only data split inside the legacy glob package, and
giving it a `.cpp` reopens exactly the glob-collision hazard the
[5d umbrella](2026-07-24-step5d-fileactions-decomposition-design.md) wrote its
new-package rule against.

**Verify at plan time** that no `backend/checksum -> backend/flash` cycle
results. Today `//src/backend/flash`'s portable targets have no checksum
dependency, so it should be clean, but this is checked, not assumed.

### The allowlist amendment

Removing `//src/backend/flash:__pkg__` requires adding
`//src/platform/desktop/common/flash/legacy:__pkg__`, because of the laundering
channel described under [Findings](#findings-that-shaped-the-scope). The
check script's own comment calls a new entry "a design failure, not a paperwork
step," so the justification is recorded here explicitly and adjudicated:

- It converts a **backend -> platform** edge — a real layer inversion, since
  `platform -> backend` is the permitted direction and never the reverse — into
  a **platform -> platform** sibling edge, which the layering rules do not
  prohibit at all.
- Across 5e the entry count falls from 14 to 13 (5e-3 removes one, 5e-1 swaps
  one for one); backend violations go 2 -> 0.
- The new entry is **genuine debt**, not a `remote_utility`-style carve-out. It
  carries a `TRANSITIONAL` comment naming the tail as its owner and shrinks as
  families migrate.

`FROZEN` and the `visibility` list must be edited in the same commit, and the
check's failure message re-read afterward to confirm it still names the right
entries.

### Result

`qt_cc_library(name = "flash")` is deleted. `//src/backend/flash` becomes a
fully portable package — `flash_types`, `flash_plan`, `flash_validation`,
`flash_executor`, `flash_device_lookup` — and its `QT_DEPS`,
`//src/algorithms/protocol:qt_compat`, and `serial_qt_compat` dependencies go
with it.

Consumers of the deleted `flash` label that must be re-pointed:
`//src/backend/definitions`, `//src/ui/desktop`, `//src/ui/desktop/flash/ecu`,
`//src/backend/flash:test_flash_utils`, and
`//src/platform/desktop/common/flash/legacy`. (The `flash` target's own comment
names this last consumer as "tests/test_flash_utils"; that spelling is stale —
the target is co-located in `src/backend/flash`, and no `tests/` target of that
name exists.)

## 5e-2: checksum dialogs hoisted to the UI

`apply_checksum_correction` is already portable and already returns a typed
`ChecksumCorrectionOutcome`. `LegacyChecksumAdapter` is nothing but dialog glue
and sequencing around it, and its only caller is
`FileActions::checksum_correction`.

**Deleted.** `src/backend/checksum/legacy_checksum_adapter.{h,cpp}`, its
`qt_cc_library` target, `FileActions::checksum_correction`
(`file_actions.cpp:1673`, `file_actions.h:212`), and the `checksumAdapter_`
member.

**New.** Package `src/ui/desktop/checksum/`, with **explicit `srcs`** rather
than co-location in `src/ui/desktop` — that package's `glob(["*.cpp"])` has no
`*_test.cpp` exclusion, the live hazard named in
[tech-debt](../../tech-debt.md) P2. It holds a `ChecksumCorrectionCommand`
owning the whole sequence:

```
build ChecksumSelection from ConfigValuesStruct + ecuCalDef
find_flash_device precheck            -> log error, return
if no definition linked               -> confirmProceedWithoutDefinition()
apply_checksum_correction(...)        -> portable, no dialogs
switch on outcome.status              -> the four dialogs
if corrected                          -> write bytes back to ecuCalDef->FullRomData
```

The four dialog methods stay `virtual` exactly as they are today, so the
existing 204-line `legacy_checksum_adapter_test.cpp` and its
`TestableChecksumAdapter` subclass convert near-mechanically into the new
package as a `fastecu_gtest`.

**Call sites.** Seven live occurrences of
`ecuCalDef[rom_number] = fileActions->checksum_correction(ecuCalDef[rom_number]);`
in `mainwindow.cpp` — lines 1144, 1640, 1643, 1645, 1695, 1698, 1700.
`menu_actions.cpp:1545` is already commented out and is deleted rather than
ported, per the 5d umbrella's "dead code is dropped, not ported" rule.

### Fidelity points

- **Log lines.** The method emits four `LOG_D` lines and one `LOG_E` through
  `FileActions`' signals. Moving it to UI must re-route these to `MainWindow`'s
  logging path with identical text, not silently drop them.
- **Recovered test coverage.** `file_actions_parsing_test.cpp:1283-1329` drives
  `actions.checksum_correction(&ecu)` in two tests — unknown MCU returns the
  ROM unmodified; valid MCU writes corrected bytes back. That coverage moves
  into the new package's test rather than being deleted with the method.

### Scope note, deliberate

This crosses the 5d umbrella's rule that "`FileActions`'s public method
signatures do not change ... so `MainWindow` call sites are untouched until step
6." It is an adjudicated exception, in the same register as 5d-5b's
consumer-conversion exception, and is recorded as an
[amendment](#amendments-to-the-5d-umbrella) below.

The alternative considered and rejected was an `IChecksumPrompt` port in
`src/backend/ports` with a `QtChecksumPrompt` implementation injected into
`FileActions` — which the existing four-port constructor injection makes easy.
It reaches the same portability result, but it invents an interface whose only
purpose is to survive until step 6 deletes it. Hoisting once is cheaper than
building a seam and then removing it.

## Enforcement

- **`PORTABLE_ROOTS`** in `scripts/check-portable-closure.py`:
  `src/backend/checksum` loses `flash_device_lookup`; `src/backend/flash` gains
  it. Both packages' other entries are unchanged.
- **`FROZEN`** in `scripts/check-serial-compat-allowlist.py`: drop
  `//src/backend/flash:__pkg__` and `//src/backend/logging/protocols:__pkg__`;
  add `//src/platform/desktop/common/flash/legacy:__pkg__`.
- Each change verified **non-vacuous** per the umbrella rule — confirmed to fail
  when the target is absent as well as when it is non-conforming, as was done
  for `//:bazel_openssl_wiring`.

## Testing

No golden-vector changes are expected anywhere in 5e: every behavior is a move
or a delete. But the assertions do not move as a block, and the plan must not
assume they do.

**`flash_utils_test.cpp` splits four ways.** It is a single 217-line QtTest
(`QTEST_GUILESS_MAIN(TestFlashUtils)`) whose 10 cases span all four functions,
so it follows them apart and changes framework twice:

| Cases | Destination |
|---|---|
| `findFlashDeviceIndex_returnsKnownDevice`, `findFlashDeviceIndex_returnsMinusOneForUnknownDevice`, `flashDeviceTable_matchesExpectedSummariesAndNamedAnomalies` | Merge into `//src/backend/flash:flash_device_lookup_test`, alongside the existing `flash_device_lookup_test.cpp` moving from `backend/checksum`. Portable gtest. |
| `cksAdd8_returnsZeroForEmptyData`, `cksAdd8_sumsBytesWithoutCarry`, `cksAdd8_addsOneOnCarry`, `cksAdd8_matchesReflashBlockShape` | Follow `cks_add8` to `//src/algorithms/checksum`. Portable gtest. |
| `configureIso15765Can_setsSharedCanTransportState`, `configureIso15765Can_defaultsTo11BitCanIds` | Follow the function to the platform legacy package. Stays a QtTest; needs `//src/platform/desktop/common/serial/testing:fake_serial_backend`. |
| `buildIso15765Request_prependsBigEndianSourceAddress` | The function is inlined away, but the assertion is real. It moves into the existing `flash_ecu_mitsu_m32r_can_operation_test.cpp` in the legacy package, covering the inlined expression at its one call site rather than being retired. |

`//src/backend/flash:test_flash_utils` is then deleted along with the target it
tests.

**The checksum command test** is the converted 204-line
`legacy_checksum_adapter_test.cpp` plus the two recovered `FileActions` cases
named under [Fidelity points](#fidelity-points).

The two `flash_device_lookup_test.cpp` suites being merged were written against
the same table from opposite sides — one `QString`-typed, one `string_view` —
so the merge must keep both sets of assertions, not adopt whichever file is
larger.

## Gates

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

Plus `>=80%` new-code coverage and the SonarCloud Quality Gate, as applied to
every slice since 5c. Coverage is budgeted during execution, not treated as a
final gate check.

## Risks

| Risk | Mitigation |
|---|---|
| Removing the `logging/protocols` entry breaks a build the grep missed | 5e-3 lands first and alone; the build proves it |
| The allowlist swap reads as "growing the list" to a future reader | The amendment rationale goes into the script's own comment, not only into this document |
| Log lines silently lost when `checksum_correction` moves to UI | Named as a fidelity point above; the plan asserts the emitted text, not just the flow |
| The 30-site `#include` change collides with tail work | 5e lands before any tail family; an include-only diff rebases trivially |
| `backend/checksum -> backend/flash` introduces a dependency cycle | Verified at plan time, before the lookup moves |
| A "dead" function turns out to be live through a path grep misses | `findFlashDevice` and `buildIso15765Request` removals are each backed by a reference check re-verified against a built binary, per the 5d-4b lesson |

## Amendments to the [5d umbrella](2026-07-24-step5d-fileactions-decomposition-design.md)

1. **`FileActions::checksum_correction` is deleted and its logic moves to
   `src/ui/desktop/checksum/`**, changing seven `MainWindow` call sites. This
   is an exception to the umbrella's "`FileActions`'s public method signatures
   do not change" rule, taken in preference to building an `IChecksumPrompt`
   port that step 6 would delete.
2. **The `serial_qt_compat` allowlist gains an entry.** The umbrella and the
   check script both treat a new entry as a design failure. Here it is a swap
   that converts a backend -> platform layer inversion into a platform ->
   platform sibling edge, reducing backend violations from 2 to 0 while the
   total falls from 14 entries to 13.

## Amendments to the [modularization plan](../../modularization-plan.md)

Step 5's sub-project list gains **5e**, after the 5a-5d series and before the
`tail`. The plan's step-5 bullet "Remove direct `QMessageBox`, `QFileDialog`,
widget, filesystem, and `SerialPortActions` access from backend code" is
discharged by 5e for `QMessageBox` and `SerialPortActions`; `QFileDialog` and
widget access remain in `src/backend/definitions` and are step-6 work.

## Follow-ups

To be filed as issues when 5e merges, not carried inside it:

- **Retire the `findFlashDeviceIndex` Qt shim.** It exists only to keep the 30
  legacy call sites on `QString`. Each tail family that migrates should convert
  its own sites to the portable `std::string_view` form; the shim is deleted
  when the last one does.
- **Drain `//src/platform/desktop/common/flash/legacy:__pkg__` from the
  allowlist.** This is the tail's completion criterion, now attributed to the
  package that owns it.
