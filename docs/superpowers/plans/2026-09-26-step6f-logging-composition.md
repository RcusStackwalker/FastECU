# Step 6f Logging Composition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Supply all desktop logging protocols from the composition root, with ECU/TCU selection carried as per-run snapshot data.

**Architecture:** A dedicated registration helper in the existing transport package captures the serial facade and clock. The composition calls it once; MainWindow captures user choices and wires signals. Existing engine execution and error semantics remain intact.

**Tech Stack:** Existing Bazel graph, C++23 build configuration, Qt, GoogleTest/GoogleMock and QtTest.

**Spec:** [Approved design](../specs/2026-09-26-step6f-logging-composition-design.md).

## Global Constraints

- No new consumer may join the frozen `serial_qt_compat` allowlist.
- Preserve desktop logging behavior, protocol wire sequences, error reporting, and support for Windows, macOS, and Linux.
- Registration itself does not perform hardware I/O.
- Factories still execute synchronously on the start caller's thread.
- Add `bool target_is_ecu = true` to `DesktopLoggingSnapshot`.
- Remove `logging_clock` from `MainWindowServices`; keep the clock owned by the composition.
- Preserve `AlreadyInMode(125000)` and all existing CDBG configuration values and errors.
- Work on a feature branch; use the repository's PR workflow, not direct commits to master.

## Review Focus

These additional cases are assigned to the tasks below:

1. ECU then TCU on consecutive runs must use the new run's choice, not stale closure state (Tasks 1–2).
2. An empty CDBG open result must not trigger the short-circuited open-state query (Task 2).
3. Adapter capability changing between runs must be observed at factory invocation (Task 2).
4. A window constructed with a test-supplied factory must not overwrite it (Task 3).
5. Registering factories without running them must be safe through immediate teardown and application reconstruction (Tasks 2–3).

## File map

- `src/platform/desktop/common/logging/logging_snapshot_adapter.h`: per-run target value.
- `src/ui/desktop/menu_actions.cpp`: capture target before both snapshot copies.
- `src/ui/desktop/mainwindow.cpp`: consume target during transitional wiring, then remove registrations.
- New `src/platform/desktop/common/transport/desktop_logging_protocol_registration.{h,cpp}`: production registration entry point and factory closures.
- New `src/platform/desktop/common/transport/desktop_logging_protocol_registration_test.cpp`: behavior through fake serial and engine APIs.
- `src/platform/desktop/common/transport/BUILD.bazel`: isolated helper and test targets.
- `apps/desktop/desktop_composition.cpp`, `apps/desktop/BUILD.bazel`: install production registrations.
- `src/ui/desktop/main_window_services.h`, `mainwindow.h`, `BUILD.bazel`: remove obsolete service/include/dependency edges.
- Existing `mainwindow_test.cpp`, `logging_adapters_test.cpp`, and `desktop_composition_test.cpp`: snapshot and integration regressions.
- New `docs/logging-composition-bench-checklist.md`, existing modularization plan, design notes, and tech-debt roadmap: close-out and qualification record.

Read the spec and this plan together. Tasks are sequential; the helper can be tested before production wiring moves. Product code is unchanged at plan creation.

### Task 1: Capture ECU/TCU selection in each logging snapshot

**Files:** Modify `logging_snapshot_adapter.h`, `src/ui/desktop/menu_actions.cpp`, and `src/ui/desktop/mainwindow.cpp`. Test in `logging_adapters_test.cpp` and `src/ui/desktop/mainwindow_test.cpp`.

**Interfaces:** Produces `DesktopLoggingSnapshot::target_is_ecu`, a `bool` defaulting to `true`. `make_desktop_logging_snapshot(...)` keeps its current signature.

- [x] **1. Add failing snapshot and UI tests.** In the adapter test, assert the new field defaults to true and survives value copies when false. In the UI fixture, register a capturing `SSM` factory after window construction, set `ecu_init_complete = true`, and use synthetic valid SSM log definitions. Exercise `toggle_realtime` through `QMetaObject::invokeMethod`, matching the existing Windows-safe private-slot pattern. Use a scripted protocol and explicitly stop/join each run. Assert captured choices equal `{true, false}` for consecutive ECU and TCU runs and that `activeLoggingSnapshot->target_is_ecu` matches each run. Configure the existing logger action as checked so the actual UI entry path runs. Avoid hardware connection setup and modal failures.

- [x] **2. Run the tests before implementation.**

  `bazel test --config=release //src/platform/desktop/common/logging:test_logging_adapters //src/ui/desktop:test_mainwindow`

  Expect the missing field to fail compilation initially; after adding the field alone, verify the TCU case fails until UI capture is implemented.

- [x] **3. Implement the snapshot field and capture.** Append `bool target_is_ecu = true` to the snapshot. After checking snapshot creation succeeded, assign the radio-button state before `activeLoggingSnapshot.emplace(*snapshot)`. Temporarily change the existing SSM factory to read `snapshot.target_is_ecu` so the intermediate commit already uses the new contract. Keep the adapter-capability query where it is.

