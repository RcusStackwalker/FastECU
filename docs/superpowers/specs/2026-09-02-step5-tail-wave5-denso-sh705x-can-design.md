# Step 5 Tail Wave 5 — Denso SH705x CAN — Design

**Status:** Approved for implementation planning on 2026-09-02 and
revalidated on 2026-09-07 against `master` at `59f4e442`, after Wave 4 reduced
`//:legacy_flash_drain` to 14 families. The four Wave 5 legacy operation
sources are unchanged from the original `cd9ab679` evidence baseline.

## Goal

Port the four Wave 5 Denso SH705x CAN flash families from Qt-bound legacy
operations to portable plans and synchronous executors:

- `FlashEcuSubaruDensoSH7058Can`
- `FlashEcuSubaruDensoSH7058CanDiesel`
- `FlashTcuSubaruDensoSH705xCan`
- `FlashEcuSubaruDensoSH705xDensoCan`

The wave ends with their legacy operations and flash dialogs removed, their
portable targets in the enforced portable closure, and the flash-drain ratchet
reduced from 14 families to 10. Automated evidence changes each qualification
row from `unqualified` to `experimental`; this wave does not claim hardware
qualification.

This is Wave 5 of the
[step-5 tail flash drain](2026-08-08-step5-tail-flash-drain-design.md). It
retains that design's family-per-PR and port-then-factor rules while correcting
one transport description: DensoCAN is not a raw-CAN-only family.

## Findings That Shape the Design

### The wave contains two transport shapes

Three families remain in ISO-15765 mode throughout execution:

- SH7058 petrol ECU, including its stock, EcuTek, RaceRom, RaceRom-alt, and
  Cobb security variants;
- SH7058/SH7059 diesel ECU;
- SH7055/SH7058 TCU.

They use a mixture of standardized diagnostic services, KWP-style services,
vendor services, and FastECU's custom kernel protocol. ISO-15765 describes the
transport; it does not make every application command UDS. Exchanges that
follow UDS response and NRC rules may use `CanFlashUdsChannel` and
`uds::UdsClient`; KWP, vendor, tolerant, and custom-kernel exchanges remain
explicit.

`FlashEcuSubaruDensoSH705xDensoCan` is different. Its observable sequence is:

1. Probe an already-running kernel over 11-bit ISO-15765 using the custom
   `BEEF` kernel envelope.
2. If no kernel responds, reset/reopen in 29-bit raw CAN mode and run the
   proprietary Denso bootloader and six-byte kernel upload.
3. Jump to the uploaded kernel, reset/reopen in 11-bit ISO-15765 mode, and use
   the custom `BEEF` kernel protocol for ROM operations.

The raw bootloader uses explicit arbitration IDs and eight-byte payloads such
as `FF86`, `7A90`, `7A98`, `7AB0`, and `7AA0`. The later `BEEF` protocol is
proprietary, not UDS. The plan and qualification matrix therefore describe
this family as **raw CAN followed by ISO-15765**, and its transport kind is
`CanRawIso15765`, not the tail design's previously proposed `CanRaw`.

### The TCU chooser is independent of its flash executor

The recent service-functions work removed TCU actions 2–4 from the legacy
flash operation. The remaining flash operation handles only ROM dump/write,
but its old dialog still owns a four-way read chooser:

- Dump
- Relearn
- Read Parameters
- Set Parameters

Registering the TCU protocol in `FlashWorkflowFactory` without moving this
chooser would bypass the three service-function paths. Wave 5 therefore moves
the choice ahead of portable flash routing. The maintenance choices continue
to use the existing portable `ServiceFunctionDialog`; only Dump enters the new
flash workflow.

## Scope

### In scope

- Four portable plan/executor pairs and their tests.
- Exact workflow routes for every currently configured protocol served by the
  four legacy classes.
- A capability-specific mixed-CAN flash transport, desktop adapter, and
  scripted test transport for DensoCAN.
- Moving the TCU action chooser ahead of flash routing without changing its
  four choices.
- Deleting the four legacy operations and their family-specific flash dialogs.
- Proven local defect corrections under the evidence rule below.
- Per-family qualification, ratchet, build graph, and portable-closure updates.
- A final cluster comparison and optional factoring pass.
- Correcting the tail design's raw-only DensoCAN description.

