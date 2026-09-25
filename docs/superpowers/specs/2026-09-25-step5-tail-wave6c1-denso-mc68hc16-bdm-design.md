# Step 5 Tail Wave 6c-1 — Denso MC68HC16Y5 BDM — Design

**Status:** design approved; implementation plan in [the 6c-1 plan](../plans/2026-09-25-step5-tail-wave6c1-denso-mc68hc16-bdm.md).
**Parent:** [wave 6 singletons](2026-09-19-step5-tail-wave6-singletons-design.md).
**Predecessors:** 6b transport foundation (#351), 6b-1 Unisia Jecs (#352), and
6b-2 Denso SH705x K-Line (#355, #356), all merged.
**Source baseline:** `d345a8fa`.

## Intent and success criteria

Migrate `FlashEcuSubaruDensoMC68HC16Y5_02_BDMOperation` (480 lines; read and
write) to a portable plan and `IKlineFlashExecutor`, routed through the
existing desktop `FlashWorkflow`. Reduce `//:legacy_flash_drain` from four
entries to three. Hardware status stays `experimental`.

Done means:

- `bdm/flash_ecu_subaru_denso_mc68hc16y5_02_bdm_operation.cpp` is out of
  `REMAINING`; the legacy operation, its dialog package
  (`src/ui/desktop/flash/bdm/`), and the `MainWindow` BDM dispatch branch are
  deleted.
- `sub_ecu_denso_mc68hc16y5_02_bdm` reaches the common `FlashDialog` through a
  `FlashWorkflow` registration instead of the `Unrouted` sentinel.
- The matrix row reads `portable=yes`, `hardware_status=experimental`, and a
  bench checklist exists.

## Findings that shape the design

### The legacy read path cannot work on the only adapter that can reach it

The BDM bridge speaks an ASCII command protocol at 115200 baud (`rpmem`,
`wdmem`, `wpcsp`, `go`; replies `ACK_CMD_WDMEM`, `ACK_WR`, and raw page
bytes). It is a plain serial device, so the path that matters is
`SerialPortActionsDirect`, not J2534.

Legacy reads every reply with `read_serial_data()`. On direct serial that
function is a K-Line frame parser: it reads at most four header bytes, drops
leading bytes that do not start `BE EF`, `80 F0 10` or `80 F0 01`, derives a
payload length from the header, and prepends an error marker to anything
short (`serial_port_actions_direct.cpp`, `read_serial_data`). A 1 KiB binary
page or an ASCII `ACK_WR` cannot survive it. `read_raw()` binds
`read_serial_obd_data()` instead, which collects bytes until an inter-byte gap
— the right primitive for this protocol, and a wire-visible change.

The parent spec assumed 6c-1 would add `write_raw()`. That method, and
`read_raw()`, landed early in #351, so this family needs no port addition.

### "Write" is a kernel bootstrap, not a ROM write

Legacy `write_mem()` loads the *kernel file* — cfg `<kernel>`
`ssmk_mc68hc916y5.bin`, the same kernel the `_02` K-Line family uses — into the
RAM block `0x20000–0x27FFF` in 32-byte `wdmem` chunks, sets the SCIB baud
register, writes PC/SP, and sends `go`. The ROM is never written. On the way
it replaces `ecuCalDef->FullRomData` with the zero-padded kernel, but
`MainWindow::start_ecu_operations` restores the saved buffer after every
non-read operation, so the operator never sees the ROM buffer mutated.

`ecuCalDef->Kernel` is `kernel_files_directory + <kernel>`
(`mainwindow.cpp`), which is exactly what `resolveKernel()` in
`flash_workflow.cpp` loads, so the kernel bytes are unchanged by moving to the
portable workflow.

## Decisions

Taken with the user during brainstorming; they override the parent spec
where they differ from it.

1. **Behavior-correction policy: the 6a-3/6a-4/6b-2 exception.** Command
   bytes, sequencing and timing budgets are preserved. Read-integrity and
   reply-gating defects are corrected so a malformed reply stops all later
   commands, and every correction is named in the matrix notes. Defects the
   design cannot correct without knowledge we lack are listed in the appendix
   below.
2. **Write stays `FlashOperation::Write` and carries a `KernelImage`, not a
   ROM image.** No new `FlashOperation` value: that enum is switched on by
   every plan, validator and workflow, and no other family needs a bootstrap
   operation. Operator-facing text names the operation a "BDM kernel
   bootstrap" and states the ROM is not written.

## Architecture

### Backend (portable)

Targets live in the existing `src/backend/flash/ecu/` package, where every
family's types, plan and executor already live (TCU families included), and
are registered by name under that package in `PORTABLE_PACKAGES`
(`bazel/portable_targets.bzl`).

- `FlashFamily::SubaruDensoMc68hc16y5_02Bdm` and a
  `SubaruDensoMc68hc16y5_02BdmPlan` `FamilyPlan` alternative.
- `subaru_denso_mc68hc16y5_02_bdm_plan.{h,cpp}`:
  `build_subaru_denso_mc68hc16y5_02_bdm_plan(operation, protocol, mcu, image, kernel)`
  and `validate_…`.
  - Accepts only `sub_ecu_denso_mc68hc16y5_02_bdm` with `MC68HC16Y5`.
  - Read: no image and no kernel.
  - Write: a `KernelImage` at load address `0x20000` whose zero-padded length
    fits `0x8000`; any ROM image is rejected. `validate_and_build()` requires
    every Write plan to carry an `image`, so the plan stores the kernel, padded
    with `0x00` to a multiple of 32 bytes as legacy did, as its `image` with
    transfer region `{0x20000, padded length}`, and leaves `kernel()` empty
    (`family_requires_kernel_v` is `false`). The image is the bytes the
    executor uploads; it is never the operator's ROM.
  - TestWrite: rejected (cfg `test_write=no`).
- `subaru_denso_mc68hc16y5_02_bdm_executor.{h,cpp}` implementing
  `IKlineFlashExecutor`:
  - `transport_setup()` returns `KlineConfig{.baud = 115200, .iso14230 = false, .parity = None}`.
  - `before_transport_configure()` calls `set_add_iso14230_header(false)`.
  - `execute()` dispatches to read or bootstrap. Synchronous, bounded,
    cancellable, dialog-free.

### Desktop

- `flash_workflow.cpp`: the route table entry moves from `Unrouted` to the new
  family, staying a prefix match ahead of the bare `_02` prefix so no
  `_02_bdm*` name can fall through to the K-Line family. A
  `SubaruDensoMc68hc16y5_02BdmWorkflow` follows the Hitachi SH7058 shape:
  Begin prompt, an operation-specific confirmation, one attempt. For Write it
  calls `resolveKernel()` and drops `request.image`; for Read it passes
  neither.
- The shared Begin prompt takes no text, so Write adds one
  `FlashPromptKind::ConfirmBdmKernelBootstrap` after Begin. The dialog renders
  it as a warning stating that the kernel is uploaded to ECU RAM and started
  over BDM and that the ROM is not written; declining cancels before any I/O.

### Deleted

- `MainWindow`'s `sub_ecu_denso_mc68hc16y5_02_bdm` branch.
- `src/ui/desktop/flash/bdm/` (dialog, header, `BUILD.bazel`) and any
  `mainwindow` include/dependency on it.
- `src/platform/desktop/common/flash/legacy/bdm/…_operation.{h,cpp}`.
- The `REMAINING` entry in `scripts/check-legacy-flash-drain.py`.

## Wire behavior

All traffic uses `write_raw()` and `read_raw()`; the executor never calls
`write()` or `read()`. "Accumulate" means calling `read_raw()` repeatedly,
appending chunks, until the required byte count is reached, an `IClock`
deadline expires, or a read returns nothing; the deadline equals the legacy
single-read timeout, and each read is given the time remaining. An empty
read ends accumulation because `read_raw()` returns nothing only after
waiting out its whole timeout.
"Discard for N ms" means accumulating for N ms and ignoring the bytes, logging
them at debug level.

### Read

Image covers `0x00000–0x2FFFF` (0x30000 bytes), matching legacy's output.

1. Discard for 200 ms.
2. For each page address `0x00000, 0x00400, … 0x1FC00` then
   `0x28000, … 0x2FC00` (160 pages):
   - send ASCII `rpmem 0x%08X 0x00000400` (uppercase hex, no terminator);
   - accumulate the page within the legacy budget: up to 50 rounds of a
     200 ms read followed by a 100 ms sleep;
   - the page must be exactly `0x400` bytes: fewer after the budget, or more at
     any point, is an error;
   - sleep 1 ms.
3. `0x20000–0x27FFF` is the RAM block: filled with `0xFF`, never requested.

Progress is reported per page.

### Bootstrap (Write)

Let `K` be the padded kernel and `L` its length.

1. Discard for 200 ms.
2. Send `wdmem 0x00020000 0x%08X` (`L`); expect `ACK_CMD_WDMEM` within 3000 ms.
3. For each 32-byte chunk of `K`: check cancellation, send the chunk, expect
   `ACK_WR` within 800 ms.
4. Discard for 200 ms.
5. Send the literal `wdmem 0xFFC28 0x4`; expect `ACK_CMD_WDMEM` within
   3000 ms; discard for 800 ms.
6. Send `00 0D 00 0C`; expect `ACK_WR` within 800 ms; discard for 200 ms.
7. Send `wpcsp`; read for 800 ms twice, logging the replies.
8. Send `go`; read for 800 ms, logging the reply.

After `go` is sent the kernel is running; cancellation is no longer checked
and the result is success with no `read_bytes`.

An expected ACK passes only when the accumulated bytes, once they reach the
token's length, equal the token exactly — legacy's `received == token`
check. Anything else fails with a hex dump of what arrived, and no later
command is sent.

### Deliberate corrections

Recorded in the matrix notes:

- Direct-serial replies no longer pass through the K-Line frame parser.
- Pages are accumulated across polls; legacy replaced the buffer on each poll.
- A short or long page is an error; legacy appended any non-empty page.
- A failed or short kernel upload now stops the bootstrap at the first bad
  ACK; legacy's `write_mem()` ignored `flash_block()`'s return value and went
  on to enable SCIB, send `wpcsp` and send `go` over a partially uploaded
  kernel.
- The operation no longer mutates the ROM buffer (`MainWindow` already
  restored it afterwards).
- A stale ISO-14230 header from an earlier session is cleared before
  configure.
- TestWrite is rejected before any I/O.

## Testing

All package-owned and co-located.

**Plan tests** (`subaru_denso_mc68hc16y5_02_bdm_plan_test`): the exact pair is
accepted; crossed pairs, `MC68HC16Y5_TPU`, and the bare `_02` protocol are
rejected; TestWrite is rejected; Read rejects an image or kernel; Write
rejects a ROM image, a missing kernel, a load address other than `0x20000`,
and a padded length over `0x8000`; a 33-byte kernel pads to 64 bytes;
`transport_setup()` is asserted field by field against the legacy setter
lines.

**Executor tests** (`ScriptedKlineFlashTransport`):

- Read: 160 byte-exact `rpmem` requests; the `0xFF` hole; a page delivered in
  split chunks is assembled; short page, over-long page and retry exhaustion
  each fail; cancellation between pages.
- Bootstrap: the full byte-exact script; split ACKs are assembled; a wrong ACK
  at each of the four gates fails and sends nothing further; cancellation
  between chunks; unchecked `wpcsp`/`go` replies still yield success.
- `set_add_iso14230_header(false)` is called, and only the raw methods are
  used — the parent spec's guard against collapsing `write_raw()` into
  `write()`.

**Workflow test** (`test_flash_workflow`): the protocol routes to the new
family and the `_02` / `_02_bdm` prefix ordering holds; Write resolves the cfg
kernel into the plan image and drops the ROM image; Write asks Begin then
`ConfirmBdmKernelBootstrap`, Read asks Begin only, and declining either
cancels. No dialog test: the dedicated dialog is deleted
and the shared `FlashDialog` is already covered.

**Guards:** `REMAINING` shrinks by one; `//:portable_closure` covers the new
package; `//:serial_compat_allowlist` is unchanged.

## Documentation

- Matrix row `FlashEcuSubaruDensoMC68HC16Y5_02_BDM`: `portable=yes`,
  `experimental`, automated evidence, notes naming the corrections and that
  Write is a kernel bootstrap.
- New [BDM bench checklist](../../denso-mc68hc16-bdm-bench-checklist.md) gating both operations. It must capture the bridge's `wpcsp` and `go`
  replies so a later PR can gate them.
- A 6c-1 implementation note in the parent spec, and the drain count in the
  [modularization plan](../../modularization-plan.md).

## Out of scope

- JTAG (6c-2), Unisia Jecs M32R (6c-3), bootmode (wave 7).
- The `MC68HC16Y5_TPU` variant.
- Adapter detection: selecting BDM on an OpenPort2 is not rejected.
- Hardware qualification.
- Operator-facing "ROM" wording: the shared `FlashDialog` title ("Write ROM
  … to ECU") and `MainWindow`'s ROM-selection/checksum preconditions still
  speak of a ROM for the BDM Write, and the `ConfirmBdmKernelBootstrap`
  prompt is the only place that states the ROM is not written.

## Appendix: preserved legacy defects

- `wpcsp` and `go` replies are logged, not gated: the bridge firmware's
  replies are not known, and guessing a token would make the only working
  sequence fail.
- Commands carry no line terminator. This is the legacy wire format; changing
  it without bridge evidence would be a guess.
- Nothing prevents selecting this protocol with an OpenPort2 adapter, where
  replies arrive through the J2534 path instead.