- [x] **4. Run the Task 1 command again.** Require both targets to pass and confirm Google Mock failures are reflected in the process result. Do not change unrelated existing UI behavior to satisfy fixtures.

- [x] **5. Commit the task's files.** Suggested message: `refactor(logging): capture ECU target in session snapshots`.

### Task 2: Extract and test desktop protocol registration

**Files:** Create the three registration files in the file map; modify the transport `BUILD.bazel`. Existing `cdbg_serial_setup.{h,cpp}` remains the shared setup implementation.

**Interfaces:** Consumes Task 1's snapshot. Produces this function in namespace `fastecu::desktop::logging`:

```cpp
void register_desktop_logging_protocols(
    LoggingEngine& engine, SerialPortActions& serial, fastecu::IClock& clock);
```

The header forward-declares argument types in their actual namespaces (`SerialPortActions` is global). References must outlive the engine. Target: `//src/platform/desktop/common/transport:logging_protocol_registration`; test: `:test_desktop_logging_protocol_registration`.

- [x] **1. Add the helper test target and failing tests.** Use `FakeBackedSerial` and QtTest with Google Mock failures wired into the exit result, following `test_mainwindow`. Fixture creation itself calls `set_add_ssm_header(false)`; account for it before asserting registration is inert. Declare the serial fixture and clock before the engine. Stop/join workers before verifying expectations. Use bounded signal waits, not arbitrary sleeps.

  Pin these test names and observable assertions:

  - `registration_performs_no_io`: a strict fake expects no calls during registration; destruction without starting succeeds.
  - `cdbg_setup_failure_stops_at_failed_step`: seven rows, one failed setter each; check `InvalidConfig`, exact existing detail, ordered values and no later setters/open.
  - `cdbg_open_failure`: empty open result and nonempty result with false open state both yield `Disconnected` and `unable to open CAN adapter for CDBG logging`. In the empty row, expect zero open-state calls.
  - `cdbg_success_preserves_start_sequence`: script current handshake replies from `cdbg_logging_protocol_test.cpp` through the real adapter; expect Running, then stop. Pin the seven configuration values before opening.
  - `ssm_target_and_adapter_are_per_run`: use the same registrations across ECU/TCU and adapter-flag variants. Assert startup write frames address `0x10` for ECU and `0x18` for TCU (source `0xF0`), built with the existing SSM codec. Script framed replies; assert the OpenPort read uses the startup timeout while the other path begins with 10 ms reads. Query the adapter flag once per factory call. Include a subsequent run with the opposite adapter flag.
  - `ssm_snapshot_offsets_reach_samples`: two synthetic channels with deliberately nonsequential response offsets; script distinct raw bytes and assert the emitted channel IDs/values match the offsets, using existing portable SSM vectors.
  - `mut_dma_preserves_initialization_and_channels`: script the existing portable MUT/DMA success vector through the real K-Line adapter, verify the 125000 baud operation and emitted sample IDs/values for selected channels. Stop and join.

  Reuse synthetic wire fixtures from the current portable protocol tests, adapting only their transport envelope to the serial fake. Do not invent new handshake behavior, loosen unexpected calls, or alter protocols. Target selection does not authorize fixing the existing SSM response framing behavior.

- [x] **2. Run the new target before the helper exists.**

  `bazel test --config=release //src/platform/desktop/common/transport:test_desktop_logging_protocol_registration`

  Expect missing helper/header failure; add declarations and then confirm missing registration behavior fails before implementing the closures.

- [x] **3. Implement the helper by moving the three factory bodies.** Register IDs `MUT_DMA`, `CDBG`, `SSM`. Capture `serial` and `clock` by reference, never `this`. Retain CDBG's calls to `configure_cdbg_serial`, open-result short circuit, and exact errors; retain MUT/DMA's `AlreadyInMode(125000)`. SSM consumes Task 1's field and queries the adapter flag at invocation.

  The dedicated `qt_cc_library` uses `normal_hdrs` (no moc). Depend on `:transport`, serial `:serial_qt_compat`, logging `:logging_runtime`, backend logging `//src/backend/logging/protocols`, and directly included backend port/protocol targets. Give visibility only to `//apps/desktop:__pkg__` and this package. The test depends on the helper, `:fake_backed_serial`, Qt clock/ports, and required test libraries. Do not put helper sources into the base transport target or expand serial visibility.

- [x] **4. Run the helper and existing protocol/runtime tests.**

  `bazel test --config=release //src/platform/desktop/common/transport:test_desktop_logging_protocol_registration //src/platform/desktop/common/logging:test_logging_engine //src/platform/desktop/common/logging:test_logging_worker //src/backend/logging/protocols/... //:serial_compat_allowlist`

  Require all tests to pass. Production still uses its existing factories until Task 3.

- [x] **5. Commit the task's files.** Suggested message: `refactor(logging): extract desktop protocol registration`.

