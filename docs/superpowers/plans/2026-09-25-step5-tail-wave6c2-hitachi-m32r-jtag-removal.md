# Step 5 Tail Wave 6c-2 — Hitachi M32R JTAG Removal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Delete the unreachable, stubbed `FlashEcuSubaruHitachiM32rJtag` legacy flash family and record its removal, taking `//:legacy_flash_drain` from three entries to two.

**Architecture:** Pure removal. No portable code, no `FlashFamily` value, no workflow route. The legacy Qt operation, its dialog package, the `MainWindow` dispatch branch and the now-unused JTAG macros go; two ratchets shrink by one entry each; the qualification matrix and three planning documents record the removal.

**Tech Stack:** Bazel (`.bazelversion`), C++23, Qt 6, Python guard scripts under `scripts/`, `prek` hooks.

**Spec:** [the 6c-2 removal design](../specs/2026-09-25-step5-tail-wave6c2-hitachi-m32r-jtag-removal-design.md). Read it first; its appendix already preserves the probe's wire sequence, so nothing in the deleted source needs copying anywhere.

## Global Constraints

- Branch: `feat/wave6c2-hitachi-m32r-jtag-removal` (exists; the spec is committed on it as `fb1bf38a`). `prek` refuses commits on `master`.
- Ratchets only shrink: remove entries, never add. No new `REMAINING`, `FROZEN` or visibility entry anywhere.
- `SUB_KERNEL_BLANK_PAGE` in `kernelcomms.h` stays; only the JTAG block is deleted.
- Do not touch `resources/shared/config/protocols.cfg`, `flash_workflow.cpp`, or any file under `src/backend/flash/`.
- Markdown cross-document references are links with readable text, not backticked paths (lychee checks them).
- Every commit message ends with:

  ```text
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01BVWAZwWDdZGx6y3Ks4ptJX
  ```

- Push and PR creation happen only after the user authorizes them.

---

### Task 1: Remove the JTAG family and shrink the ratchets

**Files:**
- Modify: `scripts/check-legacy-flash-drain.py:33-37`
- Delete: `src/platform/desktop/common/flash/legacy/jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.cpp`
- Delete: `src/platform/desktop/common/flash/legacy/jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.h`
- Delete: `src/ui/desktop/flash/jtag/BUILD.bazel`, `src/ui/desktop/flash/jtag/flash_ecu_subaru_hitachi_m32r_jtag.cpp`, `src/ui/desktop/flash/jtag/flash_ecu_subaru_hitachi_m32r_jtag.h`
- Modify: `src/platform/desktop/common/flash/legacy/BUILD.bazel:11-31`
- Modify: `src/ui/desktop/mainwindow.cpp:1284-1291`
- Modify: `src/ui/desktop/mainwindow.h:67-68`
- Modify: `src/ui/desktop/BUILD.bazel:110`
- Modify: `src/platform/desktop/common/serial/BUILD.bazel:83`
- Modify: `scripts/check-serial-compat-allowlist.py:33`
- Modify: `src/backend/definitions/kernelcomms.h:38-75`
- Test: `//:legacy_flash_drain` (`scripts/check-legacy-flash-drain.py`), `//:serial_compat_allowlist`, `//:portable_closure`, `//src/ui/desktop:test_mainwindow`, then `//...`

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: a tree in which no source, header or BUILD file mentions `M32rJtag`, `m32r_jtag` or `flash/jtag`. Task 2 relies on this when it runs the final `git grep`.

- [ ] **Step 1: Shrink the drain ratchet first (the failing test)**

In `scripts/check-legacy-flash-drain.py`, replace

```python
REMAINING = {
    "bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.cpp",
    "ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.cpp",
    "jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.cpp",
}
```

with

```python
REMAINING = {
    "bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.cpp",
    "ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.cpp",
}
```

- [ ] **Step 2: Run the guard and confirm it fails**

Run: `bazel test --config=release //:legacy_flash_drain --test_output=errors`
Expected: FAIL, with output containing

```text
FAIL: legacy flash drain grew. New entries:
  jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.cpp
```

This proves the guard sees the JTAG operation; the deletion below is what turns it green.

- [ ] **Step 3: Delete the legacy operation and the dialog package**

