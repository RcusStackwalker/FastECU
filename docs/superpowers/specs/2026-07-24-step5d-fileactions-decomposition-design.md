# Step 5d — FileActions/MainWindow Backend Decomposition — Umbrella Design

**Status:** Approved 2026-07-24. Fourth sub-project of step 5 (see the
[step-5 umbrella design](2026-07-22-step5-backend-portable-design.md), which
this document decomposes further). Depends on 5a (merged, PR #73), 5b (merged,
PR #78), and 5c (merged, PR #79).

**Goal:** replace `FileActions` — a 3,397-line `file_actions.cpp` plus two
sibling translation units (`file_defs_romraider.cpp`, 763 lines;
`file_defs_ecuflash.cpp`, 1,213 lines) and a 585-line header, all defining
methods of one `QWidget`-derived god class — with backend-owned, Qt-free use
cases behind the port set fixed by the step-5 umbrella. Remove `QFileDialog`,
`QMessageBox`, filesystem access, and `SerialPortActions` from backend.
`MainWindow`'s thin-shell rewrite stays step 6; 5d extracts backend-side use
cases only.

## Why 5d itself needs to be decomposed

The umbrella spec's line count (3,397) covers only `file_actions.cpp`; the real
surface is roughly 6,000 lines in one class. Unlike checksum (already portable
per family since step 4) or flash (seamed by 5c), the RomRaider/EcuFlash
definition-parsing and calibration slices have **no portable algorithm layer to
build on** — step 4 Amendment 1 explicitly deferred that parsing migration to
step 5, and it has not happened yet. `src/algorithms` today has no
romraider/ecuflash/definition/calibration/config package. That is new
algorithm-layer work, not extraction, and it does not fit one plan any more than
step 5 itself fit one plan. 5d is therefore sequenced the same way the step-5
umbrella sequenced 5a–5d: **each sub-project below gets its own spec → plan →
implementation cycle.**

## Sub-project decomposition and sequence

| ID | Sub-project | Depends on | Core deliverable |
|----|-------------|-----------|------------------|
| **5d-1** | Config/settings foundation — **merged** | 5a | `set_base_dirs`/`check_config_dirs`/`read_config_file`/`save_config_file`/`read_protocols_file` → portable use cases on new `IFileSystem`/`IResourceBundle` ports; `ConfigPaths`/`AppConfig`/`ProtocolCatalog` value models; pugixml adopted as the shared portable-XML primitive |
| **5d-2** | Checksum use case — **merged, PR #81** | 5a, 5d-1 | `checksum_correction` → orchestration-only use case; the 9 checksum families are already portable (step 4). Smallest, most precedented — same shape as 5c's proving pair |
| **5d-3** | Definition use case (RomRaider + EcuFlash parsing) — **merged, PR #86** (2026-07-30) | 5a, 5d-1 | Portable parsing algorithms for both definition formats (using 5d-1's XML primitive) plus the backend definition use case; replaces `file_defs_romraider.cpp`/`file_defs_ecuflash.cpp` and the definition-file dialog flows in `file_actions.cpp`. The largest and least-precedented slice — new algorithm-layer work, likely warrants its own internal sequencing when planned |
| **5d-4** | Calibration use case — ROM open/save — **merged, PR #117** (2026-07-31) plus follow-ups #126/#127/#128/#129/#131/#133 | 5d-1, 5d-3 | `open_subaru_rom_file`'s byte I/O, size validation and protocol/checksum binding; `save_subaru_rom_file` entirely; `resolve_car_models`/`find_car_model_by_protocol_name`; dialog relocation to `MainWindow`. Split during design — the map/axis decode it originally covered became 5d-4b |
| **5d-4b** | Map cell/axis value computation — **merged, PR #134** (2026-08-01) | 5d-4 | The `MapData`/`XScaleData`/`YScaleData` per-map/per-axis byte-decode-and-scale loop plus the WRX02 ROM-padding case, formerly inline in `open_subaru_rom_file`. Re-landed against the post-#126 API, superseding PR #118 |
| **5d-5** | Logging-definition glue | 5b, 5d-1 | `read_logger_definition_file`/`read_logger_conf`/`save_logger_conf`, DTC/NRC table ownership — drains the transitional `qt_dtc_parser`/`qt_nrc_parser` shims |
| **5d-6** | Flash-definition glue — **merged, PR #138** (2026-08-01) | 5c, 5d-1, 5d-4 | Replaces `LegacyFlashSnapshotAdapter` with a real backend use case, removing `EcuCalDefStructure` from the flash path — closes the placeholder 5c left explicitly for 5d. **Dependency corrected during design:** the adapter consumes protocol-catalog values (`mcu`, `kernel`, `kernel_addr`), not definition-model values, so this slice depends on 5d-1/5d-4, not 5d-3 |

**Dependency shape:** `5a → 5d-1 → {5d-2, 5d-3}`; `5d-3 → 5d-4 → 5d-4b`;
`5d-3 → 5d-6`; `5b → 5d-5`; `5c → 5d-6`. 5d-2 and 5d-5 are otherwise
independent of 5d-3/5d-4/5d-4b/5d-6. With 5d-1 through 5d-4b and 5d-6 merged,
**5d-5 is the only remaining slice**, and 5b (its sole dependency) is merged, so
it is unblocked.

**Out of scope:** `read_menu_file` (builds `QMenuBar`/`QToolBar`/`QSignalMapper`
from a `QDomDocument`-parsed menu file) has no algorithmic content — it is Qt
widget construction, not a backend use case. It stays as legacy UI-coupled code
on the shrinking class until step 6 rather than being force-fit into one of the
above.

**Per-slice design docs:** only 5d-2's survives (the
[5d-2 checksum design](2026-07-25-step5d2-checksum-use-case-design.md)). The
5d-1, 5d-4, 5d-4b and 5d-6 design docs were removed once their slices merged —
the merged PRs listed in the table above are the durable record. This table,
not a per-slice doc, is the surviving specification for **5d-5**, which has no
design doc yet.

