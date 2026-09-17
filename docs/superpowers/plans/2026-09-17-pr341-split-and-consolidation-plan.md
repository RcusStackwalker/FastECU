# PR #341 Split and Consolidation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract two self-contained slices of pull request #341 into their own
pull requests, rebase #341 onto them, then consolidate the remainder so it
reuses shared facilities instead of redeclaring them.

**Architecture:** Nothing is redesigned. Tasks 1–4 move already-written,
already-passing code onto two new branches. Task 5 rebases #341. Tasks 6–10 are
behaviour-preserving refactors on #341: every wire expectation in every
characterization test stays byte-identical, and a step that needs an assertion
edited is a failed step, not an assertion to edit.

**Tech Stack:** C++23, Bazel (version pinned in `.bazelversion`), Qt 6,
GoogleTest and Google Mock, QtTest, `prek`.

**Spec:** `docs/superpowers/specs/2026-09-17-pr341-split-and-consolidation-design.md`

## Global Constraints

- Baseline is `master` at `e0e264a2`. The subject branch is
  `codex/flash-wave5-implementation` (pull request #341).
- Bazel is the only target graph. Never add a target outside it.
- Work lands through pull requests: branch, commit, push, open a PR. `prek`
  refuses commits made directly on `master`.
- End every commit message with:
  `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`
- End every pull request description with:
  `🤖 Generated with [Claude Code](https://claude.com/claude-code)`
- Backend operations return `fastecu::Result<T>`, checked with `.has_value()`
  and never the implicit `operator bool`.
- Pure protocol and flash logic uses `bytes::Byte` / `bytes::Bytes` /
  `bytes::ByteView`. `QByteArray` is a boundary type only.
- Tests are package-owned and co-located. Mocks and fakes are package-owned.
- Register new portable targets in `PORTABLE_PACKAGES`
  (`bazel/portable_targets.bzl`). Ratchet lists only shrink; an entry may
  never go in.
- No task in this plan changes a wire byte, a timeout, a retry count, or any
  qualification row. All four wave-5 families stay automated-only and
  experimental.
- Per-task gate unless a task says otherwise:
  `bazel test --config=release //src/backend/flash/...`
- Per-pull-request gate:

```sh
bazel test  --config=release --nocache_test_results //...
bazel build --config=release //:fastecu //:portable_closure
bazel test  --config=release //:serial_compat_allowlist //:legacy_flash_drain //:windows_preprocessor_guards
bazel run   //:clang_tidy_report_changed
prek run --all-files
```

---

### Task 1: PR A — clear sticky ISO-14230 header state on CAN configure

**Files:**
- Modify: `src/platform/desktop/common/transport/desktop_can_flash_transport.cpp`
- Test: `src/platform/desktop/common/transport/desktop_can_flash_transport_test.cpp`

**Interfaces:**
- Consumes: `SerialPortActions::set_add_iso14230_header(bool)` and
  `get_add_iso14230_header()`, both already on `master` and both mocked with
  stateful `ON_CALL` defaults in `src/platform/desktop/common/serial/testing/fake_backend.h`.
- Produces: `DesktopCanFlashTransport::configure()` runs ten setters instead of
  nine, the tenth being `set_add_iso14230_header(false)`.

This is a defect on `master`, not a wave-5 defect: a CAN flash that follows a
K-Line session on the shared `SerialPortActions` facade inherits ISO-14230
auto-header state, and the first CAN frame goes out with a K-Line header
attached. It affects the eight CAN families already merged.

- [ ] **Step 1: Branch off master**

```bash
cd /Users/amarkelov/claude-hobby/sonar-fastecu
git fetch origin
git checkout -b fix/can-configure-clears-iso14230-header origin/master
```

- [ ] **Step 2: Write the failing test**

Add this to `desktop_can_flash_transport_test.cpp`, inside the
`TestDesktopCanFlashTransport` class, immediately after
`configureSucceedsWhenEverySetterSucceeds()`:

```cpp
    // A K-Line session on the same facade leaves ISO-14230 auto-headers on.
    // configure() must clear that state, or the first CAN frame goes out with
    // a K-Line header attached.
    void configureClearsStickyIso14230HeaderState()
    {
        FakeBackedSerial serial;
        QVERIFY(serial->set_add_iso14230_header(true));
        SerialPortActions *observed = serial.get();

        DesktopCanFlashTransport transport(serial.release());
        const auto result = transport.configure(
            Iso15765Config{.bitrate = 500000, .request_id = 0x7E0, .response_id = 0x7E8, .extended_id = false});

        QVERIFY(result.has_value());
        QCOMPARE(observed->get_add_iso14230_header(), false);
    }
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `bazel test --config=release //src/platform/desktop/common/transport:all --nocache_test_results`

Expected: FAIL. `configureClearsStickyIso14230HeaderState` reports
`Compared values are not the same` — `get_add_iso14230_header()` is still
`true`, because `configure()` never clears it.

- [ ] **Step 4: Write the implementation**

In `desktop_can_flash_transport.cpp`, inside `configure()`, immediately after
the `set_iso15765_destination_address` block and before `return {};`:

```cpp
        if (!serial_->set_add_iso14230_header(false))
        {
            return fail(ErrorKind::InvalidConfig, "set_add_iso14230_header failed");
        }
```

In the same function, extend the existing ordering comment's final line so it
reads:

```cpp
        // destination IDs, then clears any ISO-14230 auto-header state that
        // may survive from a previous K-Line session on the shared facade.
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `bazel test --config=release //src/platform/desktop/common/transport:all --nocache_test_results`

Expected: PASS.

- [ ] **Step 6: Extend the three existing setter-ordering tests**

`configure()` now runs ten setters, so the tests that enumerate them must
follow. Make exactly these four edits in `desktop_can_flash_transport_test.cpp`:

In `configureChecksEveryBooleanSetterInOrderAndStopsAtFirstFailure()`, after the
`set_iso15765_destination_address` line:

```cpp
        EXPECT_CALL(serial.fake(), set_add_iso14230_header(::testing::_)).Times(0);
```

In the data function feeding `configureFailsAtEachRemainingSetterInTurn()`,
after the `set_iso15765_destination_address` row:

```cpp
        QTest::newRow("set_add_iso14230_header") << 9;
```

In `configureFailsAtEachRemainingSetterInTurn()`, after the
`set_iso15765_destination_address` expectation:

```cpp
        expectSetterAt(EXPECT_CALL(serial.fake(), set_add_iso14230_header(false)), 9, setterIndex);
```

In `configureSucceedsWhenEverySetterSucceeds()`, after the
`set_iso15765_destination_address` expectation:

```cpp
        EXPECT_CALL(serial.fake(), set_add_iso14230_header(false)).WillOnce(::testing::Return(true));
```

- [ ] **Step 7: Fix the comment and the duplicated assertion**

Above `configureSucceedsWhenEverySetterSucceeds()`, change `all nine setters`
to `all ten setters`.

Inside that function the branch tip ends with `QVERIFY(result.has_value());`
written twice. Delete the second one, leaving the function ending:

```cpp
        QVERIFY(result.has_value());
    }
```

- [ ] **Step 8: Run the full transport suite**

Run: `bazel test --config=release //src/platform/desktop/common/transport:all --nocache_test_results`

Expected: PASS, all cases.

- [ ] **Step 9: Run the pull-request gate**

Run the per-pull-request gate from Global Constraints.

Expected: every command passes.

- [ ] **Step 10: Commit and open the pull request**

```bash
git add src/platform/desktop/common/transport/desktop_can_flash_transport.cpp \
        src/platform/desktop/common/transport/desktop_can_flash_transport_test.cpp
git commit -F - <<'EOF'
fix(flash): clear stale K-Line header state on CAN configure

DesktopCanFlashTransport::configure() did not clear ISO-14230 auto-header
state, so a CAN flash following a K-Line session on the shared
SerialPortActions facade inherited it and sent its first frame with a
K-Line header attached. Clear it as the tenth and final setter.

Also drops a QVERIFY duplicated in
configureSucceedsWhenEverySetterSucceeds().

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
git push -u origin fix/can-configure-clears-iso14230-header
```

Open the pull request with a body explaining that this is a `master` defect
affecting the eight already-merged CAN families, ending with the attribution
line from Global Constraints.

**Do not** cherry-pick commit `8f97613c` wholesale. It also edits two
backend-operation-trace assertions and the mixed-transport source, none of
which exist on `master`; those belong to Task 3 and to #341.

---

### Task 2: PR B, part 1 — CAN transport lifecycle contract

**Files:**
- Modify: `src/backend/flash/flash_executor.h`
- Modify: `src/backend/flash/testing/scripted_can_flash_transport.h`
- Modify: `src/platform/desktop/common/transport/desktop_can_flash_transport.h`
- Modify: `src/platform/desktop/common/transport/desktop_can_flash_transport.cpp`
- Test: `src/backend/flash/bound_flash_attempt_test.cpp`
- Test: `src/backend/flash/testing/scripted_flash_transports_test.cpp`
- Test: `src/platform/desktop/common/transport/desktop_can_flash_transport_test.cpp`

**Interfaces:**
- Produces, on `ICanFlashTransport`: `virtual Status reset_connection() = 0;`
  and `virtual Status restart_iso15765(const Iso15765Config& config, const ICancellationToken& cancellation);`
  with a default body.
- Produces, on `IKlineFlashExecutor` and `ICanFlashExecutor`:
  `virtual Status before_transport_configure(TransportType&, IClock&, const ICancellationToken&) const`
  returning `{}` by default, invoked by `BoundAttempt::run()` between the
  pre-configure cancellation check and `transport_->configure(*setup)`.
- Produces, on `ScriptedCanFlashTransport`: `reset_call_count_`,
  `configure_call_count_`, `open_call_count_`, `lifecycle_calls_` and
  `reset_result_`.

`reset_connection()` is deliberately pure virtual, so no CAN transport can
silently degrade a protocol-owned reset into configure-and-open. Outside #341
only `DesktopCanFlashTransport` and `ScriptedCanFlashTransport` implement
`ICanFlashTransport`, so this ripples nowhere else.

- [ ] **Step 1: Branch off master**

```bash
git checkout -b feat/can-transport-lifecycle-and-mixed-contracts origin/master
```

- [ ] **Step 2: Take the interface and adapter work from the branch**

```bash
git checkout codex/flash-wave5-implementation -- \
  src/backend/flash/flash_executor.h \
  src/backend/flash/testing/scripted_can_flash_transport.h
```

Then, in `flash_executor.h`, **remove** everything belonging to the mixed
transport — it arrives in Task 3, not here. Delete `struct RawCanConfig`,
`struct MixedCanConfig`, `class IMixedCanFlashTransport`, and
`class IMixedCanFlashExecutor`, and delete the
`#include "src/backend/protocol/ican_transport.h"` line they required.

Keep: `reset_connection()`, `restart_iso15765()`, the three
`before_transport_configure()` declarations, and the `BoundAttempt::run()` call
into `before_transport_configure`.

- [ ] **Step 3: Verify the build fails for exactly one reason**

Run: `bazel build --config=release //src/platform/desktop/common/transport:all`

Expected: FAIL. `DesktopCanFlashTransport` does not override the new pure
virtual `reset_connection()`, so it is abstract and cannot be instantiated.
This is the red state for the next step.

- [ ] **Step 4: Implement `reset_connection()` on the desktop transport**

In `desktop_can_flash_transport.h`, add to the public section, immediately
before `Status configure(const Iso15765Config& config) override;`:

```cpp
    Status reset_connection() override;
```

In `desktop_can_flash_transport.cpp`, add immediately after the destructor
definition:

```cpp
Status DesktopCanFlashTransport::reset_connection()
{
    if (!serial_)
    {
        return fail(ErrorKind::Disconnected, "reset_connection() called after close()");
    }
    try
    {
        if (!serial_->reset_connection())
        {
            return fail(ErrorKind::Internal, "reset_connection failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "reset_connection exception");
    }
}
```

Do **not** add a `restart_iso15765` override. The spec removes it as a no-op;
the inherited default is the intended behaviour.

- [ ] **Step 5: Verify it builds**

Run: `bazel build --config=release //src/platform/desktop/common/transport:all`

Expected: success.

- [ ] **Step 6: Take the tests for this contract**

```bash
git checkout codex/flash-wave5-implementation -- \
  src/backend/flash/bound_flash_attempt_test.cpp \
  src/backend/flash/testing/scripted_flash_transports_test.cpp \
  src/platform/desktop/common/transport/desktop_can_flash_transport_test.cpp
```

Delete from these files every test that names `MixedCan`, `IMixedCanFlashTransport`,
`ScriptedMixedCanFlashTransport` or `MixedCanConfig`; they arrive in Task 3.
Delete from `desktop_can_flash_transport_test.cpp` the
`configureClearsStickyIso14230HeaderState` test and the four
`set_add_iso14230_header` expectations, which belong to Task 1 — this branch
is cut from `master` and does not contain that fix.

- [ ] **Step 7: Run the suites**

```bash
bazel test --config=release --nocache_test_results \
  //src/backend/flash:all //src/platform/desktop/common/transport:all
```

Expected: PASS. If a `desktop_can_flash_transport_test` case fails on a setter
count or a backend-operation trace, Task 1's fix has been pulled in by mistake;
remove it rather than adapting the assertion.

- [ ] **Step 8: Commit**

```bash
git add src/backend/flash/flash_executor.h \
        src/backend/flash/testing/scripted_can_flash_transport.h \
        src/backend/flash/bound_flash_attempt_test.cpp \
        src/backend/flash/testing/scripted_flash_transports_test.cpp \
        src/platform/desktop/common/transport/desktop_can_flash_transport.h \
        src/platform/desktop/common/transport/desktop_can_flash_transport.cpp \
        src/platform/desktop/common/transport/desktop_can_flash_transport_test.cpp
git commit -F - <<'EOF'
feat(flash): add CAN transport reset and restart lifecycle

Makes reset_connection() a required part of the CAN transport contract so
a protocol-owned sequence cannot silently degrade into configure/open, and
adds a defaulted restart_iso15765() that owns reset/reconfigure/reopen
with a cancellation check at each step. BoundAttempt still owns initial
setup and final close.

Adds before_transport_configure() to the K-Line, CAN and mixed executor
interfaces as the narrow seam for protocols whose legacy startup performs
a reset and a timed quiet period before setters. The default keeps every
existing executor on the established lifecycle path.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 3: PR B, part 2 — mixed raw/ISO CAN contracts and scripted fake

**Files:**
- Modify: `src/backend/flash/flash_executor.h`
- Modify: `src/backend/flash/flash_types.h`
- Create: `src/backend/flash/testing/scripted_mixed_can_flash_transport.h`
- Modify: `src/backend/flash/testing/BUILD.bazel`
- Test: `src/backend/flash/bound_flash_attempt_test.cpp`
- Test: `src/backend/flash/testing/scripted_flash_transports_test.cpp`
- Test: `src/backend/flash/flash_types_test.cpp`

**Interfaces:**
- Consumes: Task 2's `before_transport_configure` seam.
- Produces: `struct RawCanConfig { int bitrate; std::uint32_t transmit_id; std::uint32_t receive_id; bool extended_id; };`,
  `struct MixedCanConfig { Iso15765Config kernel; RawCanConfig bootloader; };`,
  `class IMixedCanFlashTransport`, `class IMixedCanFlashExecutor` (with
  `using TransportType = IMixedCanFlashTransport;` and
  `using ConfigType = MixedCanConfig;`),
  `TransportKind::CanRawIso15765`, and `ScriptedMixedCanFlashTransport`.

This is the one genuinely novel piece of architecture in the wave: a transport
that starts in raw-CAN bootloader mode and switches to ISO-15765 for kernel
traffic. It lands with no production consumer until #341 merges, which the spec
accepts deliberately.

- [ ] **Step 1: Restore the mixed contracts**

```bash
git checkout codex/flash-wave5-implementation -- \
  src/backend/flash/testing/scripted_mixed_can_flash_transport.h \
  src/backend/flash/testing/BUILD.bazel
```

Then re-add to `flash_executor.h` the four declarations removed in Task 2 step
2, exactly as they appear on `codex/flash-wave5-implementation`: the
`#include "src/backend/protocol/ican_transport.h"` line, `struct RawCanConfig`,
`struct MixedCanConfig`, `class IMixedCanFlashTransport` and
`class IMixedCanFlashExecutor`.

- [ ] **Step 2: Add the transport kind**

In `flash_types.h`, extend `enum class TransportKind` to:

```cpp
enum class TransportKind
{
    Kline,
    CanIso15765,
    CanRawIso15765,
};
```

Do **not** add the four wave-5 `FlashFamily` enumerators, their
`FamilyPlan` variant alternatives, or their `FamilyTraits` specializations.
Those arrive with #341, because each needs its plan header, which this branch
does not have.

- [ ] **Step 3: Restore the mixed-transport tests**

```bash
git checkout codex/flash-wave5-implementation -- src/backend/flash/flash_types_test.cpp
```

That file's only wave-5 addition is one transport-kind case, so nothing needs
deleting from it:

```cpp
TEST(FlashTypesTest, MixedCanTransportKindIsDistinctFromIso15765)
{
    EXPECT_NE(TransportKind::CanRawIso15765, TransportKind::CanIso15765);
}
```

**Do not touch `src/backend/flash/flash_validation_test.cpp`.** Its wave-5
additions are the four family cases and a
`static_assert(std::variant_size_v<FamilyPlan> == std::tuple_size_v<...>)`
that ties the case table's size to the variant's. Without the four variant
alternatives — which arrive with #341 — that assert fails and the plan types do
not exist. It stays on the branch.

Re-add to `bound_flash_attempt_test.cpp` and
`scripted_flash_transports_test.cpp` the mixed-transport cases deleted in Task
2 step 6, taking them from the branch.

- [ ] **Step 4: Run the suites**

```bash
bazel test --config=release --nocache_test_results //src/backend/flash:all //src/backend/flash/testing:all
```

Expected: PASS. A failure naming an undeclared `SubaruDenso...` plan type means
`flash_validation_test.cpp` was restored despite step 3; revert it with
`git checkout origin/master -- src/backend/flash/flash_validation_test.cpp`.

- [ ] **Step 5: Commit**

```bash
git add src/backend/flash/flash_executor.h src/backend/flash/flash_types.h \
        src/backend/flash/testing/scripted_mixed_can_flash_transport.h \
        src/backend/flash/testing/BUILD.bazel \
        src/backend/flash/bound_flash_attempt_test.cpp \
        src/backend/flash/testing/scripted_flash_transports_test.cpp \
        src/backend/flash/flash_types_test.cpp
git commit -F - <<'EOF'
feat(flash): add mixed raw/ISO CAN transport capability

Adds the portable contracts for a transport that runs a raw-CAN
bootloader phase and then switches to ISO-15765 for kernel traffic:
RawCanConfig, MixedCanConfig, IMixedCanFlashTransport,
IMixedCanFlashExecutor and TransportKind::CanRawIso15765, with a
scripted fake for characterization tests.

No production consumer until the DensoCAN family lands; the contracts
and their fake are exercised by their own tests meanwhile.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 4: PR B, part 3 — desktop mixed CAN adapter

**Files:**
- Create: `src/platform/desktop/common/transport/desktop_mixed_can_flash_transport.h`
- Create: `src/platform/desktop/common/transport/desktop_mixed_can_flash_transport.cpp`
- Test: `src/platform/desktop/common/transport/desktop_mixed_can_flash_transport_test.cpp`
- Modify: `src/platform/desktop/common/transport/BUILD.bazel`
- Test: `src/platform/desktop/common/transport/desktop_transport_factory_test.cpp`

**Interfaces:**
- Consumes: Task 3's `IMixedCanFlashTransport` and `MixedCanConfig`.
- Produces: `DesktopMixedCanFlashTransport`, with owning
  (`std::unique_ptr<SerialPortActions>`) and non-owning (`SerialPortActions *`)
  constructors matching `DesktopCanFlashTransport`'s established pair.

- [ ] **Step 1: Take the adapter and its tests**

```bash
git checkout codex/flash-wave5-implementation -- \
  src/platform/desktop/common/transport/desktop_mixed_can_flash_transport.h \
  src/platform/desktop/common/transport/desktop_mixed_can_flash_transport.cpp \
  src/platform/desktop/common/transport/desktop_mixed_can_flash_transport_test.cpp \
  src/platform/desktop/common/transport/desktop_transport_factory_test.cpp \
  src/platform/desktop/common/transport/BUILD.bazel
```

- [ ] **Step 2: Remove the Task 1 fix from the adapter**

Commit `8f97613c` also added the ISO-14230 header clear to the mixed adapter's
`configure_iso()`. That belongs with #341, which rebases onto Task 1's PR. In
`desktop_mixed_can_flash_transport.cpp`, delete the
`set_add_iso14230_header(false)` block from `configure_iso()`, and delete the
matching expectations and the sticky-state test from
`desktop_mixed_can_flash_transport_test.cpp`.

- [ ] **Step 3: Run the suite**

Run: `bazel test --config=release //src/platform/desktop/common/transport:all --nocache_test_results`

Expected: PASS.

- [ ] **Step 4: Run the pull-request gate**

Run the per-pull-request gate from Global Constraints.

Expected: every command passes. `//:portable_closure` must build: the mixed
contracts live in a portable package and must not reach `//src/platform`.

- [ ] **Step 5: Commit, push and open the pull request**

```bash
git add src/platform/desktop/common/transport/
git commit -F - <<'EOF'
feat(desktop): adapt mixed raw/ISO CAN flash transport

Implements IMixedCanFlashTransport over SerialPortActions, switching the
facade between raw-CAN bootloader mode and ISO-15765 kernel mode and
refusing I/O issued against the wrong mode.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
git push -u origin feat/can-transport-lifecycle-and-mixed-contracts
```

Open the pull request. The body must state plainly that
`IMixedCanFlashTransport` and `DesktopMixedCanFlashTransport` have no
production consumer until #341 merges, and that this interval is accepted so
the transport architecture is reviewed on its own rather than as an appendix to
four protocol ports. End with the attribution line from Global Constraints.

---

### Task 5: Rebase #341 onto the two merged pull requests

**Files:** no source edits; this task is a rebase and a verification.

**Interfaces:**
- Consumes: Tasks 1–4, both pull requests merged to `master`.
- Produces: `codex/flash-wave5-implementation` rebased, roughly 2,000 lines and
  12 files lighter, with the four families, their routes, the four legacy
  removals, `denso_tcu_read_preflight` and the `mainwindow` rewiring intact.

- [ ] **Step 1: Record the pre-rebase state**

```bash
git checkout codex/flash-wave5-implementation
git rev-parse HEAD > /tmp/pr341-before.sha
git diff --stat origin/master...HEAD | tail -1
```

Note the file count and line totals; step 5 compares against them.

- [ ] **Step 2: Rebase**

```bash
git fetch origin
git rebase origin/master
```

Conflicts are expected in `flash_executor.h`, `flash_types.h`,
`scripted_can_flash_transport.h`, `desktop_can_flash_transport.{h,cpp}`,
`desktop_mixed_can_flash_transport.{h,cpp}` and their tests. In every case the
resolution is to take the `master` side for anything Tasks 1–4 moved, and keep
the branch side only for the four families: their `FlashFamily` enumerators,
`FamilyPlan` alternatives, `FamilyTraits` specializations, plan headers,
executors, routes and tests.

- [ ] **Step 3: Restore the two pieces held back from PR A and PR B**

Task 2 step 6 and Task 4 step 2 deliberately left two things behind. After the
rebase they must be present, because #341 now sits on top of Task 1's fix:

- `desktop_mixed_can_flash_transport.cpp`'s `configure_iso()` must clear
  ISO-14230 header state, with its test.
- `desktop_can_flash_transport_test.cpp`'s two backend-operation-trace
  assertions must include `"cfg:set_add_iso14230_header:0"` before
  `"open_serial_port"`.

- [ ] **Step 4: Delete the no-op override**

If the rebase preserved `DesktopCanFlashTransport::restart_iso15765` — an
override whose entire body calls `ICanFlashTransport::restart_iso15765` —
delete both the declaration and the definition. This is Tier 1 item 5 of the
spec and is free to do here, while the file is already open.

- [ ] **Step 5: Verify the rebase lost nothing**

```bash
bazel test --config=release --nocache_test_results //...
bazel build --config=release //:fastecu //:portable_closure
bazel test --config=release //:legacy_flash_drain
git diff --stat origin/master...HEAD | tail -1
```

Expected: all tests pass; `//:legacy_flash_drain` reports 10 remaining
families; the diff is roughly 2,000 lines and 12 files smaller than the figure
recorded in step 1.

- [ ] **Step 6: Push**

```bash
git push --force-with-lease origin codex/flash-wave5-implementation
```

---

### Task 6: Tier 1.1 — use the shared log templates

**Files:**
- Modify: `src/backend/flash/ecu/uds_client_exchange_common.h`
- Test: `src/backend/flash/ecu/uds_client_exchange_common_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`

**Interfaces:**
- Consumes: the existing `WithEventSink` concept and the `info()` / `error()`
  templates in `uds_client_exchange_common.h`.
- Produces: `template <WithEventSink C> void debug(const C& ctx, std::string_view message)`
  in the same header.

`uds_client_exchange_common.h` already defines `info()` and `error()` as
templates constrained on `WithEventSink`, with a header comment stating the
template form exists precisely so families carrying their own context shape can
use them unchanged. All three wave-5 ISO executors redeclare both privately and
none includes the header. The 260 call sites are unqualified, so they resolve to
the templates with no edit.

- [ ] **Step 1: Write the failing test for `debug`**

Add to `uds_client_exchange_common_test.cpp`, following the shape of the
existing `info` and `error` cases in that file:

```cpp
TEST(UdsClientExchangeCommonTest, DebugLogsAtDebugLevelThroughTheEventSink)
{
    RecordingEventSink events;
    struct Ctx
    {
        IEventSink& events;
    } ctx{events};

    debug(ctx, "kernel probe timed out");

    ASSERT_THAT(events.logs, ElementsAre(Pair(LogLevel::Debug, "kernel probe timed out")));
}
```

`RecordingEventSink` exposes a public `logs` member of type
`std::vector<std::pair<LogLevel, std::string>>`; `ElementsAre` and `Pair` are
already used this way elsewhere in this file.

- [ ] **Step 2: Run it to verify it fails**

Run: `bazel test --config=release //src/backend/flash/ecu:uds_client_exchange_common_test --nocache_test_results`

Expected: FAIL to compile — `debug` is not declared in this scope.

- [ ] **Step 3: Add `debug` to the shared header**

In `uds_client_exchange_common.h`, immediately after the `error()` template:

```cpp
template <WithEventSink C> void debug(const C& ctx, std::string_view message)
{
    ctx.events.log(LogLevel::Debug, message);
}
```

Extend the comment above `info()` so it reads "The three log shorthands every
CAN executor in this package defined identically" rather than "two".

- [ ] **Step 4: Run it to verify it passes**

Run: `bazel test --config=release //src/backend/flash/ecu:uds_client_exchange_common_test --nocache_test_results`

Expected: PASS.

- [ ] **Step 5: Record the green baseline for the three executors**

```bash
bazel test --config=release --nocache_test_results \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_executor_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_executor_test \
  //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_executor_test
```

Expected: PASS. This is the behaviour-preserving baseline.

- [ ] **Step 6: Delete the nine private definitions**

In each of the three executor sources, delete the local `info`, `debug` and
`error` definitions — nine functions in total, each of this shape:

```cpp
void info(Context& context, std::string_view message)
{
    context.events.log(LogLevel::Info, message);
}
```

Add to each file's include block, in sorted position:

```cpp
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
```

Change nothing at any call site. All 260 are unqualified calls that now resolve
to the templates.

- [ ] **Step 7: Add the build dependency**

In `src/backend/flash/ecu/BUILD.bazel`, add `":uds_client_exchange_common"` to
the `deps` of `subaru_denso_sh7058_can_executor`,
`subaru_denso_sh7058_can_diesel_executor` and
`subaru_tcu_denso_sh705x_can_executor`, in sorted position.

- [ ] **Step 8: Verify behaviour is unchanged**

```bash
bazel test --config=release --nocache_test_results //src/backend/flash/ecu:all
```

Expected: PASS, with no assertion edited. If a suite fails, the substitution is
not equivalent — stop and report rather than adjusting an expectation.

- [ ] **Step 9: Commit**

```bash
git add src/backend/flash/ecu/
git commit -F - <<'EOF'
refactor(flash): use the shared log templates in wave-5 executors

uds_client_exchange_common.h already provided info() and error() as
WithEventSink-constrained templates, written that way so families with
their own context shape could use them unchanged. The three wave-5 ISO
executors each redeclared both privately. Adds the missing debug()
alongside them and deletes the nine private copies.

No call site changes: all 260 are unqualified and resolve to the
templates.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 7: Tier 1.2 — use the shared cancellation fake

**Files:**
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_executor_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh705x_densocan_executor_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`

**Interfaces:**
- Consumes: `fastecu::FakeCancellationToken` from
  `src/backend/ports/testing/fake_cancellation_token.h`, Bazel target
  `//src/backend/ports/testing:fake_cancellation_token`.
- Produces: no new interface; four test files lose two local classes each.

`NeverCancelled` (8 lines) and `ToggleCancellation` (15 lines) are byte-identical
across all four new suites and are both expressible with the existing fake:
`NeverCancelled` is a default-constructed `FakeCancellationToken`, and
`ToggleCancellation::cancel()` is `set_cancelled(true)`.

- [ ] **Step 1: Record the green baseline**

```bash
bazel test --config=release --nocache_test_results //src/backend/flash/ecu:all
```

Expected: PASS.

- [ ] **Step 2: Delete the local classes from all four files**

In each of the four test files, delete these two definitions:

```cpp
class NeverCancelled final : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return false;
    }
};

class ToggleCancellation final : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return cancelled_.load();
    }
    void cancel()
    {
        cancelled_.store(true);
    }

  private:
    std::atomic_bool cancelled_{false};
};
```

Add to each file's include block, in sorted position:

```cpp
#include "src/backend/ports/testing/fake_cancellation_token.h"
```

- [ ] **Step 3: Substitute the types**

In all four files, replace every `NeverCancelled` and every
`ToggleCancellation` with `FakeCancellationToken`, and every `.cancel()` call
on such an object with `.set_cancelled(true)`.

Pointer members in the recording transports change type too, for example:

```cpp
    FakeCancellationToken *cancellation_to_trigger = nullptr;
```

Leave every assertion untouched.

- [ ] **Step 4: Add the build dependency**

In `src/backend/flash/ecu/BUILD.bazel`, add
`"//src/backend/ports/testing:fake_cancellation_token"` to the `deps` of all
four test targets, in sorted position.

- [ ] **Step 5: Verify behaviour is unchanged**

```bash
bazel test --config=release --nocache_test_results //src/backend/flash/ecu:all
```

Expected: PASS, with no assertion edited.

- [ ] **Step 6: Commit**

```bash
git add src/backend/flash/ecu/
git commit -F - <<'EOF'
test(flash): use the shared cancellation fake in wave-5 suites

NeverCancelled and ToggleCancellation were byte-identical in all four
wave-5 suites and are both expressible with the FakeCancellationToken
already in src/backend/ports/testing, which CLAUDE.md names the reference
implementation for fakes.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 8: Tier 1.3 and 1.4 — share the recording transport and the phase-cancelling sink

**Files:**
- Create: `src/backend/flash/ecu/testing/recording_can_flash_transport.h`
- Modify: `src/backend/flash/ecu/testing/BUILD.bazel`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_executor_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor_test.cpp`
  (comment only, plus the `PhaseCancellingEventSink` deletion)
- Modify: `src/backend/flash/ecu/BUILD.bazel`

**Interfaces:**
- Consumes: `ScriptedCanFlashTransport` (including the `lifecycle_calls_`
  recording added in Task 2), `FakeCancellationToken` from Task 7,
  `RecordingEventSink`.
- Produces, in namespace `fastecu::flash`:
  `class RecordingCanFlashTransport final : public ICanFlashTransport` with
  public members `scripted`, `reset_result`, `writes`, `read_timeouts`,
  `cancellation_to_trigger`, `cancel_prefix`, `cancellation_on_reset`,
  `cancellation_on_configure`, `cancellation_on_open`,
  `cancel_after_read_count`, `read_count` and `timeline`; and
  `class PhaseCancellingEventSink final : public RecordingEventSink` with
  constructor `(FakeCancellationToken& cancellation, std::string phase, int done)`.

The three decorators are 71, 85 and 123 lines and split two ways.

Petrol and diesel share a core and differ only in additions that are inert
unless set — petrol's `cancellation_on_reset` and `cancellation_on_configure`,
diesel's optional `timeline` pointer and `cancel_after_read_count`. Their union
preserves both.

**The TCU decorator is not unionable and is left alone.** Its
`restart_in_progress` flag is set unconditionally in `reset_connection()` and
then changes what `configure()` and `open()` do; its `write()` classifies
timeline entries by inspecting payload bytes; its `read()` cancels after a
kernel-start reply. Folding any of that into a shared type would change petrol
and diesel behaviour. Only its `PhaseCancellingEventSink` copy is removed.

The shared type drops the decorator's own `lifecycle` vector: Task 2 gave
`ScriptedCanFlashTransport` `lifecycle_calls_`, and two recordings of one fact
is the duplication being removed.

- [ ] **Step 1: Record the green baseline and the current lifecycle assertions**

```bash
bazel test --config=release --nocache_test_results //src/backend/flash/ecu:all
grep -n "\.lifecycle\b" src/backend/flash/ecu/subaru_denso_sh7058_can_executor_test.cpp \
                        src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor_test.cpp
```

Expected: PASS. The `grep` lists every assertion that must be re-pointed at
`scripted.lifecycle_calls_` in step 4; keep the output. Do not include the TCU
suite — its decorator keeps its own `lifecycle` vector.

- [ ] **Step 2: Create the shared header**

Create `src/backend/flash/ecu/testing/recording_can_flash_transport.h`. Take
the petrol suite's `RecordingCanTransport` as the base, rename it to
`RecordingCanFlashTransport`, delete its `lifecycle` member and every
`lifecycle.push_back(...)` line, and add the diesel and TCU suites' inert extras
so the union is:

```cpp
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace fastecu::flash
{

// The recording decorator the wave-5 CAN executor suites share. It wraps a
// ScriptedCanFlashTransport and adds the injection points those suites need:
// each is inert unless set, so one type serves all three families.
//
// Lifecycle is NOT recorded here. ScriptedCanFlashTransport records it in
// lifecycle_calls_; assert against scripted.lifecycle_calls_.
class RecordingCanFlashTransport final : public ICanFlashTransport
{
  public:
    Status reset_connection() override
    {
        if (timeline != nullptr)
        {
            timeline->push_back("reset_connection");
        }
        Status result = reset_result;
        if (result.has_value() && cancellation_on_reset != nullptr)
        {
            cancellation_on_reset->set_cancelled(true);
        }
        return result;
    }

    Status configure(const Iso15765Config& config) override
    {
        if (timeline != nullptr)
        {
            timeline->push_back("configure");
        }
        Status result = scripted.configure(config);
        if (result.has_value() && cancellation_on_configure != nullptr)
        {
            cancellation_on_configure->set_cancelled(true);
        }
        return result;
    }

    Status open() override
    {
        if (timeline != nullptr)
        {
            timeline->push_back("open");
        }
        Status result = scripted.open();
        if (result.has_value() && cancellation_on_open != nullptr)
        {
            cancellation_on_open->set_cancelled(true);
        }
        return result;
    }

    Status close() override
    {
        if (timeline != nullptr)
        {
            timeline->push_back("close");
        }
        return scripted.close();
    }

    void request_unblock() noexcept override
    {
        scripted.request_unblock();
    }

    Status write(bytes::ByteView data, const ICancellationToken& cancellation) override
    {
        writes.emplace_back(data.begin(), data.end());
        Status result = scripted.write(data, cancellation);
        if (result.has_value() && cancellation_to_trigger != nullptr && !cancel_prefix.empty() &&
            data.size() >= cancel_prefix.size() && std::equal(cancel_prefix.begin(), cancel_prefix.end(), data.begin()))
        {
            cancellation_to_trigger->set_cancelled(true);
        }
        return result;
    }

    Result<std::optional<bytes::Bytes>> read(std::chrono::milliseconds timeout,
                                             const ICancellationToken& cancellation) override
    {
        read_timeouts.push_back(timeout);
        Result<std::optional<bytes::Bytes>> result = scripted.read(timeout, cancellation);
        ++read_count;
        if (result.has_value() && cancellation_to_trigger != nullptr && cancel_after_read_count.has_value() &&
            read_count == *cancel_after_read_count)
        {
            cancellation_to_trigger->set_cancelled(true);
        }
        return result;
    }

    ScriptedCanFlashTransport scripted;
    Status reset_result;
    std::vector<bytes::Bytes> writes;
    std::vector<std::chrono::milliseconds> read_timeouts;
    FakeCancellationToken *cancellation_to_trigger = nullptr;
    FakeCancellationToken *cancellation_on_reset = nullptr;
    FakeCancellationToken *cancellation_on_configure = nullptr;
    FakeCancellationToken *cancellation_on_open = nullptr;
    bytes::Bytes cancel_prefix;
    std::optional<std::size_t> cancel_after_read_count;
    std::size_t read_count{};
    std::vector<std::string> *timeline = nullptr;
};

// Cancels its bound token when a specific phase reaches a specific done count.
// Byte-identical in all three wave-5 CAN executor suites before extraction
// (the token type changes from ToggleCancellation by Task 7's substitution).
class PhaseCancellingEventSink final : public RecordingEventSink
{
  public:
    PhaseCancellingEventSink(FakeCancellationToken& cancellation, std::string phase, int done)
        : cancellation_(cancellation), phase_(std::move(phase)), done_(done)
    {
    }

    void phase_progress(const PhaseProgressEvent& event) override
    {
        RecordingEventSink::phase_progress(event);
        if (event.phase_name == phase_ && event.done == done_)
        {
            cancellation_.set_cancelled(true);
        }
    }

  private:
    FakeCancellationToken& cancellation_;
    std::string phase_;
    int done_;
};

} // namespace fastecu::flash
```

Note the ordering the real bodies require and this transcription preserves:
`reset_connection()` pushes its timeline entry **before** consulting
`cancellation_on_reset`; `configure()` and `open()` call the scripted transport
and only then fire their hook; `read()` increments `read_count` after the
scripted read and compares with `==`, not `>=`.

- [ ] **Step 3: Add the Bazel target**

In `src/backend/flash/ecu/testing/BUILD.bazel`:

```python
cc_library(
    name = "recording_can_flash_transport",
    hdrs = ["recording_can_flash_transport.h"],
    deps = [
        "//src/algorithms/protocol",
        "//src/backend/flash:flash_executor",
        "//src/backend/flash/testing:scripted_flash_transports",
        "//src/backend/ports/testing:fake_cancellation_token",
        "//src/backend/ports/testing:recording_event_sink",
    ],
)
```

- [ ] **Step 4: Adopt it in the petrol and diesel suites**

In both files, delete the local `RecordingCanTransport` and
`PhaseCancellingEventSink` classes and add:

```cpp
#include "src/backend/flash/ecu/testing/recording_can_flash_transport.h"
```

Replace every `RecordingCanTransport` with `RecordingCanFlashTransport`. Using
the `grep` output from step 1, re-point every `<transport>.lifecycle` assertion
at `<transport>.scripted.lifecycle_calls_`. The expected string vectors do not
change — only the member they are read from.

- [ ] **Step 5: In the TCU suite, take only the event sink**

In `subaru_tcu_denso_sh705x_can_executor_test.cpp`, delete only the local
`PhaseCancellingEventSink` and add:

```cpp
#include "src/backend/flash/ecu/testing/recording_can_flash_transport.h"
```

**Leave its `RecordingCanTransport` exactly as it is**, including its own
`lifecycle` vector. Add this comment directly above that class so the next
reader does not re-derive the analysis:

```cpp
// Deliberately NOT RecordingCanFlashTransport from
// //src/backend/flash/ecu/testing. That shared type unions only additions that
// are inert unless set. This decorator's restart_in_progress is set
// unconditionally in reset_connection() and then changes what configure() and
// open() do, its write() classifies timeline entries by payload byte, and its
// read() cancels after a kernel-start reply. Folding any of that into the
// shared type would change the petrol and diesel suites' behaviour.
```

- [ ] **Step 6: Add the build dependency**

In `src/backend/flash/ecu/BUILD.bazel`, add
`"//src/backend/flash/ecu/testing:recording_can_flash_transport"` to the `deps`
of the petrol, diesel and TCU test targets, in sorted position. The TCU target
needs it for `PhaseCancellingEventSink` alone.

- [ ] **Step 7: Verify behaviour is unchanged**

```bash
bazel test --config=release --nocache_test_results //src/backend/flash/ecu:all
```

Expected: PASS, with no expected value edited. A failure on a lifecycle vector
means a `scripted.lifecycle_calls_` re-point was missed in the petrol or diesel
suite, or the scripted transport records a step the decorator did not; report
it rather than editing the expectation. The TCU suite's lifecycle assertions
must be untouched.

- [ ] **Step 8: Commit**

```bash
git add src/backend/flash/ecu/
git commit -F - <<'EOF'
test(flash): share the wave-5 recording CAN transport

The petrol and diesel suites each carried a recording decorator sharing
a core and differing only in additions that are inert unless set.
Extracts that union as RecordingCanFlashTransport, alongside the
PhaseCancellingEventSink all three suites carried byte-identically.

The shared type drops its own lifecycle vector in favour of
ScriptedCanFlashTransport::lifecycle_calls_, so one recording of that
fact replaces two.

The TCU decorator is left in place and commented: its restart_in_progress
flag is set unconditionally in reset_connection() and changes what
configure() and open() do, so it is not inert-by-default and cannot join
the union without altering the other two suites.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 9: Tier 1.5 — remove the `goto` from the TCU preflight branch

**Files:**
- Modify: `src/ui/desktop/mainwindow.cpp`
- Test: `src/ui/desktop/mainwindow_test.cpp`

**Interfaces:**
- Consumes: `fastecu::service_functions::run_denso_tcu_service_action(...)`,
  which returns `false` only when the caller must continue into the ROM-dump
  flash workflow.
- Produces: no interface change.

The `DesktopCanFlashTransport::restart_iso15765` half of Tier 1 item 5 was
already handled in Task 5 step 4.

- [ ] **Step 1: Record the green baseline**

Run: `bazel test --config=release //src/ui/desktop:test_mainwindow --nocache_test_results`

Expected: PASS.

- [ ] **Step 2: Read the current control flow**

Run: `grep -n "ecu_operation_cleanup\|run_denso_tcu_service_action" src/ui/desktop/mainwindow.cpp`

The handled-service path currently does `goto ecu_operation_cleanup;`, jumping
past the whole flash-routing block to a label immediately before
`vbatt_timer->stop();`.

- [ ] **Step 3: Replace the jump with a guarded block**

Wrap the flash-routing body that follows the preflight in a condition on the
preflight result, so the handled case simply falls through to the shared tail.
Introduce a local set before the branch:

```cpp
        bool tcu_service_action_handled = false;
```

Assign it where the `goto` stood:

```cpp
            tcu_service_action_handled =
                fastecu::service_functions::run_denso_tcu_service_action(action, serial, protocol, this);
```

Guard the remaining routing with `if (!tcu_service_action_handled) { ... }`, and
delete both the `goto` statement and the `ecu_operation_cleanup:` label. The
statements after the label — `vbatt_timer->stop();` and `serial->reset_connection();`
onward — stay exactly where they are and now run unconditionally, which is what
the `goto` already achieved.

- [ ] **Step 4: Verify behaviour is unchanged**

```bash
bazel test --config=release --nocache_test_results //src/ui/desktop/... //src/ui/desktop/service_functions:all
```

Expected: PASS. `mainwindow_test.cpp` already covers all four chooser outcomes
plus cancellation, so a regression in the handled path shows up here.

- [ ] **Step 5: Run clang-tidy over the changed file**

Run: `bazel run --config=release //:clang_tidy_report_changed`

Expected: 0 findings.

- [ ] **Step 6: Commit**

```bash
git add src/ui/desktop/mainwindow.cpp
git commit -F - <<'EOF'
refactor(desktop): drop the goto from the Denso TCU preflight branch

run_denso_tcu_service_action already reports whether the caller must
continue into the ROM-dump workflow, so the handled case can fall through
to the shared cleanup tail under a guard instead of jumping into a label
past the routing block.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 10: Tier 2 — extract the proven BEEF helpers

**Files:**
- Create: `src/backend/flash/ecu/denso_beef_can_common.h`
- Create: `src/backend/flash/ecu/denso_beef_can_common_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`
- Modify: `bazel/portable_targets.bzl`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp`

**Interfaces:**
- Consumes: `denso_encrypt_rom()` from `denso_iso15765_can_common.h`,
  `composeBe()` from `src/algorithms/protocol/bytes_compose.h`.
- Produces, in namespace `fastecu::flash`:
  `inline constexpr std::uint32_t kKernelStartComm = 0xBEEF;`,
  `bytes::Bytes beef_request(bytes::Byte opcode, bytes::ByteView payload = {})`,
  `bytes::Bytes encrypt_payload(bytes::ByteView payload)`,
  `template <WithCancellation C> Status cancelled_if_requested(const C& ctx, std::string_view detail)`,
  `std::uint64_t elapsed_milliseconds(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)`,
  and `template <class C> Result<std::optional<bytes::Bytes>> channel_request_optional(C& ctx, bytes::ByteView pdu, std::chrono::milliseconds timeout, std::chrono::milliseconds delay = std::chrono::milliseconds{0})`.

Of the 22 signatures common to the three ISO executors, eight have
byte-identical bodies. Three (`info`, `debug`, `error`) went to
`uds_client_exchange_common.h` in Task 6; the remaining five come here.
`parse_beef` matches petrol and diesel only, and a two-way match is where the
design says to stop, so it stays family-local. Every substantial protocol
routine — `connect_bootloader`, `flash_block`, `upload_kernel`, `write_memory`,
`compare_blocks`, `read_memory`, `beef_exchange`, `upload_b6_discard`,
`nonfatal_query`, `strict_payload`, `discard_stale_frame`, `query_crc`,
`reflash_block` — differs across all three and must not be merged.

The helpers take each family's own `Context`, which is a different type per
file, so the context-taking ones are templates, matching the `WithEventSink`
idiom already in `uds_client_exchange_common.h`.

- [ ] **Step 1: Record the green baseline**

```bash
bazel test --config=release --nocache_test_results \
  //src/backend/flash/ecu:denso_iso15765_can_common_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_executor_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_executor_test \
  //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_executor_test
```

Expected: PASS. Save the output; this is the refactor baseline the wave-5 plan's
factoring task established.

- [ ] **Step 2: Write the failing test**

Create `src/backend/flash/ecu/denso_beef_can_common_test.cpp`:

```cpp
#include "src/backend/flash/ecu/denso_beef_can_common.h"

#include <chrono>

#include <gtest/gtest.h>

#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{

using namespace std::chrono_literals;

TEST(DensoBeefCanCommonTest, BeefRequestFramesOpcodeAndPayloadLength)
{
    const bytes::Bytes payload{0x01, 0x02, 0x03};
    const bytes::Bytes framed = beef_request(0xB6, payload);

    ASSERT_EQ(framed.size(), 7U);
    EXPECT_EQ(framed[0], 0xBE);
    EXPECT_EQ(framed[1], 0xEF);
    EXPECT_EQ(framed[2], 0x00);
    EXPECT_EQ(framed[3], 0x04);
    EXPECT_EQ(framed[4], 0xB6);
    EXPECT_EQ(framed[5], 0x01);
}

TEST(DensoBeefCanCommonTest, BeefRequestWithNoPayloadStillCountsTheOpcode)
{
    const bytes::Bytes framed = beef_request(0xA0);

    ASSERT_EQ(framed.size(), 5U);
    EXPECT_EQ(framed[3], 0x01);
    EXPECT_EQ(framed[4], 0xA0);
}

TEST(DensoBeefCanCommonTest, ElapsedMillisecondsReportsAtLeastOne)
{
    const std::chrono::steady_clock::time_point start{};

    EXPECT_EQ(elapsed_milliseconds(start, start), 1U);
    EXPECT_EQ(elapsed_milliseconds(start, start + 250ms), 250U);
}

TEST(DensoBeefCanCommonTest, CancelledIfRequestedReportsTheCallerDetail)
{
    struct Ctx
    {
        const ICancellationToken& cancellation;
    };
    FakeCancellationToken token;
    const Ctx ctx{token};

    EXPECT_TRUE(cancelled_if_requested(ctx, "cancelled before CAN request").has_value());

    token.set_cancelled(true);
    const Status cancelled = cancelled_if_requested(ctx, "cancelled before CAN request");
    ASSERT_FALSE(cancelled.has_value());
    EXPECT_EQ(cancelled.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(cancelled.error().detail, "cancelled before CAN request");
}

} // namespace
} // namespace fastecu::flash
```

Add `#include "src/backend/ports/testing/fake_cancellation_token.h"` to the
include block.

- [ ] **Step 3: Run it to verify it fails**

Run: `bazel test --config=release //src/backend/flash/ecu:denso_beef_can_common_test --nocache_test_results`

Expected: FAIL — the target does not exist yet.

- [ ] **Step 4: Create the shared header**

Create `src/backend/flash/ecu/denso_beef_can_common.h`. Take each body verbatim
from `subaru_denso_sh7058_can_executor.cpp`, converting the context-taking ones
to templates:

```cpp
#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/ecu/denso_iso15765_can_common.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/result.h"

// Helpers shared by the wave-5 Denso BEEF-protocol CAN executors:
// SubaruDensoSh7058CanExecutor, SubaruDensoSh7058CanDieselExecutor and
// SubaruTcuDensoSh705xCanExecutor.
//
// Each family was ported standalone with duplication tolerated, so factoring
// happened only once all three and their independent characterization tests
// were visible. Comparing all 22 signatures common to the three, exactly these
// bodies were byte-identical. parse_beef matched petrol and diesel only and
// stays family-local, because a two-way match is where this wave's design
// says to stop.
//
// Everything substantial was compared and deliberately left family-local:
// connect_bootloader, flash_block, upload_kernel, write_memory,
// compare_blocks, read_memory, beef_exchange, upload_b6_discard,
// nonfatal_query, strict_payload, discard_stale_frame, query_crc and
// reflash_block all differ across all three in timeouts, retry counts,
// geometry, address indexing, startup ordering or log wording.
//
// The executor suites do NOT read these helpers back: each carries wire
// expectations transcribed independently from its legacy oracle, so a wrong
// change here fails those suites rather than passing silently. Keep it that
// way.
namespace fastecu::flash
{

// The two-byte frame marker every BEEF-protocol request and reply opens with.
// Byte-identical across all three consumers.
inline constexpr std::uint32_t kKernelStartComm = 0xBEEF;

// Frames a kernel-protocol request: marker, big-endian length covering the
// opcode plus payload, opcode, payload.
inline bytes::Bytes beef_request(bytes::Byte opcode, bytes::ByteView payload = {})
{
    return bytes::composeBe(std::uint16_t{kKernelStartComm}, static_cast<std::uint16_t>(payload.size() + 1), opcode,
                            payload);
}

// The padded kernel-upload payload transform. A one-line adapter onto the
// shared table in denso_iso15765_can_common.h.
inline bytes::Bytes encrypt_payload(bytes::ByteView payload)
{
    return denso_encrypt_rom(payload);
}

// Any executor context carrying a cancellation token.
template <class C>
concept WithCancellation = requires(const C& ctx) {
    { ctx.cancellation } -> std::convertible_to<const ICancellationToken&>;
};

template <WithCancellation C> Status cancelled_if_requested(const C& ctx, std::string_view detail)
{
    if (ctx.cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, std::string(detail));
    }
    return {};
}

// Legacy reported a floor of one millisecond for any completed transfer, so a
// sub-millisecond phase never renders as "0 ms".
inline std::uint64_t elapsed_milliseconds(std::chrono::steady_clock::time_point start,
                                          std::chrono::steady_clock::time_point end)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 1U;
}

// Sends `pdu` on the context's channel and returns whatever arrives within
// `timeout`, with an optional post-send settle delay. Templated because each
// family's channel member has its own type.
template <class C>
Result<std::optional<bytes::Bytes>> channel_request_optional(C& ctx, bytes::ByteView pdu,
                                                             std::chrono::milliseconds timeout,
                                                             std::chrono::milliseconds delay = std::chrono::milliseconds{0})
{
    if (const Status checkpoint = cancelled_if_requested(ctx, "cancelled before CAN request");
        !checkpoint.has_value())
    {
        return std::unexpected(checkpoint.error());
    }
    if (const Status sent = ctx.channel.send(pdu, ctx.cancellation); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    if (delay > std::chrono::milliseconds{0})
    {
        if (const Status slept = ctx.clock.sleep(delay, ctx.cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    return ctx.channel.receive(timeout, ctx.cancellation);
}

} // namespace fastecu::flash
```

- [ ] **Step 5: Add the Bazel targets**

In `src/backend/flash/ecu/BUILD.bazel`:

```python
cc_library(
    name = "denso_beef_can_common",
    hdrs = ["denso_beef_can_common.h"],
    deps = [
        ":denso_iso15765_can_common",
        "//src/algorithms/protocol",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "denso_beef_can_common_test",
    srcs = ["denso_beef_can_common_test.cpp"],
    deps = [
        ":denso_beef_can_common",
        "//src/backend/ports",
        "//src/backend/ports/testing:fake_cancellation_token",
        "//src/backend/ports/testing:result_matchers",
    ],
)
```

In `bazel/portable_targets.bzl`, add `"denso_beef_can_common"` to the
`"src/backend/flash/ecu"` list, in sorted position — it sorts immediately before
`"denso_iso15765_can_common"`.

- [ ] **Step 6: Run the test to verify it passes**

Run: `bazel test --config=release //src/backend/flash/ecu:denso_beef_can_common_test --nocache_test_results`

Expected: PASS.

- [ ] **Step 7: Adopt it in the three executors**

In each of the three executor sources, delete the local definitions of
`kKernelStartComm`, `beef_request`, `encrypt_payload`, `cancelled_if_requested`,
`elapsed_milliseconds` and `channel_request_optional`, and add to the include
block in sorted position:

```cpp
#include "src/backend/flash/ecu/denso_beef_can_common.h"
```

Leave `parse_beef`, `BeefMessage`, `uppercase_hex_compact` and every protocol
routine where they are. Change no call site.

Keep each file's `using bytes::composeBe;` — the executors still call it 8 to 11
times each outside `beef_request`, and `ByteView` is a `std::span` alias, so
ADL would not find it without that declaration. The shared header qualifies it
as `bytes::composeBe` for the same reason.

Add `":denso_beef_can_common"` to the `deps` of the three executor targets in
`src/backend/flash/ecu/BUILD.bazel`, in sorted position.

- [ ] **Step 8: Verify behaviour is unchanged**

```bash
bazel test --config=release --nocache_test_results //src/backend/flash/ecu:all
bazel build --config=release //:portable_closure
```

Expected: PASS, against the step 1 baseline, with no wire expectation edited.

If `//:portable_closure` reports `denso_beef_can_common` as an unexpected
registry entry rather than a missing one, remove the line added in step 5 —
`uds_client_exchange_common` is likewise reached transitively without being
listed. Follow what the guard reports; do not add a suppression.

- [ ] **Step 9: Commit**

```bash
git add src/backend/flash/ecu/ bazel/portable_targets.bzl
git commit -F - <<'EOF'
refactor(flash): extract the proven Denso BEEF CAN helpers

Of the 22 function signatures common to the three wave-5 ISO-15765
executors, eight had byte-identical bodies. Three went to
uds_client_exchange_common.h; the remaining five plus the 0xBEEF frame
marker land here, as templates where they take each family's own context
type.

parse_beef matches petrol and diesel only and stays family-local. Every
substantial protocol routine differs across all three and is deliberately
preserved per family; the header records what was compared.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 11: Closeout — documentation and the full gate

**Files:**
- Modify: `docs/modularization-plan.md`
- Modify: `docs/tech-debt.md`
- Modify: `docs/superpowers/specs/2026-09-17-pr341-split-and-consolidation-design.md`

**Interfaces:**
- Consumes: Tasks 1–10.
- Produces: the deferred Tier 3 items recorded where the next person will find
  them, and a green full gate on #341.

- [ ] **Step 1: Record the deferred work**

Add to `docs/tech-debt.md`, in the existing style of that file, the three Tier 3
items the spec defers, each with its evidence:

- `flash_workflow.cpp` is 1,014 lines with seven sibling workflow classes.
  `KernelBackedCanFlashWorkflow` is `SimpleCanFlashWorkflow` plus lazy kernel
  resolution, a confirmation loop and a transport parameter, and `ColtWorkflow`
  hand-rolls the same confirmation loop. Unifying them touches routing for
  every already-merged CAN family.
- Each wave-5 family's `nonfatal_query` differs from the other two and from the
  shared `non_fatal_query` in `uds_client_exchange_common.h`. Converging them is
  behaviour-changing surgery on characterization-tested sequences.
- `NeverCancelled` still appears in two pre-existing wave-4 test files and
  `RecordingClock` in six; both predate this work and were left out of scope to
  keep the diff on one subject.

- [ ] **Step 2: Mark the wave complete**

In `docs/modularization-plan.md`, mark wave 5 complete without overwriting the
newer step-6a and step-6b status, and record 10 remaining legacy families.

- [ ] **Step 3: Close the spec**

In the spec, change the `**Status:**` line to record that the design was
implemented, naming the two extracted pull requests by number.

- [ ] **Step 4: Run the full gate**

```bash
bazel test  --config=release --nocache_test_results //...
bazel build --config=release //:fastecu //:portable_closure
bazel test  --config=release //:serial_compat_allowlist //:legacy_flash_drain //:windows_preprocessor_guards
bazel run   //:clang_tidy_report_changed
prek run --all-files
```

Expected: every command passes; `//:legacy_flash_drain` reports 10 remaining
families; clang-tidy reports 0 findings.

- [ ] **Step 5: Confirm no wire expectation moved**

```bash
git diff origin/master...HEAD -- 'src/backend/flash/ecu/*_executor_test.cpp' | grep -E "^[-+].*0x[0-9A-Fa-f]{2}" | head -40
```

Expected: only lines whose change is a type substitution
(`NeverCancelled` to `FakeCancellationToken`, `RecordingCanTransport` to
`RecordingCanFlashTransport`) or a member re-point. Any changed hex literal is
a behaviour change and must be reverted.

- [ ] **Step 6: Commit and push**

```bash
git add docs/
git commit -F - <<'EOF'
docs(flash): close out wave 5 and record the deferred consolidation

Marks modularization wave 5 complete with 10 legacy families remaining,
and records the three Tier 3 items left deliberately out of scope: the
flash_workflow.cpp template unification, the nonfatal_query convergence,
and the wave-4 test-double sweep.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
git push origin codex/flash-wave5-implementation
```

- [ ] **Step 7: Update the #341 description**

Rewrite the pull request body to state the reduced scope: the transport
contracts and the K-Line header fix merged separately, this PR carries the four
families, and the consolidation commits are listed. Keep the existing
qualification paragraph verbatim — hardware completion remains automated-only,
with no claim of manual qualification. End with the attribution line from
Global Constraints.
