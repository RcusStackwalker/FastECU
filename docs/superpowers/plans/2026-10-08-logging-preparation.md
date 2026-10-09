# Portable Logging Preparation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete the first P1 logging debt action by moving run preparation and sample resolution into portable backend policy, with useful definition validation errors.

**Architecture:** Build channels through a portable definition adapter that reuses existing session validation. A validating factory creates an owned, immutable `LoggingRunSnapshot`; a portable resolver derives accepted sample identities and display precision from that snapshot. Desktop adapters convert Qt inputs, format accepted values, update caches, bind transports, and report errors.

**Tech Stack:** C++23, Qt 6 at desktop boundaries, Bazel 9.1.1 as pinned in `.bazelversion`, package-owned GoogleTest, Gazelle-managed BUILD attributes.

**Spec:** [Confirmed logging preparation design](../specs/2026-10-08-logging-preparation-design.md).

## Global Constraints

- Backend policy has no Qt, owned threads, filesystem I/O, or platform dependencies. Existing callers own execution and lifetimes.
- Addresses: hexadecimal digits, optional `0x`/`0X`; lengths: positive decimal integers. Both trim surrounding ASCII whitespace and reject signs, internal whitespace, trailing junk, and overflow. Retain existing protocol bounds.
- Formats: exactly `0`, or `0.` followed by 1–15 zeros. Empty units are valid; omitted XML attributes retain existing parser defaults.
- Reuse existing session validation and sample conversion. Preserve first-conversion semantics, identity namespaces, support rules, original SSM offsets, decimal-byte assembly, and CSV/file-lifetime contracts.
- Capture protocol, selection, support, and typed ECU/TCU target by value; later edits affect subsequent runs. CSV still reads current selection within the captured protocol.
- Invalid participating definitions prevent startup with contextual log and dialog errors. Raw conversion errors terminate before batch publication; delivered-sample/cache errors report and continue other samples.
- Use existing `InvalidConfig` and `Internal` error kinds and `.has_value()` checks. Warnings remain errors; use existing layer visibility groups.
- Regenerate managed BUILD attributes using `python3 scripts/gazelle_check.py --fix`. Preserve hand-owned names, visibility, and metadata; review generated changes.
- Read [coding conventions](../../coding-style.md), [logging contracts](../../reference/logging-contracts.md), [Gazelle ownership](../../adr/0017-generate-bazel-targets-with-gazelle.md), and [PR CI](../../../.github/workflows/pr.yml) before execution.

## Review Focus

- Inputs such as `0x`, `+10`, `10junk`, overflowing integers, and non-ASCII whitespace must not silently select a different address (Task 1).
- A malformed unsupported MUT channel must remain filtered, while a malformed unsupported SSM channel must block startup because it is still polled (Task 2).
- Shared IDs across protocols and edits during a run must not change sample identity, target, support, or CSV's captured protocol (Tasks 2–3).
- A valid sample followed by a bad sample in one polling batch must not publish a partial batch; a display error must still allow a later valid sample (Tasks 2–3).
- Author-provided error text, including markup-like content, must display literally in the startup dialog and leave no running worker or checked logging action (Task 3).

## File Structure and Dependency Order

| Files | Responsibility |
| --- | --- |
| Create `src/backend/logging/logging_channel_preparation.{h,cpp}` and `_test.cpp` | Definition fields to a validated portable channel; contextual input errors |
| Modify `src/backend/logging/logging_session.{h,cpp}` and `_test.cpp` | Share existing per-channel validation without changing wire/policy validation |
| Create `src/backend/logging/logging_run_snapshot.{h,cpp}` and `_test.cpp` | Immutable captured run and model-to-run preparation |
| Create `src/backend/logging/logging_sample_resolution.{h,cpp}` and `_test.cpp` | Accepted sample metadata, deliberate skip, or identity error |
| Modify `src/backend/logging/logging_use_case_test.cpp` | Strengthen existing fatal-error tests with whole-batch assertions |
| Modify `src/platform/desktop/common/logging/logging_snapshot_adapter.{h,cpp}` and `logging_value_adapter.{h,cpp}`, `logging_adapters_test.cpp` | Thin Qt input bridge and display-cache adapter |
| Modify `src/platform/desktop/common/logging/runtime/logging_engine.cpp`, `logging_engine_test.cpp` | Consume snapshot accessors; migrate directly constructed test snapshots |
| Modify `src/platform/desktop/common/transport/desktop_logging_protocol_registration.cpp` and `_test.cpp` | Bind validated channels, SSM offsets, and typed target to existing transports |
| Modify `src/ui/desktop/widgets/menu_actions.cpp`, `log_operations_ssm.cpp`, `mainwindow_test.cpp` | Capture target before preparation, show actionable errors, retain CSV semantics |
| Modify BUILD files in the five affected packages | Regenerated dependencies and new portable test/library targets |
| Modify `docs/reference/logging-contracts.md`, `docs/design-notes.md`, `docs/tech-debt.md` | Enduring contracts, rationale, and completed debt action |

