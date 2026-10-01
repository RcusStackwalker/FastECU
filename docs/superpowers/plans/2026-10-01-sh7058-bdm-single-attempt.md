# SH7058 and BDM join SingleAttemptFlashWorkflow — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the Hitachi SH7058 and Denso MC68HC16Y5 BDM flash families through `SingleAttemptFlashWorkflow` and delete their hand-written desktop workflows, with no observable behavior change.

**Architecture:** The two extra operator prompts become plan-owned `ConfirmationSpec`s (`StartKlineRead`, `KernelBootstrap`) emitted by the family plan builders and pinned by their validators. The desktop maps them to the existing prompt kinds. The factory picks the SH7058 K-Line or CAN alias by operation, and BDM gets a kernel-on-Write preparation policy. Finally, the three lazy preparation policies fold into one `LazyPlan<Prepare>` template.

**Tech Stack:** C++23, Bazel, GoogleTest/gMock, Qt 6 (desktop layer only).

**Spec:** [the design spec](../specs/2026-10-01-sh7058-bdm-single-attempt-design.md). Read it before starting; this plan argues from it.

## Global Constraints

- Work on branch `refactor/sh7058-bdm-single-attempt`; `prek` refuses commits on `master`. Do not push or open a PR without the user's go-ahead.
- Every commit message ends with these two lines:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01UBrg45gykzFdMmAy34bN11`
- Check `Result<T>` with `.has_value()`, never the implicit `operator bool`. Exceptions never cross a port.
- No wire, executor-protocol, dialog-text, `FlashPromptKind`, `flash_workflow.h` or `FlashDialog` changes.
- Prompt kinds, prompt order, kernel-resolution timing and pre-Begin failures stay identical for every plan a builder produces.
- Nothing is marked hardware-qualified; no bench checklist or qualification-matrix edits.
- Each commit passes `bazel test --config=release //...` on its own.
- `prek` runs clang-format on commit. If a hook rewrites files, `git add` them and run the same `git commit` again.
- If a test's BUILD deps change, run `python3 scripts/gazelle_check.py --fix` and commit the BUILD change with the code.

## Review Focus

1. **The desktop passes the operator's ROM on every BDM Write** (`portableImageForOperation` always copies it). The ROM must be dropped and the padded catalog kernel uploaded, not rejected as "plans never carry a ROM image". Pinned by `mc68BdmWriteBindsItsExecutorAndReportsEveryOutcome` (Task 1), whose `bdmWrite()` sets a 0x30000-byte image.
2. **BDM Read or TestWrite with no protocol catalog configured.** Read must reach its attempt and TestWrite must fail with `Unsupported`; neither may report a catalog error. Pinned by the existing `mc68BdmReadRoutesThroughBeginToAttempt` and `mc68BdmTestWriteFailsBeforeAnyPrompt` (both use `paths = {}`), which Tasks 3 and 4 keep passing unmodified. Task 3's mutation check 4 proves they bite.
3. **Kernel files edited or removed while the BDM bootstrap prompt is on screen.** The attempt must upload the kernel read before Begin. Pinned by `mc68BdmWriteKeepsItsKernelSnapshotAfterFilesAreRemoved` (Task 1).
4. **The factory binds the wrong SH7058 executor for an operation.** The attempt must never reach the adapter, and the tests must go red. Pinned by the SH7058 row in `singleAttemptCases()` and by `sh7058WriteBindsTheCanExecutorAfterBeginAlone` (Task 1); Task 3's mutation check 1 proves it.
5. **A plan assembled by hand without its consent** (a future frontend or bench tool building `FlashPlanFields`). It must be rejected before the adapter is configured. Pinned by the validator cases in Task 2 and by `TransportSetupRejectsAWritePlanWithoutTheBootstrapConsent` (Task 2).

---

## File Structure

| File | Change | Responsibility |
|------|--------|----------------|
| `src/platform/desktop/common/flash/flash_workflow_test.cpp` | Modify (Task 1) | Characterization of both families through the factory |
| `src/backend/flash/flash_types.h` | Modify (Task 2) | Two new `ConfirmationSpec::Id` values |
| `src/backend/flash/ecu/subaru_hitachi_sh7058_plan.cpp` | Modify (Task 2) | Builder emits `StartKlineRead` on Read; validator pins it |
| `src/backend/flash/ecu/subaru_hitachi_sh7058_plan_test.cpp` | Modify (Task 2) | Builder and validator confirmation tests |
| `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.cpp` | Modify (Task 2) | Builder emits `KernelBootstrap` on Write; validator pins it |
| `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan_test.cpp` | Modify (Task 2) | Builder and validator confirmation tests |
| `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor_test.cpp` | Modify (Task 2) | Executor rejects a Write plan without consent |
| `src/backend/flash/ecu/BUILD.bazel` | Modify (Task 2, via Gazelle) | Executor test gains `//src/backend/flash:flash_validation` |
| `src/platform/desktop/common/flash/flash_workflow.cpp` | Modify (Tasks 2, 3, 4) | Prompt mapping, aliases, factory, preparation policies |
| `docs/tech-debt.md`, `docs/design-notes.md` | Modify (Task 3) | Close the roadmap action; record the rule's new examples |

---

### Task 1: Characterize both workflows through the factory

Test-only commit. Every new test must **pass against the current code**. A failure means the test misdescribes today's behavior: fix the test, never the product code.

**Files:**
- Modify: `src/platform/desktop/common/flash/flash_workflow_test.cpp` (the `singleAttemptCases()` table near line 2187, and new tests after `kernelBackedFamiliesKeepTheirKernelSnapshotAfterFilesAreRemoved`, before `struct ColtWriteCase`)

**Interfaces:**
- Consumes: existing helpers `request`, `catalogPaths`, `recordingSerial`, `expectNoBackendIo`, `acceptEveryPrompt`, `promptKinds`, `failureDetail`, `FakeCancellationToken`, `NullEventSink`, `fastecu::testing::IsErrWith`.
- Produces: helpers `sh7058Write()`, `bdmWrite(const config::ConfigPaths&)` and `bdmCatalogKernelImage()`, plus seven new tests. Tasks 3 and 4 rely on all of them passing unmodified.

- [ ] **Step 1: Add the two Read rows to the single-attempt table**

Replace the comment above `struct SingleAttemptCase` with:

```cpp
// Characterization of the twenty-four single-attempt families' Read path:
// ten kernel-free CAN families, six kernel-free K-Line families, Colt, and
// seven kernel-backed families. Each runs preflight, Begin, the plan's
// confirmations in order, then exactly one attempt. Hitachi SH7058 and Denso
// MC68HC16Y5 BDM read over K-Line; their Write sides are characterized after
// this suite.
```

