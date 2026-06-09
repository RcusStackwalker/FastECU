# Bounds-check Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn every out-of-bounds buffer access across FastECU into an immediate, logged, located abort instead of silent undefined behavior, with a one-flag build-level net plus a sanitizer build for the residue.

**Architecture:** FastECU has ~2300 `.at()` calls and almost no raw-pointer indexing, so Qt's built-in `Q_ASSERT` inside `QByteArray::at()`/`operator[]` already covers the surface — it is only stripped by `QT_NO_DEBUG`. Re-arm it everywhere with `QT_FORCE_ASSERTS` (all builds, fail-fast), add an opt-in ASan/UBSan config for non-Qt memory bugs, then burn in: run the test suite under the flag and fix each latent over-read it surfaces.

**Tech Stack:** C++17, Qt 6.11 (qmake), QtTest, AddressSanitizer/UBSan (clang).

**Reference spec:** `docs/superpowers/specs/2026-06-09-bounds-check-hardening-design.md`

---

## File Structure

- `log_operations_ssm.cpp` — already-applied crash guard (Task 1 commits it).
- `tests/force_asserts/tst_force_asserts.cpp` + `.pro` — death test proving forced asserts fire under release flags (Task 2). New, self-contained, does **not** include `hardening.pri`.
- `FastECU.pro` — gains the hardening include (Tasks 3–4).
- `hardening.pri` (repo root) — shared: unconditional `QT_FORCE_ASSERTS` + opt-in `sanitize` block (Task 4). Included by the app and the test projects.
- `tests/tests.pro`, `tests/mut_dma_integration_tests.pro`, `tests/serial_crash_tests.pro` — include `hardening.pri` (Task 4).
- `modules/ecu/flash_ecu_subaru_denso_sh7058_can.cpp` — worked-example guard fix (Task 5).

---

### Task 1: Commit the already-applied log-polling crash guard

The crash that motivated this work (`log_operations_ssm.cpp:369`, null deref on empty `received`) was fixed in the working tree but is uncommitted. Commit it first so burn-in starts from a clean base. With `QT_FORCE_ASSERTS` on (Task 3) this site would otherwise abort loudly on the legitimate "no data" path; the guard keeps that path quiet.

**Files:**
- Modify: `log_operations_ssm.cpp` (already edited; verify the guard is present)

- [ ] **Step 1: Verify the guard is in place**

Run: `grep -n "received.length() >= 3 &&" log_operations_ssm.cpp`
Expected: one match on the `while (... received.at(0) ...)` line (~369).

- [ ] **Step 2: Confirm it still compiles**

Run:
```bash
mkdir -p /tmp/fastecu_build && qmake6 FastECU.pro -o /tmp/fastecu_build/Makefile && make -C /tmp/fastecu_build log_operations_ssm.o
```
Expected: builds with warnings only, no errors; `log_operations_ssm.o` produced.

- [ ] **Step 3: Commit**

```bash
git add log_operations_ssm.cpp
git commit -m "fix: guard log-poll header resync against empty serial buffer

read_log_serial_data() called received.at(0/1/2) without checking length.
On an empty QByteArray (no ECU data) Qt6 at() dereferences a null data
pointer -> SIGSEGV. Require length() >= 3 before indexing.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Death test proving forced asserts fire under release flags

A self-contained QtTest that forks a child, performs an out-of-range Qt access, and asserts the child died via `SIGABRT` (the `Q_ASSERT`→`qt_assert`→`abort()` path). Under release flags **without** `QT_FORCE_ASSERTS`, the same access on an empty `QByteArray` is a null deref → `SIGSEGV`, so the test is red until the flag is added.

**Files:**
- Create: `tests/force_asserts/tst_force_asserts.cpp`
- Create: `tests/force_asserts/force_asserts.pro`

- [ ] **Step 1: Write the failing test**

Create `tests/force_asserts/tst_force_asserts.cpp`:
```cpp
// Proves QT_FORCE_ASSERTS re-arms Qt's bounds-check assertions under release
// (QT_NO_DEBUG) flags. Child does an out-of-range QByteArray access:
//   - asserts armed   -> Q_ASSERT -> qt_assert -> abort()  (SIGABRT)
//   - asserts stripped -> empty.at(0) null deref            (SIGSEGV)
#include <QtTest>
#include <QByteArray>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>