```bash
git rm src/platform/desktop/common/flash/legacy/jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.cpp \
       src/platform/desktop/common/flash/legacy/jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.h \
       src/ui/desktop/flash/jtag/BUILD.bazel \
       src/ui/desktop/flash/jtag/flash_ecu_subaru_hitachi_m32r_jtag.cpp \
       src/ui/desktop/flash/jtag/flash_ecu_subaru_hitachi_m32r_jtag.h
```

Then confirm both directories are gone: `ls src/platform/desktop/common/flash/legacy/jtag src/ui/desktop/flash/jtag` must report "No such file or directory" for each.

- [ ] **Step 4: Remove the jtag entries from the legacy `BUILD.bazel`**

In `src/platform/desktop/common/flash/legacy/BUILD.bazel`, replace

```python
    srcs = [
        "bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.cpp",
        "ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.cpp",
        "flash_operation_worker.cpp",
        "jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.cpp",
        "legacy_flash_utils.cpp",
    ],
    # FlashOperationWorker (the shared QThread base) and the bootmode/ecu/
    # jtag legacy Qt operation classes, relocated together from
    # //src/backend/flash (step 5c, Task 15) -- backend owns no thread, and
    # FlashOperationWorker was the last QThread-derived class left there.
    # Every *_operation.h in the three family subdirectories declares Q_OBJECT.
    # The tcu/ family subdirectory is gone: wave 6a drained both of its
    # operations (K-Line, then CAN) to portable plans and executors.
    # The bdm/ subdirectory is gone: wave 6c-1 drained it.
    hdrs = [
        "bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.h",
        "ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.h",
        "flash_operation_worker.h",
        "jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.h",
    ],
```

with

```python
    srcs = [
        "bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.cpp",
        "ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.cpp",
        "flash_operation_worker.cpp",
        "legacy_flash_utils.cpp",
    ],
    # FlashOperationWorker (the shared QThread base) and the bootmode/ecu
    # legacy Qt operation classes, relocated together from
    # //src/backend/flash (step 5c, Task 15) -- backend owns no thread, and
    # FlashOperationWorker was the last QThread-derived class left there.
    # Every *_operation.h in the two family subdirectories declares Q_OBJECT.
    # The tcu/ family subdirectory is gone: wave 6a drained both of its
    # operations (K-Line, then CAN) to portable plans and executors.
    # The bdm/ subdirectory is gone: wave 6c-1 drained it.
    # The jtag/ subdirectory is gone: wave 6c-2 removed it (an unreachable
    # stub; see the 6c-2 removal design).
    hdrs = [
        "bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.h",
        "ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.h",
        "flash_operation_worker.h",
    ],
```

A few lines below, replace

```python
    # Every *.h in the four directories (root + three families) is in
    # hdrs above except this one -- matches prior glob behavior.
```

with

```python
    # Every *.h in the three directories (root + two families) is in
    # hdrs above except this one -- matches prior glob behavior.
```

Leave the `family_operation_sources` filegroup alone: its `*/*.cpp` glob is deliberately family-agnostic and still matches `bootmode/` and `ecu/`.

- [ ] **Step 5: Remove the `MainWindow` dispatch branch, include and dep**

In `src/ui/desktop/mainwindow.cpp`, delete exactly these lines (the "Hitachi ECU Boot Mode" comment above and the "Hitachi ECU" comment below stay):

```cpp

        /*
         * Hitachi ECU JTAG
         */
        else if (configValues->flash_protocol_selected_protocol_name.startsWith("sub_ecu_hitachi_m32r_jtag"))
        {
            FlashEcuSubaruHitachiM32rJtag flash_module(serial, ecuCalDef[rom_number], cmd_type, this);
            connect_signals_and_run_module(&flash_module);
        }
```

so the result reads

```cpp
        /*
         * Hitachi ECU Boot Mode
         */
        /*
         * Hitachi ECU
         */
```

In `src/ui/desktop/mainwindow.h`, delete

```cpp
// JTAG
#include "src/ui/desktop/flash/jtag/flash_ecu_subaru_hitachi_m32r_jtag.h"

```

In `src/ui/desktop/BUILD.bazel`, delete the line

