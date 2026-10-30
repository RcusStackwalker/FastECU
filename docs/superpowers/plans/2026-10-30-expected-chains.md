# Expected Chains Plan

**Goal:** Convert straight-line runs of `if (!r.has_value()) { return ...; }`
early returns in `src/backend/flash/` into `std::expected` `and_then` /
`transform` chains, following the shape PRs #912, #913, #919, #921, #923 and
#929 introduced (`UnlockAndKernelJump` in
`src/backend/flash/ecu/uds_client_exchange_common.cpp` is the reference).

**Rule:** [Error handling](../../coding-style.md#error-handling) in the coding
style guide.

## Constraints

- **Candidate:** two or more fallible calls in straight-line code; each step
  consumes only the previous step's value (or nothing); logging or a small
  reply check may sit between steps. A trailing `return Call(...);` counts as
  the last step.
- **Excluded:** runs inside loops; steps that need values from two or more
  earlier steps; failure branches that do more than propagate.
- **Not a run:** one fallible call followed only by reply checks or a value
  construction (`ExchangeImpl`, `Required`, `ParseRomId`, `ResolveKernel`,
  a `Run` read tail). These stay early returns (settled in #935-#940).
- **A value used after later steps** may still be chained: nest the later
  steps inside the lambda that receives it and hand it back with
  `.transform([&] { return std::move(value); })` (`Connect` in #938,
  `PrepareRead` in #939). A final `.transform` may combine the chain's value
  with a local checked before the chain (`BuildPlans` in #940).
- **Non-log statements between steps** are fine inside a lambda when they
  have no ordering side effect beyond the step itself; check destructors
  (`PhaseReporter` emits nothing on destruction, so phase start/complete
  calls moved into the lambdas in #938).
- **Partial runs** inside longer functions are converted. Extract a named
  helper only when that also removes duplication.
- **Behavior is byte-identical:** log text, log order, error kind and message
  stay as they are. No transcript test changes. A site that cannot convert
  without a log change stays an early return.
- **Untested failure branches:** if a converted step's failure branch is not
  exercised, add the failing-path test first, in the same PR, against the
  unconverted code. Split a file across PRs when that grows large. A branch
  that plan validation makes unreachable through `Execute` (empty kernel,
  unplaced image in #936) is converted anyway and named in the PR body.
- **Test cost:** sweeps must stay cheap; the CI Mull job reruns targets per
  mutant. Script only what a sweep can reach (stop when the fault no longer
  fires), build multi-MiB plans once, memoize test-side encryption, and
  anchor cancellation sweeps on a log line (`events.logs.back()`) instead of
  counting thousands of checks. Note the target's run time in the PR body
  when it grows noticeably.
- **NLOC:** record the production and test change and the SonarCloud
  duplication change in the PR body. Reject a PR that adds more than 5
  production NLOC; failure-path tests added under the rule below are exempt.
  This series is not an ADR 0029 loop PR and has no NLOC-drop floor.
- **Label:** `expected-chains`. Auto-merge after CI's Mull run shows no new
  surviving mutants.
- **Order:** one PR per file, or per small same-family group for files with
  two or fewer candidates, highest candidate count first (table below).
- **Phase 2** (`src/backend/definition`, `src/backend/diagnostics`,
  `src/platform`) lands in one PR, one commit per file, tests first (user
  decision, 2026-10-30). Its inventory is at the end of this plan.

## Per-PR steps

- [ ] Re-read the file; confirm each listed run against the rule (line numbers
      drift as earlier PRs merge) and resolve every `review` entry.
- [ ] Check failure-branch coverage for each run; add missing failing-path
      tests first and see them pass against the unconverted code.
- [ ] Convert; `clang-format`; build and run the package's tests.
- [ ] Measure NLOC before and after; open the PR with the `expected-chains`
      label; hold auto-merge until Mull is clean ("No mutants found" on the
      changed lines counts as clean).
- [ ] After each push, confirm `git ls-remote origin <branch>` matches
      `HEAD` (SSH pushes fail intermittently and quietly); after `gh pr
      create`, confirm the label landed.
- [ ] Tick the file off below. PRs prepared in parallel all add a row at the
      same spot: land them one at a time, leaving `#TBD` until the rebase
      onto the previous merge.

## Progress

| File | PR | Production NLOC | Test NLOC |
|---|---|---|---|
| `ecu/subaru_denso_sh705x_kline_executor.cpp` | #931 | -75 | +195 |
| `eeprom/denso_sh705x_eeprom_kline_executor.cpp` (+ shared `CheckCancelled`) | #932 | -77 | +163 |
| `ecu/subaru_unisia_jecs_m32r_kline_executor.cpp` (+ shared `FaultingKlineFlashTransport`) | #933 | -33 | +88 |
| `ecu/denso_iso15765_can_common.cpp` | #934 | -47 | +168 |
| `ecu/subaru_denso_sh7055_02_executor.cpp` | #935 | -30 | +217 |
| `ecu/subaru_denso_mc68hc16y5_02_executor.cpp` | #936 | -23 | +182 |
| `ecu/subaru_denso_mc68hc16y5_02_bdm_executor.cpp` (+ raw ops in `FaultingKlineFlashTransport`) | #937 | -22 | +117 |
| `ecu/subaru_hitachi_sh72543r_can_executor.cpp` | #938 | -31 | +133 |
| `ecu/subaru_hitachi_m32r_kline_executor.cpp` | #939 | -45 | +298 |
| `workflow/flash_workflow.cpp` | #940 | -7 | 0 |
| `ecu/subaru_unisia_jecs_m32r_bootmode_program_executor.cpp` | #942 | -17 | +86 |
| `flash_executor.h` | #943 | -17 | 0 |
| `testing/faulting_can_flash_transport.h` (shared, replaces a local copy) | #943 | 0 | +19 |
| `ports/testing/faulting_clock.h` (shared, replaces three local copies) | #943 | 0 | -19 |
| `eeprom/denso_sh705x_eeprom_can_executor.cpp` | #943 | -34 | +284 |
| `ecu/subaru_tcu_hitachi_m32r_can_executor.cpp` | #943 | -27 | +111 |
| `ecu/subaru_tcu_denso_sh705x_can_executor.cpp` | #943 | -9 | +67 |
| `ecu/beef_can_kernel.cpp` | #943 | -7 | +9 |
| `ecu/subaru_mitsu_m32r_kline_executor.cpp` | #943 | -6 | +240 |
| `ecu/subaru_unisia_jecs_executor.cpp` | #943 | -14 | +13 |
| `ecu/subaru_hitachi_sh7058_kline_executor.cpp` | #943 | -17 | +173 |
| `ecu/denso_beef_kline_common.cpp` | #943 | -28 | 0 |
| `ecu/subaru_unisia_jecs_m32r_bootmode_kernel_executor.cpp` | #943 | -7 | +55 |
| `ecu/denso_iso15765_bootloader_executor.cpp` | #943 | -5 | +30 |
| `ecu/single_window_plan.cpp` | #943 | -6 | +52 |
| `ecu/denso_beef_can_ecu_execution.cpp` | #943 | -8 | +16 |
| `ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.cpp` | #943 | -9 | +77 |
| `ecu/subaru_hitachi_m32r_can_executor.cpp` | #943 | -18 | +68 |
| `ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.cpp` | #943 | -4 | +75 |
| `ecu/subaru_denso_sh705x_kline_plan.cpp` | #943 | -10 | +5 |
| `ecu/subaru_denso_mc68hc16y5_02_plan.cpp` | #943 | -12 | +7 |
| `ecu/plan_primitives.h` | #943 | -3 | 0 |
| `ecu/m32r_ssm_exchange.cpp` | #943 | -4 | 0 |
| `ecu/subaru_denso_sh7058_can_diesel_executor.cpp` | #943 | -5 | +17 |
| `ecu/subaru_hitachi_sh7058_can_executor.cpp` | #943 | -5 | 0 |
| `ecu/subaru_tcu_hitachi_m32r_kline_executor.cpp` | #943 | -6 | +15 |
| `flash_validation.cpp` | #943 | -6 | 0 |
| `ecu/subaru_denso_sh7055_02_plan.cpp` | #943 | -7 | +8 |
| `ecu/subaru_tcu_hitachi_m32r_can_plan.cpp` | #943 | -7 | 0 |
| `ecu/subaru_hitachi_sh72543r_can_plan.cpp` | #943 | -7 | 0 |
| `eeprom/eeprom_read_plan.cpp` | #943 | 0 | 0 |

The rest of phase 1 landed in one PR (#943), one commit per file. Kept on
purpose: `ecu/subaru_hitachi_sh7058_can_executor.cpp`'s `Exchange` (its read
step turns a timeout into an empty reply) and its programming-mode step (the
branch picks from `mode` across a sleep); `ecu/bootloader_operation.h` (the
phase reporters' scopes carry the phase order);
`ecu/subaru_unisia_jecs_m32r_bootmode_plan.cpp`,
`ecu/subaru_unisia_jecs_m32r_kline_plan.cpp`, `ecu/denso_beef_kline_common.h`
and `ecu/subaru_tcu_denso_sh705x_can_plan.cpp` (the prefix ending at the
reused value is a single step); and `eeprom/eeprom_read_plan.cpp` 80-90 (the
input struct needs two earlier values).

## Inventory

Generated on 2026-10-30 from `master` at `0dbaa7b4` by a brace-aware scan for
propagate-only checks (`return r;`, `return std::unexpected(r.error());`)
grouped by block, then spot-checked by hand. Each PR re-verifies its file.
The scan misses runs whose calls carry a brace initializer
(`SsmStep(..., bytes::Bytes{0x81}, ...)`) and runs whose last step is a
reply check on the previous value, so reading the file finds more than it
lists. It also missed runs written as an `else if` ladder over an outcome
variable (`Run` in `subaru_hitachi_m32r_kline_executor.cpp`, #939) and runs
whose last call takes lambdas (`WriteMem` in the MC68HC16Y5 02 executor,
#936).

- `candidate`: matches the rule as scanned.
- `review`: non-log statements sit between steps (locals, phase reporters,
  out-parameters); decide when converting.
- `excluded`: reason given. For "used after a later step", the prefix of the
  run ending at that value may still qualify.
- `tail`: the run ends the function, so the whole tail becomes `return chain;`.

Totals: 90 candidate runs, 14 to review, 52 excluded, across 44 files.

| # | File | Candidate | Review | Excluded |
|---|---|---|---|---|
| 1 | `ecu/subaru_denso_sh705x_kline_executor.cpp` | 9 | 1 | 8 |
| 2 | `eeprom/denso_sh705x_eeprom_kline_executor.cpp` | 7 | 2 | 1 |
| 3 | `ecu/subaru_unisia_jecs_m32r_kline_executor.cpp` | 6 | 0 | 1 |
| 4 | `ecu/denso_iso15765_can_common.cpp` | 6 | 0 | 0 |
| 5 | `ecu/subaru_denso_sh7055_02_executor.cpp` | 5 | 2 | 4 |
| 6 | `ecu/subaru_denso_mc68hc16y5_02_executor.cpp` | 5 | 2 | 3 |
| 7 | `ecu/subaru_denso_mc68hc16y5_02_bdm_executor.cpp` | 5 | 0 | 2 |
| 8 | `ecu/subaru_hitachi_sh72543r_can_executor.cpp` | 4 | 2 | 1 |
| 9 | `ecu/subaru_hitachi_m32r_kline_executor.cpp` | 3 | 0 | 0 |
| 10 | `workflow/flash_workflow.cpp` | 3 | 0 | 1 |
| 11 | `ecu/subaru_unisia_jecs_m32r_bootmode_program_executor.cpp` | 3 | 0 | 0 |
| 12 | `flash_executor.h` | 3 | 0 | 0 |
| 13 | `eeprom/denso_sh705x_eeprom_can_executor.cpp` | 2 | 0 | 1 |
| 14 | `ecu/subaru_tcu_hitachi_m32r_can_executor.cpp` | 2 | 0 | 1 |
| 15 | `ecu/subaru_tcu_denso_sh705x_can_executor.cpp` | 2 | 0 | 1 |
| 16 | `ecu/beef_can_kernel.cpp` | 2 | 0 | 1 |
| 17 | `ecu/subaru_mitsu_m32r_kline_executor.cpp` | 2 | 0 | 0 |
| 18 | `ecu/subaru_unisia_jecs_executor.cpp` | 2 | 0 | 2 |
| 19 | `ecu/subaru_hitachi_sh7058_kline_executor.cpp` | 2 | 0 | 0 |
| 20 | `ecu/denso_beef_kline_common.cpp` | 2 | 0 | 1 |
| 21 | `ecu/subaru_unisia_jecs_m32r_bootmode_kernel_executor.cpp` | 2 | 0 | 1 |
| 22 | `ecu/denso_iso15765_bootloader_executor.cpp` | 2 | 0 | 0 |
| 23 | `ecu/single_window_plan.cpp` | 2 | 0 | 1 |
| 24 | `ecu/denso_beef_can_ecu_execution.cpp` | 1 | 0 | 3 |
| 25 | `ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.cpp` | 1 | 0 | 1 |
| 26 | `ecu/subaru_hitachi_m32r_can_executor.cpp` | 1 | 0 | 0 |
| 27 | `ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.cpp` | 1 | 0 | 1 |
| 28 | `ecu/subaru_denso_sh705x_kline_plan.cpp` | 1 | 0 | 3 |
| 29 | `ecu/subaru_denso_mc68hc16y5_02_plan.cpp` | 1 | 0 | 1 |
| 30 | `ecu/plan_primitives.h` | 1 | 0 | 0 |
| 31 | `ecu/m32r_ssm_exchange.cpp` | 1 | 0 | 0 |
| 32 | `ecu/subaru_denso_sh7058_can_diesel_executor.cpp` | 1 | 0 | 0 |
| 33 | `ecu/subaru_hitachi_sh7058_can_executor.cpp` | 0 | 3 | 0 |
| 34 | `ecu/subaru_tcu_hitachi_m32r_kline_executor.cpp` | 0 | 0 | 1 |
| 35 | `flash_validation.cpp` | 0 | 0 | 1 |
| 36 | `ecu/subaru_unisia_jecs_m32r_bootmode_plan.cpp` | 0 | 0 | 3 |
| 37 | `ecu/subaru_unisia_jecs_m32r_kline_plan.cpp` | 0 | 0 | 2 |
| 38 | `ecu/subaru_denso_sh7055_02_plan.cpp` | 0 | 0 | 1 |
| 39 | `ecu/denso_beef_kline_common.h` | 0 | 0 | 1 |
| 40 | `ecu/subaru_tcu_hitachi_m32r_can_plan.cpp` | 0 | 0 | 1 |
| 41 | `ecu/subaru_hitachi_sh72543r_can_plan.cpp` | 0 | 0 | 1 |
| 42 | `eeprom/eeprom_read_plan.cpp` | 0 | 1 | 1 |
| 43 | `ecu/bootloader_operation.h` | 0 | 1 | 0 |
| 44 | `ecu/subaru_tcu_denso_sh705x_can_plan.cpp` | 0 | 0 | 1 |

### `src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.cpp`

- 97-106 (2 steps): candidate
- 117-130 (3 steps): excluded — `received` used after a later step; a prefix ending at `received` may qualify
- 154-159 (2 steps, tail): candidate
- 207-224 (3 steps): candidate
- 282-291 (2 steps): excluded — `reply` used after a later step; a prefix ending at `reply` may qualify
- 379-401 (5 steps): excluded — `ecu_id` used after a later step; a prefix ending at `ecu_id` may qualify
- 474-487 (2 steps): candidate
- 497-521 (4 steps): candidate
- 559-571 (2 steps): excluded — `ecu_id` used after a later step; a prefix ending at `ecu_id` may qualify
- 658-679 (4 steps): review — non-log statements between steps
- 741-752 (2 steps): excluded — `reply` used after a later step; a prefix ending at `reply` may qualify
- 758-770 (2 steps): candidate
- 792-803 (2 steps): candidate
- 811-830 (3 steps): excluded — inside a loop
- 850-862 (2 steps): excluded — inside a loop
- 887-897 (2 steps): excluded — `volt` used after a later step; a prefix ending at `volt` may qualify
- 1030-1040 (3 steps, tail): candidate
- 1047-1058 (2 steps): candidate

### `src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor.cpp`

- 172-180 (2 steps): candidate
- 274-282 (2 steps): candidate
- 324-338 (2 steps): candidate
- 406-414 (2 steps): candidate
- 426-435 (2 steps): excluded — inside a loop
- 482-506 (2 steps): review — non-log statements between steps
- 570-583 (2 steps): candidate
- 595-603 (2 steps, tail): candidate
- 647-674 (4 steps): review — non-log statements between steps
- 684-696 (3 steps, tail): candidate

### `src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_executor.cpp`

- 88-97 (2 steps): candidate
- 146-160 (3 steps): candidate
- 171-197 (5 steps): excluded — `cold` used after a later step; a prefix ending at `cold` may qualify
- 297-311 (3 steps): candidate
- 316-326 (2 steps): candidate
- 345-366 (4 steps): candidate
- 417-436 (4 steps): candidate

### `src/backend/flash/ecu/denso_iso15765_can_common.cpp`

- 95-104 (2 steps): candidate
- 152-160 (2 steps, tail): candidate
- 322-327 (2 steps, tail): candidate
- 419-428 (2 steps): candidate
- 628-638 (2 steps): candidate
- 676-682 (2 steps, tail): candidate

### `src/backend/flash/ecu/subaru_denso_sh7055_02_executor.cpp`

- 96-109 (2 steps): review — non-log statements between steps
- 166-171 (2 steps, tail): candidate
- 181-191 (2 steps): excluded — inside a loop
- 320-332 (2 steps): candidate
- 347-360 (3 steps): candidate
- 447-463 (3 steps): candidate
- 490-502 (2 steps): excluded — inside a loop
- 544-555 (2 steps): excluded — `crc` used after a later step; a prefix ending at `crc` may qualify
- 584-600 (2 steps): excluded — inside a loop
- 656-662 (2 steps, tail): candidate
- 669-691 (4 steps): review — non-log statements between steps

### `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor.cpp`

- 138-147 (2 steps): candidate
- 192-202 (2 steps): excluded — inside a loop
- 251-261 (2 steps, tail): candidate
- 282-291 (2 steps): candidate
- 351-362 (2 steps): candidate
- 370-378 (2 steps): candidate
- 411-422 (2 steps): excluded — inside a loop
- 457-467 (2 steps): review — non-log statements between steps
- 498-514 (2 steps): excluded — inside a loop
- 580-601 (4 steps): review — non-log statements between steps

### `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor.cpp`

- 192-201 (2 steps): excluded — inside a loop
- 222-227 (2 steps, tail): candidate
- 249-263 (2 steps): candidate
- 267-277 (2 steps): excluded — inside a loop
- 291-302 (2 steps): candidate
- 331-341 (2 steps): candidate
- 356-365 (3 steps, tail): candidate

### `src/backend/flash/ecu/subaru_hitachi_sh72543r_can_executor.cpp`

- 58-67 (2 steps): review — non-log statements between steps
- 91-96 (2 steps, tail): candidate
- 100-109 (3 steps, tail): candidate
- 239-248 (2 steps): excluded — `id` used after a later step; a prefix ending at `id` may qualify
- 264-272 (2 steps): candidate
- 323-332 (2 steps): candidate
- 506-522 (3 steps): review — non-log statements between steps

### `src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor.cpp`

- 237-242 (2 steps, tail): candidate
- 357-365 (2 steps): candidate
- 369-377 (2 steps): candidate

### `src/backend/flash/workflow/flash_workflow.cpp`

- 502-521 (3 steps): excluded — `program` used after a later step; a prefix ending at `program` may qualify
- 657-664 (2 steps, tail): candidate
- 690-697 (2 steps, tail): candidate
- 709-716 (2 steps, tail): candidate

### `src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_program_executor.cpp`

- 127-149 (5 steps): candidate
- 191-199 (2 steps): candidate
- 226-232 (2 steps, tail): candidate

### `src/backend/flash/flash_executor.h`

- 380-385 (2 steps, tail): candidate
- 410-415 (2 steps, tail): candidate
- 474-491 (4 steps): candidate

### `src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.cpp`

- 360-367 (2 steps, tail): candidate
- 747-780 (5 steps): excluded — step uses a value from 2+ steps back
- 828-847 (3 steps): candidate

### `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_executor.cpp`

- 331-340 (2 steps): excluded — `rom_id` used after a later step; a prefix ending at `rom_id` may qualify
- 437-450 (3 steps): candidate
- 763-770 (2 steps, tail): candidate

### `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp`

- 224-238 (3 steps): excluded — inside a loop
- 417-427 (2 steps): candidate
- 484-492 (2 steps): candidate

### `src/backend/flash/ecu/beef_can_kernel.cpp`

- 121-134 (3 steps): candidate
- 157-167 (2 steps): candidate
- 365-377 (2 steps): excluded — inside a loop

### `src/backend/flash/ecu/subaru_mitsu_m32r_kline_executor.cpp`

- 98-104 (2 steps, tail): candidate
- 256-266 (2 steps): candidate

### `src/backend/flash/ecu/subaru_unisia_jecs_executor.cpp`

- 83-95 (3 steps): candidate
- 115-125 (2 steps): excluded — inside a loop
- 148-159 (2 steps, tail): excluded — inside a loop
- 168-186 (4 steps): candidate

### `src/backend/flash/ecu/subaru_hitachi_sh7058_kline_executor.cpp`

- 35-44 (2 steps): candidate
- 147-155 (2 steps): candidate

### `src/backend/flash/ecu/denso_beef_kline_common.cpp`

- 36-46 (3 steps, tail): candidate
- 59-71 (3 steps): candidate
- 78-91 (3 steps): excluded — `received` used after a later step; a prefix ending at `received` may qualify

### `src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_kernel_executor.cpp`

- 47-57 (2 steps): excluded — inside a loop
- 63-76 (2 steps): candidate
- 96-102 (2 steps, tail): candidate

### `src/backend/flash/ecu/denso_iso15765_bootloader_executor.cpp`

- 135-144 (2 steps): candidate
- 187-203 (3 steps, tail): candidate

### `src/backend/flash/ecu/single_window_plan.cpp`

- 71-76 (2 steps, tail): candidate
- 105-113 (2 steps): candidate
- 138-147 (2 steps): excluded — `plan` used after a later step; a prefix ending at `plan` may qualify

### `src/backend/flash/ecu/denso_beef_can_ecu_execution.cpp`

- 101-109 (2 steps): candidate
- 401-412 (2 steps): excluded — `seed_alter` used after a later step; a prefix ending at `seed_alter` may qualify
- 452-456 (2 steps, tail): excluded — false positive (`switch` case returns)
- 700-723 (5 steps): excluded — step uses a value from 2+ steps back

### `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.cpp`

- 113-118 (2 steps, tail): candidate
- 468-486 (3 steps): excluded — `reply` used after a later step; a prefix ending at `reply` may qualify

### `src/backend/flash/ecu/subaru_hitachi_m32r_can_executor.cpp`

- 226-234 (2 steps): candidate

### `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.cpp`

- 93-102 (2 steps): candidate
- 239-250 (2 steps): excluded — `rom` used after a later step; a prefix ending at `rom` may qualify

### `src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.cpp`

- 145-155 (2 steps): excluded — `variant` used after a later step; a prefix ending at `variant` may qualify
- 168-177 (2 steps): candidate
- 188-210 (5 steps): excluded — step uses a value from 2+ steps back
- 224-233 (2 steps): excluded — `plan` used after a later step; a prefix ending at `plan` may qualify

### `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.cpp`

- 184-196 (3 steps): candidate
- 238-247 (2 steps): excluded — `plan` used after a later step; a prefix ending at `plan` may qualify

### `src/backend/flash/ecu/plan_primitives.h`

- 96-106 (2 steps): candidate

### `src/backend/flash/ecu/m32r_ssm_exchange.cpp`

- 69-75 (2 steps, tail): candidate

### `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.cpp`

- 28-33 (2 steps, tail): candidate

### `src/backend/flash/ecu/subaru_hitachi_sh7058_can_executor.cpp`

- 36-45 (2 steps): review — non-log statements between steps
- 217-226 (2 steps): review — non-log statements between steps
- 324-340 (3 steps): review — non-log statements between steps

### `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor.cpp`

- 226-240 (3 steps): excluded — `id` used after a later step; a prefix ending at `id` may qualify

### `src/backend/flash/flash_validation.cpp`

- 148-165 (4 steps): excluded — `rom_image` used after a later step; a prefix ending at `rom_image` may qualify

### `src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.cpp`

- 143-152 (2 steps): excluded — `variant` used after a later step; a prefix ending at `variant` may qualify
- 162-171 (2 steps): excluded — `variant` used after a later step; a prefix ending at `variant` may qualify
- 184-193 (2 steps): excluded — `variant` used after a later step; a prefix ending at `variant` may qualify

### `src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.cpp`

- 97-106 (2 steps): excluded — `variant` used after a later step; a prefix ending at `variant` may qualify
- 131-140 (2 steps): excluded — `variant` used after a later step; a prefix ending at `variant` may qualify

### `src/backend/flash/ecu/subaru_denso_sh7055_02_plan.cpp`

- 155-164 (2 steps): excluded — `plan` used after a later step; a prefix ending at `plan` may qualify

### `src/backend/flash/ecu/denso_beef_kline_common.h`

- 118-127 (2 steps): excluded — `modified_count` used after a later step; a prefix ending at `modified_count` may qualify

### `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_plan.cpp`

- 147-156 (2 steps): excluded — `plan` used after a later step; a prefix ending at `plan` may qualify

### `src/backend/flash/ecu/subaru_hitachi_sh72543r_can_plan.cpp`

- 137-146 (2 steps): excluded — `plan` used after a later step; a prefix ending at `plan` may qualify

### `src/backend/flash/eeprom/eeprom_read_plan.cpp`

- 80-90 (2 steps): excluded — `eeprom_region` used after a later step; a prefix ending at `eeprom_region` may qualify
- 107-122 (3 steps, tail): review — non-log statements between steps

### `src/backend/flash/ecu/bootloader_operation.h`

- 74-84 (2 steps): review — non-log statements between steps

### `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_plan.cpp`

- 74-83 (2 steps): excluded — `entry` used after a later step; a prefix ending at `entry` may qualify

## Phase 2 inventory

Read by hand on 2026-10-30 from `master` at `06fe4a27`: every non-test file
under `src/backend/definition`, `src/backend/diagnostics` and `src/platform`
that checks a `Result`. Labels as in phase 1, plus `not a run` for one
fallible call followed only by checks or a value construction. Files with
nothing but loops or single calls are listed once at the end.

Totals: 35 candidate runs, 8 to review, across 14 files.

| # | File | Candidate | Review |
|---|---|---|---|
| 1 | `definition/parser_utils.cpp` | 7 | 2 |
| 2 | `platform/.../transport/desktop_mixed_can_flash_transport.cpp` | 5 | 2 |
| 3 | `diagnostics/ssm_identify.cpp` | 5 | 0 |
| 4 | `diagnostics/dtc_session.cpp` | 3 | 2 |
| 5 | `definition/definition_resolver.cpp` | 3 | 0 |
| 6 | `definition/ecuflash_parser.cpp` | 2 | 0 |
| 7 | `definition/definition_service.cpp` | 2 | 0 |
| 8 | `definition/definition_writer.cpp` | 2 | 0 |
| 9 | `platform/.../service_functions/serial_facade_configurator.cpp` | 2 | 0 |
| 10 | `definition/romraider_parser.cpp` | 1 | 1 |
| 11 | `definition/parser_utils.h` | 1 | 0 |
| 12 | `definition/definition_header_fields.cpp` | 1 | 0 |
| 13 | `platform/.../transport/desktop_transport_factory.cpp` | 1 | 0 |
| 14 | `platform/.../service_functions/worker/service_function_worker.cpp` | 0 | 1 |

`platform/...` is `src/platform/desktop/common`. Nothing under
`src/platform/desktop/unix` or `windows` propagates a `Result`, so every
phase 2 file builds and tests on macOS.

### `src/backend/definition/parser_utils.cpp`

- 114-119 `ValidateHeaderStructure` (2 steps, tail): candidate
- 234-239 `DefinitionIdForRom` (2 steps, tail): candidate
- 244-261 `ParseRomHeader` (2 steps, tail): candidate — `definition_id` is used after the address step; nest
- 375-390 `OptionalAddress` (2 steps, tail): candidate — an absent attribute returns early inside the lambda
- 484-512 `PopulateCommonAxisAttributes` (3 steps): review — out-parameter assignments between steps
- 553-574 `PopulateCommonMapAttributes` (3 steps): review — out-parameter assignments between steps
- 629-643 `PopulateMapHeader` (2 steps + name check, tail): candidate
- 649-653 `PopulateMapSize` (2 steps, tail): candidate
- 659-667 `PopulateMapOrientation` (3 steps, tail): candidate
- not a run: `ParseHeaderAddress`, `IdentityElement`, `ParseRoot`, `OptionalHexAttribute`, `OptionalHexAttribute32`, `PopulateOptional{Dimension,HexDimension,Boolean}`

### `src/backend/definition/parser_utils.h`

- 130-144 `ParseAxisDefinition` (2 steps + name check, partial): candidate
- 113-119 `ParseMapScaling`, 160-166 inline scaling: not a run
- 221-231 `PopulateAxes`: excluded — loop

### `src/backend/definition/romraider_parser.cpp`

- 84-98 `ParseScaling` (2 steps): review — the owner's storage type is a fallback read only when the node has none
- 121-141 `ParseTable` (4 steps, partial): candidate — the scaling is used after the run
- 166-170 `ParseTable` axes: not a run
- `ParseRomraiderIndex`, `ParseRomraiderDefinition`: excluded — one step before a loop, the rest in loops

### `src/backend/definition/ecuflash_parser.cpp`

- 196-217 `ParseTable` (5 steps, tail): candidate
- 237-242 `ParseHeader` (2 steps, tail): candidate
- not a run: `ParseScaling`, `PopulateDimensions`, `AddMapScaling`, `ParseEcuflashIndex`
- `ParseEcuflashDefinition`: excluded — one step before loops

### `src/backend/definition/definition_service.cpp`

- 370-375 `CreateDefinition` (2 steps, tail): candidate
- 381-391 `ImportDefinition` (3 steps, tail): candidate
- 315-343 `Load` loader: excluded — the read failure branch records `repository_error`
- 346-356 `Load`: excluded — the resolve failure branch swaps in the repository error
- `IdentifierCandidates`, `BuildEcuflashCatalog`: not a run; `DiscoverXml`, `BuildCatalog`: excluded — loop, skipped failures

### `src/backend/definition/definition_resolver.cpp`

- 533-548 `ApplyAxisScaling` (2 steps + check, tail): candidate — the scaling is used after the agreement check; nest
- 601-617 `ApplyMapScaling` (2 steps, tail): candidate — likewise
- 630-651 `ValidateMap` (3 steps + checks, tail): candidate
- 743-759 `ResolveRoot`, 793-829 `Resolve`, 846-868 `ResolveParent`: excluded — failure branches rewrap the error with chain context
- `ResolveScaling`, `Materialize`, `ValidateAndResolveMaps`: excluded — loop

### `src/backend/definition/definition_header_fields.cpp`

- 17-44 `ReadHeader` (2 steps + root check, tail): candidate — `rom` is used after the structure check; nest
- 60-71 `BuildDefinitionHeaderInput`: not a run (`NormalizeHeaderInput` cannot fail)

### `src/backend/definition/definition_writer.cpp`

- 210-214 `CreateEcuflashXml` (2 steps, tail): candidate
- 240-244 `RewriteEcuflashXml` (2 steps, tail): candidate
- not a run: `ValidateInput`, `UpdateHeader`, `SerializeAndValidate`

### `src/backend/diagnostics/ssm_identify.cpp`

- 142-168 `ReadSsm2InitFrame` (3 steps + checks, tail): candidate
- 176-204 `IdentifyKlineSsm2` (5 steps + ID parse, tail): candidate — the frame is used after validation; nest
- 233-244 `WakeSsm1` (3 steps, tail): candidate
- 283-311 `IdentifySsm1` (4 steps + checks, tail): candidate
- 317-345 `IdentifyIso15765Uds` (3 steps + checks, tail): candidate
- not a run: `ReadInto`, `CheckWritten`; excluded — loop: `ReadUpTo`, `WriteThenDiscard`, `AdoptTrailingFrame`

### `src/backend/diagnostics/dtc_session.cpp`

- 76-84 `Body` (3 steps, tail): candidate
- 109-140 `FiveBaud` (2 steps + check, tail): review — an ignored `SetP1Max` sits between the steps
- 146-172 `FastInit` (3 steps + check, tail): candidate
- 177-214 `CanInit` (3 steps + checks, tail): candidate
- 324-333 `ClearDtcs` (2 steps, partial, before the read loop): review — the `index` local sits between steps
- `Execute`: not a run (the epilogue runs on failure); `VehicleInfo`, `ReadDtcs`, `Request`: excluded — loop

### `src/platform/desktop/common/transport/desktop_mixed_can_flash_transport.cpp`

- 102-119 `ClearReceiveBuffer` (2 steps, tail): candidate
- 151-155 `WriteIso15765` (2 steps, tail): candidate
- 161-165 `ReadIso15765` (2 steps, tail): candidate
- 170-182 `WriteRaw` (2 steps + check, tail): candidate
- 188-220 `ReadRaw` (2 steps + checks, tail): candidate
- 84-95 `EnterRawBootloaderMode`, 133-144 `EnterIso15765KernelMode` (3 steps): review — every failure branch calls `TransitionFailure`, so one branch after the chain would do the same
- 35-40 `Configure`: not a run

### `src/platform/desktop/common/service_functions/serial_facade_configurator.cpp`

- 55-71 `ConfigureIso15765` (2 steps, tail): candidate
- 79-103 `ConfigureKLine` (3 steps + checks, tail): candidate — already a hand-written chain over `status`

### `src/platform/desktop/common/transport/desktop_transport_factory.cpp`

- 109-117 `OpenDesktopCanFlashTransport` (2 steps, tail): candidate — `transport` is returned after both steps
- `ListDesktopSerialPorts`, `MakeSerial` prefix: not a run

### `src/platform/desktop/common/service_functions/worker/service_function_worker.cpp`

- 80-91 `run` (2 steps): review — both failure branches emit `finished(FailureResult(...))`, so one branch after the chain would do the same

### No runs

`definition/definition_model.cpp` (loop), `definition/definition_catalog_session.cpp`
(failure branches log), `definition/mappack_converter.cpp` (`AppendMap` and
`ConvertMappackCsv` need two earlier values; the rest are loops),
`diagnostics/terminal_script.cpp` (loop), `diagnostics/obd_frames.cpp`, and in
`platform/...`: `qt_resource_bundle.cpp`, `logging/logging_csv_file.cpp`,
`logging/logging_value_adapter.cpp`, `transport/desktop_logging_protocol_registration.cpp`,
`diagnostics/serial_diagnostic_link.cpp` and the Qt workers (single calls).
