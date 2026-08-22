# Cancellation Token Consolidation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the two duplicate `ICancellationToken` flag implementations (`QtCancellationToken`, `CancellationSource`) with one canonical `fastecu::ManualCancellationToken` under `src/backend/ports`, and fold the `NeverCancelled` legacy stub into a static instance of it.

**Architecture:** `ManualCancellationToken` is a new header-only class (atomic-bool flag + `ICancellationToken` impl) in `src/backend/ports`, already a portable/Qt-free root. `FlashWorker` and `LoggingWorker` (the two current owners) switch to it; `flash_cancellation.h`/`CancellationSource` and `qt_cancellation_token.h`/`QtCancellationToken` are deleted outright, along with their dedicated tests (merged into one new test file). Seven ECU executor test files that instantiate `CancellationSource` directly get a mechanical rename. `transport_legacy_compat.h`'s `NeverCancelled` becomes a never-flipped static `ManualCancellationToken`.

**Tech Stack:** C++23, Bazel 9.1.1, GoogleTest (`fastecu_portable_gtest`/`fastecu_gtest`/`fastecu_qttest` from `bazel/gtest_targets.bzl` / `bazel/qt_targets.bzl`), Qt 6.8.3 (only for the Qt-linked targets being trimmed, not for the new class itself).

**Spec:** [docs/superpowers/specs/2026-08-22-cancellation-token-consolidation-design.md](../specs/2026-08-22-cancellation-token-consolidation-design.md)

## Global Constraints

- The new class lives in `namespace fastecu` (not `fastecu::flash`), is `final`, exposes `cancelled() const override` and `cancel()` — no `reset()` (confirmed dead code on the class it replaces).
- No behavior change anywhere in this plan — every task is a rename/consolidation of existing, already-tested logic. No new cancellation *semantics*.
- `src/backend/ports` is an existing `PORTABLE_ROOTS` entry (`scripts/check-portable-closure.py`) with `{"ports"}` as its required target — adding a header to the existing `ports` `cc_library` needs no genquery or `PORTABLE_ROOTS` changes.
- Each task must leave the repo in a fully buildable, fully passing state (`bazel test //...` green) before its commit — do not let intermediate task boundaries leave dangling references to a deleted type.
- `SigintCancellationToken` (`apps/bench`) and `FakeCancellationToken` (`src/backend/ports/testing`) are out of scope — do not touch them.

---

### Task 1: `ManualCancellationToken` — new canonical class

**Files:**
- Create: `src/backend/ports/manual_cancellation_token.h`
- Create: `src/backend/ports/manual_cancellation_token_test.cpp`
- Modify: `src/backend/ports/BUILD.bazel`

**Interfaces:**
- Produces: `fastecu::ManualCancellationToken`, a `final` subclass of `fastecu::ICancellationToken` (`src/backend/ports/cancellation.h`) with `bool cancelled() const override` and `void cancel()`. Default-constructible, not copyable (implicitly, via no declared copy members but an `std::atomic` member — copy/move are implicitly deleted, which is correct: this class is always held as a named local/member, never copied). Later tasks use `#include "src/backend/ports/manual_cancellation_token.h"` and `fastecu::ManualCancellationToken`.

- [ ] **Step 1: Write the failing test**

Create `src/backend/ports/manual_cancellation_token_test.cpp`:

```cpp
#include "src/backend/ports/manual_cancellation_token.h"

#include <gtest/gtest.h>

TEST(ManualCancellationToken, StartsNotCancelled)
{
    fastecu::ManualCancellationToken token;
    EXPECT_FALSE(token.cancelled());
}

TEST(ManualCancellationToken, CancelSetsFlag)
{
    fastecu::ManualCancellationToken token;
    token.cancel();
    EXPECT_TRUE(token.cancelled());
}

TEST(ManualCancellationToken, CancelIsIdempotent)
{
    fastecu::ManualCancellationToken token;
    token.cancel();
    token.cancel();
    EXPECT_TRUE(token.cancelled());
}
```

Add the test target to `src/backend/ports/BUILD.bazel`, right after the existing `result_test` target at the bottom of the file:

```python
fastecu_portable_gtest(
    name = "manual_cancellation_token_test",
    srcs = ["manual_cancellation_token_test.cpp"],
    deps = [":ports"],
)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bazel test //src/backend/ports:manual_cancellation_token_test`
Expected: FAIL — the build errors because `src/backend/ports/manual_cancellation_token.h` does not exist yet (and is not yet listed in the `ports` `cc_library`'s `hdrs`).

- [ ] **Step 3: Write the header**

Create `src/backend/ports/manual_cancellation_token.h`:

```cpp
#pragma once
#include <atomic>
#include "src/backend/ports/cancellation.h"

namespace fastecu
{

// A cancellation flag flipped by the owning code (typically GUI-thread
// teardown) and polled from wherever the ICancellationToken& ends up --
// typically a worker thread running backend logic. One instance per
// operation attempt; construct a fresh one rather than reusing across runs.
class ManualCancellationToken final : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return flag_.load(std::memory_order_relaxed);
    }
    void cancel()
    {
        flag_.store(true, std::memory_order_relaxed);
    }

  private:
    std::atomic<bool> flag_{false};
};

} // namespace fastecu
```

In `src/backend/ports/BUILD.bazel`, add `"manual_cancellation_token.h"` to the `ports` `cc_library`'s `hdrs` list, alphabetically between `"file_system.h"` and `"resource_bundle.h"`:

```python
cc_library(
    name = "ports",
    hdrs = [
        "atomic_file_writer.h",
        "cancellation.h",
        "clock.h",
        "error.h",
        "event_sink.h",
        "file_repository.h",
        "file_system.h",
        "manual_cancellation_token.h",
        "resource_bundle.h",
        "result.h",
        "settings.h",
    ],
)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `bazel test //src/backend/ports:manual_cancellation_token_test`
Expected: PASS (3 tests).

- [ ] **Step 5: Verify the portable closure still holds**

Run: `bazel run //:portable_closure`
Expected: OK — the new header is Qt/JNI-free, same as every other header in `:ports`.

- [ ] **Step 6: Commit**

```bash
git add src/backend/ports/manual_cancellation_token.h src/backend/ports/manual_cancellation_token_test.cpp src/backend/ports/BUILD.bazel
git commit -m "feat: add ManualCancellationToken canonical cancellation flag"
```

---

### Task 2: Migrate the 7 ECU executor test files off `CancellationSource`

**Files:**
- Modify: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_hitachi_m32r_can_executor_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_mitsu_m32r_kline_executor_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor_test.cpp`
- Modify (conditionally, only if the build reports a missing header): `src/backend/flash/ecu/BUILD.bazel`

**Interfaces:**
- Consumes: `fastecu::ManualCancellationToken` from Task 1 (`src/backend/ports/manual_cancellation_token.h`).

These 7 files currently `#include "src/backend/flash/flash_cancellation.h"` and instantiate `fastecu::flash::CancellationSource` (5 files, fully qualified) or bare `CancellationSource` (2 files, `subaru_hitachi_m32r_kline_executor_test.cpp` and `subaru_mitsu_m32r_kline_executor_test.cpp`, both already inside a `namespace fastecu::flash { ... }` block, so the bare name currently resolves via that enclosing namespace and will keep resolving the same way once it names `fastecu::ManualCancellationToken`, which lives in the enclosing `fastecu` namespace). This is a pure mechanical rename — confirmed by grep that every `.token()` call in these files is `cancellation.token()` or `cancellation_.token()` (no other `.token()` usage exists in this package) and every `.trip()` call is `cancellation.trip()`, `cancellation_.trip()`, or `source_.trip()`.

- [ ] **Step 1: Run the mechanical rename script**

```bash
FILES=(
  src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp
  src/backend/flash/ecu/subaru_hitachi_m32r_can_executor_test.cpp
  src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor_test.cpp
  src/backend/flash/ecu/subaru_mitsu_m32r_kline_executor_test.cpp
  src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor_test.cpp
  src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor_test.cpp
  src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor_test.cpp
)
for f in "${FILES[@]}"; do
  perl -pi -e '
    s{\#include "src/backend/flash/flash_cancellation\.h"}{#include "src/backend/ports/manual_cancellation_token.h"};
    s{fastecu::flash::CancellationSource}{fastecu::ManualCancellationToken}g;
    s{\bCancellationSource\b}{ManualCancellationToken}g;
    s{cancellation_\.token\(\)}{cancellation_};
    s{cancellation\.token\(\)}{cancellation};
    s{cancellation_\.trip\(\)}{cancellation_.cancel()};
    s{cancellation\.trip\(\)}{cancellation.cancel()};
    s{source_\.trip\(\)}{source_.cancel()};
  ' "$f"
done
```

- [ ] **Step 2: Clean up prose comments referencing the old name/verb**

These files have comments describing the cancellation helper structs in prose that should follow the rename (`trip`/`cancellation source` → `cancel()`/`token`):

```bash
perl -pi -e "s{Trips a cancellation source as soon as the first dump chunk's progress is}{Cancels the token as soon as the first dump chunk's progress is}" \
  src/backend/flash/ecu/subaru_hitachi_m32r_can_executor_test.cpp \
  src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor_test.cpp \
  src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor_test.cpp \
  src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor_test.cpp

perl -pi -e "s{Trips a cancellation source as soon as the first chunk's progress is}{Cancels the token as soon as the first chunk's progress is}" \
  src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp

perl -pi -e "s{Trips the cancellation source when a chosen log line is emitted}{Cancels the token when a chosen log line is emitted}" \
  src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp

perl -pi -e "s{Trips the cancellation source from inside write\(\) when one chosen request}{Cancels the token from inside write() when one chosen request}" \
  src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp

perl -pi -e "s{Trips the cancellation source from inside write\(\), so the request does}{Cancels the token from inside write(), so the request does}" \
  src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp

perl -pi -e "s{so a cancellation tripped}{so a cancel() call}" \
  src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp

perl -pi -e "s{cancellation trips on that chunk's}{cancel() lands on that chunk's}" \
  src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor_test.cpp \
  src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor_test.cpp \
  src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor_test.cpp
```

- [ ] **Step 3: Verify no stray references remain**

```bash
grep -rn "CancellationSource\|flash_cancellation\.h\|\.trip()\|Trips \|cancellation source" src/backend/flash/ecu/*_test.cpp
```

Expected: no output.

- [ ] **Step 4: Build and test the 7 affected targets**

```bash
bazel test \
  //src/backend/flash/ecu:mitsu_colt_m32r_can_executor_test \
  //src/backend/flash/ecu:subaru_hitachi_m32r_can_executor_test \
  //src/backend/flash/ecu:subaru_hitachi_m32r_kline_executor_test \
  //src/backend/flash/ecu:subaru_mitsu_m32r_kline_executor_test \
  //src/backend/flash/ecu:subaru_tcu_cvt_hitachi_m32r_can_executor_test \
  //src/backend/flash/ecu:subaru_tcu_cvt_mitsu_mh8104_can_executor_test \
  //src/backend/flash/ecu:subaru_tcu_cvt_mitsu_mh8111_can_executor_test
```

Expected: all 7 targets PASS with unchanged test counts. These targets currently reach `src/backend/ports/manual_cancellation_token.h` transitively (each already depends on `//src/backend/ports/testing:fake_clock`, which depends on `//src/backend/ports`) the same way they already reached `flash_cancellation.h` transitively before this change (`subaru_hitachi_m32r_kline_executor_test` and `subaru_mitsu_m32r_kline_executor_test` have no direct `//src/backend/flash:flash_executor` dep today and still successfully included `flash_cancellation.h`, proving transitive header visibility already works in this build). If any target fails with an "undeclared inclusion" or missing-header error for `manual_cancellation_token.h`, add `"//src/backend/ports",` to that target's `deps` list in `src/backend/flash/ecu/BUILD.bazel` and rerun.

- [ ] **Step 5: Commit**

```bash
git add src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp \
        src/backend/flash/ecu/subaru_hitachi_m32r_can_executor_test.cpp \
        src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor_test.cpp \
        src/backend/flash/ecu/subaru_mitsu_m32r_kline_executor_test.cpp \
        src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor_test.cpp \
        src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor_test.cpp \
        src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor_test.cpp
git add src/backend/flash/ecu/BUILD.bazel 2>/dev/null || true
git commit -m "refactor: migrate ECU executor tests from CancellationSource to ManualCancellationToken"
```

---

### Task 3: Migrate `FlashWorker`; delete `flash_cancellation.h`

**Files:**
- Modify: `src/platform/desktop/common/flash/flash_worker.h`
- Modify: `src/platform/desktop/common/flash/flash_worker.cpp`
- Delete: `src/backend/flash/flash_cancellation.h`
- Delete: `src/backend/flash/flash_cancellation_test.cpp`
- Modify: `src/backend/flash/BUILD.bazel`

**Interfaces:**
- Consumes: `fastecu::ManualCancellationToken` from Task 1.
- Produces: no change to `FlashWorker`'s public interface (`requestStop()` keeps its name and signature).

`flash_cancellation.h` is, after Task 2, only referenced by `flash_worker.h` and its own test — safe to delete both in this task with no dangling references left anywhere in the tree.

- [ ] **Step 1: Migrate `flash_worker.h`**

In `src/platform/desktop/common/flash/flash_worker.h`, replace the include and member type:

```cpp
#include "src/backend/flash/flash_cancellation.h"
```
→
```cpp
#include "src/backend/ports/manual_cancellation_token.h"
```

and:

```cpp
    CancellationSource cancellation_;
```
→
```cpp
    ManualCancellationToken cancellation_;
```

Also reword the doc comment above `requestStop()` (it still describes the right two-part contract, just with the old verb):

```cpp
    // Trips the cancellation source AND asks the transport to interrupt any
```
→
```cpp
    // Cancels the cancellation token AND asks the transport to interrupt any
```

- [ ] **Step 2: Migrate `flash_worker.cpp`**

Replace the `trip()` call and reword its surrounding comment:

```cpp
// Bounded, not absent: requestStop() (tripping cancellation_ and calling
```
→
```cpp
// Bounded, not absent: requestStop() (cancelling cancellation_ and calling
```

```cpp
    cancellation_.trip();
```
→
```cpp
    cancellation_.cancel();
```

Replace the `.token()` call at the `execute()` call site:

```cpp
    Result<FlashExecutionResult> result =
        executor_->execute(plan_, *transport_, *clock_, cancellation_.token(), events);
```
→
```cpp
    Result<FlashExecutionResult> result =
        executor_->execute(plan_, *transport_, *clock_, cancellation_, events);
```

- [ ] **Step 3: Delete `flash_cancellation.h` and its test**

```bash
git rm src/backend/flash/flash_cancellation.h src/backend/flash/flash_cancellation_test.cpp
```

- [ ] **Step 4: Update `src/backend/flash/BUILD.bazel`**

Remove `"flash_cancellation.h"` from the `flash_executor` `cc_library`'s `hdrs`:

```python
cc_library(
    name = "flash_executor",
    srcs = ["flash_executor.cpp"],
    hdrs = [
        "flash_executor.h",
    ],
    deps = [
        ":flash_plan",
        ":flash_types",
        "//src/algorithms/protocol",
        "//src/backend/ports",
        "//src/backend/protocol",
    ],
)
```

Delete the `flash_cancellation_test` target entirely:

```python
fastecu_portable_gtest(
    name = "flash_cancellation_test",
    srcs = ["flash_cancellation_test.cpp"],
    deps = [":flash_executor"],
)
```

- [ ] **Step 5: Build and test**

```bash
bazel test //src/backend/flash:... //src/platform/desktop/common/flash:test_flash_worker //src/platform/desktop/common/flash:test_flash_workflow
```

Expected: all PASS. `flash_worker_test.cpp` and `flash_workflow_test.cpp` don't reference `CancellationSource`/`flash_cancellation.h` directly (verified during spec research), so no changes needed there.

- [ ] **Step 6: Commit**

```bash
git add src/platform/desktop/common/flash/flash_worker.h src/platform/desktop/common/flash/flash_worker.cpp src/backend/flash/BUILD.bazel
git commit -m "refactor: migrate FlashWorker to ManualCancellationToken, delete CancellationSource"
```

---

### Task 4: Migrate `LoggingWorker`; delete `qt_cancellation_token.h`

**Files:**
- Modify: `src/platform/desktop/common/logging/logging_worker.h`
- Delete: `src/platform/desktop/common/ports/qt_cancellation_token.h`
- Modify: `src/platform/desktop/common/ports/BUILD.bazel`
- Modify: `src/platform/desktop/common/ports/qt_port_adapters_test.cpp`

**Interfaces:**
- Consumes: `fastecu::ManualCancellationToken` from Task 1.
- Produces: no change to `LoggingWorker`'s public interface.

`qt_cancellation_token.h` is, after this task, referenced nowhere — its only consumers were `logging_worker.h` and `qt_port_adapters_test.cpp`, both updated here. `logging_runtime` (the Bazel target for `logging_worker.h`/`.cpp`) already depends directly on `//src/backend/ports` (for `event_sink.h`), so no `deps` change is needed there. It also still needs `//src/platform/desktop/common/ports` for `qt_event_sink.h` (via `logging_engine.h`), so that dependency stays.

- [ ] **Step 1: Migrate `logging_worker.h`**

Replace the include:

```cpp
#include "src/platform/desktop/common/ports/qt_cancellation_token.h"
```
→
```cpp
#include "src/backend/ports/manual_cancellation_token.h"
```

Replace the member type:

```cpp
    QtCancellationToken cancellation_;
```
→
```cpp
    fastecu::ManualCancellationToken cancellation_;
```

(`fastecu::` is required here because, unlike `QtCancellationToken`, `ManualCancellationToken` is namespaced and this header has no `using` directive for it; `logging_worker.cpp`'s existing `cancellation_.cancel()` call at line 52 needs no change — the method name is identical.)

- [ ] **Step 2: Delete `qt_cancellation_token.h`**

```bash
git rm src/platform/desktop/common/ports/qt_cancellation_token.h
```

- [ ] **Step 3: Update `src/platform/desktop/common/ports/BUILD.bazel`**

Remove `"qt_cancellation_token.h"` from the `ports` `qt_cc_library`'s `normal_hdrs`:

```python
    normal_hdrs =
        [
            "qt_atomic_file_writer.h",
            "qt_clock.h",
            "qt_file_repository.h",
            "qt_file_system.h",
            "qt_resource_bundle.h",
            "qt_settings.h",
        ],
```

- [ ] **Step 4: Update `qt_port_adapters_test.cpp`**

Replace the include:

```cpp
#include "src/platform/desktop/common/ports/qt_cancellation_token.h"
```
→
```cpp
#include "src/backend/ports/manual_cancellation_token.h"
```

Add a `using` declaration alongside the existing ones so the `QtClockTest` cases below can stay unqualified, matching their current style:

```cpp
using fastecu::ErrorKind;
using fastecu::LogLevel;
using fastecu::Result;
using fastecu::Status;
```
→
```cpp
using fastecu::ErrorKind;
using fastecu::LogLevel;
using fastecu::ManualCancellationToken;
using fastecu::Result;
using fastecu::Status;
```

In the three `QtClockTest` cases, replace `QtCancellationToken` with `ManualCancellationToken`:

```cpp
TEST(QtClockTest, NowMsIsMonotonicNonDecreasing)
{
    QtClock clock;
    std::uint64_t first = clock.now_ms();
    QtCancellationToken token;
    Status s = clock.sleep(1, token);
    ASSERT_TRUE(s.has_value());
    std::uint64_t second = clock.now_ms();
    EXPECT_GE(second, first);
}

TEST(QtClockTest, SleepZeroSucceeds)
{
    QtClock clock;
    QtCancellationToken token;
    Status s = clock.sleep(0, token);
    EXPECT_TRUE(s.has_value());
}

TEST(QtClockTest, SleepReturnsCancelledWhenTokenAlreadyCancelled)
{
    QtClock clock;
    QtCancellationToken token;
    token.cancel();
    Status s = clock.sleep(50, token);
    ASSERT_FALSE(s.has_value());
    EXPECT_EQ(s.error().kind, ErrorKind::Cancelled);
}
```
→
```cpp
TEST(QtClockTest, NowMsIsMonotonicNonDecreasing)
{
    QtClock clock;
    std::uint64_t first = clock.now_ms();
    ManualCancellationToken token;
    Status s = clock.sleep(1, token);
    ASSERT_TRUE(s.has_value());
    std::uint64_t second = clock.now_ms();
    EXPECT_GE(second, first);
}

TEST(QtClockTest, SleepZeroSucceeds)
{
    QtClock clock;
    ManualCancellationToken token;
    Status s = clock.sleep(0, token);
    EXPECT_TRUE(s.has_value());
}

TEST(QtClockTest, SleepReturnsCancelledWhenTokenAlreadyCancelled)
{
    QtClock clock;
    ManualCancellationToken token;
    token.cancel();
    Status s = clock.sleep(50, token);
    ASSERT_FALSE(s.has_value());
    EXPECT_EQ(s.error().kind, ErrorKind::Cancelled);
}
```

Delete the entire `QtCancellationToken` test suite (its 3 cases are now covered by `manual_cancellation_token_test.cpp` from Task 1 — `ResetClearsFlag` has no replacement, since `reset()` no longer exists, per the spec's confirmed-dead-code finding). Delete everything from the blank line immediately before `// ---- QtCancellationToken ----` through the end of the `ResetClearsFlag` test, i.e. this whole span:

```cpp

// ---- QtCancellationToken ----------------------------------------------

TEST(QtCancellationTokenTest, FreshTokenIsNotCancelled)
{
    QtCancellationToken token;
    EXPECT_FALSE(token.cancelled());
}

TEST(QtCancellationTokenTest, CancelSetsFlag)
{
    QtCancellationToken token;
    token.cancel();
    EXPECT_TRUE(token.cancelled());
}

TEST(QtCancellationTokenTest, ResetClearsFlag)
{
    QtCancellationToken token;
    token.cancel();
    ASSERT_TRUE(token.cancelled());
    token.reset();
    EXPECT_FALSE(token.cancelled());
}
```

becomes nothing (delete it entirely, no replacement text). Leave the blank line that was already separating `ResetClearsFlag` from `// ---- QtFileRepository -------------------------------------------------` in place, so that comment ends up immediately after the last `QtClockTest` case, separated by exactly one blank line — the same spacing every other section boundary in this file already uses.

- [ ] **Step 5: Build and test**

```bash
bazel test //src/platform/desktop/common/ports:test_qt_port_adapters //src/platform/desktop/common/logging:test_logging_worker //src/platform/desktop/common/logging:test_logging_engine
```

Expected: all PASS. `test_qt_port_adapters` should now report 3 fewer test cases than before (the deleted `QtCancellationTokenTest` suite).

- [ ] **Step 6: Commit**

```bash
git add src/platform/desktop/common/logging/logging_worker.h \
        src/platform/desktop/common/ports/BUILD.bazel \
        src/platform/desktop/common/ports/qt_port_adapters_test.cpp
git commit -m "refactor: migrate LoggingWorker to ManualCancellationToken, delete QtCancellationToken"
```

---

### Task 5: Fold `NeverCancelled` into a static `ManualCancellationToken`

**Files:**
- Modify: `src/backend/protocol/transport_legacy_compat.h`

**Interfaces:**
- Consumes: `fastecu::ManualCancellationToken` from Task 1.
- Produces: no change to `transport_legacy_compat::detail::never_cancelled()`'s signature or behavior (`const ICancellationToken&`, always reports `false`).

`src/backend/protocol/BUILD.bazel`'s `protocol` `cc_library` already depends directly on `//src/backend/ports`, so no `deps` change is needed.

- [ ] **Step 1: Rewrite the `NeverCancelled` section**

In `src/backend/protocol/transport_legacy_compat.h`, replace:

```cpp
#include "src/backend/protocol/ican_transport.h"
#include "src/backend/protocol/ikline_transport.h"
#include "src/backend/protocol/issm_transport.h"

#include <cstdint>
#include <utility>

// Transitional adapters for pre-portability callers. They intentionally
// collapse typed failures back to the old empty/zero shapes and must not be
// used by the portable logging implementations.
namespace fastecu::transport_legacy_compat
{
namespace detail
{
class NeverCancelled final : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return false;
    }
};

inline const ICancellationToken& never_cancelled()
{
    static const NeverCancelled token;
    return token;
}
} // namespace detail
```

with:

```cpp
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/protocol/ican_transport.h"
#include "src/backend/protocol/ikline_transport.h"
#include "src/backend/protocol/issm_transport.h"

#include <cstdint>
#include <utility>

// Transitional adapters for pre-portability callers. They intentionally
// collapse typed failures back to the old empty/zero shapes and must not be
// used by the portable logging implementations.
namespace fastecu::transport_legacy_compat
{
namespace detail
{
inline const ICancellationToken& never_cancelled()
{
    static const ManualCancellationToken token; // never cancelled -- flag is never flipped
    return token;
}
} // namespace detail
```

- [ ] **Step 2: Verify no other reference to `NeverCancelled` exists**

```bash
grep -rn "NeverCancelled" src/ tests/ apps/
```

Expected: no output (the class was `detail`-scoped and only ever used internally by `never_cancelled()`).

- [ ] **Step 3: Build and test**

```bash
bazel test //src/backend/protocol:...
```

Expected: all PASS, no behavior change.

- [ ] **Step 4: Commit**

```bash
git add src/backend/protocol/transport_legacy_compat.h
git commit -m "refactor: fold NeverCancelled into a static ManualCancellationToken"
```

---

### Task 6: Full-repo verification

**Files:** none (verification only).

- [ ] **Step 1: Full test suite**

```bash
bazel test --config=release //...
```

Expected: all PASS, no regressions anywhere in the tree.

- [ ] **Step 2: Portable closure guardrail**

```bash
bazel run //:portable_closure
```

Expected: OK, same required-target count as before this plan (no roots added or removed).

- [ ] **Step 3: Local checks**

```bash
prek run --all-files
```

Expected: all hooks pass (clang-format, buildifier, pragma-once, lychee).

- [ ] **Step 4: Confirm the deleted/renamed symbols are gone repo-wide**

```bash
grep -rn "QtCancellationToken\|CancellationSource\|flash_cancellation\.h\|qt_cancellation_token\.h" src/ apps/ tests/
```

Expected: no output.

- [ ] **Step 5: Confirm `ICancellationToken`'s subclass count matches the spec's target state**

```bash
grep -rn "public ICancellationToken\|public fastecu::.*ICancellationToken" --include="*.h" .
```

Expected: exactly 3 matches — `SigintCancellationToken` (`apps/bench`), `FakeCancellationToken` (`src/backend/ports/testing`), `ManualCancellationToken` (`src/backend/ports`).

**Correction, added after the final whole-branch review:** this grep's
`--include="*.h"` filter only checks headers, so it cannot see subclasses
declared inside `.cpp` files. An unfiltered repo-wide grep finds 6 additional
pre-existing `ICancellationToken` subclasses hand-rolled inside two ECU
executor test `.cpp` files (`subaru_denso_mc68hc16y5_02_executor_test.cpp`,
`subaru_denso_sh7055_02_executor_test.cpp`) — out of this plan's scope, not
touched by any task above, tracked as a follow-up. The real post-plan
repo-wide count is 9, not 3. Do not rely on the filtered grep above as a
completeness check for a future similar audit.

No commit for this task — it's pure verification. If any step fails, return to the task whose deliverable it covers and fix forward with a new commit; do not amend a prior task's commit.
