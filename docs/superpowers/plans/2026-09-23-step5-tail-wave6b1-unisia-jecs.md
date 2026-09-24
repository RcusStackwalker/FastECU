# Wave 6b-1 — Subaru ECU Unisia Jecs — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate the read-only `FlashEcuSubaruUnisiaJecsOperation` from its legacy Qt worker to a portable `FlashPlan` and `IKlineFlashExecutor`, read the complete 64 KiB ROM instead of the legacy path's accidental 32-byte truncation, and remove one family from `//:legacy_flash_drain`.

**Architecture:** A family-specific plan accepts the two configured protocol/MCU pairs and rejects write operations before hardware I/O. The executor configures the existing K-Line adapter for 1953 baud/even parity, preserves the legacy echo-checked wake-up, and performs byte-addressed reads with the newly landed raw K-Line transport methods. The existing desktop flash workflow owns transport lifecycle and exposes the result to the dialog.

**Tech Stack:** C++23, Bazel, GoogleTest (`fastecu_portable_gtest`), QtTest for desktop workflow/dialog tests. Portable backend code contains no Qt, threads, dialogs, or filesystem access.

**Spec:** [Step 5 Tail Wave 6 — Nine Singletons](../specs/2026-09-19-step5-tail-wave6-singletons-design.md)

## Global Constraints

- This is wave 6b-1 only: `FlashEcuSubaruUnisiaJecs`; do not touch the M32R sibling.
- The family is read-only. `Read` is supported; `Write` and `TestWrite` return `ErrorKind::Unsupported` before transport configuration.
- The only deliberate behavior correction is the complete 64 KiB result. The legacy method overwrote its requested region with `start_addr = 0` and `length = 0x20`; the portable executor reads addresses `0x0000..0xffff` and returns exactly `0x10000` bytes.
- Preserve the legacy wire sequence and timing: echo-checked wake-up `{0x78,0x12,0x34,0x00}`, 500 ms sleep, 1000 ms flush read; per address raw request `{0x78,addr_hi,addr_lo,0x00}`, 45 ms sleep, repeated 5 ms raw reads, and retransmission after each accumulated 500 ms without a byte.
- Preserve legacy synchronization: before the first synchronized byte, discard one leading byte on a mismatched three-byte tuple; after synchronization, discard the whole mismatched three-byte tuple; after more than ten examined tuples, drop synchronization.
- Backend operations return `fastecu::Result<T>`, checked with `.has_value()` and never implicit `operator bool`.
- Exceptions never cross a port. Do not add an `ErrorKind`.
- Pure protocol code uses `bytes::Byte`, `bytes::Bytes`, and `bytes::ByteView`; `QByteArray` remains a desktop boundary type.
- Every new backend `cc_library` target is registered by name in `PORTABLE_PACKAGES` in `bazel/portable_targets.bzl`.
- Add no ratchet entry. Remove exactly `ecu/flash_ecu_subaru_unisia_jecs_operation.cpp` from `scripts/check-legacy-flash-drain.py` when the legacy files are deleted.
- Tests are package-owned and co-located. Any test claiming to pin wire behavior must be mutation-checked by changing the relevant production branch/value, observing the named test fail, and restoring a byte-identical tree.
- Work lands through a pull request. Branch before the first commit; do not push or open the PR without user approval.

## Review Focus

- A raw read that returns partial tuples or several tuples at once must preserve unconsumed bytes and eventually extract the addressed byte without reordering the ROM.
- A stream that never contains the requested address must terminate through cancellation or transport error; it must not manufacture a byte or silently advance.
- Address rollover at `0x00ff -> 0x0100` and the final request at `0xffff` must encode big-endian address bytes correctly.
- Short writes, disconnected reads, and cancellation at every write/sleep/read boundary must stop immediately and return the original error kind.
- Both configured protocol/MCU pairs must route to the same executor, while cross-pair combinations and prefix lookalikes are rejected before I/O.

---

### Task 1: Family plan, type registration, and read-only validation

**Files:**
- Create: `src/backend/flash/ecu/subaru_unisia_jecs_types.h`
- Create: `src/backend/flash/ecu/subaru_unisia_jecs_plan.h`
- Create: `src/backend/flash/ecu/subaru_unisia_jecs_plan.cpp`
- Create: `src/backend/flash/ecu/subaru_unisia_jecs_plan_test.cpp`
- Modify: `src/backend/flash/flash_types.h`
- Modify: `src/backend/flash/flash_plan.cpp`
- Modify: `src/backend/flash/flash_validation_test.cpp`
- Modify: `src/backend/flash/testing/flash_printers.h`
- Modify: `src/backend/flash/ecu/BUILD.bazel`
- Modify: `bazel/portable_targets.bzl`

