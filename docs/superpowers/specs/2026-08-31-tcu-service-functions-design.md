# TCU Service Functions — Portable Seam — Design

## Scope and status

Three operator-driven TCU maintenance operations move out of the legacy Qt
flash operation `FlashTcuSubaruDensoSH705xCanOperation` into a new portable
package, `src/backend/service_functions/`: **relearn**, **read parameters**,
and **set parameters** — `tcuAction` 2, 3 and 4 of that class, roughly 650 of
its 2,345 lines.

This spec is **groundwork for wave 5** of the
[step-5 tail flash drain](2026-08-08-step5-tail-flash-drain-design.md), and
lands before it. It is not itself a drain wave: the legacy class survives this
work, retaining `tcuAction` 1 (ROM dump / write), which wave 5 ports and
deletes. `//:legacy_flash_drain` therefore does not move here.

Hardware status for all three operations is **experimental**. This work claims
automated equivalence plus six named defect corrections, not bench
qualification.

## Why these are not flash operations

The [step-5 umbrella design](2026-07-22-step5-backend-portable-design.md)
converts flashing to preflight plus execution: build and validate a
`FlashPlan`, obtain UI confirmation before irreversible I/O, then execute
without backend dialogs. All three of these operations violate the shape,
and one of them violates it structurally.

`ConfirmationSpec` states its own contract in `src/backend/flash/flash_types.h`:
confirmations "are collected by the desktop dialog BEFORE the executor starts:
a synchronous, dialog-free executor cannot block mid-run for a human answer."

`tcu_relearn_subaru_ssm` issues an operator instruction **in the middle** of
its sequence — `flash_tcu_subaru_denso_sh705x_can_operation.cpp:735`, "Start
Engine, let revs settle, move stick into D, fully press brake" — sent only
after the TCU has accepted the step-2 `0xB8 00 01 FD 09` write, and before the
`0xA8` status poll begins. The instruction is meaningless earlier: it tells the
operator to react to a state the TCU has just entered. Routing it through
`FlashPlan` would mean either weakening the pre-collection rule for all
27 flash families, or shipping a relearn that asks for everything up front.

Beyond that one structural fact, none of the three carries an image, a memory
region, a kernel, or a `flashdev_t`; none performs a flash write; `read
parameters` is read-only. A `FamilyPlan` variant alternative for them would be
a plan-shaped object with every plan field empty, in a variant that other flash
code pattern-matches and that gates hardware-safety decisions.