In `singleAttemptCases()`, insert directly after the `{"sub_ecu_unisia_jecs_m3779x", ...}` row:

```cpp
        {"sub_ecu_hitachi_sh7058_can",
         "SH7058_1block",
         SubaruHitachiSh7058,
         Kline,
         std::nullopt,
         {FlashPromptKind::ConfirmSh7058Read}},
        {"sub_ecu_denso_mc68hc16y5_02_bdm", "MC68HC16Y5", SubaruDensoMc68hc16y5_02Bdm, Kline, std::nullopt, {}},
```

Every table-driven test now also covers the Read side of both families:
- the prompt order and the bind check that cancels before configure;
- declining each prompt;
- Save and Discard, which count as a decline;
- all four attempt outcomes;
- an unknown MCU, which must fail before any prompt or I/O.

- [ ] **Step 2: Add the SH7058 Write tests**

Insert after `kernelBackedFamiliesKeepTheirKernelSnapshotAfterFilesAreRemoved`:

```cpp
// Hitachi SH7058 Write runs over CAN with Begin as its only prompt; its Read
// side (K-Line) is in singleAttemptCases().
FlashWorkflowRequest sh7058Write()
{
    auto input = request("sub_ecu_hitachi_sh7058_can", FlashOperation::Write);
    input.mcu = "SH7058_1block";
    input.image = bytes::Bytes(0x100000, 0x5a);
    return input;
}

TEST(FlashWorkflowTest, sh7058WriteBindsTheCanExecutorAfterBeginAlone)
{
    auto workflow = FlashWorkflowFactory::tryCreate(sh7058Write());
    ASSERT_TRUE(workflow != nullptr);

    std::vector<FlashPromptStep> prompts;
    auto step = acceptEveryPrompt(*workflow, prompts);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << failureDetail(step);
    EXPECT_THAT(promptKinds(prompts), ::testing::ElementsAre(FlashPromptKind::Begin));

    auto& attempt = std::get<FlashAttempt>(step);
    const FlashPlan& plan = attempt.attempt->plan();
    EXPECT_EQ(plan.family(), FlashFamily::SubaruHitachiSh7058);
    EXPECT_EQ(plan.transport(), TransportKind::CanIso15765);
    EXPECT_EQ(plan.operation(), FlashOperation::Write);
    EXPECT_EQ(plan.image(), std::optional<bytes::Bytes>(bytes::Bytes(0x100000, 0x5a)));
    EXPECT_TRUE(plan.confirmations().empty());

    // The K-Line executor's transport_setup() rejects a Write plan as
    // Unsupported, so reaching the pre-configure cancellation proves the CAN
    // executor is bound. Nothing is configured or opened on the way.
    FakeCancellationToken cancelled(true);
    NullEventSink events;
    EXPECT_THAT(attempt.attempt->run(*attempt.clock, cancelled, events),
                fastecu::testing::IsErrWith(ErrorKind::Cancelled, ::testing::HasSubstr("before configure")));

    workflow->submit(
        FlashAttemptResult{.success = false, .error_kind = ErrorKind::Timeout, .error_detail = "no reply to 0x34"});
    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    EXPECT_EQ(std::get<FlashFailureStep>(step).error, (Error{ErrorKind::Timeout, "no reply to 0x34"}));
}

TEST(FlashWorkflowTest, sh7058WriteCancelsWhenBeginIsDeclined)
{
    auto workflow = FlashWorkflowFactory::tryCreate(sh7058Write());
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Decline);
    for (int repeat = 0; repeat < 2; ++repeat)
    {
        const auto step = workflow->next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::Cancelled);
    }
}

TEST(FlashWorkflowTest, sh7058WriteRejectsInvalidInputBeforeAnyPromptOrIo)
{
    FakeBackend *fake = nullptr;
    auto serial = recordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    expectNoBackendIo(*fake);

    struct Case
    {
        const char *name;
        FlashOperation operation;
        std::size_t image_size;
        ErrorKind expected;
    };
    for (const Case& test : std::to_array<Case>({
             {"test write", FlashOperation::TestWrite, 0x100000, ErrorKind::Unsupported},
             {"short image", FlashOperation::Write, 0xFFFFF, ErrorKind::InvalidConfig},
         }))
    {
        SCOPED_TRACE(test.name);
        auto input = sh7058Write();
        input.operation = test.operation;
        input.image = bytes::Bytes(test.image_size, 0x5a);
        input.serial = serial.get();
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr);
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            const auto step = workflow->next();
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            EXPECT_EQ(std::get<FlashFailureStep>(step).error.kind, test.expected);
        }
    }
}
```

- [ ] **Step 3: Add the BDM Write tests**

Insert directly after the SH7058 tests:

