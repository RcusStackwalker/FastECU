# Backend Migration Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement authorized stages inline. Items 1 and 2 are authorized on separate reviewable branches/PRs; items 3–10 require later authorization. Steps use checkbox syntax for tracking.

**Goal:** Complete a staged migration of reusable policy into the portable backend, beginning with DefinitionCatalogSession and authoring header fields.

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
- Item 1 is based on master at 571ff6dd (PR #506). Item 2 is isolated on a branch based on item 1 at be3a08fd; items 3–10 remain unimplemented.

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

## Item 1 Execution Notes

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
- Items 2–10 remain unimplemented. Original untracked Sonar files remain untouched. At item 1 completion, worktree and local branch were retained without a push or PR. Subsequent authorization and PR creation are recorded below.

## Item 2 Authorization and Scope — 2026-10-05

After item 1, the user requested its PR and authorized item 2. Item 1 is
[PR #507](https://github.com/RcusStackwalker/FastECU/pull/507). Item 2 is isolated
on `refactor/backend-definition-header`, initially stacked on `be3a08fd`, in the retained
worktree. After #507 merged as `deb0f479`, item 2 was rebased onto master; the
merged item 1 tree is identical to the original base. The original item-1-only instructions and results above record that
completed stage; items 3–10 remain outside this branch's scope.

Move imported XML field extraction and named-field conversion/validation from
`definition_header_form.cpp` into a portable `definition_header_fields` helper.
Keep Qt widget access, string conversion, labels, filename handling and dialogs
in the UI. Preserve malformed/missing-field defaults, order, nested text,
first-child traversal depth, duplicate-name precedence, Unicode whitespace,
hexadecimal syntax/range and existing errors. Add portable tests and Qt
compatibility checks, then run Gazelle, formatting, static analysis, focused
and full release tests, application build and the portable closure gate.
No new runtime dependency, ErrorKind, worker or filesystem operation is added.

## Item 2 Verification — 2026-10-05

- Portable header tests and Qt differential tests cover ordering, blanks, duplicate fields, nested text/CDATA, traversal depth, Unicode trimming, hexadecimal syntax/range and exact existing errors.
- Test-first checks reproduced migration differences in multiple document roots, encoding declarations and XML entity handling. The portable implementation preserves the former Qt behavior, including internal entity expansion, malformed references, cycles and expansion limits, without external entity I/O.
- All 14 focused definition targets passed, including catalog, writer, authoring dialog and header form tests.
- Release application build passed. The final full release suite passed: 251 targets passed, seven Windows-only targets skipped on macOS.
- Gazelle check and changed-file formatting passed. Changed-file clang-tidy passed with zero findings after making an intentional concatenated test string explicit.
- Backend/algorithm dependency closure contains no platform targets.
- Independent review identified XML compatibility gaps; regression tests and fixes were added, and follow-up review found no further serious issues.
- Android gate remains unavailable: no local NDK was found and `ANDROID_NDK_HOME` is unset.
- Desktop ownership, synchronous flows and items 3–10 remain unchanged. Original untracked Sonar files remain untouched.


## Item 2 Review Revision — 2026-10-05

The user rejected QtXml as a test dependency and Qt parser equivalence as the
format contract. This supersedes the parser compatibility decisions in the
initial item 2 scope and verification above.

- Remove the QtXml differential oracle and the custom DTD entity preprocessing
  helper, including Qt-specific entity expansion budget and external-entity
  substitution tests. Use the existing non-validating pugixml parser directly;
  custom DTD entities are not expanded and no external resources are loaded.
- Keep explicit portable expectations for field order, blank defaults, nested
  text/CDATA, numeric/predefined references, duplicate precedence, Unicode
  whitespace and hexadecimal validation. Qt adapter tests use fixed expectations.
- Public author-maintained [EcuFlash definitions](https://github.com/TD-D/SubaruDefs/blob/Alpha/ECUFlash/subaru%20metric/B9%20Tribeca/D0XJ002B.xml)
  have a `rom` root; [RomRaider definitions](https://github.com/TD-D/SubaruDefs/blob/Alpha/RomRaider/RR_D2UH001L.xml)
  have `roms` with direct `rom` children. No reviewed source justifies an arbitrary
  first-child walk or a five-level limit. Accept these two explicit layouts,
  selecting the first direct ROM for this single-header form, and reject arbitrary
  wrappers. This is a form selection policy, not a claimed format rule.
- Regression tests failed on the old traversal for a non-ROM sibling before
  `rom` and arbitrary wrappers, then passed after explicit root selection.

### Revised Verification

- Focused portable header and UI adapter tests passed. Full release suite:
  251 targets passed, seven Windows-only targets skipped. Authoring dialog and
  MainWindow tests passed with the simplified backend.
- Release application build, regenerated Gazelle stability check, changed-file
  formatting and clang-tidy passed; four translation units, zero findings.
- Portable backend/algorithm closure contains no platform targets. The header
  helper and form test closure contains no QtXml target; the form test overrides
  the shared macro's broad default Qt dependencies with its explicit widget dependency.
- Independent scoped review found no concrete issues.
- Android gate remains unavailable because no local NDK is installed.