class TestForceAsserts : public QObject
{
    Q_OBJECT
private slots:
    void outOfBoundsAtAborts();
};

void TestForceAsserts::outOfBoundsAtAborts()
{
    pid_t pid = fork();
    QVERIFY2(pid >= 0, "fork failed");
    if (pid == 0) {
        QByteArray empty;           // Qt6: null data pointer, size 0
        volatile char c = empty.at(0);
        (void)c;
        _exit(0);                   // reached only if no abort/crash
    }
    int status = 0;
    QVERIFY2(waitpid(pid, &status, 0) == pid, "waitpid failed");
    QVERIFY2(WIFSIGNALED(status), "child exited normally; bounds check did not fire");
    QCOMPARE(WTERMSIG(status), SIGABRT);
}

QTEST_APPLESS_MAIN(TestForceAsserts)
#include "tst_force_asserts.moc"
```

Create `tests/force_asserts/force_asserts.pro` (release flags, **no** force yet):
```pro
QT += core testlib
QT -= gui
CONFIG += console c++17 release
CONFIG -= app_bundle
TEMPLATE = app
TARGET = tst_force_asserts
# Simulate the shipped release build: asserts stripped.
DEFINES += QT_NO_DEBUG
SOURCES += tst_force_asserts.cpp
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cd tests/force_asserts && qmake6 force_asserts.pro && make && ./tst_force_asserts
```
Expected: FAIL — child terminates with `SIGSEGV` (signal 11), so `QCOMPARE(WTERMSIG, SIGABRT)` reports `Actual: 11, Expected: 6` (or `WIFSIGNALED` message). This is the silent-UB state we are eliminating.

- [ ] **Step 3: Add QT_FORCE_ASSERTS to the test project**

Edit `tests/force_asserts/force_asserts.pro`, change the DEFINES line to:
```pro
DEFINES += QT_NO_DEBUG QT_FORCE_ASSERTS
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cd tests/force_asserts && qmake6 force_asserts.pro && make && ./tst_force_asserts
```
Expected: PASS — child aborts via `SIGABRT`; Qt also prints `ASSERT: "size_t(i) < size_t(size())" in file .../qbytearray.h` from the child.

- [ ] **Step 5: Commit**

```bash
git add tests/force_asserts/tst_force_asserts.cpp tests/force_asserts/force_asserts.pro
git commit -m "test: death test proving QT_FORCE_ASSERTS arms bounds checks in release

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Arm forced assertions in the main application build

**Files:**
- Modify: `FastECU.pro` (DEFINES block, ~line 12)

- [ ] **Step 1: Add the define**

Edit `FastECU.pro`. Immediately after the existing line:
```pro
DEFINES += QT_DEPRECATED_WARNINGS #QT_SSL
```
add:
```pro
# Bounds-check hardening: re-arm Q_ASSERT even under QT_NO_DEBUG so every
# QByteArray/QString/QList .at()/operator[] overrun aborts loudly with file:line.
# See docs/superpowers/specs/2026-06-09-bounds-check-hardening-design.md
DEFINES += QT_FORCE_ASSERTS
```

- [ ] **Step 2: Verify the app still builds**

Run:
```bash
rm -rf /tmp/fastecu_build && mkdir -p /tmp/fastecu_build && qmake6 FastECU.pro -o /tmp/fastecu_build/Makefile && make -C /tmp/fastecu_build -j4
```
Expected: links a `FastECU` / `FastECU.app` binary with no new errors. (Warnings unchanged from baseline.)

- [ ] **Step 3: Commit**