Tasks 1 and 2 add testable backend APIs before Task 3 switches desktop consumers. Task 4 validates and delivers the complete change. Existing worker and protocol algorithms retain their APIs.

### Task 1: Prepare and Validate Definition Channels Portably

**Files:** Create channel-preparation files; modify session files and backend BUILD.

**Interfaces** (namespace `fastecu::logging`):

```cpp
// logging_session.h: per-channel checks; aggregate wire shape stays in session preparation.
fastecu::Status validate_logging_channel(LoggingProtocolId protocol,
                                        const LoggingChannel& channel);
// logging_channel_preparation.h
fastecu::Result<LoggingChannel> prepare_logging_channel(
    const LoggerParameter& parameter, LoggingProtocolId protocol);
```

The preparer takes the first conversion, parses address/length/precision, chooses existing raw assembly for the protocol, and calls the shared validator. Shared validation reports the offending field; preparation prefixes the parameter's protocol and ID. Session preparation calls the same validator for each channel and retains duplicate-ID, policy, channel-count, and aggregate wire-shape checks.

- [ ] Add parameterized `LoggingChannelPreparation` tests using a valid `SSM/rpm` parameter (`address="000010"`, `length="1"`, conversion `{"rpm", "x", "0.00", ...}`). Pin these cases with `IsOk`, `IsErrWith`, and container matchers:

  | Cases | Assertions |
  | --- | --- |
  | `000010`, `0x10`, `0X10`, ` \t0x10\r\n` | Address is `0x10` |
  | Empty, `0x`, `+10`, `-10`, `10junk`, `1 0`, `100000000` | `InvalidConfig`; detail contains `SSM`, `rpm`, `address`, and offending input |
  | UTF-8 nonbreaking spaces around an address | `InvalidConfig`, not accepted as ASCII whitespace |
  | Length `1`, ` 2 `, `04` | Length is 1, 2, 4 respectively |
  | Length empty, `0`, `+1`, `-1`, `1x`, `0x2`, `1 0`, `18446744073709551616` | `InvalidConfig` with `length` context |
  | Formats `0`, `0.0`, `0.00`, `0.` + 15 zeros | Precision is 0, 1, 2, 15 |
  | Formats empty, `banana`, `0.`, `0.0garbage0`, `0.00.000`, ` 0.00 `, 16 zeros | `InvalidConfig` with `format`, invalid value, and expected grammar |
  | Empty units; a second deliberately different conversion | Empty unit accepted; only first expression/format used |
  | Missing conversion; invalid expression `x+`; constant division by zero | `InvalidConfig` with `conversion` or `expression` context |
  | SSM `ffffff`/`1000000`; MUT `ffff`/`10000`; CDBG `ffffffff` | Existing inclusive address bounds retained |
  | SSM lengths 1/255/256; MUT/CDBG lengths 1/2/4/3 | Existing channel and protocol length bounds retained |