**Interfaces:**
- Consumes: `FlashPlan`, `FlashPlanFields`, `validate_and_build`, `find_flash_device`, `MemoryRegion`.
- Produces: `SubaruUnisiaJecsPlan`, `FlashFamily::SubaruUnisiaJecs`, `build_subaru_unisia_jecs_plan(...)`, and `validate_subaru_unisia_jecs_plan(...)`.

- [ ] **Step 1: Write the failing plan contract tests**

Create tests with literal expectations:

```cpp
TEST(SubaruUnisiaJecsPlan, MapsBothConfiguredProtocolMcuPairs)
{
    for (const auto& [protocol, mcu] : std::to_array<std::pair<std::string_view, std::string_view>>({
             {"sub_ecu_unisia_jecs_m3779x", "M3779x"},
             {"sub_ecu_unisia_jecs_m3775x", "M3775x"},
         }))
    {
        const auto plan = build_subaru_unisia_jecs_plan(FlashOperation::Read, protocol, mcu, std::nullopt);
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        EXPECT_EQ(plan->family(), FlashFamily::SubaruUnisiaJecs);
        EXPECT_EQ(plan->transport(), TransportKind::Kline);
        EXPECT_EQ(plan->transfer_region(), (MemoryRegion{0, 0x10000}));
        const auto& family = std::get<SubaruUnisiaJecsPlan>(plan->family_plan());
        EXPECT_EQ(family.initial_baud, 1953);
        EXPECT_TRUE(family.even_parity);
    }
}

TEST(SubaruUnisiaJecsPlan, RejectsCrossPairedProtocolAndMcu)
{
    EXPECT_THAT(build_subaru_unisia_jecs_plan(FlashOperation::Read, "sub_ecu_unisia_jecs_m3779x", "M3775x",
                                               std::nullopt),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(build_subaru_unisia_jecs_plan(FlashOperation::Read, "sub_ecu_unisia_jecs_m3775x", "M3779x",
                                               std::nullopt),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}

TEST(SubaruUnisiaJecsPlan, RejectsWriteOperations)
{
    for (const FlashOperation operation : {FlashOperation::Write, FlashOperation::TestWrite})
    {
        EXPECT_THAT(build_subaru_unisia_jecs_plan(operation, "sub_ecu_unisia_jecs_m3779x", "M3779x",
                                                   bytes::Bytes(0x10000, 0)),
                    fastecu::testing::IsErr(ErrorKind::Unsupported));
    }
}
```

Add direct-construction validation tests that mutate the family alternative, read region, protocol/MCU pairing, baud, or parity and expect `InvalidConfig`; this prevents workflow-only construction from making validation branches unreachable.

- [ ] **Step 2: Run the new target and observe RED**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_plan_test`

Expected: analysis failure, `no such target '//src/backend/flash/ecu:subaru_unisia_jecs_plan_test'`.

- [ ] **Step 3: Add the family type and exhaustive registrations**

Define:

```cpp
struct SubaruUnisiaJecsPlan
{
    int initial_baud;      // 1953
    bool even_parity;      // true; avoids pulling executor config types into flash_types.h
};
```

Add `FlashFamily::SubaruUnisiaJecs`, add the type to `FamilyPlan`, specialize `FamilyTraits` for K-Line, and set `family_requires_kernel_v<SubaruUnisiaJecsPlan> = false`. Update `flash_plan.cpp`, `flash_printers.h`, and `flash_validation_test.cpp`'s variant size/case table in the same step so every exhaustive construct compiles.

- [ ] **Step 4: Implement the plan builder and standalone validator**

Use an explicit pair table, not independent protocol and MCU lists:

```cpp
struct Identity
{
    std::string_view protocol;
    std::string_view mcu;
};
constexpr auto kIdentities = std::to_array<Identity>({
    {"sub_ecu_unisia_jecs_m3779x", "M3779x"},
    {"sub_ecu_unisia_jecs_m3775x", "M3775x"},
});
constexpr MemoryRegion kRom{0, 0x10000};
```

Both builder and validator must enforce: read only, exact identity pair, device `romsize == 0x10000`, family alternative/tag/transport match, transfer region exactly `kRom`, no kernel, `initial_baud == 1953`, and `even_parity == true`. The builder signature is:

```cpp
Result<FlashPlan> build_subaru_unisia_jecs_plan(FlashOperation operation, std::string_view protocol_name,
                                                 std::string_view mcu_type,
                                                 std::optional<bytes::Bytes> image);