```bash
git add FastECU.pro
git commit -m "build: enable QT_FORCE_ASSERTS in all builds for bounds-check hardening

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Shared hardening include + opt-in sanitizer build, wired into the test suite

Refactor the Task 3 define into a shared `hardening.pri` (DRY) that also carries the opt-in ASan/UBSan block, and include it from the app and the three test projects so the whole suite runs hardened (and can run under sanitizers).

**Files:**
- Create: `hardening.pri` (repo root)
- Modify: `FastECU.pro`
- Modify: `tests/tests.pro`, `tests/mut_dma_integration_tests.pro`, `tests/serial_crash_tests.pro`

- [ ] **Step 1: Create the shared include**

Create `hardening.pri`:
```pro
# Bounds-check hardening — see docs/superpowers/specs/2026-06-09-bounds-check-hardening-design.md
#
# Re-arm Q_ASSERT even in release (QT_NO_DEBUG) so every QByteArray/QString/QList
# .at()/operator[] overrun aborts loudly with file:line instead of silent UB.
DEFINES += QT_FORCE_ASSERTS

# Opt-in AddressSanitizer + UndefinedBehaviorSanitizer build for the residue Qt
# asserts cannot see (raw memory, std::, use-after-free). Enable with:
#   qmake6 <project>.pro CONFIG+=sanitize
sanitize {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -g
    QMAKE_LFLAGS   += -fsanitize=address,undefined
}
```

- [ ] **Step 2: Replace the inline define in FastECU.pro with the include**

In `FastECU.pro`, replace the three comment lines + `DEFINES += QT_FORCE_ASSERTS` added in Task 3 with:
```pro
# Bounds-check hardening (forced asserts + opt-in sanitizers).
include(hardening.pri)
```

- [ ] **Step 3: Include hardening.pri from each test project**

Add this line near the top of each of `tests/tests.pro`, `tests/mut_dma_integration_tests.pro`, and `tests/serial_crash_tests.pro` (after their `CONFIG` lines):
```pro
include(../hardening.pri)
```

- [ ] **Step 4: Verify app + tests build, and the sanitizer config links**

Run (plain hardened build of the unit tests):
```bash
cd tests && qmake6 tests.pro && make -j4 && ./mut_dma_tests
```
Expected: tests build and PASS, now with forced asserts active.

Run (sanitizer build of the same):
```bash
cd tests && make distclean; qmake6 tests.pro CONFIG+=sanitize && make -j4 && ./mut_dma_tests
```
Expected: builds and links with `-fsanitize=address,undefined`; tests PASS with no ASan/UBSan reports. (If ASan reports a leak unrelated to bounds, prefix the run with `ASAN_OPTIONS=detect_leaks=0` — leaks are out of scope for this plan.)

- [ ] **Step 5: Commit**

```bash
git add hardening.pri FastECU.pro tests/tests.pro tests/mut_dma_integration_tests.pro tests/serial_crash_tests.pro
git commit -m "build: shared hardening.pri (forced asserts + opt-in ASan/UBSan), wire into tests

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: Burn-in — run the suite hardened, fix latent over-reads

Turning forced asserts on converts latent over-reads (paths that index past the buffer but happen not to crash today) into aborts. Run the full suite hardened + under sanitizers, and fix each firing. This task includes one **known** real over-read as the worked example (`sh7058_can` seed parse: guard checks `length() > 5` but reads `at(6..9)`), then a repeatable procedure for any others.

> **Note on test granularity:** these CAN/serial flash flows have no headless seam today (they drive `serial->` directly, unlike the `mut_dma` modules which use `ScriptedKlineTransport`). For such sites the *failing test* is the assert firing during burn-in against captured/synthetic input, and the *fix verification* is replaying the same input with no abort plus a code-inspection that the guard now covers every index the block reads. Writing a per-module QtTest harness is out of scope here; extend `tests/serial_crash_tests` (PTY mock) only if a site needs ongoing regression coverage.

**Files:**
- Modify: `modules/ecu/flash_ecu_subaru_denso_sh7058_can.cpp` (~line 474 and ~494)
- Modify: any other module whose assert fires during burn-in (discovered, not pre-listed)

- [ ] **Step 1: Build everything hardened and run the suite**

