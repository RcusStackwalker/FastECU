# Hitachi SH72543R CAN Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate wave 6a-3 to portable flashing, covering both SH72543R CAN protocol aliases and reducing the legacy drain from eight families to seven.

**Architecture:** A family-specific validated FlashPlan feeds a synchronous ICanFlashExecutor. The desktop FlashWorkflow binds existing transport and clock adapters and uses the shared dialog. Protocol helpers stay private to the family; no transport or generic flashing abstraction is added.

**Tech Stack:** Existing C++/Bazel portable backend, GoogleTest/GoogleMock, Qt desktop workflow tests, scripted CAN transport and fake clock.

**Spec:** [Approved design](../specs/2026-09-20-step5-tail-wave6a3-hitachi-sh72543r-can-design.md). Read the entire spec before execution. Source baseline for legacy citations: `5dc86672`.

## Global Constraints

- Accept exactly `sub_ecu_hitachi_sh72543r_can` and `sub_ecu_hitachi_sh72543r_can_recovery`, both bound to MCU `SH72543R`.
- Read window: address zero, length `0x200000`, pages of `0x400` bytes.
- Write input: exactly `0x200000` bytes, indexed from address zero.
- Program block 1 only, `[0x6000, 0x200000)`, in `0x100`-byte frames.
- ISO-15765, 500 kbit/s, 11-bit request/response IDs `0x7E0`/`0x7E8`.
- Read and Write are supported; TestWrite returns Unsupported.
- Backend code must not depend on Qt, `SerialPortActions`, or `EcuCalDefStructure`.
- No new transport surface, kernel upload, shared protocol state machine, protocol-catalog change, or hardware qualification is included.
- Every exchange receives a comment citing the legacy file and line at the source baseline.
- Require at least 80% new-code coverage and the SonarCloud Quality Gate for the implementation PR.
- Keep hardware status experimental. No live ECU operation is part of execution.

## Review Focus

1. A suffix resembling recovery must not accidentally route; exact alias tests belong to tasks 1 and 4.
2. A manually constructed plan must not bypass dry-run rejection or geometry checks; task 1 tests validator independently, task 2 tests executor entry points.
3. Cancellation after the erase reply but before the first data write must prevent that write; task 3 pins the boundary.
4. A truncated optional identity must not corrupt an existing ROM name or abort an otherwise valid read; tasks 2 and 4 pin absent/partial metadata handling.
5. Payload indexing must cross the `0xFFFF` address boundary correctly and include the last frame; task 3 uses a nonuniform image and independent expected ciphertext.

## File map and execution setup

Create under `src/backend/flash/ecu/`:

- `subaru_hitachi_sh72543r_can_types.h`: portable family parameters.
- `subaru_hitachi_sh72543r_can_plan.{h,cpp}`: pure construction/validation.
- `subaru_hitachi_sh72543r_can_executor.{h,cpp}`: family wire sequences.
- `subaru_hitachi_sh72543r_can_plan_test.cpp`: identity/geometry/forged plans.
- `subaru_hitachi_sh72543r_can_executor_test.cpp`: complete scripted transcripts.

Modify `src/backend/flash/{flash_types.h,flash_plan.cpp,BUILD.bazel}`,
`src/backend/flash/ecu/BUILD.bazel`, and `bazel/portable_targets.bzl` for registration.
Modify `src/platform/desktop/common/flash/{flash_workflow.cpp,flash_workflow_test.cpp,BUILD.bazel}`
and `src/ui/desktop/mainwindow.{h,cpp}` for desktop routing.
Delete `src/ui/desktop/flash/ecu/flash_ecu_subaru_hitachi_sh72543r_can.{h,cpp}`
and `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_hitachi_sh72543r_can_operation.{h,cpp}`.
Update the drain script, qualification matrix, parent wave spec, and modularization plan.

At execution start use the worktree skill to establish isolation, retaining this
plan/spec commit. Do not create implementation files during plan review. Read
applicable repository instructions again in the execution checkout. Preserve
legacy line references using `git show 5dc86672:<path>` after deleting sources.

Tasks are sequential: task 2 consumes task 1; task 3 extends task 2; task 4
routes only the completed executor. Intermediate backend commits must not route
an incomplete executor to users. Do not push or create a PR merely to obtain
Sonar results without authorization for that external action.

### Task 1: Family types and validated plans