- [ ] Regenerate backend BUILD files, then run `bazel test --config=release //src/backend/logging:logging_channel_preparation_test`. The new API is absent: expect compilation failure before implementation.
- [ ] Extract existing channel validation into `validate_logging_channel`, retaining expression grammar/probes, address bounds, raw-assembly checks, and precision ceiling. Move existing MUT/CDBG per-channel length predicates from wire-shape validation into this shared validator; retain aggregate channel-count and CDBG batching checks in session preparation. Give invalid field failures specific details; have `make_logging_session` reuse it. Do not duplicate the expression parser or change conversion evaluation.
- [ ] Implement `prepare_logging_channel` with ASCII trimming and full-consumption `std::from_chars` parsing. Parse hexadecimal address into `std::uint32_t`, decimal length into `std::size_t`, and explicitly remove only an optional hexadecimal prefix. Require at least one digit. Do not catch conversion exceptions or emulate Qt acceptance rules.
- [ ] Regenerate BUILD files and run `bazel test --config=release //src/backend/logging:logging_channel_preparation_test //src/backend/logging:logging_session_test //src/backend/logging:logging_conversion_test`. Expect all PASS. Add direct `validate_logging_channel` cases to the session suite if extraction exposes gaps in existing coverage.
- [ ] Review generated dependencies and commit only this task's named source/test/BUILD paths: `feat: prepare logging definition channels in backend`.

### Task 2: Own the Prepared Run and Resolve Samples in Backend

**Files:** Create run-snapshot and sample-resolution files; modify use-case tests and backend BUILD.

**Consumes:** Task 1's `prepare_logging_channel` and existing `make_logging_session`, `LoggerModel`, `LoggerSelection`, `LogSample`.

**Produces** (namespace `fastecu::logging`):

```cpp
enum class LoggingTarget { Ecu, Tcu };

class LoggingRunSnapshot {
 public:
  const LoggingSession& session() const;
  const std::string& protocol_key() const;
  const LoggerSelection& selection() const;
  const std::vector<std::size_t>& response_offsets() const;
  LoggingTarget target() const;
  bool channel_enabled(std::string_view id) const;
  // Private owned data and constructor; preparation is its only factory.
};

fastecu::Result<LoggingRunSnapshot> prepare_logging_run(
    const LoggerModel& model, LoggingProtocolId protocol,
    std::string_view protocol_filter, LoggingPolicy policy, LoggingTarget target);

struct ResolvedLogSample {
  LoggerIdentity identity;
  double numeric_value;
  std::uint8_t decimal_precision;
};
fastecu::Result<std::optional<ResolvedLogSample>> resolve_log_sample(
    const LoggingRunSnapshot& snapshot, const LogSample& sample);
```

Private snapshot state owns the session, effective protocol key, selection, support eligibility, offsets, and target. Copies/moves are permitted; there is no public default constructor, aggregate construction, field mutation, or mutable accessor. The resolver returns `nullopt` for deliberately skipped unsupported SSM samples, `Internal` for unknown IDs, and otherwise derives `(protocol_key, channel.id)` and precision from the validated channel. Numeric conversion remains `convert_sample`'s responsibility.

- [ ] Move policy assertions from `logging_adapters_test.cpp` into new backend suites without yet deleting desktop tests. Add `LoggingRunSnapshot` cases for effective SSM filter (empty is invalid), fixed MUT/CDBG filters, duplicate definitions/selected IDs, unresolved IDs, lower-panel order, and original SSM offsets. A selection `{"other", "missing", "off", "on"}` must retain SSM offsets `{2, 3}` and disabled `off` eligibility.
- [ ] Add `MalformedSupportRules` cases: malformed unsupported MUT channel is omitted; equivalent SSM channel fails preparation; unsupported CDBG channel remains included and must validate. Test empty SSM/MUT selections and empty CDBG failure as existing session rules prescribe. Reject invalid protocol/target enum values and invalid policy.
- [ ] Add `CapturesOwnedInputs`: prepare a TCU run, alter model support and selection, replace/destroy the source model, and verify snapshot channels, support, selection, protocol key, and target remain unchanged. Use the same ID under SSM and CDBG to verify namespace isolation. Const-only accessors must be the only route to stored data.
- [ ] Add `LoggingSampleResolution` assertions: known enabled `rpm` returns `SSM/rpm`, numeric value 1234.5, precision 2; disabled SSM returns successful `nullopt`; unknown ID returns `Internal`; CDBG remains eligible despite unsupported flags. Reordering a later definition never changes identity.
- [ ] Strengthen `ConversionInvalidConfigTerminates` and `UnknownProtocolChannelTerminatesAsInternal` in `logging_use_case_test.cpp`: poll a valid sample followed by the failing sample, assert `sample_batches` is empty and stop is called once. Keep existing error classifications.
- [ ] Regenerate backend BUILD files and run `bazel test --config=release //src/backend/logging:logging_run_snapshot_test //src/backend/logging:logging_sample_resolution_test //src/backend/logging:logging_use_case_test`. Expect new API compilation failures before implementation.
- [ ] Implement the run factory by moving the existing snapshot-selection policy into backend, calling Task 1 for each participating channel and existing session preparation once for the complete channel set. Reject duplicate participating definitions and IDs; retain support-filter ordering. Build immutable data only on success; derive identities instead of storing a second identity map.
- [ ] Implement the sample resolver using the validated session and captured eligibility. Keep per-channel definition errors contextual; aggregate policy/wire-capacity errors identify the protocol and reason without inventing a parameter culprit.
- [ ] Regenerate BUILD files and run `bazel test --config=release //src/backend/logging/...`. Expect all PASS. Commit named backend files and BUILD: `feat: own logging run preparation and sample resolution in backend`.