`ISsmTransport` already fits all three exactly — see
[Transport](#transport-and-configuration-ownership) — so the move costs no new
port surface.

## Package

`src/backend/service_functions/` — portable: no Qt, no threads, no filesystem.
Registered in both `PORTABLE_ROOTS` in `scripts/check-portable-closure.py` and
the `genquery` in `BUILD.bazel`, with the registration proven non-vacuous per
the umbrella rule (verified to fail when the target is absent as well as when
it is non-conforming).

**Membership rule**, recorded in the package README so it does not become a
dumping ground: operator-gated, non-flash ECU/TCU routines that exchange bytes
over `ISsmTransport`. Today that is exactly one family's three operations.

The name avoids `diagnostics`, which already means NRC/DTC table lookup in
`src/algorithms/diagnostics`; a `src/backend/diagnostics` would give the
repository a second `definition` / `definitions` trap.

## The three operations

| Operation | Wire | Legacy source |
|---|---|---|
| `ReadParameters` | ISO-15765 `0x7E1` / `0x7E9`; one `0xA8` ten-address read decoding into nine values, up to 6 attempts | `:517-632` |
| `SetParameters` | K-Line ISO14230 at 4800 baud; twelve `0xB8 00 01 <addr> <value>` SSM writes, each expecting `0xF8`. Only the first is well-formed — see [the framing defect](#set-parameters-corrupts-every-frame-after-the-first) | `:135-517` |
| `Relearn` | ISO-15765 `0x7E1` / `0x7E9`; `0xB8 00 01 FC 01`, then `0xB8 00 01 FD 09`, then a 200-iteration `0xA8` status poll | `:632-790` |

All line references are to
`src/platform/desktop/common/flash/legacy/tcu/flash_tcu_subaru_denso_sh705x_can_operation.cpp`.

`SetParameters` runs on a different bus from its two siblings: the legacy code
switches the session to K-Line at its first statement (`:141-152`, "CAN 0xb8
command is disabled, so switch to K-Line comms"), discarding the ISO-15765 port
that `execute()` opened without ever using it. That is why these are three
sessions with three transport configurations, not one session with a mode flag.

## Operation model

The model follows `src/backend/logging`'s session shape, not
`src/backend/flash`'s plan-and-executor shape. Backend runs bounded,
cancellable I/O and yields when it needs a human; the platform owns the thread
and the dialog.

```cpp
enum class OperatorGateId { RelearnStaticSetup, RelearnEngineRunning };

struct GateStep      { OperatorGateId id; };
struct CompletedStep { ServiceFunctionOutcome outcome; };
struct FailedStep    { Error error; };
using ServiceFunctionStep = std::variant<GateStep, CompletedStep, FailedStep>;

class ServiceFunctionSession
{
  public:
    virtual ~ServiceFunctionSession() = default;

    // Pure: validates the request and returns the transport configuration this
    // session requires. Performs no I/O, so an invalid request is rejected
    // before the caller touches hardware.
    virtual Result<SsmTransportConfig> transport_setup() const = 0;

    // Runs I/O until the next operator gate, completion, or failure.
    virtual ServiceFunctionStep resume(ISsmTransport&, IClock&,
                                       const ICancellationToken&, IEventSink&) = 0;

    virtual void submit(GateResponse) = 0; // Accept | Decline
};
```

`transport_setup()` is deliberately the same idea as
`IFlashExecutor::transport_setup`: pure, pre-I/O, returning a descriptor the
platform applies. Parameter-range validation happens here, before any write.

### Why a step machine rather than an injected prompt port

The obvious alternative is an `IOperatorPrompt` port that the session calls
synchronously, with the desktop marshalling to the GUI thread and blocking the
worker until the operator answers. It has fewer moving parts, and it is
rejected: it reinstates backend-blocks-on-UI, the coupling that step 5b's
logging thread inversion removed, and it would place a Qt-shaped wait behind a
portable interface that a future Android host would have to satisfy.

The step machine keeps the backend synchronous, thread-free and dialog-free,
and leaves the waiting where the UI already is. `FlashWorkflow`
(`src/platform/desktop/common/flash/flash_workflow.h`) is the same shape one
layer up; it is a sibling, not a shared type — it constructs
`SerialPortActions`-backed transports and cannot move down into a portable
package.

### Gate contract

Two gates, both in relearn, both carrying no text: `OperatorGateId` is
semantic and the desktop owns the wording, exactly as `ConfirmationSpec::Id`
does for flash.

- `RelearnStaticSetup` (`:648`) — engine at operating temperature, car off the
  ground, engine off, ignition on, stick in P. This one precedes all I/O and
  could be pre-collected; it is not, so that both gates use one mechanism.
- `RelearnEngineRunning` (`:735`) — engine started, revs settled, stick in D,
  brake fully pressed. Issued only after step 2 is accepted. This is the gate
  the package exists for.

`submit(GateResponse::Decline)` ends the session as `Cancelled` — declining is
a normal outcome, not a failure. Calling `resume()` while a gate is
outstanding is `Internal`, and a test asserts it.

## Transport and configuration ownership

`ISsmTransport` (`src/backend/protocol/issm_transport.h`) is a raw byte-stream
request/response port whose documented contract is that wire framing
differences across K-Line, ISO14230 and CAN "are handled by `SerialPortActions`
itself, not by this seam". That is precisely these three operations: relearn
and read-parameters write `00 00 07 E1`-prefixed payloads to an
ISO-15765-configured port, set-parameters writes `SsmProtocol::addHeader`-framed
payloads to a 4800-baud K-Line port. `tester_id` and `target_id` are set and
used only on the K-Line path (`:151-152`, `:215`); the two ISO-15765 sessions
never reference them, and leave them at their zero defaults.

`ISsmTransport` has no `configure()`, so the platform configures
`SerialPortActions`. The one new portable type carries what it needs:

```cpp
struct SsmTransportConfig
{
    enum class Framing { Iso15765, Kline14230 };
    Framing framing;
    int bitrate_or_baud;        // 500000 (CAN) or 4800 (K-Line)
    std::uint32_t request_id;   // 0x7e1, ISO-15765 only
    std::uint32_t response_id;  // 0x7e9, ISO-15765 only
    std::uint8_t tester_id;     // 0xf0, K-Line only
    std::uint8_t target_id;     // 0x18, K-Line only
    bool add_iso14230_header;   // false; sessions self-frame
};
```

The desktop adapter `FastEcuSsmTransport` is reused unchanged.

## Six latent defects, and how they are resolved

**None of these three operations can complete successfully as written.** Two
can never report success, the third corrupts its own wire frames after the
first write, and two of the six defects are out-of-bounds buffer accesses. The
group is evidently unexercised code, in the same category as wave 3's
`hack_words()` discovery, and is resolved the same way: port what the code
evidently meant, correct the defect, name the divergence in the matrix.

That framing matters for review. A reviewer cannot check these ports against
"what the legacy does", because what the legacy does is fail. The check is
against what the legacy unambiguously *encodes* — addresses, values, order,
retry counts, timeouts — all of which survive intact and are transcribed with
line citations.

### Read parameters can never succeed

`:571-608`. The retry loop sets `responseOK` only when `received[4] == 0xF8`;
the post-loop check then returns an error unless `received[4] == 0xE8`. The two
conditions are mutually exclusive, so the function always returns
`STATUS_ERROR`.

The request is `0xA8` (read-address), whose SSM positive response is `0xE8`;
the `0xF8` in the retry loop is copied from a `0xB8` write loop. **Resolution:**
the retry loop accepts `0xE8`. Read-parameters becomes capable of succeeding —
this is the work's one functional fix.

### Relearn always reports failure

`:786`. The function's final statement is an unconditional `return
STATUS_ERROR;`, so a clean run is reported as a failure. Every internal error
return in the function is commented out (`:688`, `:694`, `:722`, `:733`,
`:771`, `:777`).

**Resolution:** the portable session returns success when the sequence
completes without a transport failure. The unconditional final error is a stub,
not a behavior.

The commented-out internal returns are a different matter and **stay
tolerant**: relearn logs a bad step-1 or step-2 response and proceeds, exactly
as today. That matches the call wave 3 made for MH8104's disabled response
checks — tolerance is real legacy behavior and is preserved, per the tail
design's "documented legacy quirks are preserved, not fixed".

### The relearn status poll never terminates early

`:746-763`. The poll sends `0xA8` but waits for `0xF8`, which cannot arrive, so
the loop always runs all 200 iterations instead of exiting when the TCU reports
the relearn complete.

**Resolution, deliberately partial:** the poll accepts `0xE8` so it stops
reporting every reply as an error, and the 200-iteration bound is preserved.
**No terminal condition is invented.** Which status value means "relearn
complete" is not recoverable from the legacy source, and guessing it would
fabricate a wire contract the way a misread constant does. The polled bytes are
surfaced in the relearn outcome, and the matrix carries a `VERIFY` note that
the early-exit condition is unresolved and needs a bench.

### Read-parameters can over-read its response buffer

`:592-608`. The length guard is `received.length() > 10`, but the decode reads
through `received[14]` (`:611-624`) — the nine values occupy bytes 5 through
14. A frame of 11 to 14 bytes passes the guard and is then indexed past its
end.

**Resolution:** the portable session requires at least 15 bytes before
decoding, and returns `BadResponse` otherwise.

### The relearn status poll never gets its addresses into the frame

`:740-747`. At that point `output` holds the 9-byte step-2 frame
`00 00 07 E1 B8 00 01 FD 09`. The poll rewrites it by index — and writes
`output[9]`, `output[10]` and `output[11]`, three bytes past the end. Qt 6's
non-const `QByteArray::operator[]` asserts `i < size()`, so a debug build
aborts here; a release build writes out of bounds and sends a 9-byte frame.

The addresses those three bytes carry are `0x01 0xFD` — the second of the two
status addresses the poll means to read. The frame the code evidently intends
is `00 00 07 E1 A8 00 00 01 FC 00 01 FD`: an `0xA8` read of `0x1FC` and
`0x1FD`.

**Resolution:** the portable session composes that 12-byte frame directly. This
is also why the poll is written as a fresh composition rather than a
transcribed index rewrite — the index rewrite is the defect.

### Set-parameters corrupts every frame after the first

`:210-215` builds the unframed payload `B8 00 01 6C <value>` and then
**reassigns** `output` to `SsmProtocol::addHeader`'s result — the framed
10-byte array `80 18 F0 05 B8 00 01 6C <value> <checksum>`.

Every subsequent parameter reuses that variable by index (`:237-238`,
`:261-262`, and so on through `:479`), writing `output[3]` and `output[4]`.
Those indices addressed the parameter's low address byte and value **before**
framing. Afterwards they address the SSM length byte and the `0xB8` service
ID. Each iteration then calls `addHeader` again on the already-framed array,
so the frames nest and grow five bytes per parameter:

- write 1 — `80 18 F0 05 B8 00 01 6C v1 cs` — correct;
- write 2 — `80 18 F0 0A 80 18 F0 6D v2 00 01 6C v1 cs cs2` — a framed frame,
  with the length byte and service ID overwritten by parameter data.

The TCU cannot answer `0xF8` to write 2, and write 2's check returns
`STATUS_ERROR` (`:227`, not commented out). **Set-parameters therefore performs
exactly one real write — `0x16c`, the IC 3→4 correction — and then aborts,
leaving the TCU half-configured.** That is worse than failing outright, and it
is the most consequential of the four defects.

**Resolution:** the table-driven loop composes each parameter's payload from
scratch and frames it exactly once, so all twelve frames are well-formed. This
is the same correction as the other three — port what the code evidently meant
— and it is what makes the restructure in
[Two structural improvements](#two-structural-improvements-both-named)
mandatory rather than cosmetic: a faithful transcription of the index
mutations would reproduce the corruption.

## Two structural improvements, both named

- **Read-parameters returns data, not log lines.** The legacy decodes nine
  values and emits them as `LOG_I` text (`:611-624`). The portable session
  returns a `TcuParameterReadout` struct and the dialog formats it — the
  umbrella's "samples carry stable channel ID, numeric/raw value, and unit; UI
  owns locale formatting" rule applied outside logging.
- **Set-parameters becomes a table.** The legacy is roughly 300 lines of
  twelve copy-pasted write-and-check blocks. A `constexpr` table of
  `{address, width, range, label_id}` plus one loop replaces it. The loop
  rebuilds and frames each payload from scratch, which is what corrects the
  framing defect above. Three details the restructure must not lose, each
  asserted by a test against the cited legacy lines:
  - **The write order is not the prompt order.** Prompts run 1→2, 2→3, 3→4,
    4→5, … (`:162-202`); writes run `0x16c` = 3→4, `0x16d` = 2→3,
    `0x16e` = 1→2, `0x16f` = 4→5, … (`:213-286`). The table preserves the wire
    order verbatim.
  - **AWD torque is one prompted value across two addresses** — high byte to
    `0x170`, low byte to `0x171` (`:309-334`) — so the table's rows are
    addresses, not parameters: ten parameter writes for nine values.
  - **A two-write commit closes the sequence**: `0x55` then `0xAA`, both to
    address `0x1ec` (`:455-479`). It is not a parameter and has no prompt, and
    a table keyed only on the nine prompted values would silently drop it,
    leaving every written correction uncommitted.

## Error handling

Every session covers six of the seven `ErrorKind` values:

| Kind | Raised for |
|---|---|
| `Timeout` | retries exhausted with no response |
| `Disconnected` | transport not open, or dropped mid-session |
| `BadResponse` | negative or malformed response where the legacy returns rather than tolerates |
| `Cancelled` | cancellation token observed, or an operator gate declined |
| `Unsupported` | protocol is neither `sub_tcu_denso_sh7055_can` nor `sub_tcu_denso_sh7058_can` |
| `Internal` | `resume()` called while a gate is outstanding |

**`InvalidConfig` has no producer here, deliberately.** It is the kind for an
invalid configuration or definition, and these sessions have neither: the only
configuration they carry is the protocol name, whose rejection is
`Unsupported`, and their only other input is `TcuParameterValues`, whose
members are `std::uint8_t` and `std::uint16_t`. Rather than manufacture a
runtime check that cannot fail, the taxonomy is recorded as six of seven with
this reason. A future session in this package that parses a definition would
reintroduce it.

The nine `promptInt` bounds (`:162-202`) are 0–255 for eight values and
0–65535 for AWD torque — exactly `std::uint8_t` and `std::uint16_t`. The
portable `TcuParameterValues` uses those types, so the bounds are enforced by
the value model rather than by a runtime check, and the dialog's spin-box
ranges are asserted against them in a UI test. This makes an out-of-range
parameter unrepresentable rather than rejectable, which is why `InvalidConfig`
above covers only the protocol.

## Desktop

The action picker stays where it is
(`src/ui/desktop/flash/tcu/flash_tcu_subaru_denso_sh705x_can.cpp:44-83`, shown
only when `cmd_type == "read"`). "Dump" continues to the legacy operation until
wave 5; "Relearn", "Read Param" and "Set Param" branch into a new
`ServiceFunctionDialog`. The "Turn ignition ON and press OK" warning at `:86`
stays a dialog pre-flight, unchanged.

Set-parameters gets **one form with nine spin boxes**, not nine sequential
modal dialogs: ranges come from the same table that validates them, and the
operator can review all nine before committing to any write. Read-parameters
renders `TcuParameterReadout` as a labelled table.

`src/platform/desktop/common/service_functions/` holds the worker, in the shape
of `FlashWorker`: it applies `SsmTransportConfig` to `SerialPortActions`,
constructs `FastEcuSsmTransport`, runs `resume()` on the worker thread, and
marshals `GateStep` to the GUI thread and the answer back through `submit()`.
This is the only Qt in the feature.

## Fidelity discipline

Unchanged from every wave since 5c, and it is what substitutes for a bench:

- Every exchange in a portable session carries a comment citing the legacy file
  and line it was transcribed from.
- Tests use byte-exact `expectWrite` scripts.
- Deliberate divergences are named in the matrix `notes` column, never silent:
  the six defect corrections above, plus the two structural improvements.
- `hardware_status` lands `experimental`. Nothing reaches `proven` from unit
  tests.

## Testing

`src/backend/protocol/testing/scripted_ssm_transport.h` already exists, so no
new test double is introduced; it gains gate-aware assertions.

Per session, the full seven-kind `ErrorKind` taxonomy, plus:

- **Set-parameters** — the wire order is asserted (`0x16c` = 3→4 first, not
  prompt order); an out-of-range value is rejected before any write reaches the
  transport.
- **Relearn** — both gates fire, in order; a declined gate yields `Cancelled`;
  `resume()` during an outstanding gate yields `Internal`; a bad step-1 or
  step-2 response is tolerated rather than fatal; the poll is bounded at 200
  iterations.
- **Read-parameters** — succeeds on `0xE8`, which the legacy could not; the
  nine decoded values map to the correct response offsets (`:611-624`).

Gates per PR, unchanged:

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure //:legacy_flash_drain
```

Plus at least 80% new-code coverage and the SonarCloud Quality Gate.

**`//:legacy_flash_drain` does not move in either PR.** The legacy class still
includes `serial_port_actions.h` for `tcuAction` 1, so its entry shrinks in
wave 5, not here. Stated explicitly so a flat ratchet is not read as no
progress.

## Sequence

Two PRs.

1. **Portable package** — three sessions, `SsmTransportConfig`, the parameter
   table, `ServiceFunctionSession` and its step types, tests. No UI wiring, no
   legacy edits. Registers the package in `PORTABLE_ROOTS` and the genquery.
2. **Desktop wiring and legacy removal** — the platform worker,
   `ServiceFunctionDialog`, the action dispatch rewrite; deletion of
   `tcu_setparam_subaru_ssm`, `tcu_readparam_subaru_ssm`,
   `tcu_relearn_subaru_ssm` and `promptInt` from the legacy class; matrix rows.

The split is deliberate. Unlike a drain wave, the legacy class survives this
work, so the removal is a partial edit rather than a file deletion; keeping it
out of PR 1 leaves the portable sessions reviewable against the legacy source
still sitting beside them.

## The ledger

The [flash qualification matrix](../../flash-qualification-matrix.md) gains a
clearly separated **Service functions** section with three rows —
`hardware_status=experimental`, `automated_evidence` naming the new test
labels, and `notes` carrying the six corrected defects and the unresolved
relearn poll-termination `VERIFY`. One ledger beats a second document, and
CLAUDE.md already points hardware-facing readers there.

## Handoff to wave 5

After both PRs land:

- `flash_tcu_subaru_denso_sh705x_can_operation.cpp` is flash-only, roughly
  1,700 lines down from 2,345.
- Wave 5 is **four** families, not five, and `SubaruTcuDensoSh705xCan` needs no
  mode enum and no conditional `family_requires_kernel_v`:
  `FlashEcuSubaruDensoSH7058Can`, `FlashEcuSubaruDensoSH7058CanDiesel`,
  `FlashTcuSubaruDensoSH705xCan`, `FlashEcuSubaruDensoSH705xDensoCan`.
- The drain goes 14 remaining families to 10.

Wave 5 keeps its own spec. Two findings from this investigation belong to it
and are recorded here only so they are not rediscovered: it is the first CAN
cluster that uploads a kernel, and all four of its families speak the npkern
`SUB_KERNEL_START_COMM` command protocol after upload rather than UDS; and
`FlashEcuSubaruDensoSH705xDensoCan` switches ISO-15765 to raw 29-bit CAN and
back **within one session** (`:147-150`, `:470`), which a second
`TransportKind` cannot express — superseding the tail design's
`TransportKind::CanRaw` scaling item.

## Risks

| Risk | Mitigation |
|---|---|
| The six defect corrections are misreadings and the legacy code was right | Each is a mutually exclusive condition pair, an unconditional `return STATUS_ERROR`, or an out-of-bounds index — none is a judgment call; all six named in the matrix; `experimental` is never upgraded by unit tests |
| Relearn's poll-termination condition stays unknown | The 200-iteration bound is preserved, no terminal condition is invented, the polled bytes are surfaced, and the matrix flags it `VERIFY` for the bench |
| A portable step machine is over-engineering for one family | It is the minimum that supports a mid-sequence gate without the backend blocking on UI; `logging_session` is the same shape already in the tree |
| A new package for three operations becomes a dumping ground | Named for a capability rather than a module, with the membership rule recorded in the package README |
| Relearn and set-parameters write live TCU adaptation values with no bench | Wire bytes and order are unchanged from what the legacy evidently intended; `experimental` status; the matrix is the standing list of what needs a bench |
| Set-parameters goes from writing one corrupted-then-aborted sequence to writing all twelve frames for real | This is the point of the fix, and it is the largest behavior change in the work: the operation begins doing what its UI has always claimed. Called out in the matrix `notes`, and the first bench item for this family |

## Amendments

### To the [step-5 tail flash drain design](2026-08-08-step5-tail-flash-drain-design.md)

Wave 5's `FlashTcuSubaruDensoSH705xCan` is a flash-only family. Its
`tcuAction` 2, 3 and 4 leave the flash drain entirely and are not wave-5 scope.
Wave 5's line count for that family drops accordingly; the wave still covers
every family the tail design assigned it.

### To the [modularization plan](../../modularization-plan.md)

Step 5's "Split `FileActions` and `MainWindow` responsibilities into
definition, calibration, checksum, logging, and flash use cases" gains a sixth:
service functions. It is the first portable backend package that is neither a
`FileActions` decomposition product nor part of the flash tail.
