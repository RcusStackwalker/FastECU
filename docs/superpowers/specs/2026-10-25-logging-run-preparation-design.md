# Logging run preparation

Status: implemented; retained for delivery before separate history-preserving retirement.

## Intent and scope

Concentrate the desktop choices for one logging run behind one preparation
interface. At design review, [MainWindow](../../../src/ui/desktop/widgets/menu_actions.cpp)
separately chose the engine registration key, acquisition protocol, definition
filter and polling policy, then called `MakeDesktopLoggingSnapshot`, a snapshot
adapter that only converted a string. The new module gives those choices locality and
returns matching prepared inputs together.

This is a small refactor. The user chose preparation only, preservation of the
existing SSM fallback, and a pure UI-owned function. Engine dispatch, run
lifecycle, identification, CSV, operator notices and backend validation retain
their existing owners. No new abstract seam, adapter or runtime dependency is
needed; preparation depends only on in-process values.

The [logging decisions](../../design-notes.md#logging) and
[logging contracts](../../reference/logging-contracts.md) own the implemented
behavior. This document preserves the approved design and its alternatives until
its original contents are delivered and it can be retired separately.

## Approaches considered

1. **Complete preparation function — selected.** One call resolves desktop
   defaults and delegates validation and immutable capture to the backend. Its
   interface returns the registration key and logging run snapshot together,
   providing depth without changing startup error handling.
2. **Protocol resolver.** Return the protocol, filter, policy and registration
   key, then let the caller prepare the snapshot. Defaults could be inspected
   independently, but the caller would still assemble matching values. No
   current consumer needs that flexibility.
3. **Preparation inside `DesktopLoggingRun::start()`.** The common caller would
   do less work, but preparation errors and acquisition startup failures need
   different presentation. Combining them would require a phase-bearing error
   result and changes to run-owner test setup and error precedence.

## Interface and ownership

Proposed builder choices: put the ordinary, Qt-free module at
`src/ui/desktop/logging/logging_run_preparation.{h,cpp}`, in `fastecu::ui`, with a
package-owned GoogleTest suite and generated Bazel targets. Desktop default
selection is UI-owned policy even though its computation needs no Qt.

```cpp
namespace fastecu::ui
{
struct LoggingRunChoices
{
    std::string_view saved_protocol;
    std::string_view ssm_definition_filter;
    logging::LoggingTarget target;
};

struct PreparedLoggingRun
{
    std::string registration_key;
    logging::LoggingRunSnapshot snapshot;
};

Result<PreparedLoggingRun> prepareLoggingRun(const logging::LoggerModel& model,
                                             LoggingRunChoices choices);
}
```

Inputs are borrowed only for this synchronous call. Successful output owns its
key and snapshot; no model, settings or input-string reference survives. The
implementation resolves defaults, calls the existing backend
`PrepareLoggingRun` once and propagates its error unchanged. It performs no
I/O, emits no events and changes no caller state.

The returned registration key matches the snapshot's acquisition protocol when
produced. The result is an ordinary aggregate: it does not prevent a caller
from later replacing fields. The engine deliberately continues to accept
separate configuration and snapshots, including custom test registration keys.
Do not add a new equality check or derive engine dispatch from the snapshot.

The snapshot's `ProtocolKey()` supplies the effective definition filter.
`Selection().protocol` is captured as it is today; do not normalize it or
require equality with the effective filter.

## Preserved behavior

Resolve exact, case-sensitive strings without trimming or normalization:

| Saved protocol | Registration key | Acquisition protocol | Definition filter | Poll timeout | Silence threshold | Reconnect threshold | Retry period |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `MUT_DMA` | `MUT_DMA` | `kMutDma` | `MUT_DMA` | 50 ms | 20 | 100 | 20 |
| `CDBG` | `CDBG` | `kCdbg` | `CDBG` | 50 ms | 20 | 100 | 20 |
| Every other string | `SSM` | `kSsm` | Supplied SSM definition filter | 300 ms | 10 | 30 | 10 |

The saved protocol and SSM definition filter remain distinct inputs, even when
startup normally gives them the same text. Empty, unknown and differently cased
saved strings retain the SSM fallback. Empty SSM filters retain the existing
`SSM logging protocol filter is empty` error. MUT/DMA and CDBG ignore the supplied
SSM filter, including an empty one.

Capture the selected ECU/TCU target before preparation. Backend validation order,
error kind/detail, measurement identity and ordering, support rules, compiled
conversions and owned capture remain as specified by the existing snapshot
contract. Pending edits affect subsequent runs.

MainWindow prepares before calling the existing run owner. Preparation failure
keeps the current detailed log and plain-text notice, without invoking a
protocol factory or opening CSV. Run-owner or engine startup failure retains
its current generic notice. Identification continuations and reentrancy remain
unchanged.

## Integration and deletion

MainWindow supplies captured choices, handles a preparation error as it does
today, converts the returned key to `LogSessionConfig::protocol_id`, and moves
the snapshot into the existing `DesktopLoggingRunStart`. Keep CSV directory,
filename and start sequencing with their present owners.

Remove `active_log_value_protocol_filter_`: its production uses are preparation
and the immediate display refresh, which can use the active snapshot's key.
Update its one private-field assertion to verify the actual captured snapshot
before stopping the run. Keep the current guard around post-start rendering so
a failed, stopped or replaced run is handled by the existing owner semantics.

Delete both snapshot adapter files. Replace `DesktopLoggingSnapshot` alias uses
with the identical backend-owned `LoggingRunSnapshot` type and direct includes.
This is a spelling/dependency change, including in the engine and factory
declarations; their callable behavior remains unchanged. Do not relocate the
alias into another forwarding header. Keep the value adapter and its formatting,
sample and cache assertions; its tests can prepare snapshots directly through
the backend.

Regenerate managed BUILD files and review the resulting dependencies. Use the
existing layer visibility groups without widening them. The preparation target
and its tests must have no Qt, platform runtime or worker dependency.

## Verification and documentation

The preparation interface is the test surface for the newly concentrated
decisions. Cover the three routes, representative fallback strings, a distinct
SSM filter, all four exact policy fields, both targets, ignored MUT/CDBG filters,
unchanged backend errors and ownership after inputs/model changes. Use literal
expectations rather than deriving the expected route from production code.

Retain backend validation tests and existing display assertions. Exercise the
affected snapshot, value adapter, engine/custom-factory, protocol-registration,
desktop-run, composition and MainWindow suites. In particular, preserve saved
protocol restoration, different flash/log transports, identification-before-start,
target capture, preparation failure before factory invocation, pending selection
isolation and replacement-run behavior. Add no tests of private helpers.

Before implementation delivery, run the applicable release build/full test,
formatting, Gazelle and static-analysis gates from the repository instructions;
platform packaging remains a CI gate. This design-only change uses the required
Markdown link check and scoped diff review, without building the application.

Implementation updates the desktop preparation ownership in the existing logging
contract and links to that description from the logging design notes. This
introduces no new domain concept, so use the existing
[domain language](../../../GLOSSARY.md) without a parallel glossary or new ADR.
Retire this design after completion only under the repository's
[history-preservation rules](../../README.md#recover-completed-work-and-deleted-source-citations).

Exact private helper names, local variables and test fixture layout remain
builder choices within these constraints.
