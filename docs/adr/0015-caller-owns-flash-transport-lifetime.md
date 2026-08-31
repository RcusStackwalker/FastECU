# 0015. The caller owns flash transport lifetime

Status: accepted (2026-08-23)

## Context

Whether a flash executor closed its transport varied by family with no stated
reason. Six executors closed — the two EEPROM executors, the two M32R K-Line
UDS executors, and the two Denso K-Line executors — and five did not, all of
them CAN UDS. The split tracked which porting wave produced a file, not any
protocol requirement. The step-5c "close exactly once" rule that the closers
resembled was scoped to the EEPROM mode-attempt sequence, never to
`IFlashExecutor` at large.

The divergence was invisible on hardware. Every GUI path builds its adapter
with the non-owning constructor over MainWindow's session-lifetime
`SerialPortActions`, where `close()` resets an already-null `owned_serial_`
and nulls a raw pointer on an adapter destroyed moments later. It mattered
only for the owning path used by `apps/bench` — which already configures and
opens before handing the transport off, i.e. already followed the contract
adopted here.

## Decision

The caller owns transport lifetime. An executor never calls `configure()`,
`open()`, or `close()`. It receives a transport already configured and open,
uses it, and returns. It declares its required setup through a pure
`transport_setup(plan)` that performs no I/O.

Operations that belong to a protocol sequence — `setBaud()`,
`set_add_iso14230_header()`, `disable_lec_lines()`, `pulse_lec_2_line()`,
`enable_programming_voltage_line()` — are not lifecycle and stay in the
executor. The dividing line: a lifecycle operation is one whose correct number
of calls depends on who else is using the transport; a protocol operation is
one whose correct number of calls is fixed by the ECU's state machine.

`IFlashExecutor` splits into `IKlineFlashExecutor` and `ICanFlashExecutor` so
the transport a given executor requires is carried by the signature.
`bind_flash_attempt` is the only way to build the executor/transport pair, and
its `requires` clause rejects a mismatch at compile time; `BoundFlashAttempt`
type-erases the pair only afterward.

## Consequences

Six `dynamic_cast`s and six "transport does not implement …" error strings are
deleted, along with `open_can_iso15765_transport`, `IFlashExecutor`, and
`check_family_transport_match`. A mismatched executor/transport pair becomes
unrepresentable rather than checked.

The step-5c close guarantee — exactly once past `open()`, main error wins over
close error, close-only error returned — moves to `BoundAttempt::run()`, where
it holds for all eleven families instead of the two that asserted it.

The five CAN UDS executors gain a `close()` they never had. This is a no-op in
every current wiring: `FlashWorker` only ever receives non-owning desktop
adapters.

Family <-> plan matching stays a runtime check. Which family a plan is comes
from a ROM chosen at runtime, so no type can carry it; `FlashPlan`'s
`validate_and_build` invariant already makes that check total and makes the
subsequent `std::get` non-throwing.