### Out of scope

- Bench or vehicle qualification; every migrated family remains
  `experimental`.
- Wave 6 or Wave 7 families.
- A universal CAN or universal flash state machine.
- Broad `MainWindow` or `FileActions` cleanup outside the routing needed by
  these four families.
- Changes to the already-portable TCU service-function sessions.
- Normalizing unexplained behavior across sibling families.

## Architecture

### Family plans and executors

Each family owns focused files under `src/backend/flash/ecu/`:

- `<family>_types.h`
- `<family>_plan.{h,cpp}`
- `<family>_executor.{h,cpp}`
- co-located plan and executor tests

Each plan stores immutable ROM and kernel snapshots plus only the wire and
geometry values needed by its executor. `flash_types.h` assembles the four new
plan alternatives into `FamilyPlan`; it does not absorb their fields.

Master now contains `single_window_plan`, but Wave 5 must not widen or reuse
it. That core deliberately requires kernel-free plans, rejects test-write,
and models one write/erase window. Every Wave 5 family carries a kernel; three
support test-write; and the Denso SH705x families preserve 16-block geometry.
Their builders therefore stay family-owned. A later extraction is allowed
only by the same port-then-factor evidence rule as executor sharing.

The plan types are `SubaruDensoSh7058CanPlan`,
`SubaruDensoSh7058CanDieselPlan`, `SubaruTcuDensoSh705xCanPlan`, and
`SubaruDensoSh705xDensoCanPlan`, following the existing family naming pattern.

The three ISO-only executors implement `ICanFlashExecutor` and use the existing
`ICanFlashTransport`. DensoCAN implements a separate mixed-CAN executor
contract so an ISO-only adapter cannot be paired with it.

All four executors remain synchronous, bounded, cancellable, dialog-free, and
filesystem-free. They never see `EcuCalDefStructure` and never configure,
open, or close their outer transport directly.

### Mixed-CAN transport

Add a narrow `IMixedCanFlashTransport`, `IMixedCanFlashExecutor`, and
`MixedCanConfig`. The executor interface mirrors the existing typed executor
contracts: `TransportType` is `IMixedCanFlashTransport`, `ConfigType` is
`MixedCanConfig`, `transport_setup()` is pure, and `execute()` receives only a
validated plan and already-open matching transport. Its setup contains both
validated configurations:

- kernel mode: 500 kbit, 11-bit ISO-15765, request `0x7E0`, response `0x7E8`;
- bootloader mode: 500 kbit, 29-bit raw CAN, including the legacy transmit ID
  and receive filter/address values.

The transport exposes distinct operations for ISO-15765 messages and raw CAN
frames, plus semantic mode transitions and receive-buffer clearing. Raw frames
reuse the existing `cdbg::CanFrame` value shape rather than inventing a second
ID/payload model.

`BoundFlashAttempt` still performs the initial configuration/open and the
final close. Initial configuration enters ISO-15765 kernel mode. The executor
may then call:

- `enter_raw_bootloader_mode()` after a failed kernel probe;
- `clear_receive_buffer()` after the bounded wake sequence;
- `enter_iso15765_kernel_mode()` after the raw bootloader jumps to the kernel.

Those transitions encapsulate the required desktop reset, mode setters, and
reopen. They are protocol operations under
[ADR 0015](../../adr/0015-caller-owns-flash-transport-lifetime.md): their
number and position are fixed by the ECU state machine, while ownership of the
outer lifetime remains with the caller.

The existing ISO-only interface and its current executors do not gain raw-CAN
methods. The new capability stays unrepresentable for families that do not
need it.

### Desktop composition

The desktop platform supplies:

- the existing `DesktopCanFlashTransport` for the three ISO-only families;
- `DesktopMixedCanFlashTransport` over `SerialPortActions` for DensoCAN;
- `ScriptedMixedCanFlashTransport`, which records configurations, mode
  changes, raw frames, ISO messages, buffer clears, cancellation, and
  lifecycle calls.

