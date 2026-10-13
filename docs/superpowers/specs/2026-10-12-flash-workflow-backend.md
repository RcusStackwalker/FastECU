# Flash workflow staging moves into the backend: Spec

Agreed design for moving the flash workflow module
(`src/platform/desktop/common/flash/flash_workflow.*`) into the backend behind
two ports. It came out of the 2026-10-12 architecture review (candidate 01) and
a grilling session. Read this before writing the plan for either PR.

- Tech-debt item: "Move reusable flash routing, preparation, prompt/stage
  policy into backend flash workflows" (`docs/tech-debt.md`, P1).
- Binding constraints: design-notes "Operator decisions sit outside
  executors", "Share only proven protocol equivalence", "The caller owns
  transport lifetime" (ADR 0015), backend layering (ADR 0016), package-owned
  mocks (ADR 0008), gazelle-managed BUILD files (ADR 0017).

## Why

`flash_workflow.cpp` (1186 lines) is nearly all portable policy:
- the protocol routing table `kRoutes`;
- prompt sequencing;
- the EEPROM mode 2→3→4 retry;
- Unisia programming-voltage (VPP) and boot-mode staging;
- kernel resolution for MC68 and BDM.

It sits in the platform layer only because the module constructs four
platform things inline:

| Platform dependency | Where |
| --- | --- |
| `QtFileRepository` (kernel reads) | 5 sites (`:478, :625, :659, :679, :884`) |
| `QtClock`, one per attempt | each `FlashAttempt` step (`:249, :517, :741, :896, :901`) |
| `Desktop{Kline,Can,MixedCan}FlashTransport(request.serial)` | every attempt binding |
| `AdapterSuppliesProgrammingVoltage(request.serial)` | the Unisia K-Line plan's constructor (`:205`) |

Its 95 tests (`flash_workflow_test.cpp`, 2994 lines) therefore need a
`QCoreApplication`, `QTemporaryDir` kernel files and a `SerialPortActions`
over `FakeBackend`, even though what they test is portable policy.

## What does not change

- **The `FlashWorkflow` interface:** `Next()` / `Submit(FlashPromptResponse)` /
  `Submit(FlashAttemptResult)`, the `FlashWorkflowStep` variant,
  `FlashPromptKind`, `FlashPromptResponse` and `FlashWorkflowOutcome`.
- **Behaviour:** no change to step order, prompts, routing, plan building or
  wire behaviour.
- **Structure:** each staging shape stays its own class, and operator
  decisions stay between bounded attempts, outside executors.
- **Binding:** `BindFlashAttempt` keeps its compile-time executor/transport
  pairing. Its requires-clause
  (`std::derived_from<Transport, Executor::TransportType>`) holds when
  `Transport` is the backend interface type itself.

## Design

### Flash transport source (new port)

A new backend port, `fastecu::flash::IFlashTransportSource`. New glossary term
for `src/backend/flash/CONTEXT.md`:

> **Flash transport source**:
> The connected adapter as the flash workflow sees it: it provides unopened
> K-Line, CAN or mixed-CAN flash transports and reports whether it supplies
> programming voltage itself.
> _Avoid_: Transport factory, flash adapter

```cpp
class IFlashTransportSource
{
  public:
    virtual ~IFlashTransportSource() = default;
    virtual std::unique_ptr<IKlineFlashTransport> MakeKline() = 0;
    virtual std::unique_ptr<ICanFlashTransport> MakeCan() = 0;
    virtual std::unique_ptr<IMixedCanFlashTransport> MakeMixedCan() = 0;
    virtual bool SuppliesProgrammingVoltage() const = 0;
};
```

- The transports it returns are unopened. The attempt configures, opens and
  closes them exactly as it does today (ADR 0015).
- **Desktop adapter.** `DesktopFlashTransportSource` lives in
  `src/platform/desktop/common/transport` and borrows `SerialPortActions*`
  (which may be null, as today). It absorbs
  `AdapterSuppliesProgrammingVoltage`: the free function is deleted, and its
  three assertions (null → false, not OpenPort 2.0 → false, OpenPort 2.0 →
  true) move to the source's test.
- **Scripted adapter (PR 2).** `ScriptedFlashTransportSource` lives in
  `src/backend/flash/testing`.
  - For each kind it returns a fresh default scripted transport, unless the
    test pre-loaded a transport for that kind.
  - It records the requested kinds in order.
  - The VPP answer is a constructor flag, default false.

### Workflow ports and ownership

```cpp
struct FlashWorkflowPorts
{
    std::unique_ptr<IFileRepository> files;
    std::unique_ptr<IFlashTransportSource> transports;
};

static std::unique_ptr<FlashWorkflow> TryCreate(FlashWorkflowRequest request, FlashWorkflowPorts ports);
```