**Files:** create types, plan header/source/test from the file map; modify backend
registration files and `bazel/portable_targets.bzl`.

**Interfaces produced:**

```cpp
struct SubaruHitachiSh72543rCanPlan {
    std::uint32_t request_id;
    std::uint32_t response_id;
    int bitrate;
    bool extended_id;
    std::uint32_t page_size;
    std::uint32_t write_frame_size;
};
Result<FlashPlan> build_subaru_hitachi_sh72543r_can_plan(
    FlashOperation operation, std::string_view protocol_name,
    std::string_view mcu_type, std::optional<bytes::Bytes> image);
Status validate_subaru_hitachi_sh72543r_can_plan(const FlashPlan& plan);
```

- [ ] Add plan tests and their Bazel target first. Include a concrete read-window test:

```cpp
TEST(SubaruHitachiSh72543rCanPlan, ReadCoversWholeRom) {
    auto plan = build_subaru_hitachi_sh72543r_can_plan(
        FlashOperation::Read, "sub_ecu_hitachi_sh72543r_can", "SH72543R", std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_EQ(plan->transfer_region().start, 0U);
    EXPECT_EQ(plan->transfer_region().length, 0x200000U);
    EXPECT_TRUE(plan->erase_regions().empty());
    EXPECT_FALSE(plan->kernel().has_value());
}
TEST(SubaruHitachiSh72543rCanPlan, RejectsDryRunWithoutImage) {
    auto plan = build_subaru_hitachi_sh72543r_can_plan(
        FlashOperation::TestWrite, "sub_ecu_hitachi_sh72543r_can", "SH72543R", std::nullopt);
    EXPECT_THAT(plan, fastecu::testing::IsErr(ErrorKind::Unsupported));
}
```

- [ ] Run `bazel test --config=release //src/backend/flash/ecu:subaru_hitachi_sh72543r_can_plan_test`; expect missing new interface/build input before implementation.
- [ ] Add types, enum/variant entry, FamilyTraits, kernel-free specialization, family display name, and explicit Bazel dependencies. Register each types/plan library by name in PORTABLE_PACKAGES.
- [ ] Implement identity and operation checks before image checks; use `validate_and_build` then family validation. Construct fields with operation-dependent windows:

```cpp
constexpr MemoryRegion kRead{0, 0x200000};
constexpr MemoryRegion kWrite{0x6000, 0x1FA000};
if (operation != FlashOperation::Read && operation != FlashOperation::Write)
    return fail(ErrorKind::Unsupported, "operation is not supported by Subaru Hitachi SH72543R CAN");
// In FlashPlanFields:
// .transfer_region = operation == FlashOperation::Read ? kRead : kWrite,
// .erase_regions = operation == FlashOperation::Write
//     ? std::vector{kWrite} : std::vector<MemoryRegion>{},
// .family_plan = SubaruHitachiSh72543rCanPlan{0x7E0, 0x7E8, 500000, false, 0x400, 0x100},
```

Validate the flash table's two blocks and ROM size. Treat erase_regions as the
intended programming window, not proof of hardware erase scope. Read rejects any
image; Write requires exactly 2 MiB. Both reject kernels and extra confirmations.
- [ ] Add parameterized tests for both aliases, suffix typo, wrong MCU, missing/short/long write image, read image, wrong family/transport/variant, each altered wire field, wrong transfer/erase region, kernel, extra confirmations, TestWrite and invalid enum operation. Build forged fields through the existing `validate_and_build` test pattern to exercise the family validator independently.
- [ ] Run the plan target, `//src/backend/flash:flash_types_test`, `//src/backend/flash:flash_validation_test`, and `//:portable_closure`; expect pass. Commit as `feat(flash): add validated SH72543R CAN plans`.

### Task 2: Connection and complete read execution

**Files:** create executor header/source/test; modify ECU BUILD and portable target map.
**Consumes:** task 1 builder and validator.
**Produces:**

```cpp
class SubaruHitachiSh72543rCanExecutor final : public ICanFlashExecutor {
public:
    Result<Iso15765Config> transport_setup(const FlashPlan& plan) const override;
    Result<FlashExecutionResult> execute(const FlashPlan& plan,
        ICanFlashTransport& transport, IClock& clock,
        const ICancellationToken& cancellation, IEventSink& events) override;
};
```