```python
        "//src/ui/desktop/flash/jtag",
```

- [ ] **Step 6: Shrink the serial-compat allowlist and its frozen copy**

In `src/platform/desktop/common/serial/BUILD.bazel`, inside the `serial_qt_compat` visibility list, delete

```python
        "//src/ui/desktop/flash/jtag:__pkg__",
```

In `scripts/check-serial-compat-allowlist.py`, inside `FROZEN`, delete

```python
    "//src/ui/desktop/flash/jtag:__pkg__",
```

- [ ] **Step 7: Delete the JTAG macro block from `kernelcomms.h`**

In `src/backend/definitions/kernelcomms.h`, delete from the JTAG banner through the blank line after `SUB_KERNEL_JTAG_IR_ACK`, i.e. exactly:

```c
/*************************************
 * JTAG commands
 * **********************************/
// Commands
#define SUB_KERNEL_READ_USERCODE 0x30
#define SUB_KERNEL_JTAG_COMMAND 0x40

// JTAG instruction registers
#define SUB_KERNEL_IR_EXTEST 0x00
#define SUB_KERNEL_IR_SAMPLE 0x01
#define SUB_KERNEL_IR_IDCODE 0x02
#define SUB_KERNEL_IR_BYPASS 0x3F

// Registers
#define SUB_KERNEL_IDCODE 0x02
#define SUB_KERNEL_USERCODE 0x03
#define SUB_KERNEL_MDM_SYSTEM 0x08
#define SUB_KERNEL_MDM_CONTROL 0x09
#define SUB_KERNEL_MDM_SETUP 0x0A
#define SUB_KERNEL_MTM_CONTROL 0x0F
#define SUB_KERNEL_MON_CODE 0x10
#define SUB_KERNEL_MON_DATA 0x11
#define SUB_KERNEL_MON_PARAM 0x12
#define SUB_KERNEL_MON_ACCESS 0x13
#define SUB_KERNEL_DMA_RADDR 0x18
#define SUB_KERNEL_DMA_RDATA 0x19
#define SUB_KERNEL_DMA_RTYPE 0x1A
#define SUB_KERNEL_DMA_ACCESS 0x1B
#define SUB_KERNEL_RTDENB 0x20

// Subcommands
#define SUB_KERNEL_SUB_CMD_READ 0x00
#define SUB_KERNEL_SUB_CMD_WRITE 0x01
#define SUB_KERNEL_SUB_CMD_READ_BSR 0x02

// IR ok response
#define SUB_KERNEL_JTAG_IR_ACK 0x31

```

After the edit, `#define SUB_KERNEL_BLANK_PAGE 0x25` is followed by one blank line and then the `NisProg based kernels CAN commands` banner. Confirm no other file used the macros:

Run: `git grep -n -w -e SUB_KERNEL_JTAG_COMMAND -e SUB_KERNEL_IR_SAMPLE -e SUB_KERNEL_MON_CODE -e SUB_KERNEL_MON_DATA -e SUB_KERNEL_MON_ACCESS -e SUB_KERNEL_READ_USERCODE -e SUB_KERNEL_SUB_CMD_READ_BSR -e SUB_KERNEL_JTAG_IR_ACK -e SUB_KERNEL_RTDENB`
Expected: no output.

- [ ] **Step 8: Run the guards and confirm they pass**

Run: `bazel test --config=release //:legacy_flash_drain //:serial_compat_allowlist //:portable_closure --test_output=errors`
Expected: all PASS. The drain test logs `OK: 2 families remaining, none added.`; the allowlist test logs `OK: 9 entries, none added.` (10 before this task) with no "Update FROZEN" line. (`//:portable_closure` fails at build time, not test time, if broken.)

- [ ] **Step 9: Build the app and run the full suite**

Run: `bazel build --config=release //:fastecu`
Expected: build succeeds. A failure mentioning `flash_ecu_subaru_hitachi_m32r_jtag.h` or `FlashEcuSubaruHitachiM32rJtag` means Step 5 missed a reference.

Run: `bazel test --config=release //...`
Expected: all tests pass. `//src/ui/desktop:test_mainwindow` and `//src/platform/desktop/common/flash/legacy:test_flash_operation_worker` are the ones exercising the edited packages.