`FlashWorkflowFactory` constructs the correct executor/transport pair and
hands the bound attempt to the existing `FlashWorker` and `FlashDialog`.
The new workflow reuses the existing `FlashAttemptOutcome` result-state
helper extracted on master; it does not duplicate terminal/failure/result
handling. Neither the UI nor the workflow manipulates CAN modes.

## Protocol Routing and Capability Matrix

Routing is exact. Broad `starts_with("sub_ecu_denso_sh7058_can")` and
`ends_with("_densocan")` ownership does not move into the portable registry,
because it would silently claim future protocols that have not been designed
or tested.

### SH7058 petrol ISO-15765

Accepted IDs:

- `sub_ecu_denso_sh7058_can`
- `sub_ecu_denso_sh7058_can_ecutek`
- `sub_ecu_denso_sh7058_can_ecutek_racerom`
- `sub_ecu_denso_sh7058_can_ecutek_racerom_alt`
- `sub_ecu_denso_sh7058_can_cobb`

All five accept read, test-write, and write with MCU `SH7058`. Security choice
is an explicit plan value; it is not inferred repeatedly inside the executor.

### SH7058/SH7059 diesel ISO-15765

Accepted IDs:

- `sub_ecu_denso_sh7058_can_diesel` with MCU `SH7058d`
- `sub_ecu_denso_sh7059_can_diesel` with MCU `SH7059d`

Both accept read, test-write, and write. Their kernel addresses, images,
geometry, seed behavior, and timing remain per-protocol values.

### SH705x TCU ISO-15765

Accepted IDs:

- `sub_tcu_denso_sh7055_can` with MCU `SH7055`: read only;
- `sub_tcu_denso_sh7058_can` with MCU `SH7058`: read and write.

Both reject test-write as `Unsupported`. SH7055 rejects write before any
transport call.

### SH705x DensoCAN mixed transport

Accepted IDs:

- `sub_ecu_denso_sh7055_densocan` with MCU `SH7055`
- `sub_ecu_denso_sh7058_densocan` with MCU `SH7058`
- `sub_ecu_denso_sh7058s_densocan` with MCU `SH7058`
- `sub_ecu_denso_sh7058s_diesel_densocan` with MCU `SH7058`
- `sub_ecu_denso_sh7059_diesel_densocan` with MCU `SH7059d`

All five accept read, test-write, and write. Kernel identity, load address, and
flash geometry come from the exact catalog entry and validated flash-device
record.

## Workflow and UI Data Flow

The common kernel-backed CAN workflow performs these steps:

1. Resolve the exact protocol entry through the portable protocol catalog.
2. Read and snapshot the kernel through `QtFileRepository`.
3. Build and validate the immutable plan before any hardware access.
4. Present the `Begin` prompt and every plan confirmation.
5. Bind the plan and executor to the ISO-only or mixed-CAN desktop transport.
6. Run the bound attempt on `FlashWorker`.
7. Return read bytes and ROM ID through `FlashCompletedStep` and the existing
   dialog result mapping.

`KernelBackedCanFlashWorkflow` represents this desktop-only control shape. It
is parameterized by plan builder, executor, and transport factory; it does not
share executor wire logic. Both ISO-only and mixed-CAN families use it.

DensoCAN's ignition-cycle confirmation is always collected before execution.
Legacy asks only after an ISO kernel probe fails, but a portable synchronous
executor cannot pause for a GUI decision. An already-running kernel therefore
causes one extra up-front confirmation without changing wire traffic.

For a Denso TCU read request, a small service-functions UI helper runs before
`FlashWorkflowFactory`:

- Dump continues into the portable flash workflow, whose `Begin` prompt is
  the ignition gate.
- Relearn, Read Parameters, and Set Parameters launch the existing
  `ServiceFunctionDialog` only after the helper presents the existing ignition
  gate, then return without constructing a flash workflow.
- Cancelling either the action choice or ignition preflight performs no
  transport I/O.

TCU write bypasses the chooser and enters the flash workflow directly. Once
this routing exists, the old TCU flash dialog can be deleted rather than
retained as a wrapper.

## Error Handling

Plan construction fails before hardware access for unknown protocol/MCU
pairs, missing kernels, invalid kernel ranges, wrong ROM sizes, unsupported
operations, inconsistent transport fields, and out-of-range flash regions.