### Task 3: Wire the composition and remove UI factory ownership

**Files:** Modify `apps/desktop/desktop_composition.cpp`, `desktop_composition_test.cpp`, `apps/desktop/BUILD.bazel`; `src/ui/desktop/mainwindow.cpp`, `mainwindow.h`, `main_window_services.h`, `mainwindow_test.cpp`, `BUILD.bazel`.

**Interfaces:** Consumes Task 2's function. `MainWindowServices` loses only `QtClock& logging_clock` and its now-unused forward declaration. The composition retains `QtClock logging_clock_` and existing destruction order.

- [x] **1. Add failing ownership tests.** Extend the Task 1 UI test with a factory installed before window construction and assert that exact factory is invoked, with the selected target, afterwards. Add `composition_registers_all_logging_protocols_without_window`: inspect the engine's registration keys and compare against `{CDBG, MUT_DMA, SSM}` without invoking them. Use translation-unit-local private-access testing, as already used in `mainwindow_test.cpp`, pre-including dependencies before the macro; add no public registry accessor or production test hook. Keep and run immediate destruction and repeated-composition tests. The UI override and missing composition registrations must fail on the pre-extraction implementation.

- [x] **2. Run the integration tests and record their expected failures.**

  `bazel test --config=release //apps/desktop:desktop_composition_test //src/ui/desktop:test_mainwindow`

- [x] **3. Move production ownership.** Include the helper in `desktop_composition.cpp`, call it after diagnostic connections, and add its target to `:composition`. Delete the three registration calls from `MainWindow::setupLoggingEngine()`. Remove `logging_clock` from the services struct, returned initializer, UI fixtures, and composition identity test. Remove the now-unused fixture clock if no remaining test needs it.

- [x] **4. Remove only unused dependencies.** Search all sources belonging to the UI target before deleting protocol/transport includes or BUILD deps. Preserve any dependency used by other desktop workflows. Verify the new helper target has no UI visibility and neither app nor logging runtime directly depends on `serial_qt_compat`. Keep signal wiring and diagnostic delivery unchanged.

- [x] **5. Run focused integration tests and the desktop build.**

  `bazel test --config=release //apps/desktop:desktop_composition_test //src/ui/desktop:test_mainwindow //src/platform/desktop/common/transport:test_desktop_logging_protocol_registration //src/platform/desktop/common/logging/... //:serial_compat_allowlist`

  `bazel build --config=release //:fastecu //:portable_closure`

  Require passing tests/build and a non-growing allowlist. Inspect the final diff for widget captures in the extracted factories and accidental changes to setup ordering.

- [x] **6. Commit the task's files.** Suggested message: `refactor(desktop): compose logging protocols outside MainWindow`.

### Task 4: Record qualification and close out step 6f

**Files:** Create `docs/logging-composition-bench-checklist.md`. Modify `docs/modularization-plan.md`, `docs/design-notes.md`, and `docs/tech-debt.md`.

**Interfaces:** Consumes the verified implementation from Tasks 1–3; produces the durable description of the boundary and honest qualification status.

- [x] **1. Write the bench checklist.** Include adapter/OS/protocol identifiers, build revision, observed result, and unrun status by default. Cover MUT/DMA start/stop; CDBG successful start, setter failure and open failure; SSM ECU/TCU with applicable OpenPort/non-OpenPort variants; direct/remote startup where available; stop/restart and application teardown. Preserve prior wire expectations. Do not mark any hardware scenario passed without evidence.

- [x] **2. Update durable documentation.** Mark 6f implementation complete only after automated gates pass. Record snapshot target timing, helper placement under the frozen allowlist, and engine-before-service destruction in design notes. Replace the deferred registration entry in tech debt with the achieved boundary; keep unrelated debt. Record hardware qualification separately. At final close-out, distill and remove this temporary plan/spec following the 6c–6e convention; their approved versions remain in git history.

- [x] **3. Run final checks.** Run Task 3's focused commands plus `bazel build -k --config=release //:fastecu //tests/...` and `bazel test -k --config=release //tests/... //:bazel_openssl_wiring`. Run `git diff --check`. Submit the feature branch through the normal PR workflow when authorized and require Windows/macOS/Linux CI and existing Windows/macOS packaging checks. Record unavailable CI or bench evidence as outstanding, not passed.

- [x] **4. Review and commit the documentation.** Confirm all acceptance criteria in the spec map to the changed code or recorded evidence. Suggested message: `docs: close out step 6f logging composition`.

## Execution handoff

Recommended method: native execution. The three code tasks share one small interface and are sequential; one implementer can preserve context, followed by an independent whole-branch review. Subagent-driven execution remains an option if per-task independent reviews are preferred. Written-plan review and method selection are required before product changes.

Execution evidence: local `bazel test -k --config=release //...` passed (215 passed, 7 skipped). Cross-platform CI and hardware qualification remain pending; temporary plan/spec retained for review and PR close-out.