- [ ] **Step 10: Confirm no code reference survives**

Run: `git grep -n -i -e M32rJtag -e m32r_jtag -e flash/jtag -e legacy/jtag -- ':!docs'`
Expected: no output.

- [ ] **Step 11: Run the hooks and commit**

Run: `prek run --all-files`
Expected: all hooks pass (clang-format, buildifier, ruff, header and link checks).

```bash
git add -A scripts src
git commit -m "feat(flash): remove the unreachable Hitachi M32R JTAG family (wave 6c-2)

The family had no protocols.cfg entry, empty read_mem()/write_mem()
bodies that reported success, and a probe whose failures were ignored.
Deleting it takes //:legacy_flash_drain from three entries to two and
drops //src/ui/desktop/flash/jtag from the serial_qt_compat allowlist.
The probe's wire sequence is preserved in the 6c-2 removal design.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01BVWAZwWDdZGx6y3Ks4ptJX"
```

---

### Task 2: Record the removal in the matrix and planning documents

**Files:**
- Modify: `docs/flash-qualification-matrix.md:60`
- Modify: `docs/superpowers/specs/2026-08-08-step5-tail-flash-drain-design.md:299-301`
- Modify: `docs/superpowers/specs/2026-09-19-step5-tail-wave6-singletons-design.md:4,10-12,378`
- Modify: `docs/modularization-plan.md:79-82,217`
- Modify: `docs/superpowers/specs/2026-09-25-step5-tail-wave6c2-hitachi-m32r-jtag-removal-design.md:3`
- Test: `prek run --all-files` (lychee link check), final `git grep`

**Interfaces:**
- Consumes: Task 1's tree (no code references to the family remain).
- Produces: documentation only.

- [ ] **Step 1: Rewrite the matrix row**

In `docs/flash-qualification-matrix.md`, replace the entire line beginning `| FlashEcuSubaruHitachiM32rJtag | JTAG | JTAG |` with

```markdown
| FlashEcuSubaruHitachiM32rJtag | JTAG | JTAG | none — removed in wave 6c-2 | — | — | unqualified | — | Removed rather than migrated; see the [6c-2 removal design](superpowers/specs/2026-09-25-step5-tail-wave6c2-hitachi-m32r-jtag-removal-design.md). No `protocols.cfg` entry could select it, its `read_mem()` and `write_mem()` were empty bodies that reported success, and its JTAG probe ignored every failure. The probe's wire sequence is preserved in that design's appendix; the legacy source is recoverable at `766475b8`. A `sub_ecu_hitachi_m32r_jtag*` name now reaches `MainWindow`'s "Unknown flashmethod" warning. The row stays so the family does not silently disappear from migration scope. |
```

- [ ] **Step 2: Amend the tail design**

In `docs/superpowers/specs/2026-08-08-step5-tail-flash-drain-design.md`, replace

```markdown
  own mode is BDM; `FlashEcuSubaruHitachiM32rJtag` is unreachable from the UI
  because no `protocols.cfg` entry can produce its protocol name, and is ported
  as-is without wiring a new dispatch path.
```

with

```markdown
  own mode is BDM; `FlashEcuSubaruHitachiM32rJtag` is unreachable from the UI
  because no `protocols.cfg` entry can produce its protocol name, and is ported
  as-is without wiring a new dispatch path. *Superseded:* wave 6c-2 removed
  that family instead of porting it, because its read and write were empty
  stubs that reported success — see the
  [6c-2 removal design](2026-09-25-step5-tail-wave6c2-hitachi-m32r-jtag-removal-design.md).
```

- [ ] **Step 3: Amend the wave-6 singletons design**

In `docs/superpowers/specs/2026-09-19-step5-tail-wave6-singletons-design.md`, replace the status line

```markdown
**Status:** in progress — waves 6a-1 through 6a-3 are in the base; 6a-4 is implemented on this branch. Six legacy families remain.
```

with

```markdown
**Status:** in progress — waves 6a-1 through 6c-1 are merged; 6c-2 (a removal, not a migration) is implemented on this branch. Two legacy families remain.
```

Replace

