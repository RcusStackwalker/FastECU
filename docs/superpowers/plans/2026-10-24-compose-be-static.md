# Compile-time-sized ComposeBe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Return allocation-free fixed arrays for statically sized composition and migrate suitable callers without changing wire bytes.

**Architecture:** Select array versus vector using argument types and encoded widths. Encode directly into array storage on the static path, retaining the existing explicit vector helper and sharing encoding logic where practical.

**Tech Stack:** C++23, GoogleTest, Bazel, existing first-party formatting and analysis tools.

**Spec:** [Approved design](../specs/2026-10-24-compose-be-static-design.md).

## Global Constraints

- Keep the implementation portable, header-only, Qt-free, and compatible with the existing C++23 Bazel graph and warnings-as-errors policy.
- Preserve supported field types and encoding behavior.
- Statically zero-length output is a compile-time error; dynamically empty output remains valid.
- `ComposeBeWithExtraCapacity` always returns `Bytes`, even with zero extra capacity.
- Fixed-size construction must not allocate, including temporary allocations.
- Preserve transmitted bytes and hardware qualification status.
- Use a feature branch; preserve planning artifacts in delivered history before eventual removal.

## Review Focus

- Const-qualified and zero-extent static ranges must retain correct widths and type selection.
- Mixed static/dynamic inputs must select vectors even when the dynamic input is empty.
- Unsized byte input ranges must remain consumable without a sizing pass.
- Checksum callables must see the payload only, including an empty payload.
- Callers with append operations or vector-owning interfaces must retain valid ownership and lifetime.

## Files and ownership

- `src/algorithms/protocol/bytes_compose.h`: width classification, encoding, public helpers.
- `src/algorithms/protocol/bytes_compose_test.cpp`: return-type, byte, boundary, and constexpr coverage.
- `src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.cpp`: identified fixed-frame migrations; retain package-owned protocol tests.
- Existing callers under `src/algorithms`, `src/backend`, and `src/platform`: compatibility migrations discovered by `rg -n 'ComposeBe|Write(U16|U24|U32)Be' src` and the full build.
- `docs/coding-style.md`: owning byte construction convention and examples.
- Managed `BUILD.bazel` files: regenerate only if source/dependency ownership changes.

### Task 1: Static composition and affected-caller compatibility

**Interfaces:** Preserve `ComposeBeWithExtraCapacity(std::size_t, const Args&...) -> Bytes`; change `ComposeBe(const Args&...)` to a deduced array/vector result. Preserve the checksum helper's arguments and deduce its result by the same rule.

- [x] Add failing tests in `bytes_compose_test.cpp`: `ComposeBe(0x12_b, std::uint16_t{0x3456})` has type `std::array<bytes::Byte, 3>` and bytes `{0x12, 0x34, 0x56}`; verify with `static_assert` in constant evaluation.
- [x] Cover byte and wide-integer arrays, built-in arrays, mutable/const fixed spans, `U24` truncation, and existing supported range widths. Replace the fixed-array `.capacity()` assertion with array-type and byte assertions; retain capacity checks on the explicit vector helper.
- [x] Cover `ComposeBe(0x12_b, empty_dynamic_view)` returning `Bytes{0x12}`, dynamically empty composition returning empty `Bytes`, and a zero-length static array beside a nonempty scalar yielding a one-byte array. Include an unsized byte input range to prove sizing does not consume its elements.
- [x] Cover constexpr checksum composition, checksum-only output, supported checksum widths, and dynamic checksum output. Assert the callback receives precisely the payload bytes, excluding suffix storage.
- [x] Run `bazel test --config=release //src/algorithms/protocol:bytes_compose_test`; confirm the new array-type/constexpr checks fail against the old implementation.
- [x] Implement type-known width detection and direct static encoding in `bytes_compose.h`; make the static path constexpr and reject zero total output length. Keep supported scalar/range types unchanged and preserve the dynamic single-allocation reservation strategy for sized inputs.
- [x] Verify compile-time rejection of `ComposeBe()` and only-empty static ranges using negative compile probes with the configured compiler; confirm a positive probe compiles first so environment failures cannot masquerade as rejection. Existing project compile-fail infrastructure may be reused if found; do not add a new framework for two probes.
- [x] Run the focused test again; expect PASS, including compile-time assertions. Inspect the static path to confirm it cannot construct a vector or allocate temporary storage.
- [x] Run `bazel build --config=release //:fastecu` and `bazel test --config=release //...`; use failures plus the complete caller search to migrate affected declarations, returns, and appending callers. Keep vector interfaces on the explicit vector construction path where changing the interface would broaden scope. Preserve independent test byte expectations.
- [x] Update `docs/coding-style.md` to describe automatic selection, supported static extents, zero-output behavior, constexpr construction, checksum parity, and explicit vector construction; replace the existing fixed-frame `Bytes` example with `auto`.
- [x] Rerun the focused test and affected package tests; expect PASS. Commit named changed paths as one coherent API-and-compatibility change.

### Task 2: Fixed-frame construction cleanup and delivery gates

**Interfaces:** Consume Task 1's array-returning `ComposeBe`; preserve all existing protocol function signatures and wire results.

- [ ] In `mitsu_colt_can_cdbg_protocol.cpp`, replace seed/key array construction followed by `WriteU32Be`, the security-key frame write at offset 2, logging-start write at offset 6, and pointer-frame write at offset 4 with equivalent typed composition.
- [ ] Inspect the repository search for equivalent straightforward fixed-frame constructions. Convert construction-only writes where the resulting code is clearer; retain intentional patches into existing buffers. Review zero padding and every field width against the original construction.
- [ ] Run package-owned tests for every migrated protocol/backend package using its existing Bazel test labels. Retain independently specified expected bytes and add a missing frame assertion only if existing tests do not exercise a migrated construction.
- [ ] Run `python3 scripts/gazelle_check.py --fix`, review generated changes, then run `prek run --all-files`, `bazel build --config=release //:fastecu`, `bazel test --config=release //...`, and `bazel run //:clang_tidy`; require successful results or report concrete environmental blockers. Platform packaging/CI remain required delivery gates.
- [ ] Review the final diff for temporary allocations, changed wire fields, unnecessary vector conversions, and array/span lifetime regressions. Commit named changed paths, preserving the original planning documents in delivered history before any separate cleanup/deletion change. Do not push without authorization.