Run:
```bash
cd tests
make distclean; qmake6 tests.pro CONFIG+=sanitize && make -j4 && ASAN_OPTIONS=detect_leaks=0 ./mut_dma_tests
make distclean; qmake6 mut_dma_integration_tests.pro CONFIG+=sanitize && make -j4 && ASAN_OPTIONS=detect_leaks=0 ./mut_dma_integration_tests
make distclean; qmake6 serial_crash_tests.pro CONFIG+=sanitize && make -j4 && ASAN_OPTIONS=detect_leaks=0 ./serial_crash_tests
```
Expected: each suite runs. Record any `ASSERT: "size_t(i) < size_t(size())"` aborts or ASan `heap-buffer-overflow` reports, with the printed file:line. Each one is a latent bug for the steps below.

- [ ] **Step 2: Fix the known `sh7058_can` seed over-read (worked example)**

In `modules/ecu/flash_ecu_subaru_denso_sh7058_can.cpp`, the block reads up to index 9 but only guards `length() > 5`. Change the guard to cover every index read in the block.

Find:
```cpp
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() > 5)
    {
        if ((uint8_t)received.at(4) != 0x67 || (uint8_t)received.at(5) != 0x01)
```
Replace the guard line with (covers indices 0..9 read by `received.at(6..9)` below):
```cpp
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() > 9)
    {
        if ((uint8_t)received.at(4) != 0x67 || (uint8_t)received.at(5) != 0x01)
```
Leave the `seed.append(received.at(6))` … `at(9)` lines unchanged — they are now covered by the guard, and the existing `else { LOG_E("No valid response from ECU"); return STATUS_ERROR; }` handles the short-response case.

- [ ] **Step 3: Verify the fix compiles**

Run:
```bash
make -C /tmp/fastecu_build modules/ecu/flash_ecu_subaru_denso_sh7058_can.o
```
Expected: compiles with warnings only, no errors. (If the build dir is stale, regenerate with the Task 3 Step 2 command first.)

- [ ] **Step 4: Fix each remaining firing from Step 1 (repeat per site)**

For every assert/ASan report recorded in Step 1, at the printed file:line:
1. Identify the maximum index the enclosing block reads from the buffer (`max_index`).
2. Ensure the dominating guard requires `buf.length() > max_index` (i.e. `length() >= max_index + 1`). Tighten the existing `length() > N` check, or add one if absent (mirror the `else { LOG_E(...); return STATUS_ERROR; }` rejection pattern already used in these modules).
3. Rebuild that object and re-run the suite from Step 1; confirm the firing is gone.

Do **not** suppress an assert by switching to a non-checked accessor or by catching it — the guard must make the access in-bounds.

- [ ] **Step 5: Re-run the full hardened suite green**

Run the three suite commands from Step 1 again.
Expected: all suites PASS with zero assertion aborts and zero ASan/UBSan reports.

- [ ] **Step 6: Commit**

```bash
git add modules/ecu/flash_ecu_subaru_denso_sh7058_can.cpp
# add any other module files fixed in Step 4
git commit -m "fix: tighten response-length guards surfaced by hardened burn-in

sh7058_can seed parse guarded length()>5 but read at(6..9); require >9.
<list any other sites fixed>.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-Review

- **Spec coverage:** Component 1 (forced asserts all builds) → Tasks 3–4. Component 2 (ASan/UBSan in tests) → Task 4. Component 3 (burn-in + fix latent over-reads, worked example, regression intent) → Task 5; the motivating crash fix → Task 1. Component 4 (deferred) → intentionally not implemented. Behavior contract (loud abort, all builds) → realized by `QT_FORCE_ASSERTS` unconditional in `hardening.pri` and proven by Task 2. No gaps.
- **Placeholders:** Step 4 of Task 5 is necessarily discovery-driven (the firing sites are not knowable until the flag is on), but it is a concrete repeatable procedure with a worked, fully-specified example in Steps 2–3 — not a "handle edge cases" placeholder.
- **Type/name consistency:** `hardening.pri` define `QT_FORCE_ASSERTS`, `sanitize` CONFIG scope, and `CONFIG+=sanitize` invocation match across Tasks 2/4/5. Test target names (`mut_dma_tests`, `serial_crash_tests`, `tst_force_asserts`) match their `.pro` `TARGET`s.