Status validate_subaru_unisia_jecs_plan(const FlashPlan& plan);
```

- [ ] **Step 5: Register Bazel targets and portable closure membership**

Add `subaru_unisia_jecs_plan` and `subaru_unisia_jecs_plan_test`; register the library target by name under `//src/backend/flash/ecu` in `PORTABLE_PACKAGES`.

- [ ] **Step 6: Run GREEN and mutation checks**

Run:

```bash
bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_plan_test //src/backend/flash:flash_validation_test
bazel build --config=release //:portable_closure
```

Expected: all tests pass and portable closure builds. Delete each validator identity/wire/operation check one at a time; its direct-construction test must fail, then restore it.

- [ ] **Step 7: Commit**

```bash
git add src/backend/flash/flash_types.h src/backend/flash/flash_plan.cpp \
  src/backend/flash/flash_validation_test.cpp src/backend/flash/testing/flash_printers.h \
  src/backend/flash/ecu/subaru_unisia_jecs_* src/backend/flash/ecu/BUILD.bazel bazel/portable_targets.bzl
git commit -m "feat(flash): add the Unisia Jecs read plan"
```

---

### Task 2: Raw byte-stream executor and complete 64 KiB read

**Files:**
- Create: `src/backend/flash/ecu/subaru_unisia_jecs_executor.h`
- Create: `src/backend/flash/ecu/subaru_unisia_jecs_executor.cpp`
- Create: `src/backend/flash/ecu/subaru_unisia_jecs_executor_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`
- Modify: `bazel/portable_targets.bzl`

**Interfaces:**
- Consumes: `SubaruUnisiaJecsPlan`, `validate_subaru_unisia_jecs_plan`, `IKlineFlashTransport::{write,read,write_raw,read_raw}`, `IClock`, `ICancellationToken`, `IEventSink`.
- Produces: `SubaruUnisiaJecsExecutor final : IKlineFlashExecutor`, including even-parity `transport_setup()` and a 64 KiB `FlashExecutionResult`.

- [ ] **Step 1: Write RED tests for transport configuration and the boundary addresses**

```cpp
TEST(SubaruUnisiaJecsExecutor, TransportSetupRequests1953BaudEvenParity)
{
    const auto plan = readPlan("sub_ecu_unisia_jecs_m3779x", "M3779x");
    const auto setup = SubaruUnisiaJecsExecutor{}.transport_setup(plan);
    ASSERT_THAT(setup, fastecu::testing::IsOk());
    EXPECT_EQ(setup->baud, 1953);
    EXPECT_FALSE(setup->iso14230);
    EXPECT_EQ(setup->parity, KlineParity::Even);
}

TEST(SubaruUnisiaJecsExecutor, ReadsEveryAddressThroughRawTransport)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptWakeup(transport);
    for (std::uint32_t address = 0; address < 0x10000; ++address)
    {
        transport.expectRawWrite({0x78, static_cast<bytes::Byte>(address >> 8U),
                                  static_cast<bytes::Byte>(address), 0x00});
        transport.queueRawRead({static_cast<bytes::Byte>(address >> 8U),
                                static_cast<bytes::Byte>(address),
                                static_cast<bytes::Byte>(address ^ (address >> 8U))});
    }
    const auto result = executeRead(transport);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    ASSERT_EQ(result->read_bytes->size(), 0x10000U);
    EXPECT_EQ(result->read_bytes->at(0x00ff), 0xff);
    EXPECT_EQ(result->read_bytes->at(0x0100), 0x01);
    EXPECT_EQ(result->read_bytes->at(0xffff), 0x00);
    EXPECT_TRUE(transport.scriptConsumed());
}
```

The expected byte formula is hand-derived and deliberately depends on both address bytes, so a swapped or truncated address cannot satisfy the test.

- [ ] **Step 2: Run RED**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_executor_test`

Expected: analysis failure because the executor target does not exist.

- [ ] **Step 3: Implement transport setup and the wake-up exchange**

`transport_setup()` validates the plan and returns:

```cpp
KlineConfig{.baud = 1953,
            .iso14230 = false,
            .tester_id = 0,
            .target_id = 0,
            .parity = KlineParity::Even};
