# 6n-2 Kernel Model Ownership Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the last portable kernel headers into flash ownership and retire `src/backend/definitions` without changing device data or flash behavior.

**Architecture:** Add separate header-only `memory_models` and `commands` targets under `src/backend/flash/kernel`. Migrate existing flash includes and dependencies to `memory_models`; keep the command header available without manufacturing a consumer. Preserve the header bytes and all existing lookup interfaces.

**Tech Stack:** C++23, Bazel, GoogleTest, Gazelle consistency check, repository pre-commit checks.

**Spec:** `docs/superpowers/specs/2026-09-29-kernel-model-ownership-design.md`

## Global Constraints

- Preserve both header files byte-for-byte through relocation, including names, values, table order, pointer relationships, and sentinel.
- Keep `flash_device_lookup` pointer and index APIs unchanged.
- Do not change executor-local wire constants, runtime behavior, or 6n-3 Qt byte conversions.
- No forwarding alias or compatibility header remains in `src/backend/definitions`.
- New targets have backend-and-above visibility and no Qt or platform dependency.
- Keep hardware, packaging, and Windows/Linux runtime qualification separate from structural verification.

## Review Focus

- A missing device name still returns `-1` and a known name still returns its table index: covered by existing `flash_device_lookup_test` in Task 1.
- The table's final null-name sentinel still terminates lookup: covered by existing `flash_device_lookup_test` and byte comparison in Task 1.
- Device block offsets, lengths, ROM sizes, and enum values remain exactly as before: covered by byte-for-byte comparison and existing flash plan tests in Task 1.
- Kernel command and error macros retain exact values despite no active include consumers: covered by byte-for-byte comparison in Task 1.
- A flash target that includes the moved header has the correct Bazel dependency: covered by affected-target and release builds in Task 1.

---

### Task 1: Relocate kernel headers and migrate flash consumers

**Files:**
- Move: `src/backend/definitions/kernelmemorymodels.h` → `src/backend/flash/kernel/kernelmemorymodels.h`
- Move: `src/backend/definitions/kernelcomms.h` → `src/backend/flash/kernel/kernelcomms.h`
- Create: `src/backend/flash/kernel/BUILD.bazel`
- Delete: `src/backend/definitions/BUILD.bazel`
- Modify: active includes and old-label dependencies under `src/backend/flash/`, especially `src/backend/flash/BUILD.bazel`, `src/backend/flash/ecu/BUILD.bazel`, and `src/backend/flash/flash_device_lookup.h`
- Test: `src/backend/flash/flash_device_lookup_test.cpp` and existing `src/backend/flash/ecu/*_test.cpp`

**Interfaces:**
- Produces: `//src/backend/flash/kernel:memory_models` with header `src/backend/flash/kernel/kernelmemorymodels.h`; `//src/backend/flash/kernel:commands` with header `src/backend/flash/kernel/kernelcomms.h`.
- Preserves: `fastecu::flash::find_flash_device(std::string_view) -> const flashdev_t *` and `find_flash_device_index(std::string_view) -> int`.

- [ ] **Step 1: Capture baseline header hashes and focused tests.** Run `shasum -a 256 src/backend/definitions/kernelmemorymodels.h src/backend/definitions/kernelcomms.h` and save the two digest values in the execution notes. Run `bazel test --config=release //src/backend/flash:flash_device_lookup_test //src/backend/flash/ecu/...`; expect all available targets to pass.
- [ ] **Step 2: Relocate headers and create targets.** Move both files without editing their contents. Add `cc_library(name = "memory_models", hdrs = ["kernelmemorymodels.h"])` and `cc_library(name = "commands", hdrs = ["kernelcomms.h"])` with package default visibility `//bazel/layers:backend_and_above`; remove old BUILD and empty directory.
- [ ] **Step 3: Migrate consumers.** Replace exact old header include paths with `src/backend/flash/kernel/kernelmemorymodels.h`; replace old `//src/backend/definitions:models` dependencies with `//src/backend/flash/kernel:memory_models`. Update stale source/test path comments that cite moved headers; leave data and wire literals untouched.
- [ ] **Step 4: Prove data and references are preserved.** Re-run `shasum -a 256` on the new headers and compare with Step 1. Run `rg -n '#include \"src/backend/definitions/|//src/backend/definitions:models' src`; expect no active includes or Bazel labels. Check remaining historical comments separately and update navigable path references to the new location. Run `python3 scripts/gazelle_check.py --fix` followed by `python3 scripts/gazelle_check.py`; expect no drift. Inspect any Gazelle changes before committing.
- [ ] **Step 5: Run affected tests and commit.** Run `bazel test --config=release //src/backend/flash:flash_device_lookup_test //src/backend/flash/ecu/...`; expect PASS. Run `git diff --check` and commit only the migration with message `refactor(flash): move kernel models into flash ownership`.

### Task 2: Verify closure and record slice status

**Files:**
- Modify: `docs/modularization-plan.md`
- Modify: `docs/design-notes.md` if a short ownership rationale is needed to distinguish kernel models from definition catalogs.
- Modify: this execution plan's checkboxes/results.

**Interfaces:**
- Consumes: migrated `//src/backend/flash/kernel:{memory_models,commands}` from Task 1.
- Produces: accurate 6n-2 completion status; 6n-3 remains open.

- [ ] **Step 1: Run repository verification.** Run `bazel build -k --config=release //...`, `bazel test -k --config=release //...`, and `bazel build --config=release //:portable_closure //apps/desktop:fastecu`; expect build and tests to pass, while recording any platform-only skips. Run `prek run --all-files` and the repository's changed-source clang-tidy target; expect no findings caused by this slice.
- [ ] **Step 2: Update documentation.** Mark 6n-2 implemented in `docs/modularization-plan.md`; preserve 6n and 6n-3 as open. State that kernel models now live in `src/backend/flash/kernel`, with no data changes. Record actual verification and qualification limits here in the execution notes.
- [ ] **Step 3: Inspect and commit.** Run `git diff --check`, inspect `git diff --stat` and `git status --short`, then commit documentation and execution record as `docs: record kernel model ownership migration`.

## Execution Notes

To be filled during execution with baseline/new hashes, command results, platform skips, and qualification limits. Do not pre-mark any task complete.