```cpp
// Denso MC68HC16Y5 BDM Write uploads the catalog kernel to RAM and starts it.
// The desktop hands every Write the operator's ROM; BDM must drop it.
FlashWorkflowRequest bdmWrite(const config::ConfigPaths& paths)
{
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm", FlashOperation::Write);
    input.mcu = "MC68HC16Y5";
    input.paths = paths;
    input.image = bytes::Bytes(0x30000, 0x5a);
    return input;
}

// catalog_mc68.bin (11 22 33) zero-padded to the 0x20-byte upload chunk.
bytes::Bytes bdmCatalogKernelImage()
{
    bytes::Bytes image(0x20, 0x00);
    image[0] = 0x11;
    image[1] = 0x22;
    image[2] = 0x33;
    return image;
}

TEST(FlashWorkflowTest, mc68BdmWriteBindsItsExecutorAndReportsEveryOutcome)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());

    for (const bool succeeded : {true, false})
    {
        SCOPED_TRACE(succeeded ? "succeeded" : "failed");
        auto workflow = FlashWorkflowFactory::tryCreate(bdmWrite(*paths));
        ASSERT_TRUE(workflow != nullptr);

        std::vector<FlashPromptStep> prompts;
        auto step = acceptEveryPrompt(*workflow, prompts);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << failureDetail(step);
        EXPECT_THAT(promptKinds(prompts),
                    ::testing::ElementsAre(FlashPromptKind::Begin, FlashPromptKind::ConfirmBdmKernelBootstrap));
        EXPECT_THAT(prompts, ::testing::Each(::testing::Field(&FlashPromptStep::arguments, ::testing::IsEmpty())));

        auto& attempt = std::get<FlashAttempt>(step);
        const FlashPlan& plan = attempt.attempt->plan();
        EXPECT_EQ(plan.family(), FlashFamily::SubaruDensoMc68hc16y5_02Bdm);
        EXPECT_EQ(plan.transport(), TransportKind::Kline);
        EXPECT_EQ(plan.operation(), FlashOperation::Write);
        EXPECT_EQ(plan.image(), std::optional<bytes::Bytes>(bdmCatalogKernelImage()));
        EXPECT_EQ(plan.transfer_region(), (MemoryRegion{0x20000, 0x20}));
        EXPECT_FALSE(plan.kernel().has_value());

        // transport_setup() validates the plan, so reaching the pre-configure
        // cancellation proves the BDM executor owns it.
        FakeCancellationToken cancelled(true);
        NullEventSink events;
        EXPECT_THAT(attempt.attempt->run(*attempt.clock, cancelled, events),
                    fastecu::testing::IsErrWith(ErrorKind::Cancelled, ::testing::HasSubstr("before configure")));
        EXPECT_FALSE(std::holds_alternative<FlashAttempt>(workflow->next()));

        if (succeeded)
        {
            workflow->submit(FlashAttemptResult{.success = true});
            step = workflow->next();
            ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
            EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::Succeeded);
            EXPECT_FALSE(std::get<FlashCompletedStep>(step).accepted_read_bytes.has_value());
        }
        else
        {
            workflow->submit(FlashAttemptResult{
                .success = false, .error_kind = ErrorKind::Disconnected, .error_detail = "adapter removed"});
            step = workflow->next();
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            EXPECT_EQ(std::get<FlashFailureStep>(step).error, (Error{ErrorKind::Disconnected, "adapter removed"}));
        }
    }
}

TEST(FlashWorkflowTest, mc68BdmWriteCancelsWhenEitherPromptIsDeclined)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    const std::vector<FlashPromptKind> sequence{FlashPromptKind::Begin, FlashPromptKind::ConfirmBdmKernelBootstrap};

    for (std::size_t declined = 0; declined < sequence.size(); ++declined)
    {
        SCOPED_TRACE(std::format("declining prompt {}", declined));
        auto workflow = FlashWorkflowFactory::tryCreate(bdmWrite(*paths));
        ASSERT_TRUE(workflow != nullptr);
        for (std::size_t accepted = 0; accepted < declined; ++accepted)
        {
            ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, sequence[accepted]);
            workflow->submit(FlashPromptResponse::Accept);
        }
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, sequence[declined]);
        workflow->submit(FlashPromptResponse::Decline);
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            const auto step = workflow->next();
            ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
            EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::Cancelled);
        }
    }
}

TEST(FlashWorkflowTest, mc68BdmWriteWithoutItsKernelFailsBeforeAnyPromptOrIo)
{
    QTemporaryDir without_kernels;
    ASSERT_TRUE(without_kernels.isValid());
    const auto catalog_only = catalogPaths(without_kernels, false);
    ASSERT_TRUE(catalog_only.has_value());
    FakeBackend *fake = nullptr;
    auto serial = recordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    expectNoBackendIo(*fake);

    for (const bool has_catalog : {true, false})
    {
        SCOPED_TRACE(has_catalog ? "with catalog" : "without catalog");
        auto input = bdmWrite(has_catalog ? *catalog_only : config::ConfigPaths{});
        input.serial = serial.get();
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr);
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            const auto step = workflow->next();
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            EXPECT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
        }
    }
}

TEST(FlashWorkflowTest, mc68BdmWriteKeepsItsKernelSnapshotAfterFilesAreRemoved)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto workflow = FlashWorkflowFactory::tryCreate(bdmWrite(*paths));
    ASSERT_TRUE(workflow != nullptr);

    // The kernel is loaded before Begin; nothing on disk is read afterward.
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    ASSERT_TRUE(QFile::remove(QString::fromStdString(paths->protocols_file)));
    ASSERT_TRUE(QDir(directory.filePath("kernels")).removeRecursively());
    workflow->submit(FlashPromptResponse::Accept);

    std::vector<FlashPromptStep> prompts;
    auto step = acceptEveryPrompt(*workflow, prompts);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << failureDetail(step);
    EXPECT_THAT(promptKinds(prompts), ::testing::ElementsAre(FlashPromptKind::ConfirmBdmKernelBootstrap));
    EXPECT_EQ(std::get<FlashAttempt>(step).attempt->plan().image(),
              std::optional<bytes::Bytes>(bdmCatalogKernelImage()));
}
```

- [ ] **Step 4: Run the workflow suite against the current code**

Run: `bazel test --config=release //src/platform/desktop/common/flash:test_flash_workflow --test_output=errors`
Expected: PASS. If a new test fails, it misdescribes today's behavior. Read the hand-written workflow (`SubaruHitachiSh7058Workflow` or `SubaruDensoMc68hc16y5_02BdmWorkflow` in `flash_workflow.cpp`) and fix the test.

- [ ] **Step 5: Commit**

```bash
git add src/platform/desktop/common/flash/flash_workflow_test.cpp
git commit -F - <<'EOF'
test: characterize the SH7058 and BDM flash workflows

Adds the Read side of both families to the single-attempt suite and pins
the SH7058 CAN Write and the BDM kernel-bootstrap Write: prompts, the bound
executor, declines, outcomes, pre-Begin kernel failures and the kernel
snapshot. Passes against the hand-written workflows.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01UBrg45gykzFdMmAy34bN11
EOF
```

---

### Task 2: Plan-owned confirmations

**Files:**
- Modify: `src/backend/flash/flash_types.h` (enum `ConfirmationSpec::Id`, near line 118)
- Modify: `src/backend/flash/ecu/subaru_hitachi_sh7058_plan.cpp`
- Modify: `src/backend/flash/ecu/subaru_hitachi_sh7058_plan_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel` (Gazelle output only)
- Modify: `src/platform/desktop/common/flash/flash_workflow.cpp` (`confirmationPrompt` only)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `ConfirmationSpec::Id::StartKlineRead` and `ConfirmationSpec::Id::KernelBootstrap`. A SH7058 Read plan carries exactly `{StartKlineRead}` and a Write plan carries `{}`. A BDM Write plan carries exactly `{KernelBootstrap}` and a Read plan carries `{}`. Each id has no arguments. Task 3 maps both ids to prompts.

- [ ] **Step 1: Add the ids**

In `src/backend/flash/flash_types.h`, insert after `ApplyBootModeVoltages,` inside `enum class Id`:

```cpp
        // Same contract as the four above. Hitachi SH7058 Read: the operator
        // confirmed opening the adapter and starting the K-Line ROM read.
        StartKlineRead,
        // Same contract. Denso MC68HC16Y5 BDM Write: the operator confirmed
        // uploading the kernel into RAM and starting it; the ROM is not
        // written.
        KernelBootstrap,
```

In `src/platform/desktop/common/flash/flash_workflow.cpp`, inside `confirmationPrompt`, extend the unsupported arm so the switch stays exhaustive. The `clang-diagnostic-switch` check in the blocking clang-tidy gate fails a non-exhaustive one:

```cpp
    case BeginEepromRead:
    case InspectEepromBytes:
    case ApplyProgrammingVoltage:
    case ApplyBootModeVoltages:
    case StartKlineRead:
    case KernelBootstrap:
        break;
```

This is behavior-neutral. The hand-written SH7058 and BDM workflows don't call `promptSequence`, and no other plan carries the new ids.

- [ ] **Step 2: Write the failing SH7058 plan tests**

In `src/backend/flash/ecu/subaru_hitachi_sh7058_plan_test.cpp`, add an anonymous namespace after `namespace fastecu::flash {`:

```cpp
namespace
{
FlashPlanFields read_fields()
{
    return FlashPlanFields{
        .operation = FlashOperation::Read,
        .family = FlashFamily::SubaruHitachiSh7058,
        .transport = TransportKind::Kline,
        .target_id = "sub_ecu_hitachi_sh7058_can",
        .mcu_name = "SH7058_1block",
        .transfer_region = {0x100000, 0x100000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruHitachiSh7058KlinePlan{},
        .confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::StartKlineRead}},
    };
}

FlashPlanFields write_fields()
{
    return FlashPlanFields{
        .operation = FlashOperation::Write,
        .family = FlashFamily::SubaruHitachiSh7058,
        .transport = TransportKind::CanIso15765,
        .target_id = "sub_ecu_hitachi_sh7058_can",
        .mcu_name = "SH7058_1block",
        .transfer_region = {0, 0x100000},
        .erase_regions = {{0, 0x100000}},
        .image = bytes::Bytes(0x100000),
        .kernel = std::nullopt,
        .family_plan = SubaruHitachiSh7058CanPlan{},
        .confirmations = {},
    };
}
} // namespace
```

Replace the body of `RejectsForgedTransportAndWireParameters` so it starts from the valid fixture. Its subject stays the wire parameters:

```cpp
TEST(SubaruHitachiSh7058Plan, RejectsForgedTransportAndWireParameters)
{
    auto fields = read_fields();
    fields.family_plan = SubaruHitachiSh7058KlinePlan{.initial_baud = 9600};
    auto forged = validate_and_build(fields);
    ASSERT_TRUE(forged.has_value());
    EXPECT_FALSE(validate_subaru_hitachi_sh7058_plan(*forged).has_value());
    fields.transport = TransportKind::CanIso15765;
    EXPECT_FALSE(validate_and_build(fields).has_value());
}
```

Append these tests:

```cpp
TEST(SubaruHitachiSh7058Plan, ReadCarriesTheStartKlineReadConsentAndWriteNone)
{
    auto read = build_subaru_hitachi_sh7058_plan(FlashOperation::Read, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                 std::nullopt);
    ASSERT_TRUE(read.has_value());
    ASSERT_EQ(read->confirmations().size(), 1U);
    EXPECT_EQ(read->confirmations().front().id, ConfirmationSpec::Id::StartKlineRead);
    EXPECT_TRUE(read->confirmations().front().arguments.empty());

    auto write = build_subaru_hitachi_sh7058_plan(FlashOperation::Write, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                  bytes::Bytes(0x100000));
    ASSERT_TRUE(write.has_value());
    EXPECT_TRUE(write->confirmations().empty());
}

TEST(SubaruHitachiSh7058Plan, ValidatorAcceptsTheBuilderShapes)
{
    for (auto make : {read_fields, write_fields})
    {
        auto plan = validate_and_build(make());
        ASSERT_TRUE(plan.has_value());
        EXPECT_TRUE(validate_subaru_hitachi_sh7058_plan(*plan).has_value());
    }
}

TEST(SubaruHitachiSh7058Plan, ValidatorRequiresExactlyTheReadConsent)
{
    const ConfirmationSpec start_read{.id = ConfirmationSpec::Id::StartKlineRead};
    struct Case
    {
        const char *name;
        FlashOperation operation;
        std::vector<ConfirmationSpec> confirmations;
    };
    const std::vector<Case> cases{
        {"read without consent", FlashOperation::Read, {}},
        {"read with another id", FlashOperation::Read, {ConfirmationSpec{.id = ConfirmationSpec::Id::KernelBootstrap}}},
        {"read consent with arguments",
         FlashOperation::Read,
         {ConfirmationSpec{.id = ConfirmationSpec::Id::StartKlineRead, .arguments = {{"unexpected", "argument"}}}}},
        {"read with an extra consent",
         FlashOperation::Read,
         {start_read, ConfirmationSpec{.id = ConfirmationSpec::Id::CycleIgnition}}},
        {"write with the read consent", FlashOperation::Write, {start_read}},
    };
    for (const Case& test : cases)
    {
        auto fields = test.operation == FlashOperation::Read ? read_fields() : write_fields();
        fields.confirmations = test.confirmations;
        auto plan = validate_and_build(std::move(fields));
        ASSERT_TRUE(plan.has_value()) << test.name;
        EXPECT_FALSE(validate_subaru_hitachi_sh7058_plan(*plan).has_value()) << test.name;
    }
}
```

Add `#include <vector>` to the test's includes if clang-tidy's `misc-include-cleaner` asks for it.

- [ ] **Step 3: Write the failing BDM plan and executor tests**

In `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan_test.cpp`, make `write_fields` carry the consent:

```cpp
FlashPlanFields write_fields(std::uint32_t size)
{
    FlashPlanFields fields = read_fields();
    fields.operation = FlashOperation::Write;
    fields.transfer_region = MemoryRegion{0x20000, size};
    fields.image = bytes::Bytes(size, 0xab);
    fields.confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::KernelBootstrap}};
    return fields;
}
```

In `ValidatorRejectsHandBuiltShapes`, add these cases before the `for (auto& test_case : cases)` loop:

