# 6n-3 Desktop Byte Boundary Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Finish desktop closure by moving the Qt byte helper out of algorithms without changing conversion behavior.

**Architecture:** A header-only `:qt_bytes` target under `src/platform/desktop/common/bytes` holds the existing overloads and conversions. Desktop platform packages, the root integration test, and two named UI packages consume it directly; algorithms and backend remain Qt-free. Remove the transitional algorithms package and its Qt visibility exception.

**Tech Stack:** C++23, Qt 6 Core, Bazel, GoogleTest, Gazelle, repository pre-commit checks.

**Spec:** [6n-3 desktop byte boundary design](../specs/2026-09-29-desktop-byte-boundary-design.md)

## Global Constraints

- Preserve every `bytes` namespace helper signature and function body in `qt_bytes.h`, including byte order and trailing-space hex output.
- `view` and `mutableView` remain non-owning; `fromQByteArray` and `toQByteArray` remain owning copies.
- Keep Qt buffers at existing desktop call sites; add no backend port, compatibility alias, or forwarding header.
- Target-level visibility permits platform, root tests, `src/ui/desktop`, and `src/ui/desktop/biu` only. Algorithms and backend gain no Qt grant.
- Do not claim hardware, packaging, or Windows/Linux runtime qualification from this move.

## Review Focus

- An empty `QByteArray` converts to empty views and owned buffers without dereference: add explicit assertions in Task 1.
- A byte sequence containing `0x00` and `0xFF` retains both values and size across owning conversions: add a round-trip assertion in Task 1.
- A `mutableView` write changes its source `QByteArray`, while a `fromQByteArray` result does not follow later source changes: add assertions in Task 1.
- A short `QByteArray` write overload leaves the buffer unchanged: add an assertion in Task 1.
- A backend or algorithms target cannot depend on the Qt helper: inspect the target-level visibility in Task 2 and build `//:portable_closure` in Task 3 to confirm portable roots stay platform-free.

---

### Task 1: Relocate helper and tests

**Files:**
- Move: `src/algorithms/protocol/qt_compat/qt_bytes.h` → `src/platform/desktop/common/bytes/qt_bytes.h`
- Move and extend: `src/algorithms/protocol/qt_compat/bytes_qt_compat_test.cpp` → `src/platform/desktop/common/bytes/qt_bytes_test.cpp`
- Create: `src/platform/desktop/common/bytes/BUILD.bazel`
- Delete after Task 2 migration: `src/algorithms/protocol/qt_compat/BUILD.bazel`

**Interfaces:**
- Produces: `//src/platform/desktop/common/bytes:qt_bytes`, exporting `qt_bytes.h` and every existing `bytes` overload with unchanged signatures.
- Visibility: `//bazel/layers:platform`, `//bazel/layers:tests`, `//src/ui/desktop:__pkg__`, `//src/ui/desktop/biu:__pkg__`; no package-wide UI grant.

- [x] **Step 1: Record baseline.** Run `shasum -a 256 src/algorithms/protocol/qt_compat/qt_bytes.h` and `bazel test --config=release //src/algorithms/protocol/qt_compat:test_bytes`; record hash and PASS in execution notes.
- [x] **Step 2: Move the helper and test.** Copy the header without editing its contents. Move the test, update its include to the new path, and retain existing portable and Qt cases. Create a `cc_library` with `hdrs = ["qt_bytes.h"]`, `deps = ["//bazel/qt:core", "//src/algorithms/protocol"]`, and the target-level visibility above. Use `fastecu_gtest(name = "qt_bytes_test", srcs = ["qt_bytes_test.cpp"], deps = [":qt_bytes", "//bazel/qt:core", "//src/algorithms/protocol", "@googletest//:gtest"])` for the co-located test.
- [x] **Step 3: Add focused boundary assertions.** In `qt_bytes_test.cpp`, add tests named `emptyConversions`, `ownedRoundTripPreservesBinaryBytes`, `mutableViewAliasesSource`, `fromQByteArrayCopiesSource`, and `shortQByteArrayWriteIsNoOp`. Assert exact sizes and bytes, including `0x00`/`0xFF`; mutate the source after the owning conversion and assert the copy retains its original bytes. Keep the existing trailing-space and endian tests.
- [x] **Step 4: Verify the new target.** Run `bazel test --config=release //src/platform/desktop/common/bytes:qt_bytes_test`; expect PASS. Re-run the header hash at its new path; expect the Step 1 digest. Keep the old package until Task 2 replaces its consumers.

### Task 2: Migrate consumers and retire transitional visibility

