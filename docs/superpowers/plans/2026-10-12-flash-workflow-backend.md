# Flash Workflow Backend Relocation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver PR 2: relocate flash staging policy and its tests into the portable backend, preserving current behavior and desktop composition.

**Architecture:** PR 1 already supplies owned file/transport ports and removes clocks from workflow steps. First add the portable scripted source and kernel-handle helper, then convert the existing tests while they remain in their current package. Finally move the policy and tests together; desktop port composition and the bundled-kernel capability test stay on the desktop.

**Tech Stack:** C++23, GoogleTest/Google Mock, Bazel pinned by `.bazelversion`, gazelle_cc; Qt 6 only in remaining desktop consumers.

**Spec:** [Flash workflow backend spec, PR 2](../specs/2026-10-12-flash-workflow-backend.md#pr-2--relocation). Read the whole spec for the port and ownership contracts.

## Global Constraints

- “Behaviour: no change to step order, prompts, routing, plan building or wire behaviour.”
- “Structure: each staging shape stays its own class, and operator decisions stay between bounded attempts, outside executors.”
- “Ownership. The workflow owns its ports, so their lifetime equals the workflow's and they cannot dangle.”
- “TryCreate contract. A null result still means only "no portable family owns this protocol".”
- “The transports it returns are unopened. The attempt configures, opens and closes them exactly as it does today (ADR 0015).”
- “For each kind it returns a fresh default scripted transport, unless the test pre-loaded a transport for that kind.”
- “It records the requested kinds in order.”
- “The VPP answer is a constructor flag, default false.”
- “PortableImageForOperation moves with the header. gazelle_cc generates the BUILD file.”
- “builtin_catalog_capability_test stays in the platform layer because it reads the real bundled kernel files through QtFileRepository.”
- “New coverage: new workflow-plus-executor integration tests against scripted transports.” remains out of scope; preserve existing attempt runs, but add no wire exchange scripts here.
- Backend stays Qt-free, thread-free and free of direct filesystem I/O. Use package-owned test support, existing `ErrorKind` values and target visibility groups. Fix warnings at their cause.

## Review Focus

- Multiple requests of the same transport kind must consume preloaded transports in FIFO order, then return fresh defaults; requests of another kind cannot consume that queue (Task 1).
- Removing or changing a kernel after preflight must leave the bound plan's snapshot unchanged and must not cause another read (Task 3).
- Invalid configuration or unsupported operations must return a workflow with a failure step before transport configuration/opening or wire I/O (Task 3).
- Adapter-supplied VPP must skip external-VPP prompts on success but preserve the failed/cancelled write recovery notices (Task 3).
- Bootmode must request exactly one fresh K-Line transport per attempt, with the second attempt behind the MOD1 confirmation; declining that confirmation must prevent programming (Task 3).

---

## Baseline, scope and file map

The checkout already has `IFlashTransportSource`, `DesktopFlashTransportSource`, `FlashWorkflowPorts`, `DesktopFlashWorkflowPorts` and the clock-factory changes. Do not repeat PR 1. The referenced line 162 begins PR 2, a single relocation project; the supporting fake and path helper are independently testable tasks within it.

Two differences from the spec's historical inventory matter:

- The suite has additional `FakeBackend` users for VPP and no-I/O preflight assertions. Convert all of them, not only the three named CAN binding tests. `routesSh7058ReadAndWriteWithPreTransportPrompts` currently checks plan transport kinds without serial expectations: retain that distinction and strengthen it with source observations.
- `builtin_catalog` explicitly lists the old flash package as a one-off consumer. Add the new workflow package alongside it because the platform capability test remains. Do not replace or widen a layer group.

| Files | Responsibility/change |
| --- | --- |
| `src/backend/flash/testing/scripted_flash_transport_source.h`, `scripted_flash_transport_source_test.cpp`, `BUILD.bazel` | New package-owned source fake, ordered request trace and per-kind preload queues |
| `src/backend/config/config_paths.h`, `config_paths.cpp`, `config_paths_test.cpp` | Pure kernel-handle joining contract |
| `src/backend/flash/eeprom/eeprom_read_plan.cpp` | Use the same handle helper for EEPROM preparation |
| `src/platform/desktop/common/flash/flash_workflow_test.cpp` | Convert existing fixtures/assertions before moving; keep tests in one file as the spec requires |
| `src/platform/desktop/common/flash/flash_workflow.h`, `flash_workflow.cpp`, `flash_workflow_test.cpp` → `src/backend/flash/workflow/` | Move unchanged public interfaces and staging classes; move `PortableImageForOperation` too |
| `src/backend/flash/workflow/BUILD.bazel` | New portable package; generated library/test dependencies |
| `src/platform/desktop/common/flash/desktop_flash_workflow_ports.h`, `builtin_catalog_capability_test.cpp`, `BUILD.bazel` | Keep desktop composition and real bundled-kernel coverage; point them at backend policy |
| `src/ui/desktop/flash/common/flash_dialog.h`, `BUILD.bazel`; `src/ui/desktop/flash/operation/flash_operation_controller.cpp`, `BUILD.bazel`; `src/ui/desktop/widgets/mainwindow.cpp`, `BUILD.bazel` | Update includes and generated dependencies |
| `src/backend/config/BUILD.bazel` | Explicit built-in catalog visibility for relocated test consumer |
| `src/backend/flash/flash_types.h`, `CONTEXT.md`; `docs/design-notes.md`, `docs/tech-debt.md` | Fix confirmation comment, define transport source and document completed ownership move |

Before execution, use a feature branch (suggested `refactor/flash-workflow-backend`) and the using-git-worktrees skill to obtain execution isolation. Preserve unrelated untracked `docs/superpowers` artifacts. Read [coding conventions](../../coding-style.md#tests), [ADR 0015](../../adr/0015-caller-owns-flash-transport-lifetime.md), [ADR 0016](../../adr/0016-enforce-qt-reachability-by-visibility.md), [ADR 0017](../../adr/0017-generate-bazel-targets-with-gazelle.md) and the [qualification matrix](../../flash-qualification-matrix.md). This structural move changes no wire qualification claims.

### Task 1: A scripted flash transport source with ordered preloads

**Files:** Create the two source-fake files in the map; modify `src/backend/flash/testing/BUILD.bazel`.

**Interfaces:**
- Consumes: existing `IFlashTransportSource`, `ScriptedKlineFlashTransport`, `ScriptedCanFlashTransport`, `ScriptedMixedCanFlashTransport`, and `TransportKind`.
- Produces, in namespace `fastecu::flash`, a header-only `ScriptedFlashTransportSource final : public IFlashTransportSource`:
  - `explicit ScriptedFlashTransportSource(bool supplies_programming_voltage = false);`
  - `void QueueKline(std::unique_ptr<ScriptedKlineFlashTransport> transport);`
  - `void QueueCan(std::unique_ptr<ScriptedCanFlashTransport> transport);`
  - `void QueueMixedCan(std::unique_ptr<ScriptedMixedCanFlashTransport> transport);`
  - Existing overrides `MakeKline()`, `MakeCan()`, `MakeMixedCan()`, `SuppliesProgrammingVoltage() const` with the port's exact return types.
  - `const std::vector<TransportKind>& RequestedKinds() const;`
- Trace vocabulary: `kKline`, `kCanIso15765`, `kCanRawIso15765` respectively. Preloads must be non-null and unopened (test fixture precondition); ownership transfers exactly once. Raw observers in tests remain valid only while the returned transport/attempt owns the object.

- [ ] **Step 1: Write failing GoogleTests in `scripted_flash_transport_source_test.cpp`**

Add `defaultsAreFreshAndUnopened`, `preloadsAreFifoAndIndependentAcrossKinds`, and `programmingVoltageIsAnIndependentConstructorFlag`. Use exact pointer identity checks for queued objects and keep returned owners alive while comparing default objects. Pin these assertions (with equivalent checks for all three kinds):

```cpp
ScriptedFlashTransportSource source;
auto first = std::make_unique<ScriptedCanFlashTransport>();
auto second = std::make_unique<ScriptedCanFlashTransport>();
auto* first_observer = first.get();
auto* second_observer = second.get();
source.QueueCan(std::move(first));
source.QueueCan(std::move(second));
auto kline = source.MakeKline();
auto can1 = source.MakeCan();
auto mixed = source.MakeMixedCan();
auto can2 = source.MakeCan();
auto can3 = source.MakeCan();
auto can4 = source.MakeCan();
EXPECT_EQ(can1.get(), first_observer);
EXPECT_EQ(can2.get(), second_observer);
ASSERT_NE(can3, nullptr);
ASSERT_NE(can4, nullptr);
EXPECT_NE(can3.get(), can4.get());
EXPECT_FALSE(static_cast<ScriptedCanFlashTransport*>(can3.get())->IsOpen());
EXPECT_THAT(source.RequestedKinds(), ::testing::ElementsAre(
    TransportKind::kKline, TransportKind::kCanIso15765,
    TransportKind::kCanRawIso15765, TransportKind::kCanIso15765,
    TransportKind::kCanIso15765, TransportKind::kCanIso15765));
EXPECT_FALSE(source.SuppliesProgrammingVoltage());
ScriptedFlashTransportSource powered(true);
EXPECT_TRUE(powered.SuppliesProgrammingVoltage());
EXPECT_THAT(powered.RequestedKinds(), ::testing::IsEmpty());
```

For every default object assert no configuration/open lifecycle calls; mixed CAN defaults to `ScriptedMixedCanMode::kUnconfigured`. Do not open preloads in the fake or test.

- [ ] **Step 2: Regenerate and run the test to verify failure**

Run `python3 scripts/gazelle_check.py --fix`, then `bazel test --config=release //src/backend/flash/testing:scripted_flash_transport_source_test --test_output=errors`. Expected: build failure because the source header/class does not exist.

- [ ] **Step 3: Implement the source fake**

Use one `std::deque<std::unique_ptr<...>>` per concrete kind. Each Make method records its kind, moves and pops the oldest preload, or makes a default scripted transport. Store the constructor VPP flag without deriving it from transport state. Keep the fake separate from `scripted_flash_transports`: it gets its own `cc_library(testonly = True)` named `scripted_flash_transport_source`, following package-owned mock conventions. Regenerate managed BUILD attributes.

- [ ] **Step 4: Verify the source and existing transport tests**

Run `bazel test --config=release //src/backend/flash/testing:all --test_output=errors`. Expected: all tests pass, including FIFO, cross-kind isolation, fresh defaults and VPP assertions.

- [ ] **Step 5: Commit**

```sh
git add src/backend/flash/testing/scripted_flash_transport_source.h src/backend/flash/testing/scripted_flash_transport_source_test.cpp src/backend/flash/testing/BUILD.bazel
git commit -m "test(flash): add scripted flash transport source"
```

### Task 2: One portable kernel-file handle helper

**Files:** Modify `src/backend/config/config_paths.h`, `config_paths.cpp`, `config_paths_test.cpp`, `src/platform/desktop/common/flash/flash_workflow.cpp`, and `src/backend/flash/eeprom/eeprom_read_plan.cpp`; review generated BUILD changes.

**Interfaces:**
- Consumes: `config::ConfigPaths::kernel_files_directory`, whose configured directory carries a trailing slash.
- Produces: `std::string fastecu::config::KernelFileHandle(const ConfigPaths& paths, std::string_view kernel);` declared in `config_paths.h`, defined in `config_paths.cpp`.
- Contract: return directory plus kernel verbatim. Insert no separator, normalize nothing and perform no filesystem I/O. Empty directory keeps the previous bare-handle behavior.

- [ ] **Step 1: Write `KernelFileHandleTest.preservesConfiguredHandleText` in `config_paths_test.cpp`**

```cpp
ConfigPaths paths;
paths.kernel_files_directory = "kernels/";
EXPECT_EQ(fastecu::config::KernelFileHandle(paths, "kernel.bin"), "kernels/kernel.bin");
EXPECT_EQ(fastecu::config::KernelFileHandle(paths, "nested/kernel.bin"), "kernels/nested/kernel.bin");
EXPECT_EQ(fastecu::config::KernelFileHandle(paths, ""), "kernels/");
paths.kernel_files_directory.clear();
EXPECT_EQ(fastecu::config::KernelFileHandle(paths, "kernel.bin"), "kernel.bin");
```

- [ ] **Step 2: Run the focused test to verify failure**

Run `bazel test --config=release //src/backend/config:config_paths_test --test_output=errors`. Expected: build failure naming the missing `KernelFileHandle`.

- [ ] **Step 3: Implement the helper and switch its three consumers**

Replace the concatenations in `ResolveKernel`, `ResolveKernelBytes` and `BuildEepromReadPlan`. Replace the EEPROM historical line-number comment with a short comment linking the configured trailing-slash contract to the helper. Keep all error propagation, preflight ordering and kernel snapshots unchanged. Run `python3 scripts/gazelle_check.py --fix` and review its output.

- [ ] **Step 4: Verify helper and preparation behavior**

Run `bazel test --config=release //src/backend/config:config_paths_test //src/backend/flash/eeprom:eeprom_read_plan_test //src/backend/flash/eeprom:test_eeprom_read_plan_goldens //src/platform/desktop/common/flash:test_flash_workflow --test_output=errors`. Expected: PASS; existing EEPROM read-handle assertions still name `kernels/ssmk_can_tp_sh7058.bin`, and invalid preflight still reads nothing.

- [ ] **Step 5: Commit named paths**

```sh
git add src/backend/config/config_paths.h src/backend/config/config_paths.cpp src/backend/config/config_paths_test.cpp src/platform/desktop/common/flash/flash_workflow.cpp src/backend/flash/eeprom/eeprom_read_plan.cpp
git commit -m "refactor(config): centralize kernel file handles"
```

If regeneration changed a managed BUILD file for these consumers, stage that exact reviewed path in the same commit.

### Task 3: Convert workflow policy tests to portable ports

**Files:** Modify `src/platform/desktop/common/flash/flash_workflow_test.cpp` and its `BUILD.bazel` only. Production staging stays unchanged.

**Interfaces:**
- Consumes: Task 1's source API; `InMemoryFileRepository::files`, `read_handles`, `ReadCount`; `FakeClock`; existing factory and workflow methods.
- Produces: portable test fixtures used unchanged after Task 4's move. Define these local helpers in the test's anonymous namespace:
  - `config::ConfigPaths CatalogPaths();` returns `kernel_files_directory = "kernels/"`.
  - `void PopulateCatalogKernels(InMemoryFileRepository& files);` fills exactly the existing `CatalogPaths` fixture's 13 filename/byte pairs, deduplicated by file, using `KernelFileHandle`.
  - `struct WorkflowTestPorts` with `explicit WorkflowTestPorts(bool include_kernel_files = true, bool supplies_programming_voltage = false);`, `FlashWorkflowPorts ports`, and raw observers `InMemoryFileRepository* files`, `ScriptedFlashTransportSource* transports`.
  - `std::unique_ptr<FlashWorkflow> CreateWorkflow(FlashWorkflowRequest request, WorkflowTestPorts& test_ports);` consumes `test_ports.ports` once; observers belong to the workflow thereafter.
  - `std::unique_ptr<FlashWorkflow> CreateWorkflow(FlashWorkflowRequest request);` constructs a local `WorkflowTestPorts` and delegates. It must not alter request paths; the caller chooses `CatalogPaths()` or the existing empty-path failure case.
- Every workflow in a loop gets a new port bundle. Snapshot mutations use a live workflow's `files` observer. No helper retains a raw observer after the workflow/attempt owner is destroyed.

- [ ] **Step 1: Convert source/counting assertions and add the five review-focus checks**

Replace `CountingTransportSource`, `TransportRequests` and `CountingPorts` with `WorkflowTestPorts` and `RequestedKinds()`. Preserve every existing plan family, image, prompt argument, terminal result and error expectation.

In `singleAttemptFamiliesAskTheSourceForTheirPlansTransport`, assert:

```cpp
EXPECT_THAT(test_ports.transports->RequestedKinds(), ::testing::ElementsAre(test.transport));
```

In `unisiaBootmodeWriteMakesOneKlineTransportPerAttempt`, assert the trace is empty before accepting prompts, exactly `{kKline}` at the kernel attempt, remains `{kKline}` while `kRemoveMod1` waits, and becomes `{kKline, kKline}` only after accepting MOD1. Extend `unisiaBootmodeDeclinedMod1CancelsWithNotice` to assert it remains `{kKline}` after decline and the final cancellation.

In `kernelBackedFamiliesKeepTheirKernelSnapshotAfterFilesAreRemoved` and `mc68BdmWriteKeepsItsKernelSnapshotAfterFilesAreRemoved`, erase the handle through `files->files.erase(handle)` after preflight, then assert:

```cpp
EXPECT_EQ(test_ports.files->ReadCount(handle), 1);
EXPECT_EQ(attempt.attempt->Plan().Kernel()->bytes, expected_kernel_bytes);
```

Retain the existing padded BDM expectations and all dedicated petrol/diesel/TCU snapshot checks. In adapter-VPP tests use `WorkflowTestPorts(true, true)`; preserve these exact assertions after submitting failed/cancelled attempts:

```cpp
EXPECT_EQ(notice.kind, FlashPromptKind::kRemoveProgrammingVoltage);
EXPECT_THAT(notice.arguments, ::testing::ElementsAre(
    std::pair<std::string, std::string>{"outcome", "failed"},
    std::pair<std::string, std::string>{"external_vpp", "no"}));
// Cancelled variant substitutes "cancelled" and ends with kCancelled;
// failed variant emits the original kTimeout failure after notice acceptance.
```

For every former `ExpectNoBackendIo` case, preload one transport of each kind and keep observers. Assert each `last_config` is empty, K-Line/CAN `lifecycle_calls` are empty, mixed `ConfigureCallCount()` and `OpenCallCount()` are zero, and mixed `reset_connection_call_count` is zero. Assert every transport's `ScriptConsumed()` remains true: an unexpected wire call on the empty script would fail the returned result if an attempt were run; preflight here must hand out only failure steps. Preloads remain inspectable whether requested or still queued. Assert the existing failure kind twice where the original test did. Do not assert absence of source requests: making an unopened transport is not adapter I/O.

- [ ] **Step 2: Run the changed suite to verify failure**

Run `bazel test --config=release //src/platform/desktop/common/flash:test_flash_workflow --test_output=errors`. Expected: build failure until the portable fixture helpers are defined and all old Qt/serial fixture call sites are converted.

- [ ] **Step 3: Implement the fixture helpers and finish the conversion**

Use the exact interface block above. Replace each temporary-directory setup with `CatalogPaths()` and a fresh bundle; use `include_kernel_files = false` for missing-kernel fixtures. Replace `WriteFile` changes with assignment to the repository map, preserving exact bytes, including empty kernels. Remove files with map erasure. Delete `QDir`, `QFile`, `QTemporaryDir`, `QByteArray` conversions, `RecordingSerial`, serial setup helpers and the global `CoreApplicationEnvironment` registration. Replace every `QtClock` with `FakeClock`.

Rename the three `...BindsDesktopCanTransport` tests to `...BindsCanTransport`. Preload a `ScriptedCanFlashTransport`, keep its observer, retain the existing `CancelOnCheck(5)` attempt run and `kCancelled` expectation; assert the trace is exactly `{kCanIso15765}` and:

```cpp
ASSERT_TRUE(can_observer->last_config.has_value());
EXPECT_EQ(can_observer->last_config->bitrate, 500000);
EXPECT_EQ(can_observer->last_config->request_id, 0x7E0U);
EXPECT_EQ(can_observer->last_config->response_id, 0x7E8U);
EXPECT_FALSE(can_observer->last_config->extended_id);
```

Keep cancellation early enough to avoid protocol exchanges; no exchange scripts are added. In `routesSh7058ReadAndWriteWithPreTransportPrompts`, assert the source trace stays empty until confirmation, becomes `{kKline}` for Read and `{kCanIso15765}` for Write, and stays empty for the declined Read. Do not force an ISO-15765 assertion onto that Read's K-Line transport or add a new executor run.

Convert `UnisiaM32rOpenPort2WriteAtAttempt` to `UnisiaM32rAdapterVppWriteAtAttempt()` with no parameters, constructing a true-VPP bundle and retaining its begin/attempt sequencing checks. Convert all remaining serial-backed preflight and VPP tests found by `rg`, including TCU unsupported operations, single-attempt invalid/missing kernels, SH7058 short/test-write input, BDM missing kernel and Colt wrong size. Remove full gmock mocking includes if only matchers remain.

Regenerate BUILD attributes. While the suite remains in the desktop package, its inherited `fastecu_gtest` macro may still link Qt; source and direct dependencies must now be portable. Task 4 changes the macro by moving the suite.

- [ ] **Step 4: Verify the converted suite and unchanged desktop behavior**

Run `bazel test --config=release //src/platform/desktop/common/flash/... //src/ui/desktop/flash/... --test_output=errors`. Expected: PASS with the same workflow test inventory (renamed binding tests included), unchanged capability test and existing EEPROM/dialog/worker coverage. Record the actual inventory rather than assuming the spec's historical count of 95.

Run `rg -n 'QTemporaryDir|QFile|QDir|QByteArray|QtClock|QtFileRepository|FakeBackend|SerialPortActions|DesktopFlash|CoreApplicationEnvironment|src/platform/' src/platform/desktop/common/flash/flash_workflow_test.cpp`. Expected: only its own workflow-header include matches; that disappears in Task 4.

- [ ] **Step 5: Commit**

```sh
git add src/platform/desktop/common/flash/flash_workflow_test.cpp src/platform/desktop/common/flash/BUILD.bazel
git commit -m "test(flash): run workflow policy on portable ports"
```

### Task 4: Relocate the workflow and enforce its portable closure

**Files:** Move the three workflow files and update the remaining files listed in the file map. Preserve desktop port helper and capability-test locations.

**Interfaces:**
- Consumes: Task 3's portable suite and existing PR 1 factory/step contracts.
- Produces: `src/backend/flash/workflow/flash_workflow.h`; `//src/backend/flash/workflow:flash_workflow` and `//src/backend/flash/workflow:flash_workflow_test` (portable GoogleTest macro). All public names and signatures remain identical, including `PortableImageForOperation(FlashOperation, bytes::ByteView) -> std::optional<bytes::Bytes>`.
- Desktop API remains `FlashWorkflowPorts DesktopFlashWorkflowPorts(SerialPortActions*)`; worker continues to consume a bound attempt plus a caller-owned clock, with no workflow dependency.

- [ ] **Step 1: Point the converted test at its new header and verify failure**

Change its own include to `src/backend/flash/workflow/flash_workflow.h`. Run `bazel test --config=release //src/platform/desktop/common/flash:test_flash_workflow --test_output=errors`. Expected: build failure because the destination header does not exist yet.

- [ ] **Step 2: Move the files and update consumers/build ownership**

Move the header, implementation and test into `src/backend/flash/workflow/`; update the implementation's own include. Add a new BUILD seed with `package(default_visibility = ["//bazel/layers:backend_and_above"])` before generating; use ordinary `cc_library` with `COMMON_COPTS` and the inherited `fastecu_portable_gtest` mapping. Let Gazelle name the test `flash_workflow_test`; remove the old `test_flash_workflow` rule and workflow library rule rather than leaving compatibility aliases.

Update the five production/test header consumers identified in the file map: desktop ports helper, capability test, dialog header, operation controller and MainWindow. Update the built-in catalog's explicit consumer visibility with `//src/backend/flash/workflow:__pkg__`, retaining the platform entry. Remove the obsolete serial-facade comment from the flash BUILD file if no longer relevant. Run `python3 scripts/gazelle_check.py --fix`; review generated `deps`/`implementation_deps` and use the new workflow label throughout the enumerated consumer BUILD files.

Do not modify family executors, the routing table, retry transitions, worker/dialog APIs or visibility groups. `builtin_catalog_capability_test` retains its kernel runfiles data/environment and Qt-based reads.

- [ ] **Step 3: Update enduring contracts in the same deliverable**

Add the spec's exact “Flash transport source” glossary entry to `src/backend/flash/CONTEXT.md`, including `_Avoid_: Transport factory, flash adapter`. Correct the stale `ConfirmationSpec::Id` comment: plans carry required confirmations before prompts; declining prevents execution of the bound attempt. Avoid claiming the enum's presence proves grant (that redesign is out of scope).

In [design notes](../../design-notes.md#flash-architecture), state that backend workflows own routing/preparation/staging with owned file and transport-source ports; desktop composition supplies those ports and each attempt's clock. Remove only the completed reusable-flash-policy bullet from [technical debt](../../tech-debt.md#p1-separate-ui-from-application-logic); retain the separate workflow coverage, equivalence and other remaining debt. Qualification matrix/checklist evidence stays unchanged.

- [ ] **Step 4: Verify relocation, portability and the full gates**

Run the focused relocation check:

```sh
bazel test --config=release //src/backend/flash/workflow:all //src/backend/flash/testing:all //src/backend/config:config_paths_test //src/backend/flash/eeprom:all //src/platform/desktop/common/flash/... //src/ui/desktop/flash/... //src/ui/desktop/widgets:all --test_output=errors
```

Expected: PASS, including relocated workflow policy, existing EEPROM staging consumers and real bundled-kernel capability coverage.

Run:

```sh
bazel query 'deps(//src/backend/flash/workflow:flash_workflow) union deps(//src/backend/flash/workflow:flash_workflow_test)' --output=label
```

Inspect the labels: none may start with `//src/platform/`, `//src/ui/` or `//bazel/qt`. Check obsolete references with `rg -n 'src/platform/desktop/common/flash/flash_workflow|common/flash:flash_workflow|common/flash:test_flash_workflow' src apps tests bazel scripts BUILD.bazel`; expected: no matches (exit 1).

Then run each established gate and inspect its complete result:

```sh
python3 scripts/gazelle_check.py
prek run --all-files
bazel run //:clang_tidy_report_changed
bazel build --config=release //:fastecu
bazel test --config=release //... --test_output=errors
```

Expected: zero exit status for every gate. Review and apply generated/formatter corrections to named paths, rerun an affected gate after a correction, and fix warnings without suppressions. Require applicable platform CI and packaging from `.github/workflows/pr.yml` before delivery; host verification alone does not claim other platforms pass.

- [ ] **Step 5: Commit the relocation and its owning documentation**

Stage the three old workflow paths (deletions), the four new workflow paths, and only changed paths from the file map. Use `git diff --cached --stat` and `git diff --cached --check` to confirm the commit contains no unrelated work. Commit as `refactor(flash): move workflow staging into backend`.

## Delivery and artifact lifecycle

After task completion, record focused/full gate results and portable-closure evidence in the PR description. Use the feature-branch/PR workflow; push only when authorized. Read the finishing-a-development-branch skill at integration time.

The untracked original spec and this plan must be preserved in a separate delivered history-preservation change before any cleanup PR deletes them. Do not introduce and delete them in one squashed PR. After implementation, extract enduring contracts as Task 4 specifies, then retire completed artifacts in a separate change following [document lifecycle rules](../../README.md#recover-completed-work-and-deleted-source-citations).

## Plan self-review

Spec coverage: Task 1 supplies scripted source behavior; Task 2 implements all three kernel-handle sites; Task 3 removes Qt/filesystem/serial fixtures and retains binding, VPP, safety, snapshot and prompt assertions; Task 4 moves policy and `PortableImageForOperation`, keeps desktop composition/capability coverage, generates the portable BUILD package, fixes the confirmation comment and records the glossary/ownership decision. PR 1 is the inspected baseline; all four out-of-scope candidates remain deferred.

The five review-focus conditions each have explicit tests in their owning task. All consumers use Task 1's exact source API, Task 2's helper and Task 3's fixture ownership model. No workflow-plus-executor exchange coverage is added. The only behavior exercised through existing attempt runs remains their existing cancellation/binding check.
