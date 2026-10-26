# Denso BEEF CAN deepening design

Date: 2026-10-26. Status: proposed; implementation requires approval of this
design. Governing decisions:
[ADR 0029](../../adr/0029-lower-nloc-with-sonar-nominated-right-sized-abstractions.md),
[ADR 0024](../../adr/0024-wire-transcripts-and-goldens-are-the-test-contract.md),
and [ADR 0015](../../adr/0015-caller-owns-flash-transport-lifetime.md).

## Intent

Turn the abstractions produced by the Denso BEEF CAN deduplication series into
two deep modules:

- one BEEF kernel module used by the SH7058 petrol, SH7058 diesel and Denso
  SH705x TCU flash families; and
- one ECU execution module that owns the complete cold-path and warm-kernel
  order shared by the petrol and diesel families.

The TCU keeps its own executor order because its initial probe, session entry
and post-upload handoff differ on the wire. It uses the same kernel module once
the kernel operation begins.

This improves locality without inventing a configurable state machine. The
existing family executor interface remains the caller and test seam.

## Why now

The `sonar-dedup` PRs removed substantial copied code but stopped at an
intermediate architecture:

- [`RunBeefKernelOperation`](../../../src/backend/flash/ecu/denso_beef_can_common.h)
  takes a context and five behavioural callbacks. Its interface exposes the
  operation order it is meant to hide.
- [`BeefCanSession`](../../../src/backend/flash/ecu/beef_can_session.h) exposes
  the individual query, identity, compare, initialization and write operations.
  Its implementation delegates back into the 875-line template header.
- the three family executors still build nearly identical context aggregates
  and wire the callback runner themselves;
- the TCU separately implements a BEEF read and real-write sequence whose
  request bytes, ordering, sizes and timeouts match the shared petrol/diesel
  implementation.

Understanding one warm-kernel operation therefore crosses the executor,
session class and template header seams. The deletion test says the underlying
shared behaviour is valuable: deleting it would spread hundreds of lines back
across the families. The problem is the breadth and placement of its interface,
not the existence of sharing.

## Resolved wire model

The architecture follows the current scripted wire evidence, not file
similarity.

| Operation | Proven relationship |
| --- | --- |
| Initial kernel probe | ECU families send one BEEF kernel-ID request. TCU sends proprietary `7A A0`, retries up to five times and drains fragmented replies. |
| Identity | ECU families request ECU ID, VIN, CAL ID and CVN. TCU requests ECU ID and CAL ID. |
| Session and security | ECU families tolerantly probe sessions `03` and `43`, perform security, then select programming session `02` or `42`. TCU requires session `03`, performs security, then requests session `02`. |
| Kernel payload and block transfer | All three use the same 128-byte padding, checksum complement, encryption, `34 04 33` download framing, `B6` blocks and final address-only `B6`. Timing differs by family. |
| Post-upload handoff | ECU families wait before routine start and use the BEEF kernel-ID request. TCU restarts ISO-15765, waits 500 ms and repeats its proprietary probe. |
| Read | All three use BEEF opcode `03`, `00 + U24 address + U16 0400`, 0x400-byte pages and a 2000 ms reply timeout. |
| CRC compare | All three use the same opcode `02` request and 200 ms stale-frame drain. |
| Flash initialization | The bytes and order match. TCU uses a 2000 ms timeout and uppercase size rendering; ECU families use 500 ms. |
| Real write | All three use voltage `04`/500 ms, erase `25`/3000 ms, 0x200-byte writes through `22`/800 ms, 0x1000-byte commits through `24`/3000 ms, then compare again. |
| TestWrite | Petrol and diesel additionally initialize test mode and validate through opcode `23`. TCU plan admission rejects TestWrite before transport I/O. |

The families remain members of one Denso BEEF CAN dialect because they share
these named wire operations. Dialect membership does not assert one complete
cold-path order.

## Goals

- Give the shared warm-kernel protocol one deep module with a three-operation
  interface: transfer the kernel payload, read a region, and write a plan.
- Give the petrol/diesel execution order one owner instead of a callback
  interface assembled by both family executors.
