# CAN Cancellation Source Residue Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Centralize typed pre-delegation cancellation construction and remove three zero-policy reporter wrappers without changing diagnostics, transport behavior, or family cancellation ownership.

**Architecture:** Add one named constructor at the portable error interface, then use it at the seven existing refusal sites. Remove only family-local forwarding functions whose shared reporter already owns all behavior; retain distinct transport adapters and concrete family tests.

**Tech Stack:** C++23, Bazel, GoogleTest/GoogleMock, Prek, clang-tidy

**Spec:** `docs/superpowers/specs/2026-11-02-can-cancellation-post-merge-cleanup-design.md`

## Global Constraints

- Execute this plan only after the delivery-evidence slice has landed; base its feature branch on that result.
- Preserve every existing diagnostic detail string byte-for-byte.
- Do not add an `ErrorKind` or change the conservative default `CancellationIoExposure::kMayHaveStarted`.
- Do not change `AfterIoMayHaveStarted`, delegated transport results, warning policy, wire behavior, lifecycle, checkpoints, or family continuation.
- Keep guarded CAN, guarded mixed-CAN, and K-Line as distinct execution paths; do not introduce type erasure or another guard-selection seam.
- Keep the concrete Hitachi CVT and MH8104 exposure tests separate.
- Do not modify or delete unrelated untracked plans and specifications.
- Work on a feature branch and stage named paths only; never commit directly to `master`.
- Warnings remain errors; do not add suppressions or warning opt-outs.

## Review Focus

- **Wrong refusal provenance:** the constructor must always return `kCancelled` plus `kRefusedBeforeDelegation`; Task 1 adds a direct interface test.
- **Diagnostic drift:** all seven exact detail strings must survive replacement; Task 2 runs existing guard and UDS assertions and audits literals.
- **Delegated-result mutation:** live calls must still pass through bytes, frames, timeouts, tokens, and errors unchanged; Task 2 runs both guard suites and UDS tests.
- **Reporter policy drift:** refused requests stay quiet and may-have-started cancellation keeps its warning; Task 3 runs the shared reporter and two concrete raw-exchange cases.
- **Composition drift:** production binding must still select CAN and mixed-CAN guards while K-Line remains unchanged; Task 4 runs the bound-attempt suite.

---

### Task 1: Name the typed pre-delegation refusal invariant

**Files:**
- Modify: `src/backend/ports/error.h:25-45`
- Test: `src/backend/ports/result_test.cpp:48-66`

**Interfaces:**
- Consumes: `Error`, `ErrorKind::kCancelled`, and `CancellationIoExposure::kRefusedBeforeDelegation`.
- Produces: `inline Error CancelledBeforeDelegation(std::string detail)` in namespace `fastecu`.

- [ ] **Step 1: Write the failing interface test**

Add:

```cpp
TEST(Error, BuildsTypedPreDelegationCancellation)
{
    const Error error = fastecu::CancelledBeforeDelegation("request refused");
    EXPECT_EQ(error.kind, ErrorKind::kCancelled);
    EXPECT_EQ(error.detail, "request refused");
    EXPECT_EQ(error.cancellation_io_exposure,
              fastecu::CancellationIoExposure::kRefusedBeforeDelegation);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/backend/ports:result_test --test_filter=Error.BuildsTypedPreDelegationCancellation`

Expected: compile failure because `fastecu::CancelledBeforeDelegation` is not defined.

- [ ] **Step 3: Implement the named constructor**

Add `inline Error CancelledBeforeDelegation(std::string detail)` beside `AfterIoMayHaveStarted`. It returns an `Error` with the supplied detail, `ErrorKind::kCancelled`, and `CancellationIoExposure::kRefusedBeforeDelegation`. Move the owned detail into the result and include `<utility>` for `std::move`.

- [ ] **Step 4: Run the focused test**

Run: `bazel test --config=release //src/backend/ports:result_test`

Expected: target passes.

- [ ] **Step 5: Commit the invariant helper**

