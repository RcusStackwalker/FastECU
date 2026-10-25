# Logging Run Preparation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Concentrate desktop logging protocol choices and snapshot preparation behind one pure interface.

**Architecture:** A Qt-free UI-owned preparation module returns an owned engine registration key and backend logging run snapshot. MainWindow consumes those values through the existing run owner; the conversion-only snapshot adapter and its alias are removed.

**Tech Stack:** Qt 6, C++23, GoogleTest, Bazel 9.3.0, Gazelle, prek, clang-tidy.

**Spec:** [Approved logging run preparation design](../specs/2026-10-25-logging-run-preparation-design.md).

## Global Constraints

- Keep engine dispatch and `Start(config, snapshot)` behavior unchanged, including custom registration keys.
- Exact `MUT_DMA`/`CDBG` strings choose those protocols; every other string chooses SSM with the supplied definition filter.
- Preserve literal policies: MUT/CDBG `50ms, 20, 100, 20`; SSM `300ms, 10, 30, 10`.
- Keep backend validation, error detail/order, capture, identification, run lifecycle, CSV and presentation with their current owners.
- New preparation and tests have no Qt/platform/worker dependency; use existing visibility groups and package-owned GoogleTest suites.
- Use UI camelBack function/local names and `.has_value()` for production results; assertions use repository matchers.
- Preserve unrelated `.clwb/` contents. Work on `refactor/logging-run-preparation`; do not push without authorization.

## Review Focus

- A saved protocol different from the SSM filter must preserve both meanings; Task 1 uses a custom filter and an unrelated saved string.
- Case changes/whitespace/empty saved strings must retain fallback; Task 1 uses literal fallback cases.
- Invalid filter and target together must preserve backend error precedence; Task 1 compares the existing filter-first error.
- A snapshot must outlive temporary choices and later model changes; Task 1 destroys input text and edits the model after capture.
- A start callback may end or replace the run; Task 2 retains owner reentrancy cases and uses the current active snapshot for display refresh.

---

### Task 1: Add the pure preparation module

**Files:** Create `src/ui/desktop/logging/logging_run_preparation.h`, `.cpp`, `_test.cpp` and `BUILD.bazel`.

**Interfaces:** Produce `fastecu::ui::LoggingRunChoices` with `saved_protocol`, `ssm_definition_filter` (`std::string_view`) and `target` (`logging::LoggingTarget`); `PreparedLoggingRun` with owned `registration_key` and `logging::LoggingRunSnapshot`; and `Result<PreparedLoggingRun> prepareLoggingRun(const logging::LoggerModel&, LoggingRunChoices)`.

- [ ] Write interface tests first. A literal routing table asserts returned registration key, snapshot protocol/filter and all four policy fields for MUT_DMA, CDBG, SSM, unknown, empty, lowercase and padded saved strings. Test both targets, an independently named SSM definition filter, ignored empty filters for dedicated protocols, unchanged invalid-input errors and owned capture. Use real backend preparation and a small valid one-measurement model.
- [ ] Run `bazel test --config=release //src/ui/desktop/logging:logging_run_preparation_test`; confirm failure because the preparation interface is not implemented. Add ordinary package declarations as needed for Bazel to reach the test.
- [ ] Implement the one function: resolve the three exact routes and fixed policies, delegate once to backend `PrepareLoggingRun`, propagate its error unchanged, return owned matching values. Add no validation or I/O.
- [ ] Regenerate targets with `python3 scripts/gazelle_check.py --fix`; review the new target's Qt-free closure and existing visibility groups.
- [ ] Run the new suite plus `//src/backend/logging:logging_run_snapshot_test`; require all cases pass, and commit the module and its tests.

### Task 2: Integrate preparation and remove the shallow adapter

**Files:** Modify `src/ui/desktop/widgets/{menu_actions.cpp,mainwindow.h,mainwindow_test.cpp,desktop_logging_run.h,desktop_logging_run_test.cpp}`, `src/platform/desktop/common/logging/{logging_value_adapter.h,logging_value_adapter.cpp,logging_adapters_test.cpp}`, runtime engine declarations/definitions/tests, protocol registration/tests, `apps/desktop/desktop_composition_test.cpp` and generated BUILD files. Delete `src/platform/desktop/common/logging/logging_snapshot_adapter.{h,cpp}`. Update `docs/reference/logging-contracts.md` and `docs/design-notes.md`.

**Interfaces:** Consume Task 1's `prepareLoggingRun(model, choices)` result; supply converted `registration_key` and moved `snapshot` to existing `DesktopLoggingRunStart`. Replace the alias with the identical `logging::LoggingRunSnapshot` type everywhere.

- [ ] Strengthen `loggingUsesTheSessionLogProtocol` before integration: assert the current active snapshot's `ProtocolKey()` and session protocol, with saved CDBG and a distinct stale UI filter; run the focused test to establish current behavior.
- [ ] Replace MainWindow's protocol/policy branching with the preparation call. Preserve detailed preparation error handling and generic startup failure handling; obtain post-start rendering's filter from the current active snapshot. Remove the redundant member and old include.
- [ ] Delete the adapter files; mechanically migrate alias uses and direct snapshot includes. Tests exercising values/caches prepare snapshots through the backend with their existing custom policies; retain all display assertions and rename suites referring to the deleted adapter.
- [ ] Update the logging contract with desktop default preparation, preserved fallback and independent engine dispatch; link the design note to that owner. Regenerate BUILD files and inspect explicit dependencies without visibility changes.
- [ ] Run the preparation/backend snapshot/value adapter/engine/registration/desktop-run/MainWindow/composition suites. Preserve custom `TEST` registration with SSM snapshots and existing failure/reentrancy cases. Require all pass.
- [ ] Run `bazel build --config=release //:fastecu`, `bazel test --config=release //...`, `prek run --all-files`, `python3 scripts/gazelle_check.py --fix` and applicable `bazel run //:clang_tidy` analysis; report unavailable host gates precisely. Review the whole branch with a fresh reviewer, address important findings, and commit the integration.

## Delivery

Keep original design/plan contents in retained Git history before retirement;
their removal must not be folded into a squash that loses those originals.
Extract enduring ownership into the logging contract. Platform packaging remains
a CI gate. Do not merge or push without authorization.