- Keep the TCU's different cold path visible and family-owned.
- Remove the duplicated TCU read and real-write implementations.
- Hide BEEF framing, CRC, flash initialization, write windows and template
  implementation details behind the kernel module seam.
- Preserve every current test case in the initial refactor. Tests may be
  mechanically retargeted when an implementation type disappears, but no case
  or assertion is deleted.
- Deliver the work as `sonar-dedup` PRs, each reducing first-party NLOC by at
  least 10 and satisfying every ADR 0029 gate.

## Non-goals

- No wire-byte, ordering, timeout, delay, retry, address-window or
  abort/continue change.
- No all-three configurable executor, operation selector, callback bundle or
  behaviour-bearing profile.
- No change to plan construction, capability admission or transport lifecycle.
- No family-test compression. That remains a separate assessment under
  ADR 0024.
- No new port. Existing transport, channel, clock, cancellation and event
  interfaces already have production and scripted test adapters.
- No hardware qualification claim. Passing scripted wire tests is not bench
  evidence.

## Constraints

- Backend code remains portable: no Qt, owned threads or direct filesystem I/O.
- The caller continues to configure, open and close the transport under
  ADR 0015. A protocol-owned TCU restart stays inside its cold path.
- Errors cross seams only as `Result` or `Status`; backend exceptions do not
  cross them.
- Profiles contain pure facts such as wording and timeouts. They do not contain
  functions, operation selectors or family-mode flags.
- Plan types choose compile-time family facts. Invalid combinations must not be
  representable through a runtime family enum.
- Existing family executor tests remain the principal wire evidence. Shared
  module tests supplement them and never compute expected bytes with production
  encoders.