## Shared architecture across 5d sub-projects

These decisions are made once here so 5d-2 through 5d-6 do not re-litigate
them:

- **Value models replace parallel `QStringList`s.** Every slice that currently
  models a collection as N parallel `QStringList` members (protocols,
  definition rows, log values, DTC tables) gets one struct-per-row value type
  instead. This is the concrete instance of step 4 Amendment 1's deferred
  "validated value models," scoped per sub-project rather than done once
  globally.
- **pugixml is the shared portable-XML primitive**, adopted in 5d-1 and reused
  by every later slice that parses XML (5d-3's RomRaider/EcuFlash formats,
  5d-5's logger definition file). `bazel_dep(name = "pugixml", version =
  "1.15")` (Bazel Central Registry, MIT-licensed, no further dependencies).
  Backend targets depend on it directly — the same pattern already used for
  `src/algorithms/crypto` depending directly on `@openssl` — no wrapper
  package. This is FastECU's second adjudicated third-party dependency after
  `@openssl`; it is a deliberate choice made once in 5d-1, not a side effect of
  any later slice.
- **The snapshot-adapter pattern generalizes**, with per-slice shape. 5c's
  `LegacyFlashSnapshotAdapter` is not one reusable class; it is a pattern: a
  small platform-side (or transitional backend-side, per slice) adapter that
  copies fields from the new value models into the legacy `FileActions`
  structures (`ConfigValuesStructure`, `EcuCalDefStructure`, `LogValuesStructure`)
  so `MainWindow` and other unconverted callers keep working unchanged.
  5d-1 uses `LegacyConfigAdapter`; each later slice names its own.
- **New packages, never co-located with the legacy glob.** Every slice lands
  its portable code in a new package (e.g. `src/backend/config/`,
  `src/backend/calibration/`) rather than inside `src/backend/definitions/`,
  where the legacy `file_actions.*`/`file_defs_*.cpp` glob lives. This is a
  direct response to the Bazel glob-collision hazard hit repeatedly in 5c: a
  new portable file landing beside a legacy Qt target's glob in the same
  package silently pulls it into the wrong target or vice versa.
- **Ports may be extended per slice**, following 5c's precedent
  (`IFlashTransport`, `ICanFlashTransport`, etc. were added beyond the
  umbrella's fixed five). 5d-1 adds `IFileSystem` and `IResourceBundle`; later
  slices may add their own. The umbrella's `ErrorKind` taxonomy and thread
  model are not extended — only the port set grows, as it already does per
  sub-project.
- **`FileActions`'s public method signatures do not change** as slices convert
  their bodies. Each converted method becomes a thin call into the new use case
  plus its legacy adapter, so `MainWindow` call sites are untouched until step
  6. This mirrors 5c: the worker/executor seam changed underneath, but the
  EEPROM dialogs' call shape did not.
- **Dead code is dropped, not ported.** `copy_directory_files` and the
  `copyConfigFromDirectory`/`copyKernelsFromDirectory` members are computed and
  logged but never used for actual file copying (the real provisioning path
  uses Qt resource paths directly). 5d-1 removes them. Each later slice's plan
  should budget time to independently verify — not assume — that every method
  it touches is live, since `FileActions`'s size makes vestigial code plausible
  elsewhere too. **Vindicated in 5d-4b:** the WRX02 address wraparound at
  `file_actions.cpp:2050/2126/2202` guards on a `RomInfo[FlashMethod]` value
  that `apply_flash_method_alias` has already overwritten with the canonical
  protocol name, so it never fires — and PR #118 ported it faithfully into a
  `bool` parameter threaded through three call sites before anyone checked.
  Vestigial code here hides behind *string comparisons against stale
  spellings*, not just behind unreferenced symbols, so grepping for callers is
  not sufficient verification.

## Enforcement

Extends `//:portable_closure` and `//:serial_compat_allowlist` the same way 5c
did: each sub-project adds its own converted targets to the portable closure as
it lands (never all six at once), and removes its own now-provably-dead
`serial_qt_compat` allowlist entries as its slice's Qt/dialog/filesystem/serial
access is actually removed from backend. Each sub-project's own spec names its
exact target and allowlist-entry lists; this document does not enumerate them
because 5d-2 through 5d-6 are not designed yet.