- **Ownership.** The workflow owns its ports, so their lifetime equals the
  workflow's and they cannot dangle.
- **Request.** `FlashWorkflowRequest` loses its `serial` field.
- **Caller.** `FlashOperationController` builds the desktop ports: a
  `QtFileRepository` and a `DesktopFlashTransportSource(&serial_)`. It does
  this through `DesktopFlashWorkflowPorts(SerialPortActions*)` in
  `src/platform/desktop/common/flash`, because the transport package is not
  visible to `src/ui/desktop/flash/operation` (ADR 0016). After PR 2, that
  helper stays in the platform layer.
- **`TryCreate` contract.** A null result still means only "no portable
  family owns this protocol".

### Clock

- **`FlashAttempt`** keeps its name and loses its clock:
  `struct FlashAttempt { std::unique_ptr<BoundFlashAttempt> attempt; };`
- **`FlashWorker`** becomes
  `FlashWorker(std::unique_ptr<BoundFlashAttempt>, std::unique_ptr<IClock>, QObject*)`
  and keeps owning its clock. It takes the bound attempt rather than
  `FlashAttempt`, so the worker no longer depends on the workflow package (a
  refinement made while planning PR 1).
- **`FlashDialog`** takes a clock factory
  (`std::function<std::unique_ptr<IClock>()>`) and calls it once per attempt.
  - Production passes a `QtClock` maker.
  - The dialog tests pass a `FakeClock` maker.

### Small fixes in scope (PR 2)

- **`config::KernelFileHandle(const ConfigPaths&, std::string_view kernel)`**
  in `src/backend/config/config_paths.h` replaces the three concatenations of
  `kernel_files_directory` and `kernel`: `flash_workflow.cpp:171, :186` and
  `eeprom_read_plan.cpp:117`. It relies on the trailing-slash invariant at
  `config_paths.cpp:19`.
- **The stale comment at `flash_types.h:113-118`** is fixed. It says
  declining means the dialog never builds a plan, but plans are now built
  before the prompts.

## Slicing

### PR 1 — seams in place

The module stays at `src/platform/desktop/common/flash`.

- Add `IFlashTransportSource` (backend) and `DesktopFlashTransportSource`
  (platform). Delete `AdapterSuppliesProgrammingVoltage` and move its
  assertions.
- Add `FlashWorkflowPorts`. `TryCreate` takes it, and `FlashWorkflowRequest`
  loses `serial`.
- Inside the module, replace every inline `QtFileRepository`, `Desktop*Transport`
  and `AdapterSuppliesProgrammingVoltage` with the ports.
- Move the clock out of `FlashAttempt`: `FlashWorker` takes a clock, and
  `FlashDialog` takes a clock factory.
- Update `FlashOperationController`, `builtin_catalog_capability_test` and the
  `FlashDialog` / `FlashWorker` tests.
- The 95 workflow tests get only minimal edits: they pass a desktop source over
  their existing `FakeBackend` serial plus a `QtFileRepository`, and read the
  clock from a local `FakeClock` or `QtClock` instead of `attempt.clock`.

### PR 2 — relocation

- Move `flash_workflow.h/.cpp` and the test to `//src/backend/flash/workflow`.
  `PortableImageForOperation` moves with the header. gazelle_cc generates the
  BUILD file.
- Switch the tests to `InMemoryFileRepository` and
  `ScriptedFlashTransportSource`. With that, Qt, `QTemporaryDir` and
  `FakeBackend` leave the tests.
- Convert the four tests that set serial-level `FakeBackend` expectations
  (`petrolRead…`, `dieselRead…` and `tcuRead…BindsDesktopCanTransport`, plus
  `routesSh7058ReadAndWriteWithPreTransportPrompts`). Each now asserts the
  recorded transport kind and the scripted transport's recorded
  `Iso15765Config` (ECU request/response 0x7E0/0x7E8; TCU 0x7E1/0x7E9).
- Add `KernelFileHandle`, fix the `flash_types.h` comment, and add the
  `CONTEXT.md` entry.
- `builtin_catalog_capability_test` stays in the platform layer because it
  reads the real bundled kernel files through `QtFileRepository`.

## Out of scope

- **Candidate 02:** routing derived from per-family descriptors.
- **Candidate 03:** plans valid by construction, and required versus granted
  confirmations.
- **New coverage:** new workflow-plus-executor integration tests against
  scripted transports.
- **Candidate 04:** the two places where MainWindow adds a trailing slash to
  the kernel path (`mainwindow.cpp:901, :926`).