```cpp
    {
        auto fields = write_fields(0x20);
        fields.confirmations = {};
        cases.push_back({"write without consent", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = write_fields(0x20);
        fields.confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::StartKlineRead}};
        cases.push_back({"write with another id", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = write_fields(0x20);
        fields.confirmations = {
            ConfirmationSpec{.id = ConfirmationSpec::Id::KernelBootstrap, .arguments = {{"unexpected", "argument"}}}};
        cases.push_back({"consent with arguments", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = write_fields(0x20);
        fields.confirmations.push_back(ConfirmationSpec{.id = ConfirmationSpec::Id::CycleIgnition});
        cases.push_back({"write with an extra consent", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = read_fields();
        fields.confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::KernelBootstrap}};
        cases.push_back({"read with the bootstrap consent", std::move(fields), ErrorKind::InvalidConfig});
    }
```

Append:

```cpp
TEST(SubaruDensoMc68hc16y5_02BdmPlan, WriteCarriesTheKernelBootstrapConsentAndReadNone)
{
    const auto read =
        build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt, std::nullopt);
    ASSERT_THAT(read, IsOk());
    EXPECT_TRUE(read->confirmations().empty());

    const auto write = build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt,
                                                                 kernel(bytes::Bytes(0x20, 0x01)));
    ASSERT_THAT(write, IsOk());
    ASSERT_EQ(write->confirmations().size(), 1U);
    EXPECT_EQ(write->confirmations().front().id, ConfirmationSpec::Id::KernelBootstrap);
    EXPECT_TRUE(write->confirmations().front().arguments.empty());
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, ValidatorAcceptsTheBuilderShapes)
{
    for (auto fields : {read_fields(), write_fields(0x20)})
    {
        auto plan = validate_and_build(std::move(fields));
        ASSERT_THAT(plan, IsOk());
        EXPECT_THAT(validate_subaru_denso_mc68hc16y5_02_bdm_plan(*plan), IsOk());
    }
}
```

In `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor_test.cpp`, add `#include "src/backend/flash/flash_validation.h"` next to the other `src/backend/flash/` includes. Then add after `TransportSetupIs115200BaudPlainSerial`:

```cpp
// The bootstrap consent is enforced past the builder: a Write plan assembled
// without it never reaches the adapter.
TEST(SubaruDensoMc68hc16y5_02BdmExecutor, TransportSetupRejectsAWritePlanWithoutTheBootstrapConsent)
{
    auto plan = validate_and_build(FlashPlanFields{
        .operation = FlashOperation::Write,
        .family = FlashFamily::SubaruDensoMc68hc16y5_02Bdm,
        .transport = TransportKind::Kline,
        .target_id = std::string(kProtocol),
        .mcu_name = std::string(kMcu),
        .transfer_region = MemoryRegion{0x20000, 0x20},
        .erase_regions = {},
        .image = bytes::Bytes(0x20, 0xab),
        .kernel = std::nullopt,
        .family_plan = SubaruDensoMc68hc16y5_02BdmPlan{.baud = 115200},
        .confirmations = {},
    });
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(SubaruDensoMc68hc16y5_02BdmExecutor{}.transport_setup(*plan), IsErr(ErrorKind::InvalidConfig));
}
```

Then update the BUILD deps:

Run: `python3 scripts/gazelle_check.py --fix`
Expected: `src/backend/flash/ecu/BUILD.bazel` gains `"//src/backend/flash:flash_validation"` in `subaru_denso_mc68hc16y5_02_bdm_executor_test`'s deps, and nothing else changes.

- [ ] **Step 4: Run the backend tests to verify they fail**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_hitachi_sh7058_plan_test //src/backend/flash/ecu:subaru_denso_mc68hc16y5_02_bdm_plan_test //src/backend/flash/ecu:subaru_denso_mc68hc16y5_02_bdm_executor_test --test_output=errors`
Expected: FAIL in:
- `ReadCarriesTheStartKlineReadConsentAndWriteNone`
- `ValidatorAcceptsTheBuilderShapes` (both families)
- `ValidatorRequiresExactlyTheReadConsent` (the "read without consent" case)
- `WriteCarriesTheKernelBootstrapConsentAndReadNone`
- `ValidatorRejectsHandBuiltShapes` (the "write without consent" case)
- `TransportSetupRejectsAWritePlanWithoutTheBootstrapConsent`

- [ ] **Step 5: Implement the SH7058 builder and validator**

In `src/backend/flash/ecu/subaru_hitachi_sh7058_plan.cpp`, `validate_subaru_hitachi_sh7058_plan`: in the final geometry condition, change the last line from

```cpp
        (read && !plan.erase_regions().empty()) || !plan.confirmations().empty())
```

to

```cpp
        (read && !plan.erase_regions().empty()))
```

and insert before the closing `return {};`:

```cpp
    const auto confirmations = plan.confirmations();
    const bool exactly_start_read = confirmations.size() == 1 &&
                                    confirmations.front().id == ConfirmationSpec::Id::StartKlineRead &&
                                    confirmations.front().arguments.empty();
    if (read ? !exactly_start_read : !confirmations.empty())
    {
        return fail(ErrorKind::InvalidConfig,
                    "SH7058 Read requires exactly the StartKlineRead confirmation; Write requires none");
    }
```

In `build_subaru_hitachi_sh7058_plan`, replace `.confirmations = {},` with:

```cpp
        .confirmations = read ? std::vector<ConfirmationSpec>{ConfirmationSpec{.id = ConfirmationSpec::Id::StartKlineRead}}
                              : std::vector<ConfirmationSpec>{},
```

- [ ] **Step 6: Implement the BDM builder and validator**

In `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.cpp`, `validate_subaru_denso_mc68hc16y5_02_bdm_plan`: change

```cpp
    if (!plan.erase_regions().empty() || plan.kernel().has_value() || !plan.confirmations().empty())
    {
        return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM plan shape is invalid");
    }
```

to

```cpp
    if (!plan.erase_regions().empty() || plan.kernel().has_value())
    {
        return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM plan shape is invalid");
    }
    const auto confirmations = plan.confirmations();
    const bool exactly_bootstrap = confirmations.size() == 1 &&
                                   confirmations.front().id == ConfirmationSpec::Id::KernelBootstrap &&
                                   confirmations.front().arguments.empty();
    if (plan.operation() == FlashOperation::Write ? !exactly_bootstrap : !confirmations.empty())
    {
        return fail(ErrorKind::InvalidConfig,
                    "MC68HC16Y5 BDM Write requires exactly the KernelBootstrap confirmation; Read requires none");
    }
```