- [ ] Add executor entry-point tests for validated configuration and rejection with an empty scripted transport. Use FakeClock, ManualCancellationToken, RecordingEventSink, and ScriptedCanFlashTransport as in the predecessor executor tests. Register the new test target with explicit dependencies on these fixtures.
- [ ] Run the new executor target; expect missing executor. Implement both entry points with family validation before any transport call. transport_setup returns `iso15765_config_from` of the validated family alternative. Register the executor library in PORTABLE_PACKAGES.
- [ ] Add local test framing functions and pin the probe shortcut:

```cpp
bytes::Bytes frame(std::uint32_t id, bytes::ByteView payload) {
    bytes::Bytes out;
    bytes::appendU32Be(out, id);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
// In each shortcut transcript, with Bytes = bytes::Bytes:
transport.exchange(frame(0x7E0, Bytes{0xB7}),
                   frame(0x7E8, Bytes{0x7F, 0xB7, 0x13}));
```

Add full initialization transcripts for AA, 09 02, 09 04, 09 06 and both A8
requests. Pin timeout/delay values from legacy lines 86–311 and header constants.
- [ ] Run those tests red; implement private exchange helpers that propagate write/disconnect errors, tolerate Timeout only at optional exchanges, and check cancellation around I/O and clock sleeps. Parse complete identity fields only; compose CAL-ID and ECU-ID without assuming both exist. Keep an absent ID absent on the kernel shortcut.
- [ ] Add read-session and complete-page transcripts. Transcribe the seed-key tables from legacy lines 1131–1151. Establish a literal seed/key vector with an independent transcription of the documented SSM transform; record seed, expected key, and derivation provenance in the test, then remove any temporary derivation script from deliverables. Do not derive expected keys through the production helper being tested.
- [ ] Pin requests and output with a nonuniform page image:

```cpp
for (std::uint32_t address = 0; address < 0x200000; address += 0x400) {
    bytes::Bytes request{0x23, 0x24, 0x00,
        static_cast<std::uint8_t>(address >> 16),
        static_cast<std::uint8_t>(address >> 8),
        static_cast<std::uint8_t>(address), 0x04, 0x00};
    bytes::Bytes page(0x400);
    for (std::size_t i = 0; i < page.size(); ++i)
        page[i] = static_cast<std::uint8_t>((address / 0x400 + i) & 0xff);
    bytes::Bytes reply{0x63};
    reply.insert(reply.end(), page.begin(), page.end());
    transport.exchange(frame(0x7E0, request), frame(0x7E8, reply));
}
```

Assemble the expected 2 MiB output separately from the executor, assert exact
equality and script consumption, and script the trailing 10 01 request.
- [ ] Implement the bounded page loop and required seed/key checks, with per-exchange legacy citations. Require total reply size `4 + 1 + 0x400` and service 63; preserve raw page bytes. Required read failures return no partial result. Implement up to six stop attempts, each delayed 200 ms and read at 800 ms; stop on any nonempty response, tolerate exhaustion.
- [ ] Add malformed page sizes (zero, short, oversized), wrong service, seed lengths 0–3, wrong key reply, optional identity truncation, tolerated session response, read-stop exhaustion, disconnect, write failure, timeout, and cancellation tests. Use scripted fault injection and cancellation callbacks from existing fixtures; assert no later page command. Test valid partial metadata combinations and absent identity explicitly.
- [ ] Run the executor target and portable closure; expect pass. Keep Write unavailable before any I/O in this intermediate commit, and do not route the family yet. Commit as `feat(flash): port SH72543R CAN connection and reads`.

### Task 3: Erase, programming, and verification

**Files:** executor source/test from task 2.
**Consumes/produces:** the same public executor interface; adds Write execution.

- [ ] Add a complete write transcript that initially fails on the intermediate unsupported Write result. Use a 2 MiB nonuniform image. Script probe, session 10 43, seed/key, jump 10 42, download and erase with the exact payloads:

```cpp
transport.exchange(frame(0x7E0, bytes::Bytes{0x34,0x04,0x33,0x00,0x60,0x00,0x1F,0xA0,0x00}),
                   frame(0x7E8, bytes::Bytes{0x74,0x20}));
transport.exchange(frame(0x7E0, bytes::Bytes{0x31,0x01,0x02,0x01,0x0F,0xFF,0xFF,0xFF}),
                   frame(0x7E8, bytes::Bytes{0x71,0x01,0x02}));
```

