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
- **MUT/DMA free-form capacity.** The count field's 255-entry representation
  limit is not ECU capacity. The analyzed 33520003 firmware bounds the fill loop
  to 96 entries and 96 output bytes; session preparation enforces those bounds.
  End-to-end qualification remains outstanding. The
  [wire evidence](reference/logging-wire-evidence.md) owns the firmware source.
  ACKs and returned values do not establish per-measurement availability or meaning.

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
  outcomes. A proposed zero-High program becomes a scheduling commitment only
  after its own approval.
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

- The direct serial reader currently recognizes SSM source headers `0x10`/`0x01`
  but excludes TCU source `0x18`. Qualify and repair direct-adapter TCU framing
  separately; target-change support invalidation does not establish transport
  compatibility.

- Add an in-app logger measurement editor for request ID/address code, width,
  scaling, unit, and precision, with stable identities and definition persistence.
  The current chooser selects existing IDs and cannot author definitions; use the
  [XML workflow](../resources/shared/config/README.md) until this follow-up is
  implemented. User-authored MUT measurements do not require a firmware-match
  gate; portable field and request validation still applies.
- Construct a gauge renderer for acquired gauge selections; the current desktop
  acquires, caches and exports these values but has no gauge renderer.
- `CarNotResponding` is logged without a live GUI indicator.
- Live channel/interval/protocol reconfiguration requires an explicit path through
  session config and worker; saved selection edits remain pending with a visible
  restart-needed state and current acquisition/display/CSV bindings stay frozen.
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