### Task 3: Bind Desktop Consumers and Show Definition Errors

**Files:** Desktop adapter files/tests, engine implementation/tests, protocol registration implementation/tests, UI menu/CSV code and MainWindow tests; regenerated BUILD files in backend logging, desktop logging, logging runtime, transport, and UI widgets as needed.

**Consumes:** Task 2 interfaces.

**Desktop bridge interface** (namespace `fastecu::desktop::logging`):

```cpp
using DesktopLoggingSnapshot = fastecu::logging::LoggingRunSnapshot;
fastecu::Result<DesktopLoggingSnapshot> make_desktop_logging_snapshot(
    const fastecu::logging::LoggerModel& model,
    fastecu::logging::LoggingProtocolId protocol, const QString& protocol_filter,
    fastecu::logging::LoggingPolicy policy, fastecu::logging::LoggingTarget target);
// Existing apply_log_sample signature and DesktopLoggerValues API remain.
```

The bridge only converts the filter to an owned temporary string and calls backend preparation synchronously. The alias keeps existing factory/engine signatures readable while eliminating mutable desktop-owned policy state.

- [ ] Add `MainWindowTest.loggingDefinitionErrorShowsContextAndLeavesStopped`: select a malformed channel, use the existing modal-driving pattern to capture and dismiss the Logging message box, and record `LOG_E`. Assert both contain protocol, ID, field, invalid value, and correction guidance; factory call count is zero, engine stopped, no active snapshot, and logging action/state restored. Include an invalid value containing `<b>bad</b>` and assert literal plain-text presentation.
- [ ] Add `MainWindowTest.loggingDisplayErrorContinuesOtherSamples`: deliver an unknown ID then valid `rpm`; assert the error is reported, the valid cache value updates, and the engine keeps running. Preserve the per-sample active-snapshot recheck because error signals can synchronously end the session.
- [ ] Run `bazel test --config=release //src/ui/desktop/widgets:test_mainwindow --test_filter='MainWindowTest.loggingDefinitionErrorShowsContextAndLeavesStopped:MainWindowTest.loggingDisplayErrorContinuesOtherSamples'`. Expect the contextual-dialog assertion to FAIL with the old generic message; the display-continuation characterization may already pass.
- [ ] Replace snapshot adapter policy with the bridge and alias. Change `apply_log_sample` to call `resolve_log_sample`, return its error or deliberate skip, and format/update only resolved values. Missing cache entries remain desktop `Internal` errors. Remove redundant policy tests from the adapter suite now that backend tests exist; retain fixed formatting, cache namespaces, cache failures, and one bridge integration case, including empty units.
- [ ] Update `continue_start_logging` to read ECU/TCU before calling preparation and pass the typed target. Show preparation error details through a `QMessageBox` configured with `Qt::PlainText`, retaining the existing log and UI restoration. Do not attach mutable target fields after preparation.
- [ ] Update engine and protocol factories to use `session()`, `response_offsets()`, and `target()`. Convert target to the existing SSM boolean at the transport boundary; retain capability lookup, CDBG setter/open ordering, MUT activation, thread ownership, and factory lifetimes.
- [ ] Migrate every directly constructed/mutated snapshot fixture in engine, registration, and MainWindow tests to preparation from a protocol-correct `LoggerModel`. Parameterize ECU/TCU inputs at creation; assert errors in test bodies before unwrapping. Keep helpers package-owned; do not add a public bypass constructor or private-access workaround to the new snapshot. Search all consumers with `rg -n 'DesktopLoggingSnapshot|activeLoggingSnapshot|target_is_ecu|identities_by_id|enabled_ids' src`.
- [ ] Change CSV access to `activeLoggingSnapshot->protocol_key()` while retaining current `loggerModel->selection()`. Retain or extend the existing shared-ID/current-protocol regression; do not read the snapshot selection for CSV. Keep all file-lifetime paths intact.
- [ ] Regenerate managed BUILD attributes. Run `bazel test --config=release //src/platform/desktop/common/logging:test_logging_adapters //src/platform/desktop/common/logging/runtime:test_logging_engine //src/platform/desktop/common/logging/runtime:test_logging_worker //src/platform/desktop/common/transport:test_desktop_logging_protocol_registration //src/ui/desktop/widgets:test_mainwindow`. Expect all PASS, including target capture, original SSM offsets, setup failures, and stale-worker generation tests. Reuse existing protocol/worker suites; no second wire implementation.
- [ ] Review generated changes for backend Qt dependencies, visibility widening, fixture workarounds, and unrelated edits. Commit this task's named files and BUILD paths: `refactor: bind desktop logging to validated backend runs`.