**Files:**
- Modify: active `qt_bytes.h` includes in `src/platform/desktop/common/{serial,transport,diagnostics}`, `src/ui/desktop`, `src/ui/desktop/biu`, and `tests/tst_mut_dma_integration.cpp`
- Modify: corresponding `BUILD.bazel` files in those packages and `tests/BUILD.bazel`
- Modify: `bazel/qt/BUILD.bazel`
- Delete: `src/algorithms/protocol/qt_compat/BUILD.bazel` and its now-empty package directory

**Interfaces:**
- Consumes: `//src/platform/desktop/common/bytes:qt_bytes` from Task 1.
- Produces: no active include or Bazel label referring to `src/algorithms/protocol/qt_compat`; `qt_layer` contains no algorithms or backend entry.

- [x] **Step 1: Replace includes and labels.** Replace each active old header include with `src/platform/desktop/common/bytes/qt_bytes.h` and each old dependency with `//src/platform/desktop/common/bytes:qt_bytes`. Remove the old BUILD file and remove `//src/algorithms/protocol/qt_compat` from `qt_layer`.
- [x] **Step 2: Regenerate and inspect Bazel files.** Run `python3 scripts/gazelle_check.py --fix`, inspect the BUILD diff for the intended labels and visibility, then run `python3 scripts/gazelle_check.py`; expect no drift. Do not accept unrelated generated changes.
- [x] **Step 3: Check active references and affected tests.** Run `rg -n 'src/algorithms/protocol/qt_compat' src apps tests --glob '!*.md'`; expect no matches. Run `bazel test --config=release //src/platform/desktop/common/bytes:qt_bytes_test //src/platform/desktop/common/serial/... //src/platform/desktop/common/transport/... //src/platform/desktop/common/diagnostics/... //src/ui/desktop/... //tests:mut_dma_integration_tests`; expect all compatible targets to pass.
- [x] **Step 4: Commit the relocation.** Run `git diff --check`, inspect `git status --short`, and commit the migration as `refactor(desktop): move Qt byte conversion to desktop boundary`.

### Task 3: Verify closure and record status

**Files:**
- Modify: `CLAUDE.md`, `docs/coding-style.md`, `docs/modularization-plan.md`, `docs/design-notes.md`, and `docs/tech-debt.md`
- Modify: this plan's checkboxes and execution notes

**Interfaces:**
- Consumes: the completed `:qt_bytes` migration from Task 2.
- Produces: accurate step 6 structural status, with qualification limits recorded separately.

- [x] **Step 1: Run closure and repository gates.** Run `bazel build --config=release //:portable_closure //apps/desktop:fastecu`, `bazel build -k --config=release //...`, `bazel test -k --config=release //...`, `prek run --all-files`, and `bazel run --config=release //:clang_tidy_report_changed`; expect no failures caused by this slice. Record platform skips and any pre-existing failures accurately.
- [x] **Step 2: Update current documentation.** Mark 6n-3 and step 6 structurally complete in `docs/modularization-plan.md` only if Step 1 passes. Update current path and status guidance in `CLAUDE.md`, `docs/coding-style.md`, `docs/design-notes.md`, and `docs/tech-debt.md`. Leave historical ADR decisions intact; add a dated subsequent note only if necessary to prevent their old paths from reading as current guidance. Keep hardware and packaging status separate.
- [x] **Step 3: Review and commit the record.** Run `git diff --check`, inspect `git diff --stat` and `git status --short`, then commit documentation and this plan's execution notes as `docs: record desktop byte boundary closure`.

## Execution Notes

Executed on macOS from `work/6n-3-desktop-byte-boundary` in an isolated worktree.

- Baseline `//src/algorithms/protocol/qt_compat:test_bytes` passed. The new target failed on its missing header before the header was copied, then passed. `qt_bytes.h` SHA-256 before and after relocation: `0ec100f0e944615db48c7d7fb0b8a57e6326ea10a964f2b6955659dae31491ad`.
- Removing the old package caused the expected missing-package analysis failure in the transport package. After migration, Gazelle consistency passed; the affected suite passed 41 tests with two Windows-only skips.
- Portable closure and desktop app build passed. Release `//...` build passed for 770 targets. Release `//...` tests passed: 237 pass, seven platform skips on macOS.
- `prek run --all-files` passed. Changed-file clang-tidy analyzed 17 translation units with zero findings. Its report noted that `diagnostic_link_io.h` has no co-located source in the compile database, so its includers were not analyzed by that specific command.
- No hardware bench, Windows/Linux runtime, or packaging qualification was performed for this slice. Step 6 is structurally complete; those release qualifications remain separate.