- [ ] Run executor target red. Implement write setup, preserving tolerated 10 43 but requiring complete seed/key, 50 42 jump and 74 20 download replies. Encrypt with the exact legacy table `{0xB740,0x42DA,0xA7CA,0x5FB1}` and its transformation table, using the portable SSM primitive.
- [ ] Add erase tests: immediate completion, timeout then completion, nonmatching response then completion, twenty failed subsequent reads, disconnect, cancellation during polling, and cancellation immediately after positive erase. Assert no B6 after every erase failure/cancellation. The first response is examined, never discarded.
- [ ] Implement at most one erase send and 21 total reads (initial plus up to 20 polls), preserving 100 ms before the send and 500 ms after failed polls. Track whether any response arrived for exhaustion classification: Timeout for silence, BadResponse for nonmatching responses. Propagate specific transport failures.
- [ ] Add independently calculated literal ciphertext vectors for selected nonuniform four-byte words. Generate full transcript expected ciphertext through a test-local independent transform with those literal vector checks, not the executor's encryption function. Assert every B6 request and its length; in particular addresses 0x6000, 0xFF00, 0x10000, and 0x1FFF00.
- [ ] Construct frames with append/insert, never out-of-range indexing:

```cpp
for (std::uint32_t address = 0x6000; address < 0x200000; address += 0x100) {
    bytes::Bytes payload{0xB6,
        static_cast<std::uint8_t>(address >> 16),
        static_cast<std::uint8_t>(address >> 8),
        static_cast<std::uint8_t>(address)};
    payload.insert(payload.end(), encrypted.begin() + address,
                   encrypted.begin() + address + 0x100);
    // Send framed payload through the family exchange helper, delay 10 ms,
    // read at 2000 ms; tolerate timeout/content, propagate transport failure.
}
```

Here `encrypted` is the full transformed image. The transcript asserts exactly
8,096 data frames, each 264 bytes including CAN prefix, with none below 0x6000.
- [ ] Add close/checksum scripts and retry tests. Close sends 37 and accepts 77 at 800 ms, up to 20 sends. Then sleep 100 ms; checksum sends 31 01 02 02 01 and accepts prefix 71 01 02 at 2000 ms, up to 20 sends. Pin exhaustion classification, cancellation, and no checksum after close failure.
- [ ] Implement finalization and return Write success only after checksum succeeds. Use the existing phase-progress pattern and initialized timing values so progress is monotonic and failure never emits a successful completion.
- [ ] Add tests for ignored B6 content/timeout versus fatal write/disconnect; cancellation at first/middle/last data frame; cancellation during close/checksum; and read/write progress ordering. Confirm no stale read bytes or identity on reusing an executor for a later attempt.
- [ ] Run both family test targets and portable closure; expect pass. Commit as `feat(flash): port SH72543R CAN erase and programming`.

### Task 4: Desktop routing and legacy removal

**Files:** workflow source/test/BUILD, MainWindow header/source, four deleted files
from the map, drain script, qualification matrix, parent wave spec and modularization plan.
**Consumes:** complete plan/executor interfaces from tasks 1–3.
**Produces:** both aliases reachable through `FlashWorkflowFactory::tryCreate(FlashWorkflowRequest)`.

- [ ] Add a Qt test slot for both aliases before factory registration:

```cpp
for (const char* protocol : {"sub_ecu_hitachi_sh72543r_can",
                             "sub_ecu_hitachi_sh72543r_can_recovery"}) {
    auto input = request(protocol, FlashOperation::TestWrite);
    input.mcu = "SH72543R";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);
    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(step));
    QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::Unsupported);
}
```

- [ ] Run `bazel test --config=release //src/platform/desktop/common/flash:test_flash_workflow`; expect null workflow for the new aliases.
- [ ] Add the workflow class using the existing request/plan/begun/attempted/outcome shape. Build the plan in its constructor; expose plan failure before Begin; decline cancels without binding. Bind only after acceptance:

```cpp
bind_flash_attempt(std::move(*plan_),
    std::make_unique<SubaruHitachiSh72543rCanExecutor>(),
    std::make_unique<DesktopCanFlashTransport>(request_.serial))
```