In `build_subaru_denso_mc68hc16y5_02_bdm_plan`, replace `.confirmations = {},` with:

```cpp
        .confirmations = operation == FlashOperation::Write
                             ? std::vector<ConfirmationSpec>{ConfirmationSpec{.id = ConfirmationSpec::Id::KernelBootstrap}}
                             : std::vector<ConfirmationSpec>{},
```

- [ ] **Step 7: Run the backend, executor and workflow tests to verify they pass**

Run: `bazel test --config=release //src/backend/flash/... //src/platform/desktop/common/flash:all --test_output=errors`
Expected: PASS. In particular:
- The SH7058 K-Line and CAN executor tests and the BDM executor tests still pass, because they build their plans through the builders.
- `test_flash_workflow` still passes unchanged, because the hand-written workflows ignore plan confirmations.

- [ ] **Step 8: Commit**

```bash
git add src/backend/flash/flash_types.h src/backend/flash/ecu/ src/platform/desktop/common/flash/flash_workflow.cpp
git commit -F - <<'EOF'
feat(flash): carry the SH7058 read and BDM bootstrap consents in their plans

Hitachi SH7058 Read plans now carry StartKlineRead and Denso MC68HC16Y5 BDM
Write plans KernelBootstrap, pinned by their validators like SH7055_02's
CycleIgnition. Both executors already re-validate in transport_setup, so a
plan without its consent is rejected before configure. The desktop workflows
still ask both prompts themselves; the next commit moves them onto the plans.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01UBrg45gykzFdMmAy34bN11
EOF
```

---

### Task 3: Join SingleAttemptFlashWorkflow

The tests come from Task 1 and must pass **without modification**; this task writes no new tests. It finishes with mutation checks that prove those tests detect the breakage this refactor could cause.

**Files:**
- Modify: `src/platform/desktop/common/flash/flash_workflow.cpp`
- Modify: `docs/tech-debt.md` (P1 "Consolidate flash workflow orchestration" actions)
- Modify: `docs/design-notes.md` (rule 1 under "Operator steps never block an executor"; "Keep `FlashOperation` to Read, TestWrite and Write")

**Interfaces:**
- Consumes: `ConfirmationSpec::Id::StartKlineRead` and `ConfirmationSpec::Id::KernelBootstrap` (Task 2). Also the existing `KernelFreeKlineWorkflow`, `KernelFreeCanWorkflow`, `SingleAttemptFlashWorkflow<Executor, Transport, Preparation>`, `resolveKernel(const FlashWorkflowRequest&, IFileRepository&)` and `QtFileRepository`.
- Produces: the aliases `SubaruHitachiSh7058KlineWorkflow`, `SubaruHitachiSh7058CanWorkflow` and `SubaruDensoMc68hc16y5_02BdmWorkflow`, and `class BdmKernelPlan`, a preparation policy shaped like `Mc68KernelPlan`. Task 4 folds `BdmKernelPlan`.

- [ ] **Step 1: Map the confirmations to their prompts**

In `confirmationPrompt`, add two cases after `TopRegionBootstrap`'s:

```cpp
    case StartKlineRead:
        return FlashPromptStep{FlashPromptKind::ConfirmSh7058Read, confirmation.arguments};
    case KernelBootstrap:
        return FlashPromptStep{FlashPromptKind::ConfirmBdmKernelBootstrap, confirmation.arguments};
```

and remove `case StartKlineRead:` and `case KernelBootstrap:` from the unsupported arm Task 2 added.

- [ ] **Step 2: Delete both hand-written workflows**

Delete `class SubaruDensoMc68hc16y5_02BdmWorkflow final : public FlashWorkflow { ... };` and `class SubaruHitachiSh7058Workflow final : public FlashWorkflow { ... };`. Both sit just above `confirmationPrompt`.

- [ ] **Step 3: Add the BDM preparation policy**

Insert directly after `class Mc68KernelPlan { ... };`:

```cpp
// Plan preparation for MC68HC16Y5 BDM: CachedKernelPlan's timing and
// snapshot, but only Write reads the catalog -- its "write" uploads and
// starts the cfg kernel. The operator's ROM (request.image) is never
// forwarded: BDM never writes the ROM.
class BdmKernelPlan
{
  public:
    explicit BdmKernelPlan(const FlashWorkflowRequest&)
    {
    }

    Result<FlashPlan>& plan(FlashWorkflowRequest& request)
    {
        if (!plan_.has_value())
        {
            plan_ = prepare(request);
        }
        return *plan_;
    }

  private:
    static Result<FlashPlan> prepare(const FlashWorkflowRequest& request)
    {
        if (request.operation != FlashOperation::Write)
        {
            return build_subaru_denso_mc68hc16y5_02_bdm_plan(request.operation, request.protocol, request.mcu,
                                                             std::nullopt, std::nullopt);
        }
        QtFileRepository repository;
        Result<KernelImage> kernel = resolveKernel(request, repository);
        if (!kernel.has_value())
        {
            return std::unexpected(kernel.error());
        }
        return build_subaru_denso_mc68hc16y5_02_bdm_plan(request.operation, request.protocol, request.mcu,
                                                         std::nullopt, std::move(*kernel));
    }

    std::optional<Result<FlashPlan>> plan_;
};
```

- [ ] **Step 4: Add the aliases**

At the end of the alias block, after `using SubaruDensoMc68hc16y5_02Workflow = ...;`, add:

```cpp
// Hitachi SH7058 Read runs over K-Line and Write over CAN: the plan builder
// chooses the transport by operation and the factory the matching executor.
// Read plans carry StartKlineRead; write plans carry nothing.
using SubaruHitachiSh7058KlineWorkflow =
    KernelFreeKlineWorkflow<SubaruHitachiSh7058KlineExecutor, &build_subaru_hitachi_sh7058_plan>;
using SubaruHitachiSh7058CanWorkflow =
    KernelFreeCanWorkflow<SubaruHitachiSh7058CanExecutor, &build_subaru_hitachi_sh7058_plan>;
// Write plans carry KernelBootstrap; read plans carry nothing.
using SubaruDensoMc68hc16y5_02BdmWorkflow =
    SingleAttemptFlashWorkflow<SubaruDensoMc68hc16y5_02BdmExecutor, DesktopKlineFlashTransport, BdmKernelPlan>;
```

- [ ] **Step 5: Route SH7058 by operation**

In `FlashWorkflowFactory::tryCreate`, replace

```cpp
    case SubaruHitachiSh7058:
        return std::make_unique<SubaruHitachiSh7058Workflow>(std::move(request));
```