```markdown
`FlashPlan` + `IFlashExecutor` pairs, taking `//:legacy_flash_drain` from ten
entries to one. After this wave only
```

with

```markdown
`FlashPlan` + `IFlashExecutor` pairs, taking `//:legacy_flash_drain` from ten
entries to one (one of the nine, JTAG, by removal — see the 6c-2 note below).
After this wave only
```

Append at the end of the file, after the "Wave 6c-1 implementation note" section:

```markdown

## Wave 6c-2 implementation note

The [Hitachi M32R JTAG removal design](2026-09-25-step5-tail-wave6c2-hitachi-m32r-jtag-removal-design.md)
records one departure from this design: `FlashEcuSubaruHitachiM32rJtag` is
removed, not migrated. No `protocols.cfg` entry could select it, its read and
write were empty stubs that reported success, and its probe ignored every
failure. Porting the stubs would legitimize a no-op, and porting only the probe
would add unreachable portable code. The probe's wire sequence is preserved in
that design's appendix. `//src/ui/desktop/flash/jtag` also leaves the
`serial_qt_compat` allowlist. The drain moves from three entries to two.
```

- [ ] **Step 4: Update the modularization plan**

In `docs/modularization-plan.md`, replace

```markdown
- Wave 6c-1 `FlashEcuSubaruDensoMC68HC16Y5_02_BDM` — implemented on this
  branch. The drain is three remaining families. Hardware status remains
  experimental. See the
  [family design](superpowers/specs/2026-09-25-step5-tail-wave6c1-denso-mc68hc16-bdm-design.md).
```

with

```markdown
- Wave 6c-1 `FlashEcuSubaruDensoMC68HC16Y5_02_BDM` — merged (#357). See the
  [family design](superpowers/specs/2026-09-25-step5-tail-wave6c1-denso-mc68hc16-bdm-design.md).
- Wave 6c-2 `FlashEcuSubaruHitachiM32rJtag` — removed on this branch, not
  migrated: it was unreachable and its read and write were stubs. The drain is
  two remaining families. See the
  [removal design](superpowers/specs/2026-09-25-step5-tail-wave6c2-hitachi-m32r-jtag-removal-design.md).
```

Replace the step-5 heading

```markdown
5. **Make backend workflows portable — 5a through 5e complete; flash-tail Wave 6a-3 implemented on this branch, with 7 legacy families remaining**
```

with

```markdown
5. **Make backend workflows portable — 5a through 5e complete; flash-tail Wave 6c-2 implemented on this branch, with 2 legacy families remaining**
```

- [ ] **Step 5: Link the spec to this plan**

In `docs/superpowers/specs/2026-09-25-step5-tail-wave6c2-hitachi-m32r-jtag-removal-design.md`, replace

```markdown
**Status:** design approved; implementation plan to follow.
```

with

```markdown
**Status:** design approved; implementation plan in [the 6c-2 plan](../plans/2026-09-25-step5-tail-wave6c2-hitachi-m32r-jtag-removal.md).
```

- [ ] **Step 6: Run the hooks**

Run: `prek run --all-files`
Expected: all hooks pass. A lychee failure means a relative link path above is wrong; fix the path, not the hook.

- [ ] **Step 7: Final reference check**

Run: `git grep -n -i -e M32rJtag -e m32r_jtag -e flash/jtag -- ':!docs/superpowers/plans'`
Expected: hits only under `docs/` — the matrix row, the tail, singletons, 6c-2 and older step-5c/wave-3 specs, and `modularization-plan.md`. Any hit under `src/`, `scripts/`, `bazel/` or a `BUILD.bazel` means Task 1 is incomplete.

- [ ] **Step 8: Commit**

```bash
git add docs
git commit -m "docs(flash): record the Hitachi M32R JTAG removal (wave 6c-2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01BVWAZwWDdZGx6y3Ks4ptJX"
```

- [ ] **Step 9: Hand off**

Report the three branch commits (spec, removal, docs) and the test results to the user. Push `feat/wave6c2-hitachi-m32r-jtag-removal` and open the PR against `master` only once the user authorizes it; the PR body ends with:

```text
🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01BVWAZwWDdZGx6y3Ks4ptJX
```
