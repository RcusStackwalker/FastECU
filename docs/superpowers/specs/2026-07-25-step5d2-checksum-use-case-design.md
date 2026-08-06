# Step 5d-2 — Checksum Use Case — Design

**Status:** Approved 2026-07-25. Second sub-project of step 5d (see
[`2026-07-24-step5d-fileactions-decomposition-design.md`](2026-07-24-step5d-fileactions-decomposition-design.md),
which this design inherits its shared architecture decisions from —
new-package-per-slice, the snapshot-adapter pattern, port extension). Depends
on 5a (merged, PR #73) and 5d-1 (merged, PR #80).

**Goal:** replace `FileActions::checksum_correction` — the dispatcher that
picks one of 9 already-portable checksum-family algorithms based on the
selected protocol and applies it to the in-memory ROM image — with a portable
backend use case. Remove the last backend caller of
`//src/algorithms/checksum:qt_compat` (`qt_checksum.h`), which is explicitly
marked transitional pending this move, and delete that target.

---

## Scope

### In scope

1. `ChecksumSelection` value model: the scalar inputs `checksum_correction`
   currently reads off `ConfigValuesStructure`'s `flash_protocol_selected_*`
   fields and `EcuCalDefStructure`, converted to portable types.
2. `ChecksumCorrectionOutcome` value model and the pure function
   `apply_checksum_correction`, replacing the MCU/size lookup and the
   `flashMethod.startsWith(...)` dispatch chain in
   `checksum_correction` (`file_actions.cpp:2141-2367`).
3. A portable MCU/ROM-size lookup against the existing `flashdevices` table
   (`src/backend/definitions/kernelmemorymodels.h`, already Qt-free), scoped
   to this package — not a change to `FlashUtils::findFlashDeviceIndex` or
   its other (Qt-linked) callers.
4. `LegacyChecksumAdapter`, translating `ChecksumCorrectionOutcome` back into
   the exact `QMessageBox` dialogs and `EcuCalDefStructure::FullRomData`
   mutation `checksum_correction` performs today.
5. `FileActions::checksum_correction` reduced to: build a `ChecksumSelection`,
   emit the existing informational/error `LOG_D`/`LOG_E` signals, and
   delegate to the adapter. Signature unchanged.
6. Deleting `//src/algorithms/checksum:qt_compat`, `qt_checksum.h`, and the
   file-local helpers in `file_actions.cpp`'s anonymous namespace that exist
   only to support the Qt checksum path (`applyChecksumResult`,
   `applyDensoSh7xxxChecksum`, `showChecksumResult`,
   `lineAfterClosingTag`'s checksum-only use).
7. Portable-closure enforcement for the two new portable targets.

### Out of scope

- `FlashUtils::findFlashDeviceIndex` and its other (Qt-linked) callers —
  unchanged.
- The pre-check gate's interactive text/copy — preserved verbatim, not
  revised.
- Any change to `ConfigValuesStructure`'s `flash_protocol_selected_*` fields
  or how they're populated (`protocol_select.cpp`/`vehicle_select.cpp`/
  `mainwindow.cpp`) — 5d-1 explicitly left this "currently selected protocol"
  state as caller-side derived state, and 5d-2 does not revisit that.
  `LegacyChecksumAdapter` consumes it, converted to `std::string` at the call
  boundary, the same way `LegacyFlashSnapshotAdapter` (5c) consumes
  `EcuCalDefStructure`'s Qt fields without owning them.
- `IEventSink`/`QtEventSink` wiring for `FileActions`'s log window. `LOG_D`/
  `LOG_E` are signals on the `FileActions` `QObject` itself; connecting
  `QtEventSink` to the log window (it exists but, like `QtFileRepository`
  before 5d-1, is not wired anywhere yet) is unrelated infrastructure work,
  not needed for this slice's behavior parity.
- `MainWindow` construction/composition-root changes; step 6.

---

## Repository evidence

### Current behavior inventory

`FileActions::checksum_correction(EcuCalDefStructure *ecuCalDef)`
(`file_actions.cpp:2141-2367`), called only from `mainwindow.cpp` (7 call
sites, all passing `ecuCalDef[rom_number]` and reassigning the result):

1. Reads `ConfigValuesStruct.flash_protocol_selected_{protocol_name,make,checksum,mcu}`
   and emits `LOG_D` lines for each, plus the MCU type and target ROM size.
2. Resolves `ecuCalDef->McuType` to a `flashdevices[]` index via
   `FlashUtils::findFlashDeviceIndex`; unknown MCU → `LOG_E` and return
   unchanged, no dialog.
3. **Pre-check gate:** if neither `use_romraider_definition` nor
   `use_ecuflash_definition` is set, shows a warning `QMessageBox` with
   `OK`/`"DO IT!"` buttons; `OK` (the default/first button) returns
   `ecuCalDef` unchanged, `"DO IT!"` proceeds regardless of the gate.
4. If `flash_protocol_selected_checksum == "yes"` and
   `flash_protocol_selected_make == "Subaru"`: checks ROM size against
   `flashdevices[mcu_type_index].romsize`; mismatch → an info dialog ("Bad
   ROM size!") and return unchanged. Otherwise dispatches on
   `flash_protocol_selected_protocol_name` (`flashMethod`) via a
   `startsWith(...)` `if`/`else if` chain to exactly one of 9 checksum
   families in `src/algorithms/checksum` (all already portable —
   `ChecksumEcuSubaruDensoSH7xxx`, `...SH705xDiesel`,
   `ChecksumEcuSubaruHitachiM32rKline` — itself branching further on
   `ecuCalDef->RomId`'s leading digit ("3"/"4"/"6") —
   `...M32rCan`, `...SH7058`, `...Sh72543r`, `ChecksumTcuSubaruDensoSH7055`,
   `ChecksumTcuSubaruHitachiM32rCan`, `ChecksumTcuMitsuMH8104Can`). Every
   family's `calculate_checksum_result` takes `bytes::ByteView` and returns
   the portable `ChecksumResult` (`status`, `bytes::Bytes romData`,
   `std::string message`).
5. Any other `make`/`checksum` combination, or an unmatched `flashMethod`,
   leaves `chksumModuleAvailable = false` with no family run.
6. Each family result is applied via `applyChecksumResult`
   (`file_actions.cpp:84-100`, using the transitional `QtChecksumResult`/
   `toQt` from `qt_checksum.h`): if `result.ok()`, write `romData` back to
   `ecuCalDef->FullRomData`; if `result.changed()` (status `Corrected`),
   append the family's display message to `correctedFamilies` for one
   aggregated dialog after dispatch; otherwise (`Unchanged`/`Disabled`/
   `InvalidSize`/`UnsupportedRom`/`ParseError`) show it immediately via
   `showChecksumResult` (info dialog for `Disabled`, warning dialog for the
   three error statuses, nothing for `Unchanged`).
7. If `correctedFamilies` is non-empty, show one aggregated "Checksums
   corrected" info dialog. (In practice this list has at most one entry:
   the dispatch chain is mutually exclusive, so at most one family ever
   runs per call — the aggregation exists for a multi-family future that
   hasn't materialized, and 5d-2 preserves it as designed rather than
   simplifying it away.)
8. If no module matched **and** `flash_protocol_selected_checksum != "no"`,
   show a warning `QMessageBox` with `Cancel`/`OK`; `Cancel` logs
   `"Checksum calculation canceled!"` via `LOG_D` and returns unchanged.
   Note this condition fires for `checksum == "n/a"` as well as `"yes"`
   with an unmatched `flashMethod` — **not** only `"yes"`. This is the same
   three-state distinction 5d-1's `ProtocolEntry::checksum` comment already
   documented (`protocol_catalog.h:31-39`): real `protocols.cfg` entries use
   `"n/a"` (e.g. `sub_ecu_mitsu_m32r_kline`), and collapsing it to a bool
   would silently suppress this exact warning for real M32R K-Line/CVT-CAN
   targets.

`//src/algorithms/checksum:qt_compat`'s `BUILD.bazel` entry is annotated
`TRANSITIONAL. QByteArray/QString conversion for file_actions.cpp, which is
not portable until step 5. Delete when that caller moves.` — this slice is
that move.

`FlashUtils::findFlashDeviceIndex(const QString&)`
(`src/backend/flash/flash_utils.cpp:22-34`) linear-scans
`flashdevices[]` — itself plain C data (`kernelmemorymodels.h`, no Qt,
exposed portably today via `//src/backend/definitions:models`) — using
`QString::fromLatin1(...) == mcuType`. `flash_utils.cpp` is Qt-linked (it
also depends on `SerialPortActions`) and out of scope to convert; 5d-2 adds
its own small portable equivalent rather than touching it.

---

## Chosen architecture

### Value models

```cpp
namespace fastecu::checksum {

struct ChecksumSelection {
  std::string make;           // ConfigValuesStructure::flash_protocol_selected_make
  std::string checksum_flag;  // flash_protocol_selected_checksum: "yes"/"no"/"n/a",
                               // kept as string per the n/a finding above
  std::string flash_method;   // flash_protocol_selected_protocol_name
  std::string mcu_type;       // EcuCalDefStructure::McuType
  std::string rom_id;         // EcuCalDefStructure::RomId
};

struct ChecksumCorrectionOutcome {
  enum class Status {
    UnknownMcuType,      // McuType not found in flashdevices[]
    BadRomSize,          // fullRomSize != flashdevices[index].romsize
    NoModuleForProtocol, // make/checksum_flag/flash_method matched no family
    FamilyRan,           // exactly one family dispatched
  };
  Status status;
  std::optional<ChecksumResult> family_result;  // present iff status == FamilyRan;
                                                 // ChecksumResult is the existing
                                                 // portable type from
                                                 // src/algorithms/checksum
};

// Pure, no I/O, no Qt. Replaces checksum_correction's MCU/size lookup and
// flashMethod dispatch chain.
ChecksumCorrectionOutcome apply_checksum_correction(bytes::ByteView rom_data,
                                                     const ChecksumSelection& selection);

}  // namespace fastecu::checksum
```

`apply_checksum_correction` is fully unit-testable without touching a
filesystem or Qt: a table of `(flash_method, make, checksum_flag, rom_id,
mcu_type) -> expected family / outcome` covers all 9 families plus the 3
non-family outcomes. It does not return `Result<T>` — every branch here is a
legitimate domain outcome (unknown MCU, bad size, no module, ran), not an
I/O or parse failure in the sense the `ErrorKind` taxonomy models elsewhere.

### Portable MCU/size lookup

A small function alongside `apply_checksum_correction`, in the same
`src/backend/checksum` package:

```cpp
const flashdev_t* find_flash_device(std::string_view mcu_type);
```

Scans the same `flashdevices[]` table `FlashUtils::findFlashDeviceIndex`
does today, using `std::string_view` equality. A characterization test runs
both lookups against every `mcu_type` string that actually appears in the
shipped `protocols.cfg` and asserts identical results, since
`QString::fromLatin1` comparison semantics are not assumed identical to
`std::string_view` equality without checking.

### Legacy adapter

**Placement:** backend-resident, following 5d-1's rule that adapter
placement follows whoever the caller is — `FileActions` is the caller here,
just as it was for `LegacyConfigAdapter` (unlike the platform-resident
`LegacyFlashSnapshotAdapter`, whose caller is the platform-side EEPROM
dialog).

```cpp
namespace fastecu::checksum {

class LegacyChecksumAdapter {
 public:
  // Applies the pre-check gate, runs apply_checksum_correction, and shows
  // every dialog checksum_correction shows today, in the same order, with
  // the same text. Mutates ecu_cal_def.FullRomData in place when a family
  // corrects it.
  void checksum_correction(FileActions::EcuCalDefStructure& ecu_cal_def,
                            const ChecksumSelection& selection);
};

}  // namespace fastecu::checksum
```

It owns:

- The **pre-check gate** (no ROM bytes or algorithm dispatch involved, so it
  stays adapter-side as a `QMessageBox` call ahead of
  `apply_checksum_correction`, exactly reproducing the current
  `OK`/`"DO IT!"` button roles and text).
- `bytes::view`/`bytes::toQByteArray` conversion between
  `ecu_cal_def.FullRomData` (`QByteArray`) and the portable
  `bytes::ByteView`/`bytes::Bytes` the algorithms already use — no new
  conversion helper needed, these already exist.
- Translating `ChecksumCorrectionOutcome` into dialogs:
  `BadRomSize` → the existing "Bad ROM size!" info dialog;
  `FamilyRan` with `family_result->status == Corrected` → the aggregated
  "Checksums corrected" info dialog; `FamilyRan` with `Disabled`/
  `InvalidSize`/`UnsupportedRom`/`ParseError` → the existing
  `showChecksumResult`-equivalent dialog, ported into the adapter;
  `NoModuleForProtocol` with `checksum_flag != "no"` → the "no checksum
  module" `Cancel`/`OK` dialog, `Cancel` behavior preserved.
- `UnknownMcuType` produces no dialog (matches today — only `LOG_E`, which
  stays in `FileActions`, see below).

**Logging stays in `FileActions`, not the adapter.** `LOG_D`/`LOG_E`/`LOG_W`
are signals declared on the `FileActions` `QObject` itself
(`file_actions.h:489-492`); `LegacyChecksumAdapter` is not a `QObject` and
gains no dependency on `FileActions&` purely to `emit` on its behalf.
`FileActions::checksum_correction`'s thin body keeps the informational
`emit LOG_D(...)` lines (protocol/make/checksum/mcu/size) and the
unknown-MCU `emit LOG_E(...)` line, driven off the same `ChecksumSelection`
it builds to call the adapter — so the method is "selection-building plus
logging plus one delegated call," not a pure one-liner. This mirrors 5d-1's
already-accepted position that `FileActions` remains the Qt composition
point for its own diagnostics until step 6; it does not reopen that
decision, only applies it to this slice's specific signals.

```cpp
FileActions::EcuCalDefStructure *FileActions::checksum_correction(
    FileActions::EcuCalDefStructure *ecuCalDef)
{
    const checksum::ChecksumSelection selection{
        .make = ConfigValuesStruct.flash_protocol_selected_make.toStdString(),
        .checksum_flag = ConfigValuesStruct.flash_protocol_selected_checksum.toStdString(),
        .flash_method = ConfigValuesStruct.flash_protocol_selected_protocol_name.toStdString(),
        .mcu_type = ecuCalDef->McuType.toStdString(),
        .rom_id = ecuCalDef->RomId.toStdString(),
    };

    emit LOG_D("Protocol: " + ConfigValuesStruct.flash_protocol_selected_protocol_name, true, true);
    emit LOG_D("Make: " + ConfigValuesStruct.flash_protocol_selected_make, true, true);
    emit LOG_D("Checksum: " + ConfigValuesStruct.flash_protocol_selected_checksum, true, true);
    // ... McuType/size LOG_D lines, unchanged ...

    if (checksum::find_flash_device(selection.mcu_type) == nullptr)
    {
        emit LOG_E("Unknown MCU type: " + ecuCalDef->McuType, true, true);
        return ecuCalDef;
    }

    legacyChecksumAdapter.checksum_correction(*ecuCalDef, selection);
    return ecuCalDef;
}
```

(Illustrative — exact structure, including whether the unknown-MCU check
stays duplicated here or is queried from the adapter, is pinned by the
plan's characterization tests, not finalized here.)

---

## Package layout and Bazel targets

Portable (no `QT_DEPS`, enforced by `//:portable_closure`):

- `//src/backend/checksum:checksum_selection` — `ChecksumSelection` /
  `ChecksumCorrectionOutcome` structs only.
- `//src/backend/checksum:dispatch` — `apply_checksum_correction` and
  `find_flash_device`; depends on `//src/algorithms/checksum` (all 9
  families, one target) and `//src/backend/definitions:models` (for
  `flashdev_t`/`flashdevices`).

Backend, Qt-linked (accepted exception, same shape as
`legacy_config_adapter`):

- `//src/backend/checksum:legacy_checksum_adapter` — depends on
  `//src/backend/definitions` (for `FileActions::EcuCalDefStructure`) and
  `//src/backend/checksum:dispatch`. Declared with an explicit
  `srcs = ["legacy_checksum_adapter.cpp"]`, not a glob, for the same
  glob-collision reason `legacy_config_adapter` is.

All new files land in a new `src/backend/checksum/` package — never added to
`src/backend/definitions/`'s existing glob, per the 5d umbrella rule.

**Deleted:**

- `//src/algorithms/checksum:qt_compat` target and `qt_checksum.h`.
- `applyChecksumResult`, `applyDensoSh7xxxChecksum`, `showChecksumResult`,
  and `lineAfterClosingTag`'s checksum-only call site, from
  `file_actions.cpp`'s anonymous namespace (their logic moves into
  `:dispatch`/`legacy_checksum_adapter`; `lineAfterClosingTag` itself is
  checked for other callers before removal — grep shows it is only used by
  checksum reporting today, but the plan confirms this rather than trusting
  a single grep, per the umbrella's "verify, don't assume, code is dead"
  rule).

---

## Portable-closure and serial-compat enforcement

- Add `//src/backend/checksum:checksum_selection` and
  `//src/backend/checksum:dispatch` to `PORTABLE_ROOTS` in
  `scripts/check-portable-closure.py`.
- Non-vacuous probes: temporarily inject `QT_DEPS` into `dispatch`; it must
  fail the check, then be restored.
- Checksum correction never touched `SerialPortActions`, so — like 5d-1 —
  this slice claims no `serial_qt_compat` allowlist shrinkage.

---

## Testing

Characterization before conversion (co-located
`src/backend/checksum/*_test.cpp`):

- `apply_checksum_correction`: one case per family (9), driven by the exact
  `flashMethod` prefixes from `checksum_correction`'s `startsWith(...)`
  chain, including the M32R K-Line `RomId` "3"/"4"/"6" sub-branch; plus
  `UnknownMcuType`, `BadRomSize`, and `NoModuleForProtocol` for both
  `checksum_flag == "no"` (no dialog) and `checksum_flag == "n/a"` (dialog
  fires) as distinct cases — this is the one behavior most at risk of
  silent regression, so it gets its own explicit non-collapsing test.
- `find_flash_device`: cross-checked against
  `FlashUtils::findFlashDeviceIndex` over every `mcu_type` string in the
  shipped `protocols.cfg`.
- `LegacyChecksumAdapter`: `ecu_cal_def.FullRomData` and the sequence of
  dialogs shown, after each call, matched against what
  `checksum_correction` produces today for the same fixtures (pre-check
  gate both branches; each of the 4 outcome kinds).

New-code coverage >=80%, SonarCloud Quality Gate passes,
`docs/coverage-baseline.txt` remains absent, matching 5c/5d-1's gates.

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

---

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| `checksum_flag`'s `"n/a"` vs `"no"` distinction gets re-collapsed into a bool during dispatch, silently suppressing the no-module warning for real M32R K-Line/CVT-CAN targets (the exact failure mode 5d-1's `ProtocolEntry` comment already flagged) | `ChecksumSelection::checksum_flag` stays `std::string`; test suite explicitly covers `"n/a"` and `"no"` as distinct, non-collapsing cases |
| The portable MCU/size lookup silently diverges from `FlashUtils::findFlashDeviceIndex`'s Qt-string comparison semantics | Characterization test runs both lookups against every `mcu_type` string in the shipped `protocols.cfg` and asserts identical results before trusting the portable one alone |
| `LegacyChecksumAdapter`'s dialog text/button roles get paraphrased instead of copied verbatim | Copy the exact `QString` literals and `QMessageBox::NoRole`/button wiring from the current code; characterization tests assert against them, not a rewritten version |
| `lineAfterClosingTag` or another anonymous-namespace helper marked "checksum-only" turns out to have a second caller elsewhere in `file_actions.cpp` | Plan verifies each helper's call sites explicitly (grep plus a build-breaks-if-still-referenced check) before deleting, rather than trusting a single grep pass |
| Aggregated "Checksums corrected" dialog's multi-family path (currently unreachable — dispatch is mutually exclusive, so `correctedFamilies` never has more than one entry) gets "simplified" away during the port, silently changing behavior if a future family addition makes it reachable | `ChecksumCorrectionOutcome` still models `family_result` as the result of exactly one family (matching current reality), but the adapter's aggregation step is written generically enough that a second family added later would not require re-deriving this decision |

---

## Deliverable checklist

- [ ] `ChecksumSelection`/`ChecksumCorrectionOutcome` value models and
      `apply_checksum_correction`, table-tested against all 9 families plus
      the 3 non-family outcomes (including the `"n/a"` vs `"no"` split), no
      I/O.
- [ ] Portable `find_flash_device`, cross-checked against
      `FlashUtils::findFlashDeviceIndex` over the shipped `protocols.cfg`.
- [ ] `LegacyChecksumAdapter` reproducing every dialog and
      `FullRomData` mutation field-for-field against fixtures.
- [ ] `FileActions::checksum_correction` reduced to selection-building plus
      `LOG_D`/`LOG_E` emission plus one delegated call; `MainWindow`
      unmodified.
- [ ] `//src/algorithms/checksum:qt_compat` and `qt_checksum.h` deleted.
- [ ] Portable-closure extension with the negative probe.
- [ ] >=80% new-code coverage; `docs/coverage-baseline.txt` remains absent.
- [ ] Full build/test gate green on Linux/macOS/Windows.