```

At execution start: validate/check family; reject non-read defensively; write `{0x78,0x12,0x34,0x00}` with `transport.write()`; require the full four-byte write; sleep 500 ms; then call `transport.read(1000ms, cancellation)` once and discard any frame, matching the legacy flush.

- [ ] **Step 4: Implement one addressed raw-byte read with bounded cancellation/error exits**

Use a helper with this contract:

```cpp
struct RawReadState
{
    bytes::Bytes pending;
    bool synchronized = false;
    unsigned tuples_since_sync = 0;
};

Result<bytes::Byte> read_address(std::uint16_t address, RawReadState& state,
                                 IKlineFlashTransport& transport, IClock& clock,
                                 const ICancellationToken& cancellation);
```

It emits `{0x78,hi,lo,0x00}` via `write_raw()`, checks the full four-byte count, sleeps 45 ms, and accumulates `read_raw(5ms)` chunks in `state.pending`, which deliberately survives across addresses so a chunk containing multiple tuples is not truncated when the first byte is returned. Examine three-byte tuples until one begins with `hi,lo`; return its third byte. Before synchronization erase one byte on mismatch; after any successful address, preserve synchronization state for the next address and erase three bytes on mismatch. After more than ten examined tuples set synchronization false. Track no-match time in 5 ms read quanta; at 100 quanta (the legacy 500 ms deadline) resend the same request and reset the counter. Every loop checks cancellation, and all port/clock errors propagate unchanged.

- [ ] **Step 5: Assemble the complete image and report progress**

Reserve `0x10000`, call `read_address()` for each address from `0x0000` through `0xffff`, append exactly one byte, and call `events.progress(bytes_done, 0x10000)` after each byte. Return:

```cpp
FlashExecutionResult{
    .operation = FlashOperation::Read,
    .read_bytes = std::move(image),
    .rom_id = std::nullopt,
};
```

- [ ] **Step 6: Add focused malformed-stream and retry tests**

Add literal scripts proving:

```cpp
// Noise before sync: 99 00 12 34 AB -> returns AB for address 0x1234.
// Split tuple: reads {12}, then {34,AB} -> returns AB without losing bytes.
// Multiple tuples: {00,00,AA, 00,01,BB} supplies two consecutive addresses.
// After synchronization, one wrong tuple is discarded as three bytes.
// 100 empty 5 ms reads cause the exact address request to be retransmitted.
// 0x00ff and 0x0100 requests are {78,00,ff,00} and {78,01,00,00}.
```

Use a small test seam that executes a supplied half-open address range while production always passes `[0,0x10000)`. Keep that seam in an unnamed-namespace helper exposed only through a focused friend/test adapter; do not add a production option that permits partial ROM success.

- [ ] **Step 7: Run GREEN and mutation checks**

Run:

```bash
bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_executor_test
bazel test --config=release //src/backend/flash/ecu:all
bazel build --config=release //:portable_closure
```

Expected: all pass. Mutation-check at least: `>> 8U` to `>> 7U`; `write_raw` to `write`; `read_raw` to `read`; 45 ms to 44 ms; 500 ms retry to 495 ms; image bound `0x10000` to `0x20`; pre-sync erase one to three; post-sync erase three to one. Each mutation must fail a named test, then restore cleanly.

- [ ] **Step 8: Commit**

```bash
git add src/backend/flash/ecu/subaru_unisia_jecs_executor.* \
  src/backend/flash/ecu/subaru_unisia_jecs_executor_test.cpp \
  src/backend/flash/ecu/BUILD.bazel bazel/portable_targets.bzl
git commit -m "feat(flash): port the Unisia Jecs raw read executor"
```

---

### Task 3: Cancellation and failure coverage

**Files:**
- Modify: `src/backend/flash/ecu/subaru_unisia_jecs_executor_test.cpp`

**Interfaces:**
- Consumes: Task 2 executor and scripted raw transport.
- Produces: regression coverage for every externally visible failure boundary; no new production API.

- [ ] **Step 1: Add table-driven cancellation/error tests before changing production code**

Cover these literal cases independently:

```cpp
struct FailureCase
{
    std::string_view name;
    ErrorKind injected;
    std::size_t expected_writes;
};
```

- wake-up echo write error;
- short wake-up write;
- cancellation during the 500 ms wake-up delay;
- wake-up flush read error;
- first raw write error and short raw write;
- cancellation during the 45 ms pacing delay;
- raw read error;
- cancellation before a retransmission;
- cancellation after at least one successfully appended byte.

For each, assert the exact `ErrorKind`, that later scripted requests remain unconsumed, and that no successful `read_bytes` is returned.

- [ ] **Step 2: Run the focused tests and verify meaningful RED where coverage exposes a missing checkpoint**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_executor_test --test_filter='*Cancellation*:*Failure*:*ShortWrite*'`