Use QtClock and the existing FlashAttemptOutcome result handling. Register two
RouteMatch::Exact entries plus the corresponding internal route-kind dispatch.
- [ ] Add Read/Write routing tests for each alias with MCU SH72543R and correct image sizes. Inspect the bound plan; assert the complete image is retained and read/erase windows differ. Add wrong-MCU/size rejection, declined Begin, attempt failure, returned ROM ID, absent ROM ID, and a misspelled recovery suffix. A rejected suffix must not fall through to the removed legacy prefix dispatch.
- [ ] Verify read inspect/save/discard at the existing shared-dialog boundary: add family cases to existing applicable parameterized tests or extend `src/ui/desktop/flash/common/flash_dialog_test.cpp` if necessary. Do not add a second family-specific save workflow. Confirm absent metadata leaves existing naming behavior intact.
- [ ] Remove the family-specific MainWindow branch/include and delete the four obsolete sources. Inspect `src/ui/desktop/BUILD.bazel` and legacy BUILD globs: remove explicit references if present; do not edit working globs for ceremony. Add workflow library dependencies on the new plan/executor.
- [ ] Remove exactly the SH72543R operation entry from REMAINING. Update matrix with portable=yes, experimental, actual test labels, all eight correction categories, uninterpreted B6 replies, and unverified physical erase scope. Update wave progress: 6a-1/#347 and 6a-2/#348 merged; 6a-3 implemented on this branch, not claimed merged. Add corresponding modularization progress entry.
- [ ] Run workflow/family tests and `python3 scripts/check-legacy-flash-drain.py`; expect seven families. Search obsolete class/path references with `rg -n 'FlashEcuSubaruHitachiSH72543rCan|flash_ecu_subaru_hitachi_sh72543r_can' src`; expect no live references. Legacy citations in new backend comments are allowed.
- [ ] Commit as `feat(flash): route SH72543R CAN through portable workflow`.

### Task 5: Cross-check fidelity, coverage, and integration gates

**Files:** family tests and documentation as needed to close concrete gaps; no unrelated refactors.
**Consumes:** all prior deliverables.
**Produces:** reviewable migration with recorded verification and remaining external gates.

- [ ] Compare every executor exchange against `git show 5dc86672:src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_hitachi_sh72543r_can_operation.cpp`. Check request bytes, read timeouts, delays, retry counts, encryption tables, and image offsets. Confirm every departure appears in the matrix/spec correction ledger.
- [ ] Run the focused family, workflow, and shared dialog test labels. Resolve the dialog label from its existing BUILD declaration; do not guess it. If a defect emerges, add a reproducing test, observe failure, fix, and rerun the affected tests before the full gates.
- [ ] Run the inherited gates:

```sh
bazel build -k --config=release //:fastecu //tests/...
bazel test -k --config=release //tests/... //:bazel_openssl_wiring \
  //:serial_compat_allowlist //:portable_closure //:legacy_flash_drain
bazel test --config=release \
  //src/backend/flash/ecu:subaru_hitachi_sh72543r_can_plan_test \
  //src/backend/flash/ecu:subaru_hitachi_sh72543r_can_executor_test \
  //src/platform/desktop/common/flash:test_flash_workflow
```

- [ ] Inspect the repository's existing coverage/CI configuration, run its supported coverage procedure on the new portable targets, and report measured new-code coverage. Close missing required branches with meaningful tests. Keep the >=80% threshold and SonarCloud Quality Gate visible; if external CI cannot run in the session, record that gate as pending rather than claiming it passed.
- [ ] Run `git diff --check`, inspect `git diff --stat`, and verify each new library is named in PORTABLE_PACKAGES. Confirm no adapter/allowlist growth, seven remaining families, and no accidentally staged unrelated files.
- [ ] Commit verification-driven tests/docs as `test(flash): complete SH72543R CAN migration coverage` if there are additional changes. Report exact commands/results, coverage, commit range, and any pending CI/bench limits. Follow the chosen execution method's review and branch-finishing workflow; do not imply hardware qualification.

## Plan self-review

The five tasks cover plan identity/geometry, all connection/read/write sequences,
explicit corrections, desktop ownership/routing, legacy removal, documentation,
portable registration, and verification. Review Focus cases have owning test
steps. No shared port is introduced. Public names and parameter types are
consistent throughout; temporary incomplete Write behavior exists only before
routing and is removed by task 3. Implementation begins only after plan review
and execution-method selection.