## Testing and gates

Same umbrella-wide commands as step 5 generally:

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

Each sub-project characterizes existing behavior with golden tests before
converting it (step 4/5c precedent), keeps `docs/coverage-baseline.txt` absent,
and meets the >=80% new-code coverage / SonarCloud Quality Gate applied to 5c.

## Risks (forward-looking, from 5c's experience)

| Risk | Mitigation |
|---|---|
| Regressions live in hidden state one layer below the tested seam (5c's invisible-to-every-test bug) | Each slice's plan budgets independent verification beyond the written test suite, not just plan thoroughness, before claiming completion |
| Bazel glob collisions when new portable files land beside a legacy Qt target | New-package-per-slice rule (above), checked as part of each slice's own portable-closure enforcement extension |
| Shared, long-lived object ownership breaking an exclusive-ownership assumption (5c's `SerialPortActions*` case) | Each slice's design explicitly states `FileActions`'s own lifetime/mutation story for the state it owns before writing the use case, the way this document states 5d-1's `ConfigPaths`/`AppConfig` ownership |
| The genquery-based portable-closure check's diagnostic quality at larger scale | Each slice's plan re-evaluates whether the check still gives an actionable failure message as more targets are added, rather than assuming it scales for free |
| Coverage becomes a late surprise | Budgeted during execution per slice, not treated as a final gate check, per the coverage-gate lesson from 5c |
| The serial-compat allowlist "clean removal count" is off by one or two | Expected; each slice verifies its exact removal list against the live allowlist file rather than trusting a count written during design |

## Deliverable checklist

- [x] This umbrella document: sub-project table, dependency shape, shared
      architecture decisions (value models, pugixml, adapter pattern, package
      placement, port extension, dead-code handling).
- [x] 5d-1 design doc, plan, and implementation — merged.
- [x] 5d-2 design doc, plan, and implementation — merged as PR #81.
- [x] 5d-3 design doc, plan, and implementation — merged as PR #86
      (2026-07-30).
- [x] 5d-4 design doc, plan, and implementation — merged as PR #117
      (2026-07-31), with follow-ups #126/#127/#128/#129/#131/#133.
- [x] 5d-4b design doc, plan, and implementation — merged as PR #134
      (2026-08-01), superseding PR #118. Follow-up debt filed as #135
      (map-value text formatting contract).
- [x] 5d-6 design doc, plan, and implementation — merged as PR #138
      (2026-08-01). K-Line reachability follow-up: #137.
- [ ] 5d-5 design doc, plan, and implementation — the last remaining slice.
