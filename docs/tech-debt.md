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
- Extract DataTerminal script execution into portable diagnostics. Script
  parsing and validation (`parse_terminal_script`) is already portable; the
  desktop still owns the send/delay/read loop, SSM header and CAN ID framing.
  Delay steps are standalone ordered pauses on both buses.

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
- **SSM logging byte requests and response mapping.** Current acquisition emits
  one base address per parameter and decodes using Digital-slot offsets, even
  for multi-byte parameters or unresolved selection gaps. SSM returns one data
  byte per requested address; characterize and correct byte-address expansion,
  ordered response mapping, and physical-entry capacity checks before extending
  selection acquisition. Existing fixtures that return multiple bytes for one
  requested address do not establish wire correctness. The
  [SSM protocol evidence](reference/logging-contracts.md#ssm-protocol-evidence)
  provides primary sources. Separate switch capability bits from sample bits and
  normalize standard/legacy XML address forms when adding switch acquisition.
- **MUT/DMA free-form capacity and request interpretation.** FastECU's 255-entry
  limit establishes one-byte count representability, not ECU capacity. OEM static
  analysis of ROM 33520003 finds fill-loop bounds of 96 elements and 96 output
  bytes at flash `0x11600`, and request-code assembly at `0x1164c`–`0x11660` that
  differs from the codec's big-endian ID encoding. Characterize the request layout,
  width handling, and capacity against firmware and bench evidence before changing
  them; do not generalize one ROM's limits to every MUT/DMA implementation. The
  [logging evidence reference](reference/logging-contracts.md#mutdma-protocol-evidence)
  identifies the parent OEM research. ACKs and XML enablement do not establish
  per-measurement availability; requests can resolve through either a MUT table
  entry or compact RAM mapping.
- **MUT memory read integrity/write bounds.** The currently uncalled helpers can
  omit a timed-out read chunk and continue, returning a gapped buffer. The
  `0x4000`–`0xBFFF` write guard checks only the start, so data may extend beyond
  the window. Resolve both before wiring callers; never relax the guard.

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

- Add an in-app logger measurement editor for request ID/address code, width,
  scaling, unit, and precision, with stable identities and definition persistence.
  The current chooser selects existing IDs and cannot author definitions; use the
  [XML workflow](../resources/shared/config/README.md) until this follow-up is
  implemented. User-authored MUT measurements do not require a firmware-match
  gate; portable field and request validation still applies.
- `CarNotResponding` is logged without a live GUI indicator.
- Live channel/interval/protocol reconfiguration requires an explicit path through
  session config and worker; current changes require stop/start.
- `LoggingEngine::stop()` disconnects worker signals and publishes
  `StoppedByUser` itself; its worker-Cancelled mapping is not reached by stop.
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