with

```cpp
    case SubaruHitachiSh7058:
        if (request.operation == FlashOperation::Read)
        {
            return std::make_unique<SubaruHitachiSh7058KlineWorkflow>(std::move(request));
        }
        return std::make_unique<SubaruHitachiSh7058CanWorkflow>(std::move(request));
```

The `SubaruDensoMc68hc16y5_02Bdm` case is unchanged; it now instantiates the alias.

- [ ] **Step 6: Run the workflow suite**

Run: `bazel test --config=release //src/platform/desktop/common/flash:all --test_output=errors`
Expected: PASS with `flash_workflow_test.cpp` unmodified. That includes the pre-existing `routesSh7058ReadAndWriteWithPreTransportPrompts` and all the `mc68Bdm*` tests. Confirm with `git diff --stat -- src/platform/desktop/common/flash/flash_workflow_test.cpp`, which must print nothing.

- [ ] **Step 7: Update the roadmap and design notes**

In `docs/tech-debt.md`, under "P1: Consolidate flash workflow orchestration" → "Actions:", delete the whole first bullet. It begins `- Decide whether the last two hand-rolled single-attempt workflows in` and ends `family's extra step is genuinely shared.`. Then replace the next bullet

```markdown
- Treat the multi-stage workflows as separate work, not part of that
  consolidation: the EEPROM workflow's ignition-cycle and inspect-read
  re-attempts, the Unisia Jecs M32R boot-mode workflow's staged attempts, and
  the programming-voltage apply/remove notices.
```

with

```markdown
- Treat the multi-stage workflows as separate work from
  `SingleAttemptFlashWorkflow`, which now runs every single-attempt family:
  the EEPROM workflow's ignition-cycle and inspect-read re-attempts, the
  Unisia Jecs M32R boot-mode workflow's staged attempts, and the
  programming-voltage apply/remove notices.
```

In `docs/design-notes.md`, rule 1 under "Operator steps never block an executor", replace

```markdown
1. **Permission before starting** is a `ConfirmationSpec` collected up front;
   its presence in the plan means "granted" (the Unisia Jecs M32R "apply VPP"
   prompt).
```

with

```markdown
1. **Permission before starting** is a `ConfirmationSpec` collected up front;
   its presence in the plan means "granted" (the Unisia Jecs M32R "apply VPP"
   prompt, the Denso MC68HC16Y5 BDM kernel bootstrap, the Hitachi SH7058
   K-Line read).
```

In "Keep `FlashOperation` to Read, TestWrite and Write", replace the last line

```markdown
the operator with a dedicated confirmation prompt.
```

with

```markdown
the operator with a dedicated `ConfirmationSpec` (BDM's `KernelBootstrap`).
```

- [ ] **Step 8: Run the full suite**

Run: `bazel test --config=release //...`
Expected: PASS.

- [ ] **Step 9: Commit**

```bash
git add src/platform/desktop/common/flash/flash_workflow.cpp docs/tech-debt.md docs/design-notes.md
git commit -F - <<'EOF'
refactor: run Hitachi SH7058 and Denso MC68HC16Y5 BDM through SingleAttemptFlashWorkflow

The SH7058 read and BDM kernel-bootstrap prompts now come from their plans'
confirmations, so both hand-written workflows are deleted. The factory binds
the SH7058 K-Line executor for Read and the CAN executor otherwise; BDM gets a
preparation policy that reads the catalog only for Write and never forwards
the operator's ROM. Prompt kinds, order and kernel timing are unchanged; the
characterization tests pass without modification.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01UBrg45gykzFdMmAy34bN11
EOF
```

- [ ] **Step 10: Mutation checks (no commit)**

Make each edit below to `src/platform/desktop/common/flash/flash_workflow.cpp` alone. Run `bazel test --config=release //src/platform/desktop/common/flash:test_flash_workflow --test_output=errors` and confirm it FAILS, naming the listed tests. Then restore with `git checkout -- src/platform/desktop/common/flash/flash_workflow.cpp`, which is safe because Step 9 committed everything.

1. Swap the two SH7058 aliases in the factory case: Read → `SubaruHitachiSh7058CanWorkflow`, otherwise → `SubaruHitachiSh7058KlineWorkflow`. Expected failures: `singleAttemptFamiliesPromptInOrderAndBindTheirExecutorOnce` (SH7058 row: `Unsupported`, not `Cancelled`) and `sh7058WriteBindsTheCanExecutorAfterBeginAlone`.
2. Map `StartKlineRead` to `FlashPromptKind::Begin`. Expected failures: `singleAttemptFamiliesPromptInOrderAndBindTheirExecutorOnce` and `routesSh7058ReadAndWriteWithPreTransportPrompts`.
3. Map `KernelBootstrap` to `FlashPromptKind::Begin`. Expected failures: `mc68BdmWriteBindsItsExecutorAndReportsEveryOutcome` and `mc68BdmWriteBootstrapsTheCatalogKernelNotTheRom`.
4. In `BdmKernelPlan::prepare`, delete the `if (request.operation != FlashOperation::Write) { ... }` block so every operation reads the catalog. Expected failures: `mc68BdmReadRoutesThroughBeginToAttempt` and `mc68BdmTestWriteFailsBeforeAnyPrompt`.

If any mutation leaves the suite green, stop and report it: the characterization has a gap. Finish with `git status --short`, which must print nothing.

---

### Task 4: Fold the lazy preparation policies into LazyPlan

No behavior change. The kernel-backed and MC68 suites from #455 and #460 plus Task 1's BDM tests cover it.

**Files:**
- Modify: `src/platform/desktop/common/flash/flash_workflow.cpp` (`CachedKernelPlan`, `Mc68KernelPlan`, `BdmKernelPlan`)

**Interfaces:**
- Consumes: `KernelBackedPlanBuilder`, `resolveKernel`, `normalizeMc68Image`, `QtFileRepository`, `build_subaru_denso_mc68hc16y5_02_plan` and `build_subaru_denso_mc68hc16y5_02_bdm_plan`.
- Produces: `using PlanPreparation = Result<FlashPlan> (*)(FlashWorkflowRequest&)`, `template <PlanPreparation Prepare> class LazyPlan`, and `CachedKernelPlan<Build>`, `Mc68KernelPlan` and `BdmKernelPlan`, which become aliases with unchanged names. Every alias that uses them compiles unchanged.

- [ ] **Step 1: Replace the three classes**