Expected: any missing checkpoint test fails at the first later transport action. If all pass because Task 2 already implemented every checkpoint, record that this task is test-only and proceed; do not manufacture a production change.

- [ ] **Step 3: Add only missing cancellation checkpoints**

Every new branch uses:

```cpp
if (cancellation.cancelled())
{
    return fail(ErrorKind::Cancelled, "cancelled while reading Unisia Jecs ROM");
}
```

Do not convert a transport `Timeout`, `Disconnected`, or `Internal` error into `Cancelled` unless the token is actually cancelled.

- [ ] **Step 4: Verify and mutation-check the checkpoints**

Run the focused target and then `bazel test --config=release //src/backend/flash/ecu:all`. Delete each added checkpoint one at a time and confirm its named test consumes a forbidden later action or returns the wrong result.

- [ ] **Step 5: Commit**

```bash
git add src/backend/flash/ecu/subaru_unisia_jecs_executor.cpp \
  src/backend/flash/ecu/subaru_unisia_jecs_executor_test.cpp
git commit -m "test(flash): pin Unisia Jecs read failures"
```

---

### Task 4: Desktop workflow routing and dialog migration

**Files:**
- Modify: `src/platform/desktop/common/flash/flash_workflow.cpp`
- Modify: `src/platform/desktop/common/flash/flash_workflow_test.cpp`
- Modify: `src/platform/desktop/common/flash/BUILD.bazel`
- Modify: `src/ui/desktop/flash/ecu/flash_ecu_subaru_unisia_jecs.h`
- Modify: `src/ui/desktop/flash/ecu/flash_ecu_subaru_unisia_jecs.cpp`
- Modify: `src/ui/desktop/flash/ecu/BUILD.bazel`
- Modify: `src/ui/desktop/mainwindow.cpp`

**Interfaces:**
- Consumes: Task 1 builder, Task 2 executor, `DesktopKlineFlashTransport`, `QtClock`, `FlashWorker`, existing dialog workflow helpers.
- Produces: exact workflow routes for both protocols and a dialog with no legacy worker dependency.

- [ ] **Step 1: Add failing routing tests**

In `flash_workflow_test.cpp`, add one case per exact protocol/MCU pair. Assert the first workflow step is `FlashBeginStep`, the second is a K-Line `FlashAttempt`, and invalid cross-pairs terminate with `InvalidConfig`. Add prefix-lookalikes such as `sub_ecu_unisia_jecs_m3779x_extra` and require `tryCreate()` to return `nullptr`.

- [ ] **Step 2: Run RED**

Run: `bazel test --config=release //src/platform/desktop/common/flash:test_flash_workflow --test_filter='*UnisiaJecs*'`

Expected: failure because the protocols are not in `kRoutes`.

- [ ] **Step 3: Add the workflow**

Add one `Route::Kind::SubaruUnisiaJecs`, two `RouteMatch::Exact` entries, includes/dependencies for the plan and executor, and a workflow matching the established single-attempt K-Line shape:

```cpp
return FlashWorkflowStep{
    std::in_place_type<FlashAttempt>,
    bind_flash_attempt(std::move(*plan_), std::make_unique<SubaruUnisiaJecsExecutor>(),
                       std::make_unique<DesktopKlineFlashTransport>(request_.serial)),
    std::make_unique<QtClock>()};
```

- [ ] **Step 4: Convert the dialog to `FlashWorker`**

Follow the current migrated ECU dialogs: build a `FlashWorkflowRequest` from the selected protocol, MCU, operation and paths; preserve the existing confirmation text and success/failure/cancel presentation; connect worker log/progress signals; on successful read copy the full 64 KiB into `ecuCalDef->FullRomData`; and make `closeEvent()` request worker cancellation/unblock. Remove `FlashEcuSubaruUnisiaJecsOperation` and `FlashOperationWorker` references from the header and implementation.

- [ ] **Step 5: Remove the two legacy `MainWindow` branches**

Delete only the `m3779x` and `m3775x` legacy branches at `mainwindow.cpp`; routing now occurs through `FlashWorkflowFactory`. Preserve the neighboring M32R Unisia branches.

