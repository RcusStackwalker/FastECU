# Macro-to-constant migration implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement the constants workstream of issue #499 without changing values, conversions, or ABI.

**Architecture:** Replace object-like numeric macros with namespace-scope inline constexpr constants, using the repository's kCamelCase naming. Keep the existing platform headers and ABI structs; migrate corresponding Windows constants together so shared callers have one spelling.

**Tech Stack:** C++23, Qt 6, Bazel, GoogleTest.

**Spec:** [Issue #499, W4 in its linked design](https://github.com/RcusStackwalker/FastECU/issues/499).

## Global constraints

- Preserve numeric values and original literal integral types (int, unsigned int for the two high-bit voltage sentinels, quint64 for the chunk mask).
- Preserve serial timing IDs separately from J2534 configuration IDs.
- Use collision-free serial status names and explicit empty QByteArray returns.
- Preserve ABI structs, calling conventions, hardware behavior, and analysis exclusions.
- Treat scanner confirmation and other-platform CI as separate evidence from local checks.

## Review focus

- Windows SDK macro collisions and shared Windows/Unix consumers.
- Zero-valued return expressions that previously selected pointer constructors.
- Flag combinations, aliases, high-bit sentinels, and ABI layout.
- Unsigned hex-editor alignment mask and boundary edits.
- Preprocessor consumers and dormant invalid definitions.

## Task 1: Migrate constants and their consumers

- [x] Inventory all 178 baseline anchors, active macros, preprocessor uses, and C consumers.
- [x] Run existing direct-backend, Unix J2534, and hex-editor tests for regression coverage.
- [x] Capture old numeric values/types and ABI layouts for independent compile-time comparison.
- [x] Replace the six QByteArray success returns with explicit empty values; retain integer/bool behavior.
- [x] Convert facade codes, both J2534 platform headers, chunks.cpp constants, and MainWindow restart code; update C++ tokens without changing diagnostic strings.
- [x] Remove the unused invalid TX_PARAM_STOP_BITS definition rather than invent a value; retain supported IDs.
- [x] Verify every migrated value/type and ABI layout against the captured baseline; run focused tests.

## Task 2: Validate and deliver

- [x] Record all 178 Sonar keys and their source-level actions, pending fresh analysis confirmation.
- [x] Run release build/test, pre-commit, Gazelle, changed-file clang-tidy, and macOS packaging where available.
- [x] Obtain independent whole-change review and address material findings.
- [ ] Commit, push the feature branch, create a PR referring to #499, and report remaining CI/analysis limitations.

## Execution record

- Implementation base: 6b17dbf9. Existing user triage files remain untouched in the original checkout.
- The user explicitly requested this workstream and a PR; proceed with native implementation.
- No in-repository preprocessor or C consumers of the migrated numeric macros were found.
- TX_PARAM_STOP_BITS is unused and expands an undefined TX_PARAM_BASE_BASE. Remove it; Windows already comments it out. Guessing a vendor ID is outside this migration.
- Local Sonar scanner/build-wrapper is unavailable; PR CI provides fresh analysis. Do not claim baseline findings resolved until that evidence exists.

- Focused serial/J2534/hex-editor tests: six targets passed, one Windows target skipped.
- Full `bazel test --config=release //...`: 247 targets passed, seven platform-specific targets skipped.
- Temporary compile-time checks compared all 342 retained header constants with original macro expansions, including exact integral types. J2534 layout/value/type checks also passed with Clang targets i686-pc-windows-msvc, x86_64-pc-windows-msvc, x86_64-unknown-linux-gnu, and native arm64 macOS. A simulated pre-existing STATUS_SUCCESS macro did not affect the facade header.
- All pre-commit checks and Gazelle passed. The release application build passed.
- Independent review found no material findings; two stale comments were corrected. Token/literal comparison confirmed diagnostic strings were unchanged.
- Per-finding source actions are recorded in the [constants ledger](../../sonar-constants-2026-10-04.json). All 178 entries await fresh Sonar confirmation; no server dispositions or exclusions changed.
- Changed-file clang-tidy with LLVM 23: 16 translation units clean, zero findings.
- macOS packaging passed with the matching Bazel Qt 6.8.3 deployment tool (zero deployment errors). The first run used Homebrew Qt and emitted dependency-resolution errors despite exit zero; its archive was not used as validation evidence.