- The [share-only-proven-equivalence rule](../../design-notes.md#share-only-proven-protocol-equivalence)
  governs every move.

## Approaches considered

### One overloaded execution facade

One `DensoBeefCanExecution` class would expose a `Run` overload for each of the
three plan types. It would contain one ECU sequence and one TCU sequence over a
shared private kernel implementation.

This gives the smallest interface and maximum encapsulation. It also places two
different cold paths in one large compilation module, couples that module to all
three plan types, and makes its shared class name look more uniform than its
wire order really is.

### Capability-oriented package modules

Separate ECU and TCU executors would use a kernel module, an upload core, and
separate ECU/TCU handoff modules. A future family could reuse any subset.

This maximizes extension points, but it risks replacing the current fragmented
call graph with more shallow internal modules. The upload core has only one
current consumer on each side of an artificial seam unless it remains private
to the kernel implementation.

### Shared ECU execution plus one kernel module — selected

The existing family-named executors remain adapters at the established
`ICanFlashExecutor` seam. Petrol and diesel delegate their `Run` implementation
to one fixed-order ECU execution module. The TCU retains its own fixed-order
cold path. Both enter the same BEEF kernel module for the proven-equivalent
operations.

This gives the common petrol/diesel caller the most leverage, keeps the TCU
sequence honest, and creates only one new internal seam justified by three
adapters. It is the selected approach.

## Target module structure

```text
ICanFlashExecutor seam
├── SubaruDensoSh7058CanExecutor ───────┐
├── SubaruDensoSh7058CanDieselExecutor ─┴─> Denso BEEF CAN ECU execution
│                                           └─> BEEF kernel
└── SubaruTcuDensoSh705xCanExecutor
    └─> TCU cold path ──────────────────────> BEEF kernel
                                                └─> UDS channel / CAN transport
```

The family-named executors continue to own their existing setup and
pre-transport hooks. They are adapters from dispatch's existing family-specific
types to the deep execution modules; they do not expose new BEEF concepts to
callers.

### BEEF kernel module

The package-owned module has this conceptual interface:

```cpp
struct BeefKernelTransferPolicy
{
    uds::ExchangePolicy download_policy;
    std::chrono::milliseconds block_reply_timeout;
};

class BeefCanKernel
{
  public:
    BeefCanKernel(ICanFlashTransport&, IClock&, const ICancellationToken&,
                  IEventSink&, uds::IUdsChannel&,
                  const DensoBeefCanProfile&);

    Status TransferPayload(const KernelImage&,
                           const BeefKernelTransferPolicy&);
    Result<bytes::Bytes> Read(const MemoryRegion&, PhaseReporter&);
    Status Write(const FlashPlan&, PhaseSequence&,
                 PhaseReporter& compare_progress);
};
```

The exact C++ file split is left to the implementation plan, but this interface
shape is fixed:

- `TransferPayload` owns payload validation, padding, checksum complement,
  encryption, the `34 04 33` download request, every 128-byte `B6` block and the
  final address-only `B6`. It stops before transfer exit, routine start and
  post-upload verification, because those belong to the distinct ECU and TCU
  handoffs.
- `Read` owns opcode `03`, the fixed page request, 0x400-byte loop, timeout,
  cancellation, progress and returned bytes.
- `Write` owns CRC comparison, flash initialization, voltage query, erase,
  buffer writes, validate/commit, progress, recovery diagnostics and final
  comparison. TCU reaches it only with Write because its plan rejects
  TestWrite; ECU plans may reach it with TestWrite.

The module also hides BEEF request framing, reply parsing, strict kernel
exchange behavior and stale-frame draining. Its profile retains only pure data:
device/family wording, reply rendering, flash-initialization timeout and size
rendering. Transfer timing that differs by family is supplied as pure policy
data; it does not select an operation.

The current `BeefCanSession` implementation is absorbed rather than layered
under `BeefCanKernel`. Its warm-kernel methods and the relevant
`denso_beef_can_common.h` templates become the kernel implementation. Cold-path
identity and session probes stay private to the ECU or TCU execution source.

### Denso BEEF CAN ECU execution module

The internal module owns one fixed order:

1. Probe for the running BEEF kernel.
2. If absent, request ECU ID, VIN, CAL ID and CVN.
3. Prepare any petrol security state, including RaceRom-alt RAM reads.
4. Tolerantly probe bootloader sessions `03` and `43`.
5. Request the security seed and send the plan-selected key.
6. Enter programming session `02` or `42` according to the accepted session.
7. Ask the kernel module to transfer the payload.
8. Perform the ECU transfer-exit, start-delay, routine-start and required
   post-upload BEEF kernel-ID exchange.
9. Ask the kernel module to read or write.

Its conceptual entry point is:

```cpp
template <typename Plan>
Result<FlashExecutionResult>
RunDensoBeefCanEcu(const Plan& family, const FlashPlan& plan,
                   ICanFlashTransport& transport, IClock& clock,
                   const ICancellationToken& cancellation,
                   IEventSink& events);
```

Only the admitted petrol and diesel plan types instantiate it. Compile-time
specializations provide pure family facts and the security calculation. They
are implementation seams, not arguments to the entry point. There is no
callback for probe, connect, upload, read or write.

Petrol's security specialization owns stock, EcuTek, RaceRom, RaceRom-alt and
Cobb key calculation plus the optional RAM preparation. Diesel's specialization
owns the stock Denso key. A family whose cold-path order differs does not gain a
specialization; it keeps a separate executor, as the TCU does.

### TCU execution

The TCU executor retains this order:

1. Run the proprietary `7A A0` probe with its five attempts and bounded fragment
   drain.
2. If absent, request ECU ID and CAL ID.
3. Require diagnostic session `03`.
4. Perform the TCU seed/key exchange and request programming session `02`.
5. Ask the kernel module to transfer the payload.
6. Perform transfer exit and routine start.
7. Restart ISO-15765, wait 500 ms and strictly repeat the proprietary probe.
8. Ask the kernel module to read or write.

These steps remain code, not data. The outer phase/result lifecycle may stay
locally expressed if sharing it would recreate `RunBeefKernelOperation`'s wide
callback interface.

## Error and cancellation behavior

- Every existing checkpoint remains before the same request or loop iteration.
- Cancellation and disconnection remain terminal wherever the current family
  treats them as terminal.
- Tolerated malformed, missing and stale replies remain tolerated by the same
  family operations.
- Kernel-transfer errors return before the family-specific exit/start handoff.
- A post-erase write failure retains the current kernel-recovery warning.
- Plan admission still occurs through `FamilyExecutor` before any transport
  interaction. TCU TestWrite remains rejected there.
- Non-wire wording may be normalized only where ADR 0029 permits it and tests
  are updated deliberately; it is not an objective of this work.

## Tests and evidence

No current test case is deleted in either PR.

The petrol, diesel and TCU executor suites remain the end-to-end test surface
for bytes, order, timeouts, retries, cancellation, continuation policy, progress
and results. In particular, they already distinguish:

- the ECU and TCU kernel probes;
- tolerant ECU session discovery from required TCU session entry;
- family upload timings and TCU restart;
- identical read boundaries and real-write exchanges; and
- ECU TestWrite from TCU's admission rejection.

Focused `BeefCanSession` tests retain every assertion while being retargeted to
the kernel or ECU execution owner when the old type disappears. Tests added for
the new deep interface assert observable results and wire traffic, not private
state.

Plan goldens and scripted wire expectations do not change to make the refactor
pass. A difference requires stopping the refactor and explaining whether the
current equivalence judgment was wrong.

Each delivery PR runs focused executor and shared-module suites while changing,
then the applicable full gates from the repository instructions. Each
`sonar-dedup` PR additionally requires:

- first-party NLOC reduction of at least 10;
- no surviving Mull mutant on changed lines;
- changed-line coverage;
- no new duplicated lines; and
- an independent right-sizing review against ADR 0029.

## Delivery

### PR 1: deepen the BEEF kernel module

- Introduce the final `BeefCanKernel` ownership and interface.
- Move payload preparation/download/block transfer, read, compare,
  initialization and write behind it.
- Migrate petrol, diesel and TCU callers.
- Delete the TCU's duplicated read and real-write loops.
- Remove absorbed template implementation from
  `denso_beef_can_common.h`; do not leave a forwarding layer.
- Correct the flash sharing contract and any stale source comments that claim
  the TCU read or real-write wire path differs.
- Preserve every test case and satisfy the `sonar-dedup` gates independently.

### PR 2: concentrate the ECU execution sequence

- Add the fixed-order `RunDensoBeefCanEcu` module and compile-time petrol/diesel
  security specializations.
- Migrate both ECU executors while retaining their named adapter types and
  lifecycle hooks.
- Keep the TCU cold path explicit and route its warm-kernel work through
  `BeefCanKernel`.
- Delete `RunBeefKernelOperation` and its behavioural callback interface.
- Absorb the remaining `BeefCanSession` and common-header implementation into
  the two deep owners; do not layer new modules over them.
- Update the glossary and flash sharing contract to name the resulting owners.
- Preserve every test case and satisfy the `sonar-dedup` gates independently.

If PR 1 cannot meet the NLOC gate with the final seam, stop and revise this
design rather than adding unrelated deletion. PR 2 does not merge merely to
complete the architecture if it cannot independently meet the same gate.

## Documentation ownership

- [FastECU flash language](../../../src/backend/flash/GLOSSARY.md) owns the
  distinction between dialect membership, the ECU cold path and the TCU cold
  path.
- [Flash sharing contracts](../../reference/flash-contracts.md) owns the final
  code-sharing coverage and preserved deviations.
- The [qualification matrix](../../flash-qualification-matrix.md) remains the
  only owner of hardware status; this work changes none.
- This proposed design leaves the checkout after both PRs are complete and its
  enduring knowledge has moved to those owners, with its original contents
  preserved in delivered Git history.

## Left to the implementation plan

- Exact source filenames and Bazel target splits within
  `src/backend/flash/ecu`.
- Whether the pure payload/framing functions remain private free functions or
  private static members of the kernel implementation.
- The mechanical order for moving existing tests without weakening assertions.
- Focused Bazel target names and the per-PR mutation scopes.

Those choices may change implementation mechanics but may not widen the three
kernel operations, add behaviour to profiles, combine the ECU and TCU cold
orders, or weaken the delivery gates.
