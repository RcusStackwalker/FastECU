# Step 5 Tail Wave 6c-3 — Subaru Unisia Jecs M32R K-Line — Design

**Status:** design approved; implementation plan to follow.
**Parent:** [wave 6 singletons](2026-09-19-step5-tail-wave6-singletons-design.md).
**Predecessors:** 6c-1 Denso MC68HC16Y5 BDM (#357) and 6c-2 Hitachi M32R JTAG
removal (#358), both merged.
**Source baseline:** `a90bb178`.

## Intent and success criteria

Migrate `FlashEcuSubaruUnisiaJecsM32rOperation` (747 lines; read and write) to
a portable plan and `IKlineFlashExecutor`, routed through the desktop
`FlashWorkflow` and the common `FlashDialog`. This is the last wave-6 family:
`//:legacy_flash_drain` moves from two entries to one, leaving only
`FlashEcuSubaruUnisiaJecsM32rBootMode` for wave 7. Hardware status stays
`experimental`.

Done means:

- `ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.cpp` is out of `REMAINING`;
  the legacy operation, the `//src/ui/desktop/flash/ecu` package, and the four
  non-bootmode Unisia Jecs branches in `MainWindow` are deleted.
- `sub_ecu_unisia_jecs_20`, `_30`, `_40` and `_70` reach the common
  `FlashDialog` through `FlashWorkflow` route entries.
- `//src/ui/desktop/flash/ecu:__pkg__` leaves the `serial_qt_compat` allowlist.
- The matrix row reads `portable=yes`, `hardware_status=experimental`, and a
  bench checklist exists.

## Findings that shape the design

### One class serves four protocols with different capabilities

`MainWindow` dispatches every `sub_ecu_unisia_jecs_{20,30,40,70}` prefix to
this class, after the two `_bootmode` branches. The cfg gives them different
MCUs and operations:

| Protocol | MCU | ROM size | cfg read / test_write / write |
|---|---|---|---|
| `sub_ecu_unisia_jecs_20` | `M32R_128KB` | 128 KiB | yes / no / yes |
| `sub_ecu_unisia_jecs_30` | `M32R_256KB` | 256 KiB | yes / no / yes |
| `sub_ecu_unisia_jecs_40` | `M32R_384KB` | 384 KiB | yes / no / no |
| `sub_ecu_unisia_jecs_70` | `M32R_512KB` | 512 KiB | yes / no / no |

Every one of these MCUs has `fblocks[0].start == 0`, so legacy's read base
`fblocks[0].start + 0x100000` is `0x100000` for all four.

### The legacy write path can erase under VPP after a rejected handshake

Measured against the source baseline:

- A rejected `AF 11` enter-flash-mode request (`…_operation.cpp:411-428`) is
  logged and ignored. Legacy then raises programming voltage and sends erase.
- Cancellation during erase polling or block writes returns `0`, which equals
  `STATUS_SUCCESS` (`:452`, `:492`, `:539`); the dialog reports success.
- The erase-started poll (`:448-484`) fails only when nothing at all arrived;
  one to six bytes after twenty rounds fall through. The erase-complete poll
  (`:488-516`) has no failure on exhaustion and proceeds to program after
  about 20 s of silence.
- The reply to the final `AF 69` block is never read (`:564`).
- The image length is never checked against ROM size; a trailing partial
  128-byte block is silently dropped (`:524`).
- The first progress log reads `curspeed` and `tleft` uninitialized (`:594`).

### The legacy read path has no integrity check

Each `A0` block-read reply longer than four bytes with `E0` at offset 4 has its
header and last byte stripped and is appended (`:265-285`). Length and
checksum are never checked, so a short or corrupted page silently shortens or
corrupts the ROM.

### Programming voltage is prompted twice, once after the run

The operation prompts "Apply VPP" mid-session when the adapter is not
OpenPort2 (`:434-438`). The dialog prompts "Remove VPP voltage" after the run
(`flash_ecu_subaru_unisia_jecs_m32r.cpp:73-77`), but only on success — though
the line was raised on failure paths too. Legacy drops the LEC lines after
`write_mem()` unconditionally (`…_operation.cpp:71-72`).

### The workflow already supports post-attempt prompts

`FlashWorkflow` is a `next()` / `submit()` step machine; `InspectRead` is
already emitted after an attempt. A post-attempt VPP notice is a new prompt
kind, not a new mechanism.

### No port or codec addition is needed

`ssm::addHeader` and `ssm::hasValidFrame` in
`src/algorithms/protocol/ssm/ssm_protocol_core.h` cover framing, and the
existing `IKlineFlashTransport` covers every legacy call: `setBaud()`,
`write()` (legacy `write_serial_data_echo_check`), `read()`,
`enable_programming_voltage_line()` and `disable_lec_lines()`.

## Decisions

Taken with the user during brainstorming; they override the parent spec where
they differ from it.

1. **Behavior-correction policy: the 6a-3 / 6a-4 / 6b-2 / 6c-1 exception.**
   Command bytes, sequencing and timing budgets are preserved; reply-gating,
   integrity and cancellation defects are corrected so a bad reply stops every
   later command. Each correction is named in the matrix notes.
2. **A post-attempt `RemoveProgrammingVoltage` prompt**, emitted for every
   outcome of a Write attempt whose plan carried `ApplyProgrammingVoltage`.
   Legacy showed it on success only.
3. **One family with a per-protocol variant table**, not two families and not
   a shared read loop with `subaru_hitachi_m32r_kline_executor`. The entry
   sequence and the whole write protocol differ from that family; wave 6
   already found cluster factoring's payoff small.

## Architecture

### Backend (portable)

Targets live in `src/backend/flash/ecu/` and are registered by name under that
package in `PORTABLE_PACKAGES` (`bazel/portable_targets.bzl`).

- `FlashFamily::SubaruUnisiaJecsM32rKline`, a `SubaruUnisiaJecsM32rKlinePlan`
  `FamilyPlan` alternative in `subaru_unisia_jecs_m32r_kline_types.h`, and
  `family_requires_kernel_v` = `false`.
- `subaru_unisia_jecs_m32r_kline_plan.{h,cpp}`:
  `build_subaru_unisia_jecs_m32r_kline_plan(operation, protocol, mcu, image, adapter_supplies_programming_voltage)`
  and `validate_…`.
  - Accepts exactly the four `(protocol, mcu)` pairs in the table above;
    crossed pairs and suffix variants are rejected.
  - TestWrite is rejected for all four; Write is rejected for `_40` and `_70`.
  - Read: no image. Region `{0x100000, rom_size}`.
  - Write: the image must be exactly `rom_size` bytes. Transfer region
    `{0x000000, rom_size}`.
  - A Write plan carries `ConfirmationSpec::Id::ApplyProgrammingVoltage` if and
    only if `adapter_supplies_programming_voltage` is false. Read plans never
    carry it.
- `subaru_unisia_jecs_m32r_kline_executor.{h,cpp}` implementing
  `IKlineFlashExecutor`:
  - `transport_setup()` returns
    `KlineConfig{.baud = 4800, .iso14230 = false, .parity = None}`
    (`…_operation.cpp:50-57`).
  - `before_transport_configure()` calls `set_add_iso14230_header(false)` so a
    stale header from an earlier session is cleared.
  - `execute()` dispatches to read or write. Synchronous, bounded,
    cancellable, dialog-free. Tester ID `0xF0`, target ID `0x10`.

### Desktop

- `flash_types.h`: `ConfirmationSpec::Id::ApplyProgrammingVoltage`, with a
  comment stating the parent spec's "presence means granted" contract.
- `flash_workflow.h`: `FlashPromptKind::ApplyProgrammingVoltage` and
  `FlashPromptKind::RemoveProgrammingVoltage`.
- `flash_workflow.cpp`: four `RouteMatch::Exact` entries for
  `sub_ecu_unisia_jecs_{20,30,40,70}`. The two `_bootmode` names match none of
  them and keep reaching their `MainWindow` branch until wave 7.
  `SubaruUnisiaJecsM32rKlineWorkflow` asks
  `adapter_supplies_programming_voltage(request_.serial)` — the legacy
  `get_use_openport2_adapter()` check, moved out of the operation body as the
  parent spec requires — and builds the plan with it. That helper lives beside
  `DesktopKlineFlashTransport`, because the flash package is not on the frozen
  `serial_qt_compat` visibility list; a null serial answers false, so the
  operator is prompted.
  Its sequence:
  1. `Begin`.
  2. One `ApplyProgrammingVoltage` prompt per confirmation; declining cancels
     before any I/O.
  3. The attempt, with the new executor and `DesktopKlineFlashTransport`.
  4. If the plan carried `ApplyProgrammingVoltage`: one OK-only
     `RemoveProgrammingVoltage` prompt, with an `outcome` argument of
     `succeeded`, `failed` or `cancelled`.
  5. The completed step, carrying `rom_id` when the executor returned one.
- `flash_dialog.cpp` renders the two new kinds. `ApplyProgrammingVoltage` is a
  warning with OK / Cancel: "Apply VPP voltage to the ECU, then press OK".
  `RemoveProgrammingVoltage` is OK-only: "Remove VPP voltage from the ECU";
  on `failed` or `cancelled` it adds legacy's advice not to power off the ECU,
  because the kernel is still running and flashing can be retried.

### Deleted

- `MainWindow`'s four `sub_ecu_unisia_jecs_{20,30,40,70}` branches and the
  `flash_ecu_subaru_unisia_jecs_m32r.h` include, plus the
  `//src/ui/desktop/flash/ecu` dependency.
- The `//src/ui/desktop/flash/ecu` package: dialog, header and `BUILD.bazel`.
- `//src/ui/desktop/flash/ecu:__pkg__` from the `serial_qt_compat` allowlist
  in `scripts/check-serial-compat-allowlist.py`.
- `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.{h,cpp}`.
- The `REMAINING` entry in `scripts/check-legacy-flash-drain.py`.

## Wire behavior

All writes use `write()`. "Expect `XX`" means one `read()` within the stated
timeout that returns a frame accepted by `ssm::hasValidFrame(frame, 0xF0, 0x10)`
whose first payload byte is `XX`; anything else fails with a hex dump, and no
later command is sent. "Probe" means the same check, but a mismatch is logged
and the sequence continues, as legacy did. Every legacy request timeout is
kept: 2000 ms for `BF`, `B8`, `AF` and `AF 11`; 3000 ms for `A0` pages and
`AF 61` blocks; 500 ms per erase poll.

### ECU identification

After an accepted `BF` reply, the ECU ID is the five bytes at frame offset 8
(`…_operation.cpp:162-163`). It is always logged. Only Read returns it, as
`rom_id = "<ID as uppercase hex>_"`: legacy assigned `RomId` only when
`cmd_type == "read"` (`:171-174`, `:405-408`).

### Read

1. `setBaud(38400)`; send `BF`; probe `FF`. On a match, record the ECU ID and
   go to step 3.
2. Cold init: `setBaud(4800)`; send `BF`, expect `FF`, record the ECU ID; send
   `B8 00 00 00 75`, expect `F8`; `setBaud(38400)`; send `BF`, expect `FF`.
3. For each 128-byte page address from `0x100000` to `0x100000 + rom_size`:
   check cancellation; send `80 10 F0 06 A0 00 <addr24> 7F <sum8>`; expect
   `E0` with exactly 128 data bytes after the SID; append them; report
   progress; sleep 1 ms.

The result is `rom_size` bytes as `read_bytes`, plus `rom_id`.

### Write

1. OBK probe: `setBaud(19200)`; send `AF`; probe `EF`. On a match, go to
   step 3.
2. Enter flash mode: `setBaud(4800)`; send `BF`, expect `FF`, record the ECU
   ID; send `AF 11 <id×5> <rom_size24>`, **expect `EF`**; `setBaud(19200)`.
3. Check cancellation; `enable_programming_voltage_line()`. From here the
   cleanup guard is armed.
4. Send `AF 31`, with no immediate read.
5. Erase started: up to 20 rounds of a 500 ms `read()`, each followed by a
   500 ms sleep when it returned nothing. `read()` returns whole frames, so
   legacy's byte accumulation has no counterpart: an empty read continues the
   poll, and any frame returned must be exactly `EF 42`, or the erase fails.
   Exhausting the rounds fails. Cancellation is observed by every read and
   sleep.
6. Erase complete: the same, up to 40 rounds, for `EF 52`.
7. One 500 ms `read()`, discarded and logged at debug level.
8. For each 128-byte block `i` at flash address `i × 128`: check
   cancellation; send `AF 61 <addr24> <128 bytes XOR 0x82>`, or `AF 69 …` for
   the last block; report progress.
   - After `AF 61`: expect `EF 52` within 3000 ms.
   - After `AF 69`: read for 3000 ms. A valid `EF 52` succeeds; any other
     non-empty reply fails; an empty read succeeds with a warning. See the
     appendix.

**Cleanup.** Once `execute()` begins a Write, `disable_lec_lines()` runs on
every exit path — success, failure and cancellation — through a scope guard,
matching legacy's unconditional line drop. A cleanup failure turns an
otherwise successful run into a failure and never replaces an earlier error.

### Deliberate corrections

Recorded in the matrix notes:

- A rejected `AF 11` fails before programming voltage is raised or erase is
  sent.
- Cancellation reports cancelled; legacy reported success.
- Both erase polls fail on exhaustion; legacy's second poll never failed.
- The final `AF 69` reply is read, and a malformed or negative reply fails.
- Write images must be exactly the ROM size; legacy dropped a trailing
  partial block and accepted any length.
- Read pages must be complete, checksummed `E0` frames of 128 data bytes.
- The VPP prompt moves before the connection opens, and the removal notice is
  shown for every Write outcome, not only success.
- A stale ISO-14230 header from an earlier session is cleared before
  configure.
- TestWrite is rejected before any I/O on all four protocols, and Write on
  `_40` and `_70`, matching the cfg.

## Testing

All package-owned and co-located.

**Plan tests.** Each of the four exact pairs is accepted; crossed pairs and
suffixes are rejected; Write is rejected on `_40` and `_70`, and TestWrite on
all four; Write images one byte short, one byte over, and with a trailing
partial block are rejected. `ApplyProgrammingVoltage` is present if and only
if the operation is Write and the adapter does not supply VPP.
`transport_setup()` fields are asserted against the legacy lines that set them.

**Executor tests** (`ScriptedKlineFlashTransport`, byte-exact):

- Read: the already-in-read-mode path, the cold-init path, and short,
  bad-checksum and wrong-SID pages, each stopping every later command.
- Write: the OBK-running path and the cold path; a rejected `AF 11`, asserting
  `enable_programming_voltage_line()` is never called; exhaustion of each
  erase poll; a bad `AF 61` reply; `AF 69` answered by `EF 52`, by silence
  (warning logged), and by a bad frame.
- Cleanup: `disable_lec_lines()` on every Write exit, including cancellation
  at each checkpoint; a cleanup failure never replaces an earlier error.
- Scripts cover the full ROM: the plan pins each variant's size exactly, so
  there is no smaller ROM to test with, and 1,024 to 4,096 scripted exchanges
  are cheap. One parameterized test reads each of the four ROM sizes.

**Workflow tests** (`test_flash_workflow`): the prompt sequence with and
without adapter-supplied VPP; decline at each prompt; the post-attempt notice
and its `outcome` argument for success, failure and cancellation; `rom_id`
propagation; routing for all four names, and both `_bootmode` names left
unrouted.

**Guards.** `REMAINING` shrinks from two entries to one; the
`serial_qt_compat` allowlist shrinks by one; `//:portable_closure` stays green.

## Delivery

One PR. The `ConfirmationSpec` id and both prompt kinds land in it, because
this family is their first caller. 6d, the docs-only wave close, follows as
its own PR.

Documentation in the same PR:

- This spec.
- A new bench checklist, `docs/unisia-jecs-m32r-bench-checklist.md`, which
  includes observing the `AF 69` reply.
- The [flash qualification matrix](../../flash-qualification-matrix.md) row:
  `portable=yes`, `hardware_status=experimental`, automated evidence, and
  every correction above.
- A wave 6c-3 implementation note in the
  [wave 6 singletons design](2026-09-19-step5-tail-wave6-singletons-design.md).

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| The ECU answers `AF 69` differently than assumed | Silence succeeds with a warning; only a malformed or non-`EF 52` reply fails. The bench checklist records the observation needed to tighten this. |
| Stricter gating fails a flash that legacy "succeeded" | Intended: legacy's successes on those paths were unverified. No hardware qualification exists to regress. |
| Operators miss VPP removal after a failed write | The removal notice now appears for every Write outcome. |
| A `_bootmode` name is captured by a new route | Exact route matches only; workflow tests assert both `_bootmode` names remain unrouted. |

## Appendix: unresolved knowledge

- **The `AF 69` reply.** Legacy never read a reply to the final block, so its
  shape is unknown. The executor accepts `EF 52` or silence. Tighten after a
  bench observation.
- **The unused 4800-baud switch.** Legacy defines
  `send_sid_b8_change_baudrate_4800()` (`B8 00 00 00 15`) but never calls it.
  It is not ported.