Delete `template <KernelBackedPlanBuilder Build> class CachedKernelPlan { ... };`, `class Mc68KernelPlan { ... };` and `class BdmKernelPlan { ... };`, with their leading comments. Put this in their place, after `EagerPlan`:

```cpp
using PlanPreparation = Result<FlashPlan> (*)(FlashWorkflowRequest&);

// Prepares the plan on the first next(), before Begin, and keeps the result:
// a preparation failure is reported before any prompt, and the attempt
// carries the snapshot taken then even if the files change afterward.
template <PlanPreparation Prepare> class LazyPlan
{
  public:
    explicit LazyPlan(const FlashWorkflowRequest&)
    {
    }

    Result<FlashPlan>& plan(FlashWorkflowRequest& request)
    {
        if (!plan_.has_value())
        {
            plan_ = Prepare(request);
        }
        return *plan_;
    }

  private:
    std::optional<Result<FlashPlan>> plan_;
};

// A kernel-backed family: the catalog and kernel file are read, then the
// family builder runs.
template <KernelBackedPlanBuilder Build> Result<FlashPlan> prepareKernelBacked(FlashWorkflowRequest& request)
{
    QtFileRepository repository;
    Result<KernelImage> kernel = resolveKernel(request, repository);
    if (!kernel.has_value())
    {
        return std::unexpected(kernel.error());
    }
    return Build(request.operation, request.protocol, request.mcu, std::move(request.image), std::move(*kernel));
}

// MC68HC16Y5_02: prepareKernelBacked with two family steps before the kernel
// is read.
Result<FlashPlan> prepareMc68(FlashWorkflowRequest& request)
{
    // Desktop FullRomData is physically addressed after the legacy
    // calibration adapter inserts the 0x20000-0x27fff RAM/kernel hole.
    // Portable MC plans and executors use the packed flash-block image.
    request.image = normalizeMc68Image(std::move(request.image), request.mcu);
    // Run the family builder first so recognized-but-unsupported
    // revision 04 is rejected by the plan even without a catalog.
    Result<FlashPlan> preflight = build_subaru_denso_mc68hc16y5_02_plan(
        request.operation, request.protocol, request.mcu, request.image,
        KernelImage{.id = request.protocol + "-kernel", .load_address = 0x20000, .bytes = {0}});
    if (!preflight.has_value())
    {
        return std::unexpected(preflight.error());
    }
    QtFileRepository repository;
    Result<KernelImage> kernel = resolveKernel(request, repository);
    if (!kernel.has_value())
    {
        return std::unexpected(kernel.error());
    }
    return build_subaru_denso_mc68hc16y5_02_plan(request.operation, request.protocol, request.mcu,
                                                 std::move(request.image), std::move(*kernel));
}

// MC68HC16Y5 BDM: only Write reads the catalog -- its "write" uploads and
// starts the cfg kernel. The operator's ROM (request.image) is never
// forwarded: BDM never writes the ROM.
Result<FlashPlan> prepareBdm(FlashWorkflowRequest& request)
{
    if (request.operation != FlashOperation::Write)
    {
        return build_subaru_denso_mc68hc16y5_02_bdm_plan(request.operation, request.protocol, request.mcu,
                                                         std::nullopt, std::nullopt);
    }
    QtFileRepository repository;
    Result<KernelImage> kernel = resolveKernel(request, repository);
    if (!kernel.has_value())
    {
        return std::unexpected(kernel.error());
    }
    return build_subaru_denso_mc68hc16y5_02_bdm_plan(request.operation, request.protocol, request.mcu, std::nullopt,
                                                     std::move(*kernel));
}

template <KernelBackedPlanBuilder Build> using CachedKernelPlan = LazyPlan<&prepareKernelBacked<Build>>;
using Mc68KernelPlan = LazyPlan<&prepareMc68>;
using BdmKernelPlan = LazyPlan<&prepareBdm>;
```

`prepareMc68`'s body must match the deleted `Mc68KernelPlan::prepare` exactly. Diff it against `git show HEAD:src/platform/desktop/common/flash/flash_workflow.cpp` before moving on.

If clang-tidy flags `prepareBdm`'s parameter as one that could be const, keep it non-const and don't suppress anything: every preparation must have the `PlanPreparation` signature, and `prepareMc68` and `prepareKernelBacked` mutate the request.

- [ ] **Step 2: Run the workflow suite**

Run: `bazel test --config=release //src/platform/desktop/common/flash:all --test_output=errors`
Expected: PASS, with `flash_workflow_test.cpp` unchanged since Task 1.

- [ ] **Step 3: Commit**

```bash
git add src/platform/desktop/common/flash/flash_workflow.cpp
git commit -F - <<'EOF'
refactor: fold the lazy flash plan preparations into LazyPlan

CachedKernelPlan, Mc68KernelPlan and BdmKernelPlan repeated the same
prepare-on-first-next() cache. One LazyPlan template now holds it; each
family supplies only its preparation function. No behavior change.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01UBrg45gykzFdMmAy34bN11
EOF
```

---

### Task 5: Verify the branch and drop the working documents

**Files:**
- Delete: `docs/superpowers/specs/2026-10-01-sh7058-bdm-single-attempt-design.md`
- Delete: `docs/superpowers/plans/2026-10-01-sh7058-bdm-single-attempt.md`

- [ ] **Step 1: Run every gate**

Run each command; each must succeed:

```bash
bazel test --config=release //...
prek run --all-files
bazel run //:clang_tidy_report_changed
python3 scripts/gazelle_check.py
```

Expected: all pass, `gazelle_check.py` reports no stale BUILD file, and the clang-tidy report shows no findings in the changed files.

- [ ] **Step 2: Run the portable-core gate**

The backend changed, so run `scripts/android-cross-compile.sh`. It needs `ANDROID_NDK_HOME`. If that variable is unset, don't skip silently: tell the user the local run was not possible and that the CI job `android-cross-compile` will run it on the PR.
Expected: PASS.

- [ ] **Step 3: Drop the spec and plan, as in the earlier consolidation PRs**

```bash
git rm docs/superpowers/specs/2026-10-01-sh7058-bdm-single-attempt-design.md docs/superpowers/plans/2026-10-01-sh7058-bdm-single-attempt.md
git commit -F - <<'EOF'
docs: drop the design spec and plan from the change

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01UBrg45gykzFdMmAy34bN11
EOF
```

- [ ] **Step 4: Report**

Report the branch's commits (`git log --oneline master..HEAD`), the result of each gate, the mutation-check results from Task 3, and anything skipped. Do not push or open a PR until the user says so.
