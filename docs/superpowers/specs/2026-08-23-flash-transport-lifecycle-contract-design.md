# Flash Transport Lifecycle Contract — Design

## Scope and status

Resolves [issue #208](https://github.com/RcusStackwalker/FastECU/issues/208):
whether a flash executor closes its transport is inconsistent across families,
and the inconsistency is an artifact of porting order rather than a decision.

This design does not document the divergence. It removes the executor's ability
to have one, by moving transport lifetime out of `IFlashExecutor` entirely and
making the executor/transport pairing a compile-time property.

Hardware status is unchanged: all eleven migrated families remain
**experimental** in the
[flash qualification matrix](../../flash-qualification-matrix.md), and no family
currently holds hardware evidence, so no qualification is affected. The change
is behavior-preserving on the wire — see [Wire behavior](#wire-behavior).

### In scope

- A stated lifecycle contract, recorded as ADR 0015.
- Splitting `IFlashExecutor` into `IKlineFlashExecutor` and `ICanFlashExecutor`.
- `BoundFlashAttempt` / `bind_flash_attempt`, and the relocation of
  configure/open/close into the single bound-attempt seam.
- Migrating all eleven executors, their tests, `FlashWorker`, `flash_workflow`,
  and `flash_dialog`.

### Out of scope

- A central `FlashFamily` → executor factory with an exhaustive-`switch`
  ratchet. Considered and deferred; see
  [Alternatives](#alternatives-considered).
- Any change to protocol sequences, timing, retry counts, or wire bytes.
- Bench qualification of any family.

## What the investigation found

The issue's inventory was incomplete, and its central premise was wrong in a way
that changes the risk calculus.

**Six executors close, not two.** Beyond the two EEPROM executors and the two
K-Line UDS executors named in the issue,
`subaru_denso_sh7055_02_executor.cpp:1006` and
`subaru_denso_mc68hc16y5_02_executor.cpp:915` also close, with the same
"main error wins over close error" ladder. The tally is six closers (two EEPROM,
four K-Line) and five non-closers (all CAN UDS).

**`close()` is not a wire operation in the shipping GUI.** Every GUI path builds
its adapter with the *non-owning* constructor over MainWindow's
session-lifetime `SerialPortActions` (`flash_workflow.cpp:172, 278, 375, 464,
552, 652, 657`). In that configuration `DesktopKlineFlashTransport::close()` and
`DesktopCanFlashTransport::close()` reset an already-null `owned_serial_` and
null the raw pointer; the port is never touched, and the adapter is destroyed
moments later regardless. The divergence the issue flags is therefore currently
unobservable on hardware. It is a live risk only for the *owning* path
(`open_desktop_can_flash_transport`), where `close()` genuinely destroys
`SerialPortActions`.

**The step-5c "close exactly once" rule was never universal.** Read in context
([step-5c design](2026-07-22-step5c-flash-preflight-execution-seam-design.md),
L505-529), it governs the EEPROM mode-attempt sequence, not `IFlashExecutor` at
large. The four K-Line closers were not following a global rule; they are the
files with no stated rationale, exactly as the issue suspected.

**`apps/bench` already implements the target contract.**
`open_desktop_can_flash_transport()` configures and opens before handing the
transport off, and bench never runs executors. The contract this design adopts
is the one half the codebase already follows.

## The contract

> **The caller owns transport lifetime.** An executor never calls `configure()`,
> `open()`, or `close()` on the transport it is given. It receives a transport
> that is already configured and open, uses it, and returns. Opening it, closing
> it, and deciding whether it outlives the call are the caller's business.
>
> An executor declares *how* its transport must be set up via
> `transport_setup(plan)`, which is pure: it validates the plan and returns the
> configuration, performing no I/O. A caller must apply that setup before
> `execute()`.
>
> Mid-session transport operations that are part of a protocol sequence —
> `setBaud()`, `set_add_iso14230_header()`, `disable_lec_lines()`,
> `pulse_lec_2_line()`, `enable_programming_voltage_line()` — are **not**
> lifecycle. They stay in the executor.

The dividing line: a lifecycle operation is one whose correct number of calls
depends on who else is using the transport. A protocol operation is one whose
correct number of calls is fixed by the ECU's state machine.

This is an ADR rather than a coding-style-guide entry by
[the ADR index's own test](../../adr/README.md): it changes how components fit
together — lifecycle ownership crosses the backend/platform seam, an interface
splits in two, and `FlashWorker` holds a different thing — not how a line of
code is written.

## Static guarantees

Three pairings are involved. They have different achievable guarantees, and
conflating them is what made the original divergence hard to see.

### Executor ↔ transport type: static

`IFlashExecutor` splits so the requirement lives in the signature:

```cpp
class IKlineFlashExecutor
{
  public:
    using TransportType = IKlineFlashTransport;
    using ConfigType = KlineConfig;

    virtual ~IKlineFlashExecutor() = default;

    // Pure: validates `plan` and returns the transport configuration this
    // executor requires. No I/O -- an invalid plan is rejected before the
    // caller touches hardware.
    virtual Result<KlineConfig> transport_setup(const FlashPlan& plan) const = 0;

    // `transport` is already configured per transport_setup() and open.
    // This call never opens or closes it.
    virtual Result<FlashExecutionResult> execute(const FlashPlan& plan, IKlineFlashTransport& transport,
                                                 IClock& clock, const ICancellationToken& cancellation,
                                                 IEventSink& events) = 0;
};
```

`ICanFlashExecutor` mirrors it with `ICanFlashTransport` and `Iso15765Config`.

This deletes all six `dynamic_cast`s in `src/backend/flash` (five in K-Line
executors, one in `open_can_iso15765_transport`) and all six
`"transport does not implement …"` error strings. Because each executor returns
its concrete config type, no `std::variant<KlineConfig, Iso15765Config>` is
needed and no runtime visit occurs.

The pairing then survives the type erasure `FlashWorker` needs — it holds
executor and transport separately today, and `requestStop()` needs
`request_unblock()` from the transport. Erase *after* the compiler has checked
the pair:

```cpp
class BoundFlashAttempt
{
  public:
    virtual ~BoundFlashAttempt() = default;
    virtual Result<FlashExecutionResult> run(IClock&, const ICancellationToken&, IEventSink&) = 0;
    virtual void request_unblock() noexcept = 0;
};

template <class Executor, class Transport>
    requires std::derived_from<Transport, typename Executor::TransportType>
std::unique_ptr<BoundFlashAttempt> bind_flash_attempt(FlashPlan, std::unique_ptr<Executor>,
                                                      std::unique_ptr<Transport>);
```

No API in the codebase then accepts a mismatched executor/transport pair. The
mismatch is unrepresentable, not merely checked. The seven construction sites in
`flash_workflow.cpp` already pair executor and adapter literally in a single
expression; they become `bind_flash_attempt(...)` calls the compiler validates
in place.

### Executor ↔ plan family: runtime, by necessity

`SubaruHitachiM32rKlineExecutor` must receive a `SubaruHitachiM32rKlinePlan`.
This cannot be made static: which family a plan is comes from a ROM the user
selects at runtime, so no compile-time type can carry it.

What makes the runtime check cheap and total is that **`FlashPlan` is already a
validated type**. `validate_and_build` enforces family ↔ transport-kind ↔
variant-alternative consistency (`flash_validation.cpp:24-60`, checked at L133)
through a `switch` over `FlashFamily` with no `default:`. Any `FlashPlan` that
exists has passed it. Two consequences:

- `check_family_transport_match` is replaced by `check_family(plan,
  FlashFamily)`. Its transport half is provably unreachable for a constructed
  plan.
- Once `check_family` succeeds, `std::get<PlanT>(plan.family_plan())` **cannot
  throw** — which matters because exceptions must never cross a port. That
  safety is implicit today; here it becomes a stated consequence of a documented
  invariant.

### Plan family ↔ transport kind: falls out

Once the family selects the executor and the executor's type fixes the transport
type, this pairing has no independent way to be wrong.

## The bound-attempt seam

`BoundAttempt<Executor, Transport>` is the single place the whole sequence
lives, replacing six per-executor prologues and six close ladders:

```cpp
Result<FlashExecutionResult> run(IClock& clock, const ICancellationToken& cancellation,
                                 IEventSink& events) override
{
    // Pure: validates the plan, derives config, touches no hardware. A bad
    // plan is rejected before the adapter is configured or opened.
    Result<typename Executor::ConfigType> setup = executor_->transport_setup(plan_);
    if (!setup.has_value())       return std::unexpected(setup.error());
    if (cancellation.cancelled()) return fail(ErrorKind::Cancelled, "cancelled before configure");
    if (Status s = transport_->configure(*setup); !s.has_value()) return std::unexpected(s.error());
    if (cancellation.cancelled()) return fail(ErrorKind::Cancelled, "cancelled after configure");
    if (Status s = transport_->open(); !s.has_value())            return std::unexpected(s.error());

    Result<FlashExecutionResult> outcome = executor_->execute(plan_, *transport_, clock, cancellation, events);

    // Exactly once, on every exit path past open(). Main error wins over close
    // error; a close-only error is returned. This is step-5c L521-529, promoted
    // from an EEPROM-family rule to the universal one.
    const Status closed = transport_->close();
    if (!outcome.has_value())
    {
        if (!closed.has_value())
        {
            events.log(LogLevel::Warning, "close failed after execution error");
        }
        return outcome;
    }
    if (!closed.has_value())
    {
        return std::unexpected(closed.error());
    }
    return outcome;
}
```

Ordering is preserved exactly. Today: validate → configure → cancel-check →
open → I/O. After: identical, on the same worker thread, within the same call.
The cancellation checks currently between `configure()` and `open()` in
`sh7055_02` and `mc68hc16y5_02` move here verbatim rather than being dropped —
the only place the existing prologues differ in shape, and the one detail to
watch while implementing.

`BoundFlashAttempt`, `bind_flash_attempt`, and the two split interfaces go in
the existing `flash_executor` `cc_library`, which is already a `PORTABLE_ROOTS`
entry, so `//:portable_closure` needs no registration changes.

## Caller changes

`FlashWorker` holds `std::unique_ptr<BoundFlashAttempt> attempt_` and
`std::unique_ptr<IClock> clock_` in place of three pointers. `run()` becomes
`attempt_->run(*clock_, cancellation_, events)`; `requestStop()` becomes
`cancellation_.cancel(); attempt_->request_unblock();`. The teardown contract —
`kTeardownWaitMs`, the `Qt::DirectConnection` event wiring and its thread-
affinity rationale — is untouched.

`FlashAttempt` in `flash_workflow.h` collapses to:

```cpp
struct FlashAttempt
{
    std::unique_ptr<BoundFlashAttempt> attempt;
    std::unique_ptr<IClock> clock;
};
```

`flash_dialog.cpp:90` moves two arguments instead of four — the only consumer of
`FlashAttempt`'s fields.

## Per-executor migration

Eleven files, identical in shape:

1. Base class becomes `IKlineFlashExecutor` or `ICanFlashExecutor`.
2. Add `transport_setup()`: `check_family` → `std::get<PlanT>` → the family's
   own `validate_*_plan()` → construct and return the config. Every line already
   exists in the prologue; it moves verbatim.
3. `execute()` takes the concrete transport reference. The five K-Line
   executors drop their `dynamic_cast`, its null check and error string, and
   their explicit `configure()`/`open()` pair. The six CAN executors drop their
   single `open_can_iso15765_transport()` call, which performed the same three
   steps. Six of the eleven additionally drop a close block — both `ScopedClose`
   structs and the four `if (!outcome) … if (!closed) …` ladders.
4. `check_family` stays at the top of `execute()` — cheap, and it keeps direct
   calls from unit tests safe.

Deleted outright: `open_can_iso15765_transport()`, `IFlashExecutor`,
`check_family_transport_match()`, and the `flash_executor.h:117-119` comment
describing the divergence. The `"Legacy never closes the port"` comments in the
five CAN UDS executors are replaced by a pointer to ADR 0015 — the
legacy-fidelity rationale no longer applies to a decision the executor does not
make.

## Tests

The scripted fakes gain a way to start configured-and-open (`start_open()` on
`ScriptedKlineFlashTransport` and `ScriptedCanFlashTransport`), since
`isOpen()` currently derives from the executor having called `open()`. No
production code reads `IKlineFlashTransport::isOpen()`, so nothing else shifts.

The roughly 62 close-related and 25 configure/open-related assertions across ten
executor test files relocate rather than disappear, and mostly sharpen:

- Assertions of the form "configure was called with baud X, tester Y" become
  direct assertions on `transport_setup()`'s returned config — a stronger test
  of the same fact, with no transport involved.
- Open/close failure injection (`set_open_result`, `set_close_result`) moves out
  of the ten executor tests into one new `bound_flash_attempt_test.cpp`.

`bound_flash_attempt_test.cpp` covers: invalid plan → no transport call at all;
configure fails; open fails; cancelled before configure; cancelled after
configure; execute error with close ok (execute error returned); execute ok with
close error (close error returned); execute error with close error (execute
error wins, warning logged); and `close_call_count == 1` on every path past
`open()`.

That list is exactly the step-5c L521-529 guarantee. It is currently asserted
twice — EEPROM CAN and EEPROM K-Line — and unasserted for the other nine
families. Afterward it is asserted once and holds for all eleven.

`flash_worker_test.cpp`'s fake executor becomes a fake `BoundFlashAttempt`,
which no longer needs a transport at all.

## Wire behavior

| Family group | configure/open | close | Wire delta |
|---|---|---|---|
| 6 CAN (5 UDS + EEPROM CAN) | moves from `execute()` prologue to `BoundAttempt::run()`, same thread, same order | **new** for the 5 UDS | none |
| 5 K-Line (2 UDS + 2 Denso + EEPROM K-Line) | same relocation | relocated, still exactly once | none |

The five CAN UDS executors gain a `close()` they deliberately never had. This is
the only behavioral delta in the design, and it is a no-op in every current
wiring: `FlashWorker` only ever receives non-owning desktop adapters, where
`close()` nulls a pointer on an object destroyed microseconds later. No
`FlashWorker` path receives an owning transport.

## Verification

```sh
bazel test --config=release //...          # all suites, incl. bound_flash_attempt_test
bazel build //:portable_closure            # no new registration; flash_executor is already a root
prek run --all-files
bazel run //:clang_tidy_report_changed
```

Plus one deliberate manual check: read the diff of each of the eleven executors
filtered to removed prologue and close lines, confirming each removed line
reappears verbatim in either `transport_setup()` or `BoundAttempt::run()`. That
is what turns "no wire change" from a claim into something a reviewer verifies
per file.

## Rollout

Two pull requests.

1. **Contract and mechanism.** ADR 0015, the two split interfaces,
   `BoundFlashAttempt`, `bind_flash_attempt`, `check_family`, and
   `bound_flash_attempt_test.cpp`. Nothing references the new interfaces yet, so
   it reviews on its own merits.
2. **Migration.** All eleven executors, their tests, `flash_workflow.cpp`,
   `FlashWorker`, `flash_dialog.cpp`; deletes `IFlashExecutor`,
   `open_can_iso15765_transport`, and `check_family_transport_match`.
   Necessarily atomic — an interface cannot be half-split — but the per-family
   diffs are independent and review family by family.

A transitional `bind_flash_attempt` overload taking the old `IFlashExecutor`
would let PR 2 split further. It is rejected: that is the shape of debt
`//:serial_compat_allowlist` exists to prevent.

## Documentation

- **ADR 0015**, a new `0015-caller-owns-flash-transport-lifetime.md`, added to
  the index table in [the ADR README](../../adr/README.md). ADRs 0009-0014 are
  retired and their numbers are not reused.
- The contract comment on the two executor interfaces in `flash_executor.h`.
- A one-line pointer from the
  [step-5c design](2026-07-22-step5c-flash-preflight-execution-seam-design.md)
  noting its EEPROM-scoped close rule was promoted to the universal contract.
- **No entry in the [tech-debt roadmap](../../tech-debt.md).** The issue
  proposed one on the assumption the
  divergence would be documented rather than removed. Since it is removed, an
  entry would describe debt that no longer exists.
- The [flash qualification matrix](../../flash-qualification-matrix.md) needs no
  row changes: `portable` stays `yes` and every test-label name is unchanged.

Issue #208 closes with a summary noting the two things the investigation got
wrong: the inventory was six closers rather than two, and `close()` was never
wire-visible in the GUI.

## Alternatives considered

**Document the divergence, change no code.** The issue's own suggestion. Leaves
the next ported family to rediscover the rule by reading eleven files, and
leaves an interface where the wrong answer is expressible.

**Executors close what they open.** Add `ScopedClose` to the five CAN UDS
executors so every executor closes. Symmetric with `open()` and consistent with
the step-5c rule, but it breaks session reuse on the *owning* bench transport,
and it is a behavior change on five deliberately legacy-faithful hardware paths
rather than a no-op.

**A free `transport_setup_for(plan)` dispatcher** in a new target, switching on
`FlashFamily`. Fewer interface changes, but it needs dependencies on all nine
per-family validator libraries to keep the no-I/O-before-rejection guarantee,
and it splits each family's knowledge across two files — the exact problem the
issue describes.

**Caller derives the setup inline in `FlashWorker`.** Smallest diff; puts family
knowledge in the platform layer, above the backend boundary.

**A central family → bound-attempt factory** with a total `switch` and
`-Werror=switch`, so a new `FlashFamily` fails the build until wired. Deferred
rather than rejected. The seven `flash_workflow.cpp` sites already pair executor
and plan literally, `bind_flash_attempt`'s `requires` clause type-checks the
transport half at each site, and `family_matches_transport_variant`'s
`default:`-free switch already forces an edit per new family. Adding the factory
also means introducing `-Werror` to a repository that currently sets none. Worth
revisiting if executor selection ever spreads beyond `flash_workflow.cpp`.