```bash
git add src/backend/ports/error.h src/backend/ports/result_test.cpp
git commit -m "refactor: name pre-delegation cancellation errors"
```

### Task 2: Route all seven refusal sites through the named constructor

**Files:**
- Modify: `src/backend/flash/guarded_can_flash_transport.cpp:40-59`
- Modify: `src/backend/flash/guarded_mixed_can_flash_transport.cpp:50-91`
- Modify: `src/backend/protocol/uds/uds_client.cpp:27-38`
- Test: `src/backend/flash/guarded_can_flash_transport_test.cpp`
- Test: `src/backend/flash/guarded_mixed_can_flash_transport_test.cpp`
- Test: `src/backend/protocol/uds/uds_client_test.cpp`

**Interfaces:**
- Consumes: `fastecu::CancelledBeforeDelegation(std::string detail)` from Task 1.
- Produces: Six guarded-transport refusal paths and one UDS pre-request refusal path using the named invariant.

- [ ] **Step 1: Capture the seven-site inventory and exact details**

Run:

```bash
rg -n -C 2 "kRefusedBeforeDelegation" \
  src/backend/flash/guarded_can_flash_transport.cpp \
  src/backend/flash/guarded_mixed_can_flash_transport.cpp \
  src/backend/protocol/uds/uds_client.cpp
```

Expected: two CAN guard sites, four mixed-CAN guard sites, and one UDS site with their existing detail literals.

- [ ] **Step 2: Replace aggregate construction at each site**

Return `std::unexpected(CancelledBeforeDelegation(<existing literal>))` in the guard adapters and `std::unexpected(fastecu::CancelledBeforeDelegation("cancelled before request"))` in UDS. Do not alter any literal or any live-delegation branch.

- [ ] **Step 3: Run the owner tests**

Run:

```bash
bazel test --config=release \
  //src/backend/flash:guarded_can_flash_transport_test \
  //src/backend/flash:guarded_mixed_can_flash_transport_test \
  //src/backend/protocol/uds:uds_client_test
```

Expected: all three targets pass, including exact refusal errors, delegated outcomes, token forwarding, and send/receive exposure promotion.

- [ ] **Step 4: Audit production construction**

Run:

```bash
rg -n "Error\{ErrorKind::kCancelled.*kRefusedBeforeDelegation|kRefusedBeforeDelegation" \
  src/backend/flash/guarded_can_flash_transport.cpp \
  src/backend/flash/guarded_mixed_can_flash_transport.cpp \
  src/backend/protocol/uds/uds_client.cpp
```

Expected: no direct `kRefusedBeforeDelegation` construction remains in these three production files; each cancelled entry uses `CancelledBeforeDelegation` exactly once.

- [ ] **Step 5: Commit the call-site migration**

```bash
git add src/backend/flash/guarded_can_flash_transport.cpp src/backend/flash/guarded_mixed_can_flash_transport.cpp src/backend/protocol/uds/uds_client.cpp
git commit -m "refactor: share typed cancellation refusal construction"
```

### Task 3: Remove zero-policy family reporter wrappers

**Files:**
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.cpp:89-133`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor.cpp:132-140,382-387`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.cpp:67-75,278-284`
- Modify: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h:8-10`
- Test: `src/backend/flash/ecu/uds_client_exchange_common_test.cpp`
- Test: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor_test.cpp`
- Test: `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor_test.cpp`
- Test: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor_test.cpp`

**Interfaces:**
- Consumes: `Error ReportExchangeFailure(IEventSink&, const Error&, std::string_view, std::string_view)` from the shared exchange module.
- Produces: Three family callers that invoke the shared reporter directly and a Colt comment that describes production composition only.

- [ ] **Step 1: Run the observable behavior baseline**

Run:

```bash
bazel test --config=release \
  //src/backend/flash/ecu:uds_client_exchange_common_test \
  //src/backend/flash/ecu:subaru_tcu_cvt_mitsu_mh8104_can_executor_test \
  //src/backend/flash/ecu:subaru_tcu_cvt_hitachi_m32r_can_executor_test \
  //src/backend/flash/ecu:subaru_tcu_cvt_mitsu_mh8111_can_executor_test
```