Executors use the existing taxonomy:

- `InvalidConfig` for invalid plans and catalog/geometry mismatches;
- `Timeout` for bounded reads or retry sequences that expire normally;
- `Disconnected` for configure/open failures and adapter loss;
- `BadResponse` for truncated envelopes, wrong IDs or commands, negative
  replies, and malformed lengths;
- `Cancelled` when cooperative cancellation is observed;
- `Unsupported` for unavailable operations;
- `Internal` only for invariant violations and unexpected adapter exceptions.

Every mixed-CAN mode transition is fallible and fail-closed. Execution never
continues after a transition whose resulting mode is uncertain.

The existing outer-lifecycle rule remains unchanged: once open succeeds,
close occurs exactly once; the execution error wins over a close error, and a
close-only error is returned. `request_unblock()` suppresses subsequent I/O
and releases any scripted blocking read.

Cancellation checkpoints cover every poll, retry, upload block, read page,
erase boundary, and write block. DensoCAN's 1,000-frame raw wake loop gains a
checkpoint per iteration. Cancellation after erase reports `Cancelled` and
retains the established warning that the kernel may still permit recovery.

## Behavior-Correction Rule

This design deliberately permits proven legacy defect corrections, but not
speculative sibling normalization. A correction is accepted only when all of
the following hold:

1. Local evidence proves a defect: an out-of-bounds access, malformed frame,
   impossible condition, ignored supported-operation flag, or contradiction
   with an authoritative local definition.
2. A focused test reproduces the defect or demonstrates the violated
   invariant.
3. The corrected behavior is pinned by byte-exact or outcome-exact tests.
4. The family's qualification-matrix notes cite the legacy behavior, the
   evidence, and the correction.

Similarity to a sibling is evidence for investigation, not authority to
change behavior. Unexplained differences in addresses, timeouts, retry counts,
response tolerance, seed logic, or ordering remain per family and receive
characterization tests.

Every transcribed wire exchange cites the legacy file and line range at
source revision `59f4e442`. Once the legacy file is deleted, those
citations and tests become the audit trail.

## Testing

### Plan tests

Each plan suite covers:

- every exact protocol ID and its required MCU;
- near-miss and unknown IDs;
- its read/test-write/write capability matrix;
- missing and incorrectly sized ROM images;
- kernel address, padding, and containing-region limits;
- transfer and erase geometry;
- transport kind and configuration;
- rejection before any transport interaction.

### Executor tests

Each executor has byte-exact scripted success tests for every supported
operation and every behaviorally distinct protocol variant. The suites cover:

- kernel-already-running and kernel-upload paths;
- stock, EcuTek, RaceRom, RaceRom-alt, and Cobb security behavior;
- SH7058/SH7059 and ECU/TCU geometry differences;
- timeout, disconnect, malformed/negative response, cancellation, and
  unsupported outcomes;
- per-family retries, timing, response tolerance, and test-write behavior;
- character-for-character operator log lines and ordered progress phases.

The mixed-CAN suite additionally proves:

- initial ISO-15765 kernel probing;
- no raw transition when the kernel is already alive;
- ISO-15765 → raw CAN → ISO-15765 ordering when upload is required;
- exact 29-bit IDs and eight-byte raw bootloader payloads;
- the bounded 1,000-frame wake, buffer clear, six-byte upload blocks, and
  `BEEF` kernel exchanges;
- cancellation during wake and upload;
- failure at either mode transition;
- initial configure/open once and final close exactly once.

### Desktop and guard tests

Desktop tests cover all four TCU chooser results, Dump-to-`FlashDialog`, each
service choice-to-`ServiceFunctionDialog`, and cancellation before I/O.
Factory tests cover all 14 accepted Wave 5 IDs plus near misses and preserve
the existing priority of explicitly routed EEPROM protocols.

Every family PR:

- registers its portable targets in both portable-closure lists;
- removes exactly one ratchet entry;
- updates its qualification row to `portable=yes`,
  `hardware_status=experimental`;
- proves changed guard behavior non-vacuously.

The full verification gates are:

```bash
bazel test --config=release //...
bazel build --config=release //:fastecu
prek run --all-files
bazel run //:clang_tidy_report_changed
```

