# Step 5 Tail Wave 7 — Subaru Unisia Jecs M32R Bootmode and Legacy Teardown — Design

**Status:** 7a merged (#360); 7b implemented on this branch.
**Parent:** [step 5 tail flash drain](2026-08-08-step5-tail-flash-drain-design.md).
**Predecessor:** [wave 6 singletons](2026-09-19-step5-tail-wave6-singletons-design.md),
whose last family, 6c-3 Unisia Jecs M32R K-Line, merged as #359.
**Source baseline:** `3ae8e981`.

## Intent and success criteria

Wave 7 is the last step of the flash drain. It migrates the one remaining
legacy family, `FlashEcuSubaruUnisiaJecsM32rBootModeOperation` (675 lines;
read and write), to portable plans and `IKlineFlashExecutor`s routed through
`FlashWorkflow` and the common `FlashDialog`, then deletes the legacy flash
package and everything that existed only to fence it. Hardware status stays
`experimental`.

Done means the tail design's
[completion criterion](2026-08-08-step5-tail-flash-drain-design.md#completion-criterion)
holds:

- `REMAINING` in `scripts/check-legacy-flash-drain.py` is empty, then the
  script, the `//:legacy_flash_drain` guard, and
  `src/platform/desktop/common/flash/legacy/` are deleted.
- `//src/platform/desktop/common/flash/legacy:__pkg__` and
  `//src/ui/desktop/flash/bootmode:__pkg__` are out of the `serial_qt_compat`
  allowlist.
- `//src/algorithms/protocol/ssm/qt_compat` is deleted. The other shims the
  criterion names are already gone; `protocol:qt_compat` is retained by
  ADR 0004.
- Every row in the [flash qualification matrix](../../flash-qualification-matrix.md)
  reads `portable=yes`, `hardware_status=experimental`.

## Findings that shape the design

### Bootmode Read is the 6c-3 Read

`execute()` configures even parity at 39063 baud (`…_bootmode_operation.cpp:52-58`),
but `read_mem()` immediately calls `reset_connection()` and switches to no
parity (`:112-113`) before any I/O. What follows — a 38400-baud `BF` probe,
a 4800-baud cold init with `B8 00 00 00 75`, then 128-byte `A0` pages from
`fblocks[0].start + 0x100000` (`:123-275`), and `RomId = "<ID>_"` (`:184`) —
is the wire sequence `SubaruUnisiaJecsM32rKlineExecutor` already sends for
Read. Legacy's cold-init check `received == "" && received.at(4) …` (`:145`)
indexes an empty array; the 6c-3 executor's frame gating already corrects
it.

### Write has an operator step in the middle

Write raises VPP and MOD1 (`set_lec_lines(RTS enabled, DTR enabled)`, `:66`),
uploads the raw kernel at 39063 baud with even parity (`upload_kernel()`,
`:287-342`), then `write_mem()` resets the connection, reopens at 19200 baud
with no parity, drops MOD1 while keeping VPP (`:361-366`), and asks the
operator to "Remove MOD1 voltage and press ok to continue" (`:367`) before
erasing and programming.

`ConfirmationSpec`'s contract is that confirmations are collected before the
executor starts, because executors are synchronous and dialog-free. A
mid-attempt prompt cannot be expressed in one attempt.

### Legacy already breaks the session where the prompt sits

`SerialPortActionsDirect::reset_connection()` closes both the J2534 and the
serial port (`serial_port_actions_direct.cpp:653-658`). The prompt therefore
sits at a real connection boundary, with parity and baud reconfigured on
either side. Two attempts separated by a workflow prompt reproduce that
boundary. `FlashDialog::advance()` already drives any number of
`FlashAttempt` steps.

### Port item 4 shrinks to one method

The wave-6 spec reserved "`set_parity()` mid-session, and
`enable_boot_mode_lines()`" for this family. With two attempts, each
configures its own parity through `KlineConfig` (landed in 6b), so
mid-session parity has no caller. Only `enable_boot_mode_lines()` — RTS
enabled with DTR **enabled** — remains.

### Legacy write defects

Measured against the source baseline:

- Cancellation during kernel upload, erase polling, or block writes returns
  `0`, which equals `STATUS_SUCCESS` (`:324`, `:397`, `:439`, `:488`).
- The erase-started poll fails only when nothing at all arrived (`:423`);
  one to six bytes after twenty rounds fall through. The erase-complete poll
  (`:437-463`) has no failure on exhaustion.
- The reply to the final `AF 69` block is never read (`:517`).
- The image length is never checked against ROM size; a trailing partial
  block is dropped.
- The first progress log reads `curspeed` and `tleft` uninitialized.
- Nothing verifies the kernel upload; its reply is read and discarded
  (`:338-340`).

## Decisions

Taken with the user during brainstorming.

1. **Write is two attempts in one workflow**, with a `RemoveMod1` prompt
   between them, not one attempt with an injected operator-gate port and
   not a dropped prompt.
2. **Two PRs.** 7a migrates the family and lands the port method with its
   first caller; 7b is teardown plus the docs-only wave-6 close (the planned
   6d), folded in.
3. **Behavior-correction policy: the 6a-3 through 6c-3 exception.** Command
   bytes, sequencing and timing budgets are preserved; reply-gating,
   integrity and cancellation defects are corrected.
4. **Bootmode Read reuses the 6c-3 plan and executor.**
5. **The bootmode failure notice carries no don't-power-off advice.** The
   mask boot ROM is always re-enterable with MOD1, so a power cycle is the
   recovery path; legacy bootmode said "press OK to exit and try again".

## Architecture (PR 7a)

### Backend (portable)

Read:

- `build_subaru_unisia_jecs_m32r_kline_plan` accepts two more exact pairs,
  `sub_ecu_unisia_jecs_20_bootmode` / `M32R_128KB` and
  `sub_ecu_unisia_jecs_30_bootmode` / `M32R_256KB`, for Read only. Write and
  TestWrite remain rejected for them in that builder.

Write, in `src/backend/flash/ecu/`, each target registered by name in
`PORTABLE_PACKAGES`:

- `subaru_unisia_jecs_m32r_bootmode_types.h`: two `FlashFamily` values and
  their `FamilyPlan` alternatives, one per attempt, so each executor's
  `check_family()` rejects the other attempt's plan.
  - `SubaruUnisiaJecsM32rBootModeKernel`
  - `SubaruUnisiaJecsM32rBootModeProgram`

  Both set `family_requires_kernel_v = false`. The kernel attempt carries the
  zero-padded kernel as its **plan image**, as 6c-1 BDM does:
  `validate_and_build` requires every Write plan to carry an image, and the
  `_bootmode` cfg entries have no `<kernel_addr>`, so the shared
  `resolveKernel()` (which parses one) cannot load them.
- `subaru_unisia_jecs_m32r_bootmode_plan.{h,cpp}`:
  `build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(operation, protocol, mcu, kernel)`,
  `build_subaru_unisia_jecs_m32r_bootmode_program_plan(operation, protocol, mcu, image)`
  and `validate_…`. Both accept exactly the two `_bootmode` pairs and Write
  only.
  - Kernel plan: image = kernel zero-padded to a multiple of 128 bytes
    (`:312-315`); an empty kernel is rejected; transfer region
    `{0, padded size}`.
  - Program plan: image exactly `rom_size`; transfer region `{0, rom_size}`.
  - **Both** carry `ConfirmationSpec::Id::ApplyBootModeVoltages`: both
    executors drive programming-voltage lines, and presence means granted.
- `subaru_unisia_jecs_m32r_bootmode_kernel_executor.{h,cpp}` and
  `subaru_unisia_jecs_m32r_bootmode_program_executor.{h,cpp}`, each
  implementing `IKlineFlashExecutor`. Two classes rather than one with a
  stage switch: the attempts share no setup, sequence, lines or cleanup.

Port:

- `IKlineFlashTransport::enable_boot_mode_lines()`, bound in
  `DesktopKlineFlashTransport` to
  `set_lec_lines(get_requestToSendEnabled(), get_dataTerminalEnabled())`
  (`:66`), with a `ScriptedKlineFlashTransport` expectation.
- `ConfirmationSpec::Id::ApplyBootModeVoltages` in `flash_types.h`: the
  operator has connected VPP and MOD1. Presence means granted.

### Desktop

- `flash_workflow.h`: `FlashPromptKind::ApplyBootModeVoltages` and
  `FlashPromptKind::RemoveMod1`. `RemoveProgrammingVoltage` gains an
  optional `power_off_advice` argument, `yes` or `no`; absent means `yes`,
  so 6c-3 is unchanged.
- `flash_workflow.cpp`: two `RouteMatch::Exact` entries for
  `sub_ecu_unisia_jecs_{20,30}_bootmode`, both to
  `SubaruUnisiaJecsM32rBootModeWorkflow`.

  The kernel bytes come from a new `resolveKernelBytes()` beside
  `resolveKernel()`: the cfg `<kernel>` file, with no `<kernel_addr>`
  parsed. Both plans are built before `Begin`, so a missing kernel file or a
  wrong-size image fails before any prompt.
  - **Read:** `Begin`, then one attempt with the 6c-3 plan and executor,
    then the completed step carrying `rom_id`.
  - **Write:**
    1. `Begin`.
    2. `ApplyBootModeVoltages`, OK / Cancel, for every adapter, as legacy
       asked every adapter (`flash_ecu_subaru_unisia_jecs_m32r_bootmode.cpp:38`).
       Declining cancels before any I/O.
    3. Attempt 1, the kernel upload.
    4. Only if attempt 1 succeeded: `RemoveMod1`, OK / Cancel. Declining
       cancels; the kernel is running and nothing has been erased.
    5. Attempt 2, erase and program.
    6. After every Write that started attempt 1 — attempt-1 failure or
       cancellation, `RemoveMod1` declined, or any attempt-2 outcome —
       one OK-only `RemoveProgrammingVoltage` with `outcome`,
       `external_vpp=yes`, and `power_off_advice=no`.
    7. The completed step.
- `flash_dialog.cpp` renders the new kinds:
  - `ApplyBootModeVoltages`: warning, OK / Cancel, "Connect VPP and MOD1 to
    the ECU, turn ignition ON, then press OK".
  - `RemoveMod1`: OK / Cancel, "Remove MOD1 voltage, then press OK to
    continue". Legacy offered OK only.
  - `RemoveProgrammingVoltage` text moves into a static
    `FlashDialog::programmingVoltageNotice(const FlashPromptStep&)` so it is
    testable. With `power_off_advice=no`, a failed or cancelled write says
    "Press OK to exit and try again" (legacy bootmode's wording) instead of the
    don't-power-off advice; a successful one adds "Power cycle the ECU and
    request SSM Init to confirm the write."
- Close-to-cancel needs no dialog change. `finishCancelledAttempt()` submits
  one cancelled result and shows only prompts. The `RemoveMod1` box is modal
  and no worker is live while it shows, so closing it is a decline, handled by
  the workflow.

### Deleted in 7a

- `MainWindow`'s two `_bootmode` branches and the bootmode dialog include.
- The `//src/ui/desktop/flash/bootmode` package, and its entries in the
  `serial_qt_compat` allowlist and in `serial/BUILD.bazel` visibility.
- `legacy/bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.{h,cpp}`
  and its `REMAINING` entry. The ratchet stays, empty, until 7b.
- The workflow test `unisiaJecsM32rBootmodeAndLookalikesStayLegacy`,
  replaced by routing tests.
- The legacy package's `family_operation_sources` filegroup gains
  `allow_empty = True`: after `bootmode/` goes, its `*/*.cpp` pattern
  matches nothing until 7b deletes the package.
- The legacy library's `//src/algorithms/protocol/ssm/qt_compat` dependency;
  only the bootmode operation used it.

## Wire behavior

All writes use `write()` (legacy `write_serial_data_echo_check`). "Expect
`XX`" means one `read()` returning a frame accepted by
`ssm::hasValidFrame(frame, 0xF0, 0x10)` whose first payload byte is `XX`;
anything else fails with a hex dump, and no later command is sent.

### Attempt 1 — kernel upload

1. `before_transport_configure()`: `reset_connection()` (`:52`), then
   `set_add_iso14230_header(false)`.
2. `transport_setup()`: `KlineConfig{.baud = 39063, .iso14230 = false, .parity = Even}`
   (`:53-58`). A plan without `ApplyBootModeVoltages` fails before any I/O.
3. `enable_boot_mode_lines()` (`:66`). The cleanup guard is armed.
4. For each 128-byte kernel chunk: check cancellation; `write()` the chunk
   unframed; report progress (`:320-335`).
5. Sleep 500 ms; one 200 ms `read()`, discarded and logged at debug level
   (`:338-340`).
6. **Cleanup:** `disable_lec_lines()` on every exit. Legacy dropped the lines
   only implicitly, by closing the port in `write_mem()`'s
   `reset_connection()`; the explicit drop is deterministic across adapters.

Nothing in this attempt can verify the kernel. Attempt 2's first gated reply
is the proof it is running.

### Attempt 2 — erase and program

1. `before_transport_configure()`: `reset_connection()` (`:361`), then
   `set_add_iso14230_header(false)`.
2. `transport_setup()`: `KlineConfig{.baud = 19200, .iso14230 = false, .parity = None}`
   (`:362-364`).
3. `enable_programming_voltage_line()` — VPP on, MOD1 off (`:366`). The
   cleanup guard is armed.
4. Send `AF 31` (`:378-389`); sleep 500 ms.
5. Erase started: up to 20 rounds of a 10 ms `read()`, each followed by a
   500 ms sleep (`:393-422`). An empty read continues; any frame must be
   `EF 42`, or the erase fails. Exhausting the rounds fails.
6. Erase complete: up to 20 rounds of a 10 ms `read()`, each followed by a
   1000 ms sleep, for `EF 52` (`:435-463`). Same rules.
7. Sleep 1000 ms (`:465`).
8. For each 128-byte block `i` at flash address `i × 128`: check
   cancellation; send `AF 61 <addr24> <128 bytes>`, or `AF 69 …` for the
   last block (`:485-513`). Data is sent as-is; unlike 6c-3 there is no XOR.
   - After `AF 61`: expect `EF 52` within 3000 ms (`:518-535`); sleep 10 ms.
   - After `AF 69`: read for 3000 ms. `EF 52` succeeds; silence succeeds
     with a warning; anything else fails. This is the 6c-3 policy.
9. **Cleanup:** `disable_lec_lines()` on every exit. A cleanup failure turns
   an otherwise successful run into a failure and never replaces an earlier
   error.

**Negative replies.** Legacy's success replies are `EF 42` and `EF 52`, a
status byte after `EF`; the documented error codes (`:346-353`) are read as
statuses in the same position. A valid `EF xx` frame in a failure detail is
annotated: `48` missing VPP, `5C` checksum error, `72` address error,
`8A` FENTRY bit not set, `42` erase started, `52` done; any other status is
"unknown". The bench checklist confirms the position.

### Deliberate corrections

Recorded in the matrix notes:

- Cancellation reports cancelled; legacy reported success.
- Both erase polls fail on exhaustion, and any non-matching frame fails;
  legacy's first poll fell through on one to six bytes and its second never
  failed.
- The final `AF 69` reply is read; a malformed or negative reply fails.
- Write images must be exactly the ROM size.
- Lines are dropped explicitly on every exit of both attempts.
- The uninitialized first progress log is gone.
- Read inherits every 6c-3 read correction, including the empty-reply
  index at `:145`.
- **Operator flow:** `RemoveMod1` gains Cancel; the VPP notice follows every
  Write outcome, not only success, and omits the don't-power-off advice.
- TestWrite is rejected before any I/O, matching the cfg.

## PR 7b — teardown and wave close

Depends on 7a's empty `REMAINING`. No behavior change.

**Delete:**

- `src/platform/desktop/common/flash/legacy/` entirely:
  `FlashOperationWorker`, `legacy_flash_utils`, their two tests, and
  `BUILD.bazel`.
- `scripts/check-legacy-flash-drain.py` and the root `//:legacy_flash_drain`
  target.
- `//src/platform/desktop/common/flash/legacy:__pkg__` from the allowlist
  and from `serial/BUILD.bazel` visibility.
- `//src/algorithms/protocol/ssm/qt_compat` and its `qt_layer` entry in
  `bazel/qt/BUILD.bazel`.

Each deletion is backed by a zero-consumer grep in the PR description, and
the PR body walks the completion criterion line by line.

**Docs:**

- Wave-6 spec: status "complete"; port item 4 amended to
  `enable_boot_mode_lines()` only, with a pointer here.
- Tail design: the wave-6 premise correction and the "six calls" count
  (six, then two, then one), plus a wave-7 completion note.
- [Modularization plan](../../modularization-plan.md): step 5's tail closed.
- [Tech debt](../../tech-debt.md): P1 flash-orchestration isolation
  updated.
- Matrix header notes stop naming `legacy/{…}` directories.

## Documentation in PR 7a

- This spec.
- The `FlashEcuSubaruUnisiaJecsM32rBootMode` matrix row: `portable=yes`,
  `hardware_status=experimental`, automated evidence, the corrections and
  the operator-flow change.
- A new `docs/unisia-jecs-m32r-bootmode-bench-checklist.md`: the kernel
  survives the explicit line drop between attempts; the `AF 69` reply; erase
  poll timings; observed negative codes.
- A wave-7 note in the wave-6 spec.

## Testing (PR 7a)

All package-owned and co-located.

**Plan tests.** Both `_bootmode` pairs are accepted for Write; crossed pairs,
suffixes, Read and TestWrite are rejected by the bootmode builder. Both the
kernel plan and the program plan always carry `ApplyBootModeVoltages`; the
kernel plan is padded to 128 bytes. Program images one byte short, one byte
over, and with a trailing partial block are rejected. `transport_setup()`
fields are asserted against the legacy lines above. In the 6c-3 plan test,
both `_bootmode` pairs are accepted for Read and rejected for Write and
TestWrite.

**Kernel executor** (`ScriptedKlineFlashTransport`, byte-exact):
`reset_connection()`, configure, `enable_boot_mode_lines()`, then every
chunk; zero-padding; a missing confirmation fails before I/O; cancellation
at each chunk; `disable_lec_lines()` on every exit.

**Program executor:** the full happy path at both ROM sizes; exhaustion and a
wrong frame for each erase poll; a bad `AF 61` reply; `AF 69` answered by
`EF 52`, by silence (warning logged), and by a bad frame; each named
negative code; cancellation at each checkpoint; cleanup never masking an
earlier error.

**Port.** `enable_boot_mode_lines()` reaches RTS enabled and DTR enabled in
`desktop_kline_flash_transport_test.cpp`.

**Workflow** (`test_flash_workflow`): Read routing for both names; the Write
prompt order; decline at each prompt; attempt-1 failure skipping
`RemoveMod1` and attempt 2; the notice's `outcome`, `external_vpp` and
`power_off_advice` for every outcome.

**Dialog** (`flash_dialog_test`): a workflow yielding two attempts with a
prompt between them runs both through `advance()`; `programmingVoltageNotice`
text for `power_off_advice` absent (6c-3 unchanged), `no` on success, and `no`
on failure.

**Guards.** `REMAINING` becomes empty; the allowlist shrinks by one;
`//:portable_closure` stays green.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| The kernel does not survive the line drop between attempts | Legacy's port close dropped the lines at the same point; the bench checklist makes it the first observation. |
| Stricter erase gating fails a flash legacy "passed" | Intended: those passes were unverified. No hardware qualification exists to regress. |
| An operator declines `RemoveMod1` after the kernel is running | Nothing has been erased; the notice tells them to remove VPP and power cycle. |
| 7b deletes something still reached | Zero-consumer greps and the full test suite gate the PR; the guards being deleted are run green first. |

## Appendix: unresolved knowledge

- **The `AF 69` reply**, as in 6c-3.
- **Negative code `5A`** is listed in the legacy comment with no meaning; it
  is reported as unknown.
- **The unused 4800-baud switch** (`B8 00 00 00 15`, `:621-638`) is never
  called and is not ported.
