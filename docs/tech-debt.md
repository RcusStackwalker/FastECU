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
- **Denso checksum firmware coverage.** The table engine applies the record
  check disassembled from SH7055 and SH7058 petrol firmware
  ([#668](https://github.com/RcusStackwalker/FastECU/pull/668)). The SH7059,
  SH72531, 1N83M, SH705x and SH72543 diesel, and SH7058 TCU routes have not
  been checked against their own firmware. Well-formed tables give the same
  result either way, so only malformed records are at stake. The diesel
  override words at `0x0FFAFC` and `0x17FAFC`, summed as `0xFFFFFFFF`, have no
  firmware evidence at all. Disassemble a diesel ROM's checksum routine before
  changing or relying on them.

### P1: Turn surviving Mull mutants into tests

[#656](https://github.com/RcusStackwalker/FastECU/issues/656) tracks the
surviving mutants package by package and owns the per-file counts; the
[coding guide](coding-style.md#tests) owns the rules. Keep each PR to one
package. Quote its before and after survivors from a scoped run
(`gh workflow run mutation -r <branch> -f scope=//pkg/...`), and list its
suppressions. Remaining packages, in order of what a missed defect could do to
an ECU:

- **Flash executors, shared guards** (`backend/flash/ecu`). Add the shared
  boundary cases to `CanExecutorConformance` and `SingleWindowPlanContract`:
  image offset plus block length against the image size, a page against the
  region's remaining length, and block-count rounding. Then record pulling the
  copied guards into a shared helper here.
- **Flash executors, per family.** Cover what the shared cases leave, including
  the flash-voltage log line in `subaru_denso_sh7058_can_executor.cpp`.
- **Flash plans**, including `flash_validation.cpp`.
- **EEPROM** (`denso_sh705x_eeprom_*`).
- **Calibration, definitions, protocol codecs, logging, and diagnostics.**
- **Key recovery** (`algorithms/crypto/subaru_key_recovery.cpp`).
- **The rest:** `expression`, the CDBG driver, config, ports, and service
  functions.

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
- `FastEcuSsmTransport::write()` discards `WriteSerialDataEchoCheck`'s return,
  as do the CAN, mixed-CAN and K-Line flash transports: the call never signals
  failure (every path returns a null `QByteArray`), so each transport checks
  `IsSerialPortOpen()` afterwards. The behavior is consistent across siblings;
  an echo-mismatch signal would need a new backend contract.

### P2: Naming and source/data organization

Move misplaced code into owning files/namespaces while touching it. Replace
integer facade statuses with typed results using names that cannot collide with
Windows SDK macros; see the [desktop boundary contract](reference/desktop-contracts.md#composition-lifetime).
Remove stale commented-out code as nearby behavior changes.

### P2: Convert legacy test suite names

Legacy suites still use a `Test` suffix. Rename them for the type under test as
their files are touched, following the [test conventions](coding-style.md#structure).
The [native clang-tidy rollout](reference/clang-tidy-plugin-qualification.md)
completed the Result assertion migration and now enforces it; ordinary optional
checks and low-level Result API tests remain permitted.

### P2: Analyze fuzz harnesses with clang-tidy

`*_fuzz_test.cpp` targets build only under `--config=asan` or
`--config=fuzztest`, so the default compilation database the clang-tidy gate
refreshes has no entry for them (a changed one is reported as a note, not
analyzed). Give the refresh a fuzz-enabled pass (or a database merged from one).

### P2: Give the clang-tidy tooling its own directory

The clang-tidy gate's seven `clang_tidy_*.py` modules, their tests and
`clang_tidy_plugin/` sit flat in `scripts/` among unrelated scripts. Move them
into `scripts/clang_tidy/`, with that package's `BUILD.bazel` owning the Python
targets. Keep the `//:clang_tidy_*` labels that CI and the
[tooling guide](tooling.md#clang-tidy) run (root aliases, or keep the runner
macros at the root). Paths that name these files: root `BUILD.bazel`,
`bazel/clang_tidy.bzl` (`srcs`, `main`, `imports`), `.clang-tidy` comments,
the `.clang-tidy-scope.toml` plugin prefix, a `MODULE.bazel` comment, `pr.yml`
comments and the tooling guide. The runner finds the adapter beside itself
(`Path(__file__).with_name`), so the two must move together.

### P3: Flash protocol behaviors awaiting external evidence

Characterization preserves these behaviors; none is a demonstrated defect.
Changing one needs mapped protocol functions or captured family traffic plus a
separate correction decision (see
[corrections](design-notes.md#corrections-require-local-evidence)).

- **Denso BEEF K-Line (MC68, SH7055).** Capability layout and final verification
  lack independent evidence; MC68 CRC decoding accumulates ten bytes without
  checksum validation. No local EcuFlash artifact maps family pacing,
  address windows, upload silence or the CRC failure marker.
- **Denso ISO-15765 bootloader.** The legacy pending-first checksum discrepancy
  and the discarded first bench jump read (4M, SH72531) are unverified. No
  mapped RomRaider or EcuFlash page-read oracle covers timeout, response policy,
  short-page acceptance or stop-error precedence, and ECUFlash family names do
  not map the addressing. Every per-family deviation in the
  [family deviations](reference/denso-iso15765-families.md) page is likewise
  transcribed, unverified; collapse the ones bench evidence shows are
  unneeded, which also lets more of each executor move into the common module.
- **Subaru TCU CVT.** EcuFlash security and reflash/retry functions for the
  Hitachi M32R and MH8111 families are unmapped; legacy line citations establish
  only crypto table provenance.

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
