# FastECU technical debt roadmap

This document owns unresolved defects and cleanup actions. Remove resolved items;
use [Git history](README.md#recover-completed-work-and-deleted-source-citations) for
completed work. Recheck an item's applicability against current code before
scheduling it. The goals are 80% coverage for maintained non-generated application
code and reusable policy that can be tested without hardware, widgets, or user
configuration. [Design notes](design-notes.md) own current decisions; the
[Android roadmap](modularization-plan.md) owns native-seam milestones.

## Priorities

### P1: Separate UI from application logic

Reusable policy still exists in UI/platform consumers. Move it in scoped PRs
with reproductions for demonstrated defects; keep operator decisions, dialogs,
workers, transports, and timers with their desktop owners. A Qt-free closure
alone does not make a presentation flow backend policy.

Remaining actions from the backend migration roadmap:

- Move reusable field resolution and patch application from the UI map-edit
  adapter, and selectable encoding from `MainWindow`, into backend
  calibration/session policy. Preserve selection bounds, definition-less behavior,
  and the [typed calibration contracts](reference/calibration-compatibility.md#decoded-values-and-map-edits).
- Finish moving logging snapshot/channel preparation and sample validation from
  desktop adapters into backend logging; reuse the existing portable preparation
  rather than starting a second policy implementation.
- Extract logger configuration/model installation and selection persistence
  orchestration from `MainWindow` into backend use cases.
- Move CSV column resolution and serialization from UI code into backend logging.
  Preserve [identity, formatting, and file-lifetime contracts](reference/logging-contracts.md).
- Move reusable flash routing, preparation, prompt/stage policy into backend
  flash workflows. Desktop code binds plans to transports/workers and renders
  prompts; operator decisions stay outside executors. Do not unify different
  ECU state machines merely to eliminate branches.
- Extract BIU decoding/settings/session policy into portable diagnostics/codecs.
  Keep BIU exchanges synchronous and timer ownership on the desktop.
- Extract DataTerminal parsing/validation/execution into portable diagnostics.
  Fix `delay(n)` as an ordered standalone pause for both buses. Its current
  `split(")").at(1).split("(").at(0)` parses `delay(100)` as an empty string,
  yielding zero milliseconds. This correction changes script timing and needs
  explicit regression expectations; it is not a five-baud hardware unknown.

The portable definition catalog and header policy are already in place. Do not
retain their completed migrations as debt. Connection/calibration presentation
coordinators retain their UI roles; see the relevant
[desktop](reference/desktop-contracts.md) and calibration contracts.

### P1: Resolve known correctness gaps

- **OpenPort five-baud ASCII comparison.** `five_baud_header` checks J2534 bytes
  `[5]`/`[7]` for iso9141 and `[8]`/`[9]` for iso14230 against `'8'`/`'8'` and
  `'8'`/`'f'`. Direct serial checks numeric `0x08`/`0x08` at `[1]`/`[2]` and
  `0x8F` at `[2]`. Obtain a bench capture before deciding whether the firmware
  actually returns ASCII; do not infer the answer from the sibling branch.
- **DTC coverage and NRC interpretation.** Audit coverage for clear-loop short
  frames/NRCs/wrong IDs, CAN-init short/non-`0x41` responses, fast-init cancellation,
  and event-log strings. Add missing scripted cases before changing those paths.
  The CAN-init NRC description uses offset 3 while other ISO-15765 NRCs use 4;
  reproduce it and distinguish a description fix from wire behavior changes.
- **MUT memory read integrity/write bounds.** The currently uncalled helpers can
  omit a timed-out read chunk and continue, returning a gapped buffer. The
  `0x4000`–`0xBFFF` write guard checks only the start, so data may extend beyond
  the window. Resolve both before wiring callers; never relax the guard.
- **Configuration directory persistence.** The writer's `logfiles_directory`
  versus the reader's `datalog_files_directory` prevents round-tripping the datalog
  directory. `ConfigSessionSave.DatalogDirectoryDoesNotRoundTrip` pins it; fix
  with explicit read/write compatibility tests.

### P1: Resolve the wrx02 address predicate

`element_byte_address` subtracts `0x8000` under different conditions for reads
and writes. A boundary cell can display one byte and write another. The write
predicate matches `apply_flash_method_padding` (insert `0x8000` bytes at
`0x20000` for images under `190 * 1024`), suggesting the read predicate is wrong.

An earlier search of the `mmc-definitions` and `mmc-patches` corpora found no
real definition declaring `wrx02`, so that reasoning lacks definition evidence.
Keep `PinnedDefect_Wrx02FixupDiffersBetweenReadAndWrite` pinned until a real
`wrx02` definition establishes the correct predicate, or an explicit accepted-
risk decision chooses the write predicate from the padding rule alone. Also
recheck associated `map_edit.cpp` complexity findings when resolving this item.

### P1: Consolidate flash workflow orchestration

Actions:

- Extend scripted workflow/dialog coverage where a family has only plan/executor
  unit tests. Include handshake failure, read success, cancellation, erase/write
  rejection, timeout, stop requests, and checksum mismatch.
- Treat multi-stage EEPROM inspect/ignition retries, bootmode kernel/program
  attempts, and programming-voltage notices separately from single-attempt
  workflows. Preserve their operator and connection boundaries.
- Investigate the two Denso BEEF CAN `nonfatal_query` implementations in
  `subaru_denso_sh7058_can_executor.cpp` and its diesel sibling against
  `uds_client_exchange_common.h`'s `non_fatal_query`. Their reply-returning
  behavior differs; this is a protocol change requiring characterization,
  not a mechanical replacement.
- Recheck family-aware response-validator/block-planning duplication. Helpers
  return structured byte-native results; family code retains logs, retry policy,
  sequencing, and safety rules. Follow the
  [sharing decision](design-notes.md#share-only-proven-protocol-equivalence).

### P1: Narrow serial and hardware interfaces

The remaining separation is inside the desktop facade/direct serial/J2534
implementation, which combines configuration, discovery, blocking I/O, and
adapter management. Actions:

- Move pure framing/checksum/header interpretation into algorithms/protocol.
- Separate discovery, PE-bitness/bridge lifecycle, PassThru types/configuration,
  and message transport from higher-level serial behavior.
- Retain lifecycle coverage for teardown with in-flight calls, helper-process
  failure, timeouts, and adapter removal on every supported platform.

### P2: Preserve edit lookup after an initial map decode failure

If a map fails its initial structural decode and later recovers, initialization
renames its table with the map-type suffix while the enclosing MDI window retains
its earlier name. The view displays recovered values, but edit lookup cannot find
the table. Synchronize the lookup identity and cover initial failure → recovery
→ edit through the MDI window. Closing and reopening the map restores editing.

### P2: Identify Subaru CAN ECUs with SSM `AA`

Current ISO-15765 identification sends UDS `22 F1 82`, obtains an ID without
capability bits, and cannot filter log channels by ECU support. Raw CAN identifies
nothing. The intended SSM `AA`/`EA` exchange to `0x7E0` needs a bench capture before
adding an `SsmVariant` and routing CAN transports to it. Validate like SSM2 and
qualify on the [connection checklist](checklists/connection-bench-checklist.md).

### P2: Pay down the SonarCloud code-smell backlog

Obtain a successful analysis of the implementation base and export all issue
pages before scheduling work; an issue query alone is not a new scan. Dated
triage and [proposed designs](README.md#active-work) are baselines, not
current totals or authorization to implement a proposed program. Do not copy
per-rule counts into this roadmap.

Order cleanup by risk:

- Review shadowing (`S1117`), narrowing (`S5276`), and ownership (`S5025`) for
  correctness. Characterize suspicious assignments and trace ownership across
  exits/threads before converting allocations to RAII.
- Audit macro modernization (`S5028`) for types, preprocessor/SDK collisions,
  ABI, and implicit conversions. Numeric equality does not prove behavior parity.
- Fold byte/container modernization and commented-code removal (`S6022`, `S5945`,
  `S125`) into touched files.
- Recheck complexity/nesting (`S3776`, `S134`) by subsystem, retaining protocol
  outcomes. `wrx02` stays separately evidence-gated. A proposed zero-High program
  becomes a scheduling commitment only after its own approval.
- Remeasure duplication in definition-authoring validate/write tails and transport
  read/write guards before extracting it.
- Replace the transitional `WriteSelection.ReproducesTheFourSpaceQDomIndent`
  golden with pugixml-owned output plus the write/read round trip; preserve
  four-space indentation itself.

Keep Clean-as-You-Code enforcement. Passing a new-code gate does not establish
that old findings are resolved; avoid separate permanent ratchet machinery unless
an actual gap is demonstrated.

### P2: Logging engine follow-ups

Hardware checks are owned by the [logging engine checklist](checklists/logging-engine-bench-checklist.md).
The open protocol-specific evidence needs are plain-serial continuous logging,
adapter removal/teardown, SSM per-cycle `0xA8 0x01` request-versus-stream behavior,
and [CDBG setup/security/streaming](checklists/cdbg-can-logging-bench-checklist.md).
The [Colt CAN checklist](checklists/colt_czt_47110032_can_bench_checklist.md) owns
flash-worker prompt/progress qualification.

Remaining behavior and code gaps:

- `CarNotResponding` is logged without a live GUI indicator.
- Live channel/interval/protocol reconfiguration requires an explicit path through
  session config and worker; current changes require stop/start.
- `LoggingEngine::stop()` disconnects worker signals and publishes
  `StoppedByUser` itself; its worker-Cancelled mapping is not reached by stop.
- `ISsmTransport` is in the global namespace unlike MUT/DMA and CDBG interfaces.
- `FastEcuSsmTransport::write()` reports input size while discarding returned
  echo-check bytes, so it does not expose an echo failure. Recheck before fixing.

### P2: Naming and source/data organization

Move misplaced code into owning files/namespaces while touching it. Replace
integer facade statuses with typed results using names that cannot collide with
Windows SDK macros; see the [desktop boundary contract](reference/desktop-contracts.md#composition-lifetime).
Remove stale commented-out code as nearby behavior changes.

## Coverage growth sequence toward 80%

Prioritize parser/model fixtures and validation; checksum golden vectors and
invalid inputs; calibration undo/redo workflows; scripted
flash-family orchestration; and serial/J2534 lifecycle failures. Thin widget tests
cover wiring, typed dispatch, and display. Grow most coverage in portable logic,
not by constructing the GUI. Keep test/generated/vendored/platform exclusions
explicit and reviewed in the owning coverage configuration.

New behavior needs focused automated evidence or a documented bench-only
justification. New debt is recorded here; implementation/testing conventions
belong to the [coding guide](coding-style.md) and [repository instructions](../AGENTS.md).