### Task 4: Record Contracts, Verify Gates, and Deliver

**Files:** [Logging contracts](../../reference/logging-contracts.md), [design notes](../../design-notes.md#logging), [technical debt](../../tech-debt.md#p1-separate-ui-from-application-logic), confirmed spec, this plan, and the existing glossary/index changes.

**Consumes:** Completed APIs and passing focused tests from Tasks 1–3.

- [ ] Update the owning logging reference with explicit numeric/format grammar, empty units, contextual startup errors, immutable run/typed target ownership, sample-resolution outcomes, and links to the new source owners. Keep current-selection CSV behavior explicit. Update design notes with concise rationale and link to the contract rather than duplicating it.
- [ ] Remove only the completed first P1 action from technical debt. Keep logger installation/persistence, CSV, logging reconfiguration, protocol evidence, and other unrelated debt. Review the glossary term against the implementation; no additional ADR is needed for applying the existing backend policy boundary.
- [ ] Run `python3 scripts/gazelle_check.py --fix` and review its output. Then run `bazel build --config=release //:fastecu`, `bazel test --config=release //...`, `prek run --all-files`, and `bazel run //:clang_tidy_report_changed`. Expect zero failures and no warnings or suppressions. If a gate is blocked by host setup, report that gate and cause; do not claim it passed. Applicable platform build/test/packaging and Sonar gates remain required in PR CI.
- [ ] Review the complete scoped diff against the spec, including field-context failures, batch atomicity, filtered channels, captured target, and literal dialog text. Record focused/full checks and platform limits for the PR. Stage named paths only and preserve unrelated Sonar reports and existing design/plan files.
- [ ] Preserve the original spec and plan in delivered Git history before removing completed artifacts. With squash merges, deliver their introduction in a separate documentation/history-preservation PR before the deletion PR; introducing and deleting them in one squash loses the originals. If their original contents already exist in retained delivered history, the completed implementation PR can remove them after extracting enduring knowledge.
- [ ] Commit enduring documentation and lifecycle changes with named paths. Prepare the feature-branch PR description from final behavior and evidence. Push/publish only when authorized, and leave unresolved platform or qualification evidence explicit.

## Execution Setup and Plan Review

- [ ] Before implementation, preserve the current documentation changes with named paths and use the git-worktrees skill to establish an isolated feature workspace. Never implement on `master`; never carry unrelated untracked reports/plans into commits.
- [ ] Self-review this plan against the confirmed spec: all input syntax, ownership, support/offset, target, CSV, error, and documentation-lifecycle decisions have tasks and tests above. Task 1 exposes one shared validator; Task 2 defines the run/resolver; Task 3 consumes those exact signatures. No parallel implementation boundary is needed because the desktop migration depends on both backend tasks.

This plan has not been executed. Application tests listed here are future implementation gates; writing this document requires only the repository's Markdown checks and scoped diff review.
