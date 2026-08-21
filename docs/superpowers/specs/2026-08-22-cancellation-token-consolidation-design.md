# Cancellation Token Consolidation

## Scope and status

Resolves [issue #88](https://github.com/RcusStackwalker/FastECU/issues/88),
"Refactor ICancellationToken subclasses." `ICancellationToken`
(`src/backend/ports/cancellation.h`) currently has five subclasses; two of
them are duplicate implementations of the same idea (an atomic-bool flag,
flipped from the GUI thread, polled from a worker thread) that arose
independently in two different layers:

- `QtCancellationToken` (`src/platform/desktop/common/ports/qt_cancellation_token.h`)
  — owned by `LoggingWorker`. Despite its name and its home in the Qt-linked
  `ports` build target, it includes nothing but `<atomic>`; it has no Qt
  dependency.
- `CancellationSource`/`Token` (`src/backend/flash/flash_cancellation.h`) —
  owned by `FlashWorker`, and directly instantiated in 7 ECU executor test
  files. Same underlying mechanism, but split into a source class plus a
  private nested token class, with `trip()` instead of `cancel()` and no
  `reset()`.

Both owners use their instance the same way: construct one per run, flip it
from the GUI thread on `requestStop()`, hand the `ICancellationToken`
reference to backend code. Neither owner's split-vs-unified shape is load
bearing — nothing in either call site relies on the token being a distinct
object from the flag owner.

This is a pure internal refactor: it changes no externally observable
behavior, no wire protocol, no XML/patch format, nothing bench-qualification
relevant. It touches the build graph (`//:portable_closure` roots, several
`BUILD.bazel` files) and roughly 20 files across two layers, which is why it
gets a spec rather than being handled as a bounded change.

## Current state: the other three subclasses

Two of `ICancellationToken`'s five subclasses are the duplicates above. The
other three are correctly scoped as-is and are **out of scope**:

- `SigintCancellationToken` (`apps/bench/sigint_cancellation_token.h`) —
  hooks `SIGINT` via a signal handler for the non-GUI bench CLI. A genuinely
  different cancellation source (OS signal vs. a GUI-flipped flag), not a
  duplicate of the flag mechanism.
- `FakeCancellationToken` (`src/backend/ports/testing/fake_cancellation_token.h`)
  — the project's one test double for this interface: predicate support,
  cancel-on-check-N support, thread-safe. Already package-owned per
  [ADR 0008](../../adr/0008-use-package-owned-mocks.md), used across ~20 test
  files. No changes.
- `NeverCancelled` (`src/backend/protocol/transport_legacy_compat.h`) — a
  trivial always-`false` stub, `detail`-scoped, used only to satisfy legacy
  adapter call sites that predate cancellation-token parameters. This one
  *is* touched (see below) but not because it's broken — folding it into the
  new canonical class removes a third redundant "always reports the flag
  value" implementation for free.

## New canonical class: `ManualCancellationToken`

`src/backend/ports/manual_cancellation_token.h`, header-only, in
`namespace fastecu` (matching `FakeCancellationToken`'s namespace):

```cpp
#pragma once
#include <atomic>
#include "src/backend/ports/cancellation.h"

namespace fastecu
{

// A cancellation flag flipped by the owning code (typically GUI-thread
// teardown) and polled from wherever the ICancellationToken& ends up --
// typically a worker thread running backend logic. One instance per
// operation attempt; construct a fresh one rather than reusing across runs.
class ManualCancellationToken final : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return flag_.load(std::memory_order_relaxed);
    }
    void cancel()
    {
        flag_.store(true, std::memory_order_relaxed);
    }

  private:
    std::atomic<bool> flag_{false};
};

} // namespace fastecu
```

Design decisions carried over from the brainstorming discussion:

- **Single self-contained class, not a source/token split.** Both existing
  call sites already get the encapsulation a split would provide — for free
  — because the concrete `ManualCancellationToken` stays a private member of
  its owner (`FlashWorker`/`LoggingWorker`), and only the abstract
  `ICancellationToken&` crosses into backend code. A separate `Token` type
  buys nothing that ownership doesn't already provide.
- **`cancel()`, not `trip()`.** Matches the interface's own `cancelled()`
  naming.
- **No `reset()`.** `QtCancellationToken::reset()` is confirmed dead code —
  grep across the repo shows no caller. Both current owners already
  construct a fresh instance per run rather than reusing one. YAGNI.
- **`final`**, matching `FakeCancellationToken`/`SigintCancellationToken`/
  `NeverCancelled`'s existing convention (`QtCancellationToken` was
  inconsistently non-`final`).

`src/backend/ports` is already a `PORTABLE_ROOTS` entry
(`scripts/check-portable-closure.py`) with `{"ports"}` as its required
target, and `manual_cancellation_token.h` joins the existing `ports`
`cc_library`'s `hdrs` list in `src/backend/ports/BUILD.bazel`. No new
`//:portable_closure` genquery or `PORTABLE_ROOTS` entries are needed — this
is a header added to an already-checked target, and it was already Qt-free
under the old name.

## Consumer migration

**`FlashWorker`** (`src/platform/desktop/common/flash/flash_worker.h`/`.cpp`):

- `#include "src/backend/flash/flash_cancellation.h"` →
  `#include "src/backend/ports/manual_cancellation_token.h"`.
- `CancellationSource cancellation_;` → `ManualCancellationToken cancellation_;`.
- `cancellation_.trip();` → `cancellation_.cancel();`.
- `cancellation_.token()` (passed into `executor_->execute(...)`) →
  `cancellation_` directly (same object now).

**`LoggingWorker`** (`src/platform/desktop/common/logging/logging_worker.h`/`.cpp`):

- `#include "src/platform/desktop/common/ports/qt_cancellation_token.h"` →
  `#include "src/backend/ports/manual_cancellation_token.h"`.
- `QtCancellationToken cancellation_;` → `ManualCancellationToken cancellation_;`.
- `cancellation_.cancel();` call site is unchanged (method name already
  matches).

**`transport_legacy_compat.h`**: delete the `NeverCancelled` class. Rewrite
`detail::never_cancelled()`:

```cpp
inline const ICancellationToken& never_cancelled()
{
    static const ManualCancellationToken token; // never flipped
    return token;
}
```

Requires adding `#include "src/backend/ports/manual_cancellation_token.h"`
to this header (it already depends on `//src/backend/ports` in
`src/backend/protocol/BUILD.bazel`, so no new `deps` entry).

## Deletions

- `src/backend/flash/flash_cancellation.h` — deleted (`CancellationSource`/
  `Token` fully replaced).
- `src/backend/flash/flash_cancellation_test.cpp` — deleted; its three cases
  merge into the new test below.
- `src/platform/desktop/common/ports/qt_cancellation_token.h` — deleted;
  removed from the Qt-linked `ports` `cc_library`'s `normal_hdrs` in
  `src/platform/desktop/common/ports/BUILD.bazel`.
- `src/backend/flash/BUILD.bazel`: remove `flash_cancellation.h` from
  `flash_executor`'s `hdrs` and delete the `flash_cancellation_test`
  `fastecu_portable_gtest` target. No `deps` change needed —
  `flash_executor` already depends on `//src/backend/ports` directly (it
  needs `cancellation.h` today for the `ICancellationToken&` parameter on
  `IFlashExecutor::execute`).

## Test consolidation

New `src/backend/ports/manual_cancellation_token_test.cpp`
(`fastecu_portable_gtest`, added to `src/backend/ports/BUILD.bazel`) absorbs:

- From `flash_cancellation_test.cpp`: starts-not-cancelled,
  cancel-makes-cancelled-true (renamed from "trip makes token report
  cancelled"), cancel-is-idempotent.
- From `qt_port_adapters_test.cpp`'s `QtCancellationTokenTest` suite:
  fresh-token-is-not-cancelled (duplicate of the above, drop it),
  cancel-sets-flag (duplicate, drop it).

Net: one test file, five cases collapse to three (duplicates removed
between the two source suites, no coverage lost).

`qt_port_adapters_test.cpp`'s `QtClockTest` cases that use
`QtCancellationToken token;` purely as a throwaway token argument to
`clock.sleep(...)` switch to `ManualCancellationToken` — no behavior change,
`//src/backend/ports` is already a dep of this test target.

## Mechanical migration: ECU executor tests

`grep -rl "CancellationSource" src/backend/flash/ecu/*_test.cpp` currently
matches 7 files (`mitsu_colt_m32r_can_executor_test.cpp`,
`subaru_hitachi_m32r_can_executor_test.cpp`,
`subaru_mitsu_m32r_kline_executor_test.cpp`,
`subaru_hitachi_m32r_kline_executor_test.cpp`,
`subaru_tcu_cvt_mitsu_mh8104_can_executor_test.cpp`,
`subaru_tcu_cvt_hitachi_m32r_can_executor_test.cpp`,
`subaru_tcu_cvt_mitsu_mh8111_can_executor_test.cpp`), each with several
dozen call sites of the shape:

```cpp
fastecu::flash::CancellationSource cancellation;
...
const auto result = executor.execute(plan, transport, clock, cancellation.token(), events);
```

and occasionally:

```cpp
cancellation.trip();
```

This is a pure rename with no semantic change:

- `fastecu::flash::CancellationSource` → `fastecu::ManualCancellationToken`
- `cancellation.token()` → `cancellation`
- `cancellation.trip()` → `cancellation.cancel()`

The implementation plan should do this as a scripted find/replace across the
matched files (verified by a full test run afterward), not hand-edited file
by file — the volume is mechanical, not judgment-requiring. Each affected
test target's `BUILD.bazel` `deps` swaps `//src/backend/flash:flash_executor`
(previously the transitive source of `CancellationSource`) for
`//src/backend/ports` where it isn't already present — expected to already
be present in most of these targets via other portable dependencies; verify
per-target at implementation time.

## Testing

- `bazel test //src/backend/ports:manual_cancellation_token_test` — new
  test, covers the consolidated class.
- `bazel test //src/backend/flash/ecu/...` — the 7 migrated executor test
  targets, confirming the rename preserved behavior.
- `bazel test //src/platform/desktop/common/flash:test_flash_worker` and
  `//src/platform/desktop/common/logging:test_logging_worker` — confirm both
  GUI workers still cancel correctly through the new type.
- `bazel test //src/platform/desktop/common/ports:test_qt_port_adapters` —
  confirms the merged/removed cases and the `QtClockTest` token swap.
- `bazel test //src/backend/protocol:...` (transport_legacy_compat's
  package) — confirms `never_cancelled()`'s behavior is unchanged.
- `bazel run //:portable_closure` — confirms the new header doesn't
  introduce a Qt/JNI leak into `src/backend/ports`.
- Full `bazel test //...` before merge, per this repo's normal gate.

No new test *behavior* is being specified — every case above already exists
today under the old types; this is a rename-and-consolidate, verified by
the existing assertions continuing to pass under the new names.

## Out of scope

- `SigintCancellationToken` and `FakeCancellationToken` — already correctly
  designed and scoped, not touched.
- Any change to `ICancellationToken` itself (the interface) — untouched.
- Any change to cancellation *semantics* (when/how a worker decides to
  cancel) — this is a type consolidation, not a behavior change.