- [ ] **Step 6: Run workflow and UI tests**

Run:

```bash
bazel test --config=release //src/platform/desktop/common/flash:test_flash_workflow
bazel test --config=release //src/ui/desktop:mainwindow_test
bazel build --config=release //:fastecu
```

Expected: tests pass and desktop target builds.

- [ ] **Step 7: Commit**

```bash
git add src/platform/desktop/common/flash/flash_workflow.cpp \
  src/platform/desktop/common/flash/flash_workflow_test.cpp src/platform/desktop/common/flash/BUILD.bazel \
  src/ui/desktop/flash/ecu/flash_ecu_subaru_unisia_jecs.* src/ui/desktop/flash/ecu/BUILD.bazel \
  src/ui/desktop/mainwindow.cpp
git commit -m "feat(flash): route Unisia Jecs through the portable workflow"
```

---

### Task 5: Legacy deletion, ratchet, qualification record, and complete gate

**Files:**
- Delete: `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_unisia_jecs_operation.h`
- Delete: `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_unisia_jecs_operation.cpp`
- Modify: `src/platform/desktop/common/flash/legacy/BUILD.bazel`
- Modify: `scripts/check-legacy-flash-drain.py`
- Modify: `docs/flash-qualification-matrix.md`
- Create: `docs/unisia-jecs-bench-checklist.md`

**Interfaces:**
- Consumes: Tasks 1–4 complete family migration.
- Produces: five-family legacy ratchet, experimental matrix row, and explicit bench stop for the corrected full-ROM read.

- [ ] **Step 1: Make the drain test fail for the intended reason**

Delete the legacy `.h/.cpp` and remove them from `legacy_flash_operations`, but leave `REMAINING` unchanged. Run:

`bazel test --config=release //:legacy_flash_drain`

Expected: FAIL with `drain shrank` and exactly `ecu/flash_ecu_subaru_unisia_jecs_operation.cpp` listed as migrated.

- [ ] **Step 2: Shrink the ratchet and flip the matrix row**

Remove exactly that `REMAINING` entry. Change the matrix row to portable `yes`, coverage naming the plan/executor/workflow suites, status `experimental`, and notes that:

- both exact protocol/MCU pairs are supported;
- read-only is enforced;
- even parity at 1953 baud is configured through `KlineConfig`;
- the raw transport foundation landed in PR #351;
- deliberate correction: full 64 KiB returned instead of the legacy 32-byte truncation;
- no hardware qualification exists.

- [ ] **Step 3: Add the bench checklist**

The checklist must start with a blocking section:

```markdown
## 0. STOP — full-ROM read is not hardware-qualified

Do not treat this family as proven until both M3779x and M3775x hardware have
confirmed 1953-baud even-parity startup, the wake-up/flush timing, raw byte
address rollover, and an exact 65,536-byte dump against a trusted image.
```

Then list adapter setup, cancellation/unblock, boundary addresses `00ff/0100/ffff`, retry/noise recovery, saved-file length/hash, and a requirement to record adapter/ECU identifiers and results separately for both MCU variants.

- [ ] **Step 4: Run the full verification gate**

Run each command fresh and read its complete result:

```bash
bazel test --config=release //src/backend/flash/ecu:all
bazel test --config=release //src/platform/desktop/common/flash:test_flash_workflow //src/ui/desktop:mainwindow_test
bazel test --config=release //:legacy_flash_drain //:serial_compat_allowlist //:windows_preprocessor_guards
bazel build --config=release //:fastecu //:portable_closure
bazel test --config=release //...
prek run --all-files
bazel run //:clang_tidy_report_changed
git diff --check
```

Expected: all commands exit zero; legacy drain reports five families remaining; changed-file clang-tidy reports no findings.

- [ ] **Step 5: Commit**

```bash
git add -A src/platform/desktop/common/flash/legacy \
  scripts/check-legacy-flash-drain.py docs/flash-qualification-matrix.md \
  docs/unisia-jecs-bench-checklist.md
git commit -m "chore(flash): retire the legacy Unisia Jecs operation"
```

- [ ] **Step 6: Prepare final review evidence**

The review package must compare the feature branch merge-base with `origin/master` through `HEAD`, include every ledger `Ruling:` line, and explicitly ask the reviewer to examine the five `Review Focus` items plus the full-ROM correction, raw/echo method selection, retry termination, and exact-route behavior.
