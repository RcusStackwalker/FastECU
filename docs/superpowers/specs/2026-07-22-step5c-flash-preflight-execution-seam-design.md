# Step 5c — Flash Preflight & Execution Seam — Design

**Status:** Approved 2026-07-22. Third sub-project of step 5. This design
inherits the fixed `Result`/`Error`, port, thread, and enforcement vocabulary
from the [step-5 umbrella design](2026-07-22-step5-backend-portable-design.md)
and the merged step 5a foundation (`14799c3`, PR #73).

**Repository baseline:** step 5a is on `master`. Step 5b is PR #78 and is not
merged into the inspected `master`; its design and feature-branch commits were
reviewed for patterns, but 5c does not assume its transport-interface changes
are present. If #78 merges first, 5c rebases onto its `Result`-based,
cancellation-aware `IKlineTransport` and `ICanTransport` signatures instead of
reintroducing compatibility overloads.

**Goal:** establish the complete portable flash seam: immutable and fully
validated `FlashPlan`s, dialog-free synchronous execution, platform-owned Qt
workers, structured transport/cancellation/error handling, one representative
K-Line family and one CAN family, enforceable portable closure, and a durable
qualification matrix. Backend flash code owns no thread, widget, filesystem,
mutable `FileActions` state, `SerialPortActions`, or synchronous user dialog.

Wire bytes, timing order, EEPROM-mode order, response rules, output bytes,
progress ordering, and existing desktop copy are compatibility contracts unless
this design explicitly identifies an unsupported legacy branch.

---

## Scope

### In scope

1. Portable flash models, common validation, prompt declarations, execution
   results, and executor interfaces under `src/backend/flash/`.
2. A family-tagged `FlashPlan` with typed family-specific details. There is no
   stringly typed property bag and no universal configurable flash state machine.
3. Synchronous, bounded, cancellable, dialog-free executors for the proving
   pair:
   - Subaru Denso SH705x EEPROM over K-Line;
   - Subaru Denso SH705x EEPROM over CAN/ISO-15765.
4. Result-based flash transport operations, checked desktop transport setup,
   bounded reads, cancellation unblocking, clock/event injection, and exception
   translation.
5. Moving the Qt flash worker and all still-unconverted Qt flash operation
   implementations to desktop-platform ownership.
6. Desktop snapshot/preflight adapters, worker/event adapters, prompt
   orchestration, result-to-legacy-ROM adaptation, and unchanged UI copy.
7. Portable-closure and serial-compat enforcement for the converted roots.
8. The [flash qualification matrix](../../flash-qualification-matrix.md),
   initially recording every current operation family and distinguishing
   migration from real-hardware proof.

### Out of scope

- Migrating any family other than the proving pair to portable execution.
- A universal flash protocol, executor selected through free-form strings, or a
  shared transfer loop whose equivalence has not been demonstrated.
- Definition parsing, calibration ownership, generic ROM/file use cases, or
  removal of `FileActions`; those are step 5d.
- MainWindow/dialog thin-shell redesign, composition-root consolidation, and
  removal of all desktop wrappers; those are step 6.
- Real-ECU qualification. Both proving families remain `experimental` in docs;
  no badge, extra warning, or changed confirmation appears in the UI.
- JNI, Kotlin, a native ABI, Android USB, or exposing C++ plan layouts across an
  ABI; those are step 7 (called Step 6 in older planning discussions, but step 7
  in the current modularization roadmap).

---

## Repository evidence and proving pair

### Chosen pair

The proving pair is:

| Transport | Family ID | Current implementation |
|---|---|---|
| K-Line | `DensoSh705xEepromKline` | `src/backend/flash/eeprom/eeprom_ecu_subaru_denso_sh705x_kline_operation.*` |
| CAN/ISO-15765 | `DensoSh705xEepromCan` | `src/backend/flash/eeprom/eeprom_ecu_subaru_denso_sh705x_can_operation.*` |

These are the right seam proof for repository-specific reasons:

- They are parallel SH705x EEPROM workflows with the same three-mode user
  decision loop, so tests can prove both common planning and genuinely separate
  transport state machines without inventing a universal abstraction.
- They exercise both required transport categories and the hardest prompt case:
  the user must inspect bytes produced at runtime before choosing Save or trying
  the next EEPROM mode.
- Together they are the only two operations in
  `src/backend/flash/eeprom/`. Migrating both makes that whole package portable
  and removes the exact serial allowlist entry
  `//src/backend/flash/eeprom:__pkg__`; a one-file choice in the aggregate
  `ecu` package would not honestly drain a package-level allowlist entry.
- They exercise kernel upload, security variants, bounded chunked reads,
  progress, elapsed-time diagnostics, malformed/negative responses, adapter
  setup, and cancellation. A small handshake-only proof would leave the risky
  seam untested.
- Their current implementations are 1,010 and 1,406 lines and have no focused
  operation tests. 5c therefore first freezes their behavior with scripted
  golden traces rather than relying on accidental coverage.

### Alternatives considered

1. **The two SH705x EEPROM variants (chosen).** Best prompt, transport, package
   closure, and matched-family proof. It requires substantial characterization,
   which is appropriate for the proving seam.
2. **Mitsubishi M32R CAN plus a small K-Line ECU family.** The CAN operation has
   useful existing tests, but the two choices would share little preflight or
   prompt behavior and would leave the aggregate `ecu` package on the serial
   allowlist. It proves two rewrites, not one reusable seam.
3. **Denso SH705x full-ROM K-Line plus DensoCAN.** These are representative but
   much larger (roughly 1,670 and 1,415 lines), live beside many unconverted ECU
   operations, and do not give a truthful allowlist reduction. They belong in
   the per-family tail after the seam is proven.

### Current behavior that must be made explicit

Both EEPROM operations initialize mode `2`, then try modes `2`, `3`, and `4`.
After each successful read they ask the user to save the bytes or discard them;
discard asks for an ignition cycle before the next mode. The existing
`write`/`test_write` branches log a write message but have the actual write call
commented out. 5c does not legitimize that as portable flashing: the new plan
builders return `Unsupported` for write and test-write for these two family IDs
before transport setup. Existing menu capability metadata must not offer those
commands for these EEPROM protocols; a characterization test pins that routing.
If repository configuration currently exposes one, correcting the capability
metadata is an intentional safety fix and is recorded in the matrix notes.

---

## Chosen architecture

### Why one synchronous attempt is the unit of execution

Three prompt architectures were considered:

1. **One bounded execution per EEPROM mode (chosen).** Execute mode 2 and return
   its bytes. The desktop previews the bytes and either accepts or confirms an
   ignition cycle before building mode 3, then mode 4. No backend call waits for
   a human.
2. **A blocking prompt callback inside `execute`.** Rejected: it merely renames
   `QMetaObject::BlockingQueuedConnection`, makes shutdown unbounded, and lets an
   event port conceal a dialog.
3. **A resumable executor/continuation object.** Rejected for 5c: serializable
   continuation state and lifetime rules add machinery needed by only this
   prompt today. A fresh immutable plan expresses the same three-attempt policy
   more safely.

The desktop orchestration preserves the observable sequence:

```text
build mode-2 plan -> initial confirmation -> worker executes -> preview
  -> Save: publish bytes and finish
  -> Discard: discard bytes -> ignition-cycle confirmation
       -> Cancel: finish as user cancellation
       -> OK: build mode-3 plan -> worker executes -> preview
            -> same decision, then mode 4 at most
```

Only transport execution runs on the worker. Confirmations and byte preview run
on the GUI thread between worker invocations. `IEventSink` carries notifications,
logs, and progress only; it never solicits or returns an answer.

### Ownership

- A `FlashPlan` owns every byte and value used by an execution attempt.
- A worker owns its plan by value, cancellation source/token, executor, transport
  adapter, event adapter, and output until its thread has joined.
- The executor borrows only those worker-owned objects for the synchronous call.
- No `FileActions::EcuCalDefStructure*`, `MainWindow*`, `QWidget*`, path-backed
  kernel, mutable ROM object, or `SerialPortActions*` reaches portable code.
- The backend creates no thread and retains no platform reference after
  `execute()` returns.

---

## Portable FlashPlan model

### Common model

The public plan is a value type with private construction. Callers receive it
only from a family builder returning `Result<FlashPlan>`.

```cpp
namespace fastecu::flash {

enum class FlashOperation { Read, TestWrite, Write };
enum class FlashFamily {
  DensoSh705xEepromKline,
  DensoSh705xEepromCan,
  // Added one-by-one by the per-family tail.
};
enum class TransportKind { Kline, CanIso15765 };

struct MemoryRegion {
  std::uint32_t start;
  std::uint32_t length;
};

struct KernelImage {
  std::string id;                 // diagnostic identity, not a filesystem path
  std::uint32_t load_address;
  bytes::Bytes bytes;             // immutable snapshot owned by the plan
};

struct ConfirmationSpec {
  enum class Id { BeginEepromRead, InspectEepromBytes, CycleIgnition };
  Id id;
  // Stable semantic arguments; desktop owns translated title/body/buttons.
  std::vector<std::pair<std::string, std::string>> arguments;
};

enum class DensoSecurityVariant { Stock, EcuTek, Cobb, EcuTekRaceRom };
enum class EepromReadMode : std::uint8_t { Mode2 = 2, Mode3 = 3, Mode4 = 4 };

struct DensoSh705xEepromKlinePlan {
  EepromReadMode mode;
  DensoSecurityVariant security;
  std::uint8_t tester_id;         // 0xf0 for the current family
  std::uint8_t target_id;         // 0x10 for the current family
  int initial_baud;               // 4800
  int kernel_baud;                // snapshotted resolved family value
};

struct DensoSh705xEepromCanPlan {
  EepromReadMode mode;
  DensoSecurityVariant security;
  std::uint32_t request_id;       // 0x7e0
  std::uint32_t response_id;      // 0x7e8
  int bitrate;                    // 500000
  bool extended_id;               // false
};

using FamilyPlan = std::variant<
    DensoSh705xEepromKlinePlan,
    DensoSh705xEepromCanPlan>;

class FlashPlan {
 public:
  FlashOperation operation() const;
  FlashFamily family() const;
  TransportKind transport() const;
  const std::string& target_id() const;
  const std::string& mcu_name() const;
  const MemoryRegion& transfer_region() const;
  std::span<const MemoryRegion> erase_regions() const;
  const std::optional<bytes::Bytes>& image() const;
  const KernelImage& kernel() const;
  const FamilyPlan& family_plan() const;
  std::span<const ConfirmationSpec> confirmations() const;
  std::uint64_t total_transfer_bytes() const;
  std::string_view experimental_family_id() const;
 private:
  // Builder-only validated construction.
};

}  // namespace fastecu::flash
```

`target_id` is the stable selected protocol/configuration identifier, not the
display name. `mcu_name` is the resolved flash-device identity. Read plans have
`image == std::nullopt` and empty `erase_regions`; write plans for later families
own the complete image and exact erase regions. `total_transfer_bytes` is
derived with checked arithmetic, never supplied independently.

`ConfirmationSpec` declares UI decisions around execution; it is not an event.
For the proving pair, each plan declares `BeginEepromRead` before execution and
`InspectEepromBytes` after success. Modes 3 and 4 additionally declare
`CycleIgnition` before execution. The platform verifies that every required
confirmation was answered before starting that worker invocation.

### Builder inputs and 5d boundary

Portable builders do not consume `FileActions` or paths:

```cpp
struct DensoSh705xEepromInput {
  FlashOperation operation;
  FlashFamily family;
  std::string target_id;
  std::string mcu_name;
  std::string flash_method;
  KernelImage kernel;
  EepromReadMode mode;
  DensoSecurityVariant security;
  MemoryRegion eeprom_region;
};

Result<FlashPlan> build_denso_sh705x_eeprom_plan(
    DensoSh705xEepromInput input);
```

In 5c a desktop `LegacyFlashSnapshotAdapter` copies the required definition
fields, resolves the selected flash-device entry, reads the kernel through the
platform file repository, converts all strings/numbers, and calls the portable
builder. It returns `Result<FlashPlan>` and catches/converts platform exceptions.
Step 5d replaces this adapter with a backend definition/flash use case; the plan
and executor contracts do not change.

The adapter checks every result-returning or Boolean setup/conversion call. It
does not pass a kernel path into the plan and does not defer parsing
`KernelStartAddr` until execution.

### Common preflight validation

All failures provable from inputs are returned before adapter configuration,
port open, handshake, kernel upload, erase, or write:

- operation and family are a supported combination;
- stable target ID and MCU identity are non-empty and resolve consistently;
- all addresses, lengths, ends, sums, and byte counts fit their declared widths
  without overflow;
- transfer region is non-empty and contained in the selected device region;
- regions do not overlap illegally and erase regions exactly cover only allowed
  write blocks;
- reads have no image or erase regions;
- writes/test-writes have an image of the required size and valid block mapping;
- kernel ID and bytes are non-empty, load address is valid for the target RAM
  region, and upload end does not overflow or escape it;
- family variant tag matches `FlashPlan::family` and transport kind;
- confirmation declarations match the operation/mode and contain no duplicate
  semantic IDs;
- total progress denominator is nonzero and representable.

Defensive checks remain in executors, but a scripted transport test proves that
each common invalid input produces `InvalidConfig` or `Unsupported` with zero
setup calls and zero writes.

### Proving-pair validation

The Denso SH705x EEPROM builder additionally requires:

- `operation == Read`; `Write` and `TestWrite` return `Unsupported`;
- mode is exactly 2, 3, or 4;
- one EEPROM region exists and matches the resolved MCU table entry;
- K-Line uses tester `0xf0`, target `0x10`, initial baud `4800`, and a positive
  supported resolved kernel baud;
- CAN uses standard IDs `0x7e0`/`0x7e8`, 500 kbit/s, and 11-bit identifiers;
- the security variant is supported by that transport/family combination;
- kernel load range is within the selected SH705x RAM/kernel range;
- modes advance only through the UI orchestrator as 2 -> 3 -> 4. A builder may
  create any one mode for deterministic tests, but the production orchestrator
  cannot skip or repeat a mode.

The builder copies resolved region values into the plan. The executor never
indexes the legacy global `flashdevices` table and never parses definition text.

---

## Executor, transport, cancellation, clock, and event interfaces

### Execution contract

The umbrella's illustrative `Result<void> execute(...)` is refined because a
read must return bytes without mutating `FileActions` or smuggling data through
an event:

```cpp
struct FlashExecutionResult {
  FlashOperation operation;
  std::optional<bytes::Bytes> read_bytes;  // present for successful Read
};

// Lifetime/unblock capability only; it deliberately has no universal I/O API.
class IFlashTransport {
 public:
  virtual ~IFlashTransport() = default;
  virtual void request_unblock() noexcept = 0;
};

class IFlashExecutor {
 public:
  virtual ~IFlashExecutor() = default;
  virtual Result<FlashExecutionResult> execute(
      const FlashPlan& plan,
      IFlashTransport& transport,
      IClock& clock,
      const ICancellationToken& cancellation,
      IEventSink& events) = 0;
};
```

Concrete K-Line and CAN executor classes remain separate. Dispatch validates
the family/variant/transport match and returns `InvalidConfig` without I/O if it
is wrong. A successful read result owns exactly the bytes currently assigned to
`ecuCalDef->FullRomData`; only the desktop adapter performs that legacy update
after the user accepts the preview.

### Transport shape

Do not collapse byte-stream K-Line and CAN into a lowest-common-denominator
read/write port. 5c uses the backend-owned existing transport capabilities and
adds only flash-proven requirements:

```cpp
struct KlineConfig {
  int baud;
  bool iso14230;
  std::uint8_t tester_id;
  std::uint8_t target_id;
};

class IKlineFlashTransport : public IFlashTransport,
                             public mutdma::IKlineTransport {
 public:
  virtual Status configure(const KlineConfig&) = 0;
  virtual Status open() = 0;
  virtual Status close() = 0;
  virtual Status set_baud(int) = 0;
  virtual Status write_all(bytes::ByteView,
                           const ICancellationToken&) = 0;
  virtual Result<std::optional<bytes::Bytes>> read(
      int timeout_ms, const ICancellationToken&) = 0;
};

struct Iso15765Config {
  int bitrate;
  std::uint32_t request_id;
  std::uint32_t response_id;
  bool extended_id;
};

class ICanFlashTransport : public IFlashTransport {
 public:
  virtual ~ICanFlashTransport() = default;
  virtual Status configure(const Iso15765Config&) = 0;
  virtual Status open() = 0;
  virtual Status close() = 0;
  virtual Status write(bytes::ByteView,
                       const ICancellationToken&) = 0;
  virtual Result<std::optional<bytes::Bytes>> read(
      int timeout_ms, const ICancellationToken&) = 0;
};
```

`ICanTransport` remains the raw-CAN frame port used by CDBG. The proving CAN
family currently configures `SerialPortActions` for ISO-15765 and exchanges
framed byte messages, so it uses the distinct `ICanFlashTransport`; pretending
that this is raw CAN would change the adapter contract. Later raw-CAN flash
families use `ICanTransport` directly.

If step 5b lands first, its `Result`-based `IKlineTransport` methods are reused
and `IKlineFlashTransport` adds only configure/open/close/unblock; there is no
second incompatible byte-stream interface. Any temporary legacy signatures are
isolated in `transport_legacy_compat.h` outside portable target dependencies.

Every Boolean legacy serial setter is checked by the concrete adapter. The CAN
adapter checks, in order, connection mode, ISO-15765 mode, 11/29-bit selection,
bitrate, request/response CAN IDs, ISO-15765 source/destination IDs, then open.
The K-Line adapter checks connection modes, header/address settings, baud, then
open. The first failure returns its original `ErrorKind`; no handshake write
occurs. Adapter/driver exceptions are caught and converted to `Internal`, or
`Disconnected` where the exception specifically reports adapter loss.

### Reads, cancellation, and clock

- Every read names a finite timeout copied from the existing state machine.
- A successful empty optional means no bytes arrived by that exchange deadline;
  the family response validator maps it to `Timeout`, never success.
- Closed/dropped adapters return `Disconnected`.
- Cancellation observed before or after an unblocked driver read returns
  `Cancelled`.
- Malformed, wrongly addressed, unexpected-service, negative-response, and
  checksum-invalid replies return `BadResponse`.
- Partial writes and rejected configuration are failures; a requested byte
  count is not assumed written merely because the legacy API lacks a count.
- All existing `QThread::msleep`/`delay` and `QElapsedTimer` use becomes
  `IClock::sleep`/`now_ms`. Every sleep result is checked.
- Transfer loops poll cancellation before setup, before each write, after every
  read/sleep, before kernel jump, and between chunks.

`IFlashTransport` is only a lifetime/unblock marker so the platform worker can
own either capability; it does not erase K-Line and CAN I/O into a universal
API. Each executor checks and downcasts to its exact capability before setup,
returning `InvalidConfig` with zero I/O on a mismatch.

`request_unblock()` is platform teardown machinery, not a backend workflow
operation. It is noexcept, idempotent, thread-safe, and causes an active read to
return promptly. The executor itself only observes the cancellation/result.

### Events and prompts

5a's `IEventSink` remains unchanged:

- `log(level, text)` carries diagnostics corresponding to current LOG signals;
- `progress(done, total)` uses byte counts, not pre-rounded percentages;
- `notice(text)` carries noninteractive status copy only.

Events are advisory and cannot change control flow. The returned `Result` is the
single authoritative completion outcome. Prompt requests live on the plan and
are answered by the desktop before or after a worker call. An event sink must
not show a modal, block for an answer, or perform indefinite interactive work.

The desktop translates semantic confirmation IDs to the exact existing titles,
bodies, buttons, defaults, and preview behavior. Prompt answers belong to the UI
orchestrator, not the executor or event adapter. Declining the initial prompt or
cancelling the ignition-cycle prompt yields a UI-owned cancellation with no
generic failure dialog. Discarding bytes is not an executor failure.

---

## Family execution and behavior preservation

Each proving executor is a readable family state machine. It may share already
portable SSM framing/cipher/CRC code, response-validation primitives, checked
region helpers, and progress math. It must not share a configurable sequence of
services merely because the two files look similar.

For each mode attempt, preserve this broad sequence and all concrete bytes from
the characterized implementation:

1. validate plan/family/transport and cancellation;
2. configure and open the adapter;
3. detect an already-running kernel using the existing request/reply;
4. otherwise perform the family-specific bootloader/security sequence;
5. upload the snapshotted kernel at the validated address with existing chunk
   sizes, transforms, CRCs, delays, and acknowledgements;
6. start and identify the kernel;
7. issue the selected EEPROM-mode request;
8. read the exact planned region in existing chunk/page order;
9. return owned bytes only after complete success;
10. close exactly once through the cleanup path.

The executor never returns partial bytes as success. It may log the failing
address, but callers branch on `ErrorKind`, not detail text.

Cleanup follows one scope-guarded path. If execution and close both fail, the
original error wins and close failure is logged. If close is the only failure,
it is returned. An already-cancelled call performs no setup or I/O. Cancellation
after open still closes exactly once.

---

## Platform worker, UI orchestration, and teardown

### Worker relocation

`FlashOperationWorker` is a Qt lifecycle adapter and moves to
`src/platform/desktop/common/flash/`. The new worker is composition-based, not
the base class for a portable executor:

```cpp
class FlashWorker final : public QThread {
  Q_OBJECT
 public:
  FlashWorker(FlashPlan plan,
              std::unique_ptr<IFlashExecutor> executor,
              std::unique_ptr<IFlashTransport> transport,
              QObject* parent = nullptr);
  void requestStop();
 signals:
  void logEvent(...);
  void progressChanged(int done, int total);
  void finished(FlashWorkerResult result);
};
```

`run()` invokes exactly one synchronous executor call, catches any unexpected
platform exception as `Internal`, stores/queues one value result, and exits. It
does not prompt, parse definitions, choose EEPROM modes, mutate `FileActions`,
classify protocol replies, or implement retries.

### Legacy operations after the move

The remaining Qt/QThread/`SerialPortActions` operation classes are not portable
backend code. In 5c they move mechanically, preserving filenames/class names as
needed, to `src/platform/desktop/common/flash/legacy/{bdm,bootmode,ecu,eeprom,
jtag,tcu}` (the converted EEPROM files are replaced by portable executors, not
copied). Their shared legacy worker also lives there. UI includes and Bazel
labels are updated; behavior is otherwise unchanged.

This move is necessary to satisfy the umbrella rule that backend owns no
threads. Leaving a `QThread` base in `src/backend/flash` until the tail is not an
accepted intermediate state. The legacy platform targets may still depend on
`SerialPortActions`, Qt compatibility shims, and mutable definition structures;
they are explicitly outside portable closure and drain one family at a time in
the tail.

The portable `src/backend/flash` package retains only models, builders,
validation, executor interfaces, shared pure helpers, and the two proving
executors. `flash_utils` is split: portable byte/block helpers remain backend;
`configureIso15765Can(SerialPortActions*)` moves to the desktop legacy adapter
and is removed from backend.

### Desktop orchestration and failure reporting

The existing EEPROM dialogs remain the presentation shell for 5c. Their run
path becomes:

1. snapshot all input values and kernel bytes;
2. build/validate mode 2 before opening a port;
3. show the existing initial confirmation;
4. run one worker and marshal its events with queued Qt connections;
5. on success, preview the returned bytes with existing copy;
6. on Save, assign accepted bytes to the legacy ROM snapshot and finish;
7. on Discard, destroy/discard those bytes, show the existing ignition-cycle
   confirmation, and repeat for the next mode;
8. after mode 4 discard, finish with the same unsuccessful/no-selection outcome
   pinned by compatibility tests.

Only the orchestration layer writes back to `ecuCalDef->FullRomData`, and only
after Save. The worker and portable executor never retain the pointer.

The final worker `Result` is the authority for failure reporting. The event sink
may have emitted diagnostic logs, but it never emits a generic completion error.
The dialog shows at most one terminal failure, selected by `ErrorKind`:

- user-initiated `Cancelled`: no failure dialog;
- `InvalidConfig`/`Unsupported`: preflight/capability message, no worker start;
- `Disconnected`: adapter-disconnected message;
- `Timeout`: ECU-no-response/timeout message;
- `BadResponse`: rejected/malformed ECU-response message;
- `Internal`: one generic internal failure with diagnostic detail in logs.

### Teardown

Dialog close and application shutdown follow a fixed order:

1. atomically request cancellation;
2. call the worker-owned transport adapter's idempotent `request_unblock()`;
3. wait for the bounded executor to return and its exactly-once close path;
4. join the `QThread`;
5. disconnect event adapters and destroy transport/executor/plan/result;
6. destroy the dialog or parent object.

No `terminate()`, detached thread, `deleteLater()` dependency on a stopped event
loop, concurrent `close()` from the GUI thread, or unbounded `wait()` is allowed.
The teardown test uses a driver fake blocked on a condition variable and proves
unblock/join deterministically without wall-clock sleeps.

---

## Build layout, legacy seams, and allowlist

### Portable targets

The implementation plan may split files further, but these target names are
part of the enforcement contract:

- `//src/backend/flash:flash_types`
- `//src/backend/flash:flash_plan`
- `//src/backend/flash:flash_validation`
- `//src/backend/flash:flash_executor`
- `//src/backend/flash/eeprom:denso_sh705x_eeprom_common`
- `//src/backend/flash/eeprom:denso_sh705x_eeprom_kline`
- `//src/backend/flash/eeprom:denso_sh705x_eeprom_can`
- `//src/backend/protocol:protocol` (the existing transport port/support target,
  including the final 5b signatures if #78 lands first)

All are ordinary `cc_library` targets with no `QT_DEPS`, Qt/JNI headers,
`:qt_compat`, platform dependency, filesystem API, or thread API.

### Platform targets

- `//src/platform/desktop/common/flash:flash_worker`
- `//src/platform/desktop/common/flash:flash_snapshot_adapter`
- `//src/platform/desktop/common/flash:flash_event_adapter`
- `//src/platform/desktop/common/flash:legacy_flash_operations`
- `//src/platform/desktop/common/transport:flash_transports`

Names may be subdivided in the implementation plan, but platform ownership and
dependency direction are fixed.

### Temporary seams retained

- The desktop snapshot adapter reads legacy `FileActions` fields until 5d.
- EEPROM dialogs and MainWindow routing remain until step 6.
- Unconverted legacy operations keep their class APIs, Qt signals/copy, and
  `SerialPortActions` access under platform ownership until the family tail.
- Algorithm `:qt_compat` shims remain only for those legacy platform/UI callers.
- If #78 is not merged first, narrowly scoped transport legacy adapters remain
  outside portable closure; 5c does not duplicate 5b's intended final API.

### Seams removed in 5c

- `src/backend/flash/flash_operation_worker.*` and all backend Qt worker
  inheritance;
- backend `PromptFn`, `confirm()`, `QMessageBox`, `QMetaObject` blocking prompt,
  `QThread`, `QElapsedTimer`, and `QFile` use in converted targets;
- `SerialPortActions*` and mutable `EcuCalDefStructure*` from the proving pair;
- `FlashUtils::configureIso15765Can(SerialPortActions*, ...)` from backend;
- the backend serial compatibility visibility entry
  `//src/backend/flash/eeprom:__pkg__`.

The UI EEPROM package still remains on the transitional allowlist until step 6
because its dialog receives the serial platform object. Because every remaining
Qt flash operation moves out of backend, 5c removes these seven exact backend
entries from both `serial_qt_compat.visibility` and the checker's `FROZEN` set:

- `//src/backend/flash:__pkg__`
- `//src/backend/flash/bdm:__pkg__`
- `//src/backend/flash/bootmode:__pkg__`
- `//src/backend/flash/ecu:__pkg__`
- `//src/backend/flash/eeprom:__pkg__`
- `//src/backend/flash/jtag:__pkg__`
- `//src/backend/flash/tcu:__pkg__`

No replacement allowlist entry is added. The serial package exposes a
same-layer `serial_platform_api` alias/facade, visible to
`//src/platform:__subpackages__`, whose actual implementation remains the
existing serial library. Platform legacy flash targets depend on that explicit
same-layer API. The frozen debt target therefore only loses entries; it does
not gain `//src/platform/desktop/common/flash` or a broad platform exception.

---

## Portable-closure enforcement

Replace the current root-discovery-only check with explicit required targets,
following the corrected step 5b pattern. `PORTABLE_ROOTS` must name the required
target sets above, including dependency-support targets. A root
`genquery(name = "portable_backend_closure", expression =
"deps(set(<all required labels>))")` supplies the resolved transitive label
closure to the Python test; the BUILD-file scan still checks required names and
forbidden macros that a label query cannot see. The check fails if:

- a required root BUILD file is absent;
- any required target name is absent or renamed without updating the contract;
- a required target or another portable target under the roots declares Qt,
  JNI, `QT_DEPS`, or `:qt_compat`;
- a required flash target depends on a platform label, directly or transitively.

The Bazel `//:portable_closure` target lists the `genquery` output and every
inspected BUILD file in `data`, including `src/backend/flash/BUILD.bazel`,
`src/backend/flash/eeprom/BUILD.bazel`, and
`src/backend/protocol/BUILD.bazel`.

Required non-vacuous probes, performed and documented during implementation:

1. temporarily rename/remove `denso_sh705x_eeprom_can`; the check must fail with
   a missing-required-target error;
2. temporarily inject `QT_DEPS` into `flash_plan`; it must fail for Qt;
3. temporarily inject `//src/platform/desktop/common/flash:flash_worker` into a
   portable executor; it must fail for a platform dependency even if no Qt
   token appears in the immediate target;
4. temporarily inject the JNI label into the K-Line target; it must fail;
5. restore each probe and prove the check passes.

The checker remains deterministic source/build-graph enforcement, not a grep of
C++ filenames. `@openssl` remains the umbrella's adjudicated exception for step
7; 5c neither expands nor resolves it.

---

## Testing and equivalence proof

### Characterization before extraction

Before production conversion, add scripted tests around each legacy EEPROM
operation and capture exact ordered adapter configuration, writes, read
deadlines, clock delays, prompt points/answers, progress, output bytes, and
terminal success/failure. Golden expectations must be independent literals or
fixtures, not generated by the production helper being tested.

At minimum freeze:

- kernel-already-running and bootloader-plus-upload paths;
- stock and every currently reachable EcuTek/Cobb/RaceRom security branch;
- EEPROM modes 2, 3, and 4;
- Save after mode 2, discard/retry to mode 3, discard/retry to mode 4, ignition
  cancellation, and all-three-discard behavior;
- exact request/response bytes, chunk addresses/lengths, CRC/cipher output,
  baud/ID configuration order, delays, and returned EEPROM bytes;
- the current unsupported write/test-write routing/capability behavior.

After migration, run the same trace fixtures against the portable executors and
compare the ordered wire/configuration timeline byte-for-byte. Desktop prompt
adapter tests separately assert exact current translated copy/buttons/defaults.

### FlashPlan and validation tests

Qt-free co-located tests cover:

- a valid plan for each family and each EEPROM mode;
- deep-copy/value ownership of kernel bytes and all strings;
- target/MCU mismatch, unknown target, empty kernel, invalid kernel range,
  invalid/overflowing EEPROM range, zero length, bad CAN IDs/bitrate, bad K-Line
  addresses/baud, invalid security variant, and variant/tag mismatch;
- `Write` and `TestWrite` returning `Unsupported`;
- no transport factory/configuration/write call for every preflight rejection;
- deterministic confirmation declaration and derived transfer byte count.

### Executor state-machine and ErrorKind tests

Scripted K-Line and ISO-15765 transports plus fake clock/cancellation/event sinks
cover every transition and assert complete script consumption. Required outcome
tests are:

- success with identical bytes, logs of semantic significance, and progress;
- no response at every handshake/upload/read stage -> `Timeout`;
- adapter closed or dropped at every stage -> `Disconnected`;
- malformed length, wrong address/service, negative response, bad checksum, and
  rejected key -> `BadResponse`;
- cancellation before setup, during sleep, blocked read, kernel upload, and
  EEPROM chunk loop -> `Cancelled`;
- wrong executor/plan/transport pairing -> `InvalidConfig` with no I/O;
- unsupported operation -> `Unsupported` in preflight;
- injected invariant/adapter exception -> `Internal`, with no exception crossing
  the executor/worker boundary;
- partial setup/write failures preserve their original kind and stop before the
  next wire action;
- close exactly once on success and every post-open failure;
- main error wins over close error; close-only error is returned.

No test treats silence as success or a cached/previous response. No timing test
uses real sleeps.

### Worker, prompt, and teardown tests

Desktop Qt tests prove:

- the worker owns plan bytes after the source definition/snapshot is mutated or
  destroyed;
- queued log/progress/result delivery occurs on the GUI thread;
- one and only one terminal result is emitted;
- initial decline and UI-requested cancellation do not show a generic error;
- a structured failure produces one family-specific dialog, not a diagnostic
  event plus duplicate generic dialog;
- mode orchestration is exactly 2 -> 3 -> 4, never repeats/skips, and accepted
  bytes alone update `FullRomData`;
- closing while a fake read is blocked calls cancel, unblocks, joins within the
  deterministic bound, and destroys objects only after join;
- adapter configuration checks every Boolean/result call and stops at the first
  failure;
- adapter exceptions become the required `ErrorKind` and never cross Qt signal
  or worker boundaries.

### Coverage and gates

New tests are co-located under `src/**/*_test.cpp` or existing `tests/`; new-code
coverage is at least 80% and the SonarCloud Quality Gate passes.
`docs/coverage-baseline.txt` remains absent and must not be recreated.

Run:

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

Also run the new portable flash tests explicitly on all desktop CI platforms,
Windows/macOS packaging, and both packaging scripts' `//:fastecu` resolution.
Real-ECU tests are recorded in the qualification matrix but are not a merge gate.

---

## Flash qualification matrix

Create the [flash qualification matrix](../../flash-qualification-matrix.md)
with this normative schema:

| Column | Meaning |
|---|---|
| `family_id` | Stable code identifier, one row per selectable operation family/transport variant |
| `scope` | ECU, TCU, EEPROM, JTAG, BDM, or bootmode |
| `transport` | K-Line, raw CAN, ISO-15765, SSM, JTAG, or BDM |
| `operations` | Actual supported `read`, `test_write`, `write` set; no inferred capability |
| `portable` | `yes` only when plan + executor are in enforced portable closure |
| `automated_evidence` | Golden/state-machine test labels and last qualifying revision |
| `hardware_status` | `unqualified`, `experimental`, or `proven` |
| `hardware_evidence` | Date, ECU/TCU/adapter identity, operation, operator/report reference; `—` if absent |
| `notes` | Known compatibility quirks, unsupported branches, and safety limitations |

Rules:

- Seed one row for every current backend flash/eeprom/jtag/bdm/bootmode family,
  not only the proving pair. No family silently disappears from migration scope.
- The proving pair has `portable=yes`, test labels, and
  `hardware_status=experimental`; it is never marked proven from unit tests.
- Unmigrated rows have `portable=no` and `hardware_status=unqualified` unless
  concrete historical hardware evidence is entered with a reference.
- `proven` requires a real read comparison and, where the family supports it,
  test-write/write/verify/recovery checks on named hardware. A future change
  records evidence; it does not infer proof from similarity to another family.
- Matrix status is documentation only. It does not drive UI badges, dialogs, or
  runtime feature flags in step 5.

The document begins with the schema/rules, then a table grouped by scope and
stable family ID, followed by a hardware qualification checklist covering
adapter setup, battery voltage/power stability, read/hash comparison,
test-write, write, post-write read/hash, ignition cycle, recovery path, logs,
and operator/date. Unsupported operations are marked `n/a`, never silently
treated as passed.

---

## Explicit deferrals

### Per-family tail after 5c

- Migrate each remaining ECU/TCU/EEPROM/JTAG/BDM/bootmode family independently.
- Add its typed family-plan variant, validation, scripted golden traces, executor,
  explicit closure target, matrix row update, and allowlist reduction.
- Remove legacy platform operation classes and Qt algorithm compatibility shims
  only when their final caller drains.
- Generalize a response validator, block planner, or transfer primitive only
  after at least two migrated families have byte-level tests proving the same
  contract.

### Step 5d

- Replace `LegacyFlashSnapshotAdapter` with portable definition and flash use
  cases produced from immutable parsed models.
- Move generic ROM/kernel file acquisition behind `IFileRepository` and remove
  remaining flash-related `FileActions` responsibilities.
- Own calibration/checksum/definition validation and expose `FlashPlan` to the
  desktop without mutable legacy structures.

### Step 6 — thin desktop shell

- Consolidate construction and dependency injection in `apps/desktop`.
- Reduce MainWindow/dialogs to presentation and calls into use cases.
- Remove obsolete legacy facades, compatibility wrappers, duplicate status
  macros/signals, and remaining UI serial-object coupling.
- Re-run affected real-hardware bench checklists when hardware is available.

### Step 7 — Android/native seam

- Define the versioned C ABI and map plans/results/errors without exposing STL
  or C++ layouts.
- Supply Kotlin/coroutine scheduling and Android USB transport adapters.
- Cross-compile the already-portable backend; do not redesign family state
  machines in the ABI step.

---

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Runtime preview prompt tempts a blocking backend callback | One execution per mode; UI preview and ignition prompt occur only between joined workers |
| Mutable definition/kernel data outlives its owner | Plan deep-copies every value and byte; worker ownership test destroys the source before execution |
| Moving only the worker leaves backend subclasses depending upward on platform | Mechanically relocate all unconverted Qt operations to platform legacy ownership in the same change |
| K-Line and ISO-15765 are forced into one leaky transport | Keep capability-specific ports and separate family executors; share only proven pure helpers |
| Invalid ROM/region/kernel reaches the ECU before rejection | Builder validates all provable facts; zero-I/O rejection tests instrument factory and transport |
| Adapter setup partially succeeds and code continues | Check every setter/result in order; first failure test asserts no later setup/open/write |
| Cancellation cannot interrupt the legacy driver | Platform adapter owns idempotent `request_unblock`; deterministic blocked-read teardown test is a merge gate |
| Error log plus worker result causes two dialogs | Returned `Result` is authoritative; events never report generic completion; UI test asserts one dialog |
| Closure check passes after target deletion or hidden support contamination | Explicit required target names plus missing-target, Qt, platform, and JNI negative probes |
| Pair migration claims hardware safety | Matrix records `experimental`; proof requires named real hardware and is explicitly deferred |

---

## Deliverable checklist

- [ ] Immutable typed `FlashPlan`, builders, common/family validation, and owned
      execution result.
- [ ] Dialog-free synchronous Denso SH705x EEPROM K-Line and CAN executors with
      unchanged golden wire traces.
- [ ] Result-based bounded flash transports, checked setup, cancellation unblock,
      clock/event injection, and exception translation.
- [ ] One-attempt-per-mode UI prompt orchestration with exact existing copy and
      no blocking prompt hidden in an event sink.
- [ ] Qt flash worker plus all unconverted Qt operations owned by desktop
      platform; backend owns no thread.
- [ ] Legacy snapshot/result adapters with value ownership and one authoritative
      failure-reporting path.
- [ ] All seven `//src/backend/flash*` entries listed above removed from the
      serial compatibility allowlist; no replacement debt entry added.
- [ ] Explicit portable-closure targets and all four negative probes.
- [ ] Equivalence, plan validation, state-machine, cancellation, teardown,
      configuration, exception, and every-`ErrorKind` tests; >=80% new-code
      coverage.
- [ ] The [flash qualification matrix](../../flash-qualification-matrix.md)
  seeded for every family, proving pair marked portable/experimental, no
  hardware claims.
- [ ] Umbrella build/test/package/SonarCloud gates pass and
      `docs/coverage-baseline.txt` remains absent.