`//:portable_closure`, `//:serial_compat_allowlist`, and
`//:legacy_flash_drain` are also run explicitly when their inputs change.
Because every family adds files under `src/backend`, `//:backend_no_widgets`
is part of each family gate and the full-wave gate.

## Delivery Sequence

Wave 5 is five reviewable changes:

1. **DensoCAN mixed transport and family port.** Add the capability-specific
   transport, desktop adapter, scripted transport, plan/executor, workflow
   route, and tests; remove the DensoCAN legacy operation and dialog. Resolving
   the wave's unique transport risk first prevents it from surfacing after the
   three routine ISO-only ports.
2. **Denso TCU port.** Add its ISO-15765 plan/executor, move the chooser ahead
   of flash routing, preserve all service-function paths, and delete the
   legacy TCU flash operation and dialog.
3. **SH7058 petrol port.** Cover all five security variants and delete the
   legacy family.
4. **SH7058/SH7059 diesel port.** Preserve its distinct MCU geometry, kernel
   addresses, timings, and seed behavior; delete the legacy family.
5. **Cluster close and factoring.** Compare the four tested portable
   implementations and extract only byte-identical, independently meaningful
   helpers. A no-common result is valid and still completes the wave.

Each family change removes legacy dispatch in the same commit that installs
portable routing; no merged state routes one protocol through both paths.
Each change independently shrinks the drain, so partial Wave 5 progress stays
mergeable.

## Cluster Factoring

Port-then-factor remains mandatory. Candidate common code includes custom
kernel envelope construction/validation, crypto tables, and small transfer
helpers, but this list is investigative, not a commitment.

The shared Wave 5 seed/index/encrypt data applies to stock security and padded
kernel uploads, not normal-ROM transformation. Every Wave 5 normal-ROM BEEF
read returns its payload bytes raw; DensoCAN also compares, buffers, validates,
and CRCs the caller's raw image. The shared decrypt table remains for its Wave
4 normal-ROM consumers and is not a Wave 5 read-path candidate.

The final pass may extract code only when the tested portable implementations
show an identical contract. It must not merge:

- raw/ISO mode switching with ISO-only execution;
- family-specific security selection;
- timeout or retry policies;
- response-tolerance differences;
- flash geometry or address indexing.

Any extracted unit states its dependencies and has its own focused tests. If
no useful common survives, the close change records that finding and leaves
the executors separate.

## Documentation and Completion

The cluster-close change:

- amends the tail design's `CanRaw` statement to describe the mixed
  raw-CAN/ISO-15765 sequence;
- marks Wave 5 merged in the modularization plan;
- records 10 remaining legacy families;
- records every proven correction and preserved hazard in the qualification
  matrix;
- leaves all four hardware statuses at `experimental`.

Wave 5 is complete when:

- the four legacy operation sources and family-specific flash dialogs are
  gone;
- their 14 exact protocol IDs route only to portable workflows;
- all portable targets are in the enforced closure;
- `//:legacy_flash_drain` reports exactly 10 remaining families;
- the full verification gates pass;
- the qualification matrix contains the automated evidence and no hardware
  claim.

## Risks and Mitigations

| Risk | Mitigation |
|---|---|
| DensoCAN is modeled as raw-only or UDS and loses one of its real modes | Dedicated mixed-CAN contract; explicit mode-order tests; documentation says raw CAN followed by proprietary kernel traffic over ISO-15765 |
| TCU portable registration hides service functions | Move the chooser before factory dispatch; desktop tests cover all four choices and cancellation |
| Clone similarity erases family-specific wire behavior | Port independently; factor last; preserve unexplained differences; require characterization tests |
| A legacy defect is "fixed" from intuition | Four-part local-evidence rule and mandatory matrix disclosure |
| A cancelled or failed mode switch leaves further traffic in an unknown mode | Every transition is fallible and terminal on failure; cancellation checked before subsequent I/O |
| Up-front DensoCAN ignition prompt differs from the conditional legacy prompt | Documented UI-only consequence required by a dialog-free executor; no wire traffic starts before acceptance |
| Automated tests are mistaken for hardware proof | Qualification stays `experimental`; bench work remains a separate follow-up |
