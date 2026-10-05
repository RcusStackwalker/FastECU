# Backend Migration Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement the authorized item 1 inline. Steps use checkbox syntax for tracking. Items 2–10 require separate reviewable PRs and are not authorized for implementation in this worktree.

**Goal:** Complete a staged migration of reusable policy into the portable backend, beginning only with DefinitionCatalogSession.

**Architecture:** Backend owns portable policy and consumes injected ports. Desktop composition owns sessions, borrowed services, transports, workers and timers; UI owns presentation and operator decisions.

**Tech Stack:** C++23, Bazel, GoogleTest, existing pugixml and Qt desktop adapters.

**Spec:** The agreed user roadmap, recorded below; follow the [modularization plan](../../modularization-plan.md), [design notes](../../design-notes.md), and [coding style](../../coding-style.md).

## Global Constraints

- Staged full migration; separate reviewable PRs.
- Include locally demonstrated defects with regression tests.
- BIU/DataTerminal remain synchronous; retain desktop timer ownership.
- Terminal delay(n) becomes an ordered standalone pause for both buses.
- Preserve logger CSV formatting and documented model ownership.
- Defer hardware-dependent five-baud and wrx02 corrections.
- Backend stays Qt-free, thread-free, and free of direct filesystem I/O.
- No new runtime dependencies or ErrorKind values.
- Implement item 1 only in this branch, based on master at 571ff6dd (PR #506).

## Agreed Roadmap

1. DefinitionCatalogSession: platform → backend/definition.
2. Definition XML header extraction/validation: UI → backend/definition.
3. Calibration field resolution, patch application, selectable encoding: UI → backend/calibration/session.
4. Logging snapshots/channel preparation/sample validation: platform → backend/logging.
5. Logger configuration/model installation/selection persistence: MainWindow → backend/logging.
6. Logger CSV column resolution/serialization: UI → backend/logging.
7. Pure serial framing/checksums/header interpretation: platform → algorithms/protocol.
8. Flash routing, preparation, prompts and stage policy: platform → backend/flash; desktop binds plans to transports/workers.
9. BIU decoding/settings/session policy: UI → portable diagnostics and BIU codecs.
10. DataTerminal parsing/validation/execution: UI → backend/diagnostics.

## Review Focus

- Fresh catalogs must not replace startup provenance; deleted indexed files retain the established ROM-open notice.
- Ordered lookup retains first-match behavior even when a fresh catalog rejects conflicting identities.
- Failed refresh and empty configured sources retain indexes; a successful scan containing only unusable files clears them.
- Failed writes and invalid headers never register destinations; successful writes register outside-directory handles.
- Desktop composition keeps consumers shorter-lived than the session and its borrowed services.

## Task 1: Move DefinitionCatalogSession into the backend

**Files:**
- Move `src/platform/desktop/common/definition/definition_catalog_session.{h,cpp}` and `definition_catalog_session_test.cpp` to `src/backend/definition/`.
- Modify both definition packages' BUILD files, desktop composition, MainWindow/services, authoring dialog consumers and their tests.
- Keep `src/platform/desktop/common/definition/definition_catalog_session_integration_test.cpp` in its Qt platform package.
- Update ownership in the [design notes](../../design-notes.md) and [modularization plan](../../modularization-plan.md).

**Interfaces:** Keep `calibration::IDefinitionCatalogs`, every method, constructor dependency and behavior. Change only the class namespace from `fastecu::desktop::definition` to `fastecu::definition`. Dependencies remain `DefinitionService&`, `config::ConfigSession&`, `IFileSystem&`, `IEventSink&`; composition continues owning the session and borrowed services.

- [x] Reverify baseline catalog, logging adapter, map edit adapter and flash workflow tests in the worktree.
- [x] Relocate portable characterization tests and switch their include/namespace to the backend API before moving production sources; verify the existing platform dependency cannot satisfy the backend test boundary.
- [x] Move production sources without behavioral changes, add the backend `cc_library` and `fastecu_portable_gtest`, remove old production/unit-test targets, and bind platform integration tests to the backend target.
- [x] Update all consumer includes, forward declarations, namespace qualifications and BUILD references; update ownership documentation.
- [x] Regenerate Gazelle with `python3 scripts/gazelle_check.py --fix`, review output, and verify a second run is unchanged.
- [x] Run catalog unit/integration, authoring dialog, MainWindow, desktop composition, logging adapter, map edit adapter and flash workflow tests.
- [x] Run formatting hooks and changed-file clang-tidy, then `bazel build --config=release //:fastecu` and `bazel test --config=release //...`.
- [x] Verify `filter('^//src/platform/', deps(//src/backend/... + //src/algorithms/...))` is empty. Run `scripts/android-cross-compile.sh` if an NDK is available; otherwise record the limitation.
- [x] Review the scoped diff, commit on the feature branch, and deliver worktree/branch, plan and verification results. Push/open a PR only when authorized.

## Execution Notes

This is a mechanical ownership migration with existing regression coverage, not a behavioral rewrite. No defect correction or implementation of items 2–10 is included. The original checkout's three untracked Sonar files remain untouched.

## Item 1 Verification — 2026-10-05

- Baseline reverified in the new worktree: catalog unit/integration, logging adapters, map edit adapter and flash workflow; five targets passed.
- Test-first boundary check: the relocated backend test failed because the old platform production target was not visible to backend. After relocation, the backend portable target passes without widening visibility.
- Focused verification: eight targets passed. Catalog unit tests: 13; Qt catalog integration: 9; authoring dialog: 7; MainWindow: 88; desktop composition: 21.
- Gazelle regenerated with `--fix`; a second check passed unchanged. Changed-file `prek` checks passed after formatting.
- Release application build passed: `bazel build --config=release //:fastecu`.
- Full release suite passed: `bazel test --config=release //...`; 250 targets passed, seven Windows-only targets skipped on macOS.
- Changed-file clang-tidy passed with LLVM 23.1.0: nine translation units clean, zero findings. The header-only MainWindow services file has no co-located source; changed composition and MainWindow consumers were included in analysis.
- Portable dependency closure query returned no platform targets.
- Android cross-compilation was not run: `ANDROID_NDK_HOME` is unset and no NDK was found in the checked local SDK locations.
- Independent diff review found no issues. Comparison against the original catalog production/test files confirms only relocation, namespace, formatting, redundant using declarations and the ownership comment changed.
- Items 2–10 remain unimplemented. Original untracked Sonar files remain untouched. Worktree and local branch are retained; no push or PR creation performed.