Expected: all four targets pass before the structural deletion.

- [ ] **Step 2: Remove the three local forwarding functions**

Delete each anonymous-namespace overload whose only body calls `::fastecu::flash::ReportExchangeFailure(ctx.events, ...)`. At each former call site, invoke that shared function directly with `ctx.events`, the existing error, rejection prefix, and operation. Preserve `AfterIoMayHaveStarted` at the Hitachi raw-read call site and preserve every string literal.

- [ ] **Step 3: Repair comments that described the wrappers**

Delete wrapper-specific commentary. Keep the explanations for why the affected raw or retry exchanges do not use `FatalRequest`; refer directly to the shared reporter where that distinction remains useful.

- [ ] **Step 4: Narrow the Colt composition comment**

Replace the direct-caller requirement with: `BoundAttempt installs the production GuardedCanFlashTransport.` Do not add a requirement for direct family tests.

- [ ] **Step 5: Audit wrapper deletion and retained calls**

Run:

```bash
rg -n "^Error ReportExchangeFailure\(Ctx&|::fastecu::flash::ReportExchangeFailure|BoundAttempt installs" \
  src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.cpp \
  src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor.cpp \
  src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.cpp \
  src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h
```

Expected: no local `Ctx&` overload remains; exactly one direct shared call remains in each family source; the Colt header contains the narrowed production statement.

- [ ] **Step 6: Run the shared and concrete family tests**

Run the Step 1 Bazel command again.

Expected: all four targets pass; the two `RawExchangeCancellationWarningDependsOnRequestExposure` cases remain separate and unchanged.

- [ ] **Step 7: Commit the forwarding cleanup**

```bash
git add src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.cpp src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor.cpp src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.cpp src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h
git commit -m "refactor: remove CAN failure reporter forwarding"
```

### Task 4: Verify the complete behavior-preserving source slice

**Files:**
- Verify: all files modified by Tasks 1-3
- Test: complete repository gates

**Interfaces:**
- Consumes: The named refusal constructor and direct shared-reporter calls from Tasks 1-3.
- Produces: A review-ready source slice with unchanged behavior and convergent generated metadata.

- [ ] **Step 1: Run the composition proof**

Run: `bazel test --config=release //src/backend/flash:bound_flash_attempt_test`

Expected: target passes, including guarded CAN, guarded mixed-CAN, and unchanged K-Line cases.

- [ ] **Step 2: Regenerate and check Bazel metadata**

Run: `python3 scripts/gazelle_check.py --fix`

Expected: no BUILD-file change is required for the existing source and test ownership.

- [ ] **Step 3: Build the release application**

Run: `bazel build --config=release //:fastecu`

Expected: build passes with warnings treated as errors.

- [ ] **Step 4: Run the full release suite**

Run: `bazel test --config=release //...`

Expected: all applicable targets pass; only configured platform/fuzz skips are reported.

- [ ] **Step 5: Run repository hooks**

Run: `prek run --all-files`

Expected: all applicable hooks pass.

- [ ] **Step 6: Run static analysis**

Run: `bazel run //:clang_tidy`

Expected: analysis completes with zero findings.

- [ ] **Step 7: Review the exact source slice**

Run:

```bash
git diff --check HEAD~3
git diff --stat HEAD~3
git diff HEAD~3 -- src/backend/ports/error.h src/backend/ports/result_test.cpp src/backend/flash/guarded_can_flash_transport.cpp src/backend/flash/guarded_mixed_can_flash_transport.cpp src/backend/protocol/uds/uds_client.cpp src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.cpp src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor.cpp src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.cpp src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h
```

Expected: no whitespace errors; only the named helper, seven call sites, three wrapper deletions, their comments, and the Colt ownership comment changed. No family test, guard interface, or BUILD target changed.

