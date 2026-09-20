<!-- docs/superpowers/specs/2026-09-19-step5-tail-wave6-singletons-design.md -->
# Step 5 Tail Wave 6 — Nine Singletons — Design

**Status:** in progress — 6a-1 merged (#347), 6a-2 merged (#348); 6a-3 implemented on the current branch (not yet merged). Seven legacy families remain on this branch.
**Predecessor:** [wave 5, Denso SH705x CAN](2026-09-02-step5-tail-wave5-denso-sh705x-can-design.md), merged as PR #341
**Umbrella:** [step 5 tail flash drain](2026-08-08-step5-tail-flash-drain-design.md)

## Goal

Migrate the nine remaining non-bootmode legacy flash families to portable
`FlashPlan` + `IFlashExecutor` pairs, taking `//:legacy_flash_drain` from ten
entries to one. After this wave only
`FlashEcuSubaruUnisiaJecsM32rBootMode` remains, and wave 7 is that family
plus package teardown.

The wave also specifies, in full, the port surface bootmode needs, so wave 7
adopts an agreed shape rather than designing it cold. Wave 6 lands three of
the four additions; the fourth is specified and deliberately unlanded.

## Findings That Shape the Design

### The umbrella's premise for this wave is wrong; its conclusion is right

The tail design describes these nine as calling only
`write_serial_data` / `read_serial_data` — "a raw byte stream that
`IKlineFlashTransport` already covers" — and concludes they need no new,
5c-style shared ports. The conclusion holds. The premise does not: seven of
the nine call `write_serial_data_echo_check`, not `write_serial_data`, and
four call `change_port_speed`.

They need no new ports anyway, because `DesktopKlineFlashTransport` already
binds the whole vocabulary:

| Legacy call | Existing port surface |
|---|---|
| `write_serial_data_echo_check` | `IKlineFlashTransport::write()` |
| `change_port_speed` | `IKlineFlashTransport::setBaud()` |
| `is_serial_port_open` | folded into every adapter method's precondition |
| `open_serial_port` / connection flags | `configure()` + `open()` |
| `set_add_iso14230_header` | `set_add_iso14230_header()` |
| `set_lec_lines(RTS-disabled, DTR-disabled)` | `disable_lec_lines()` |
| `set_lec_lines(RTS-enabled, DTR-disabled)` | `enable_programming_voltage_line()` |

This matters beyond bookkeeping: it is why the wave can be ordered by port
gap at all, and it shrinks wave 7's "six calls that exist on no port" to two.

### The real gap list is four items, not six

Measured across all ten remaining families (wave 6's nine plus bootmode):

| Gap | Families needing it |
|---|---|
| configure-time parity | `unisia_jecs` |
| `reset_connection()` on K-Line | `denso_sh705x_kline`, bootmode |
| raw (non-echo) write | `jtag`, `bdm` |
| mid-session parity + a fourth LEC combination | bootmode only |

Four wave-6 families need nothing new at all:
`tcu_hitachi_m32r_can`, `tcu_hitachi_m32r_kline`, `hitachi_sh7058_can`,
`hitachi_sh72543r_can`.

### `write_serial_data` and `write_serial_data_echo_check` diverge only on direct-serial

Both apply the same header logic and, when `use_openport2_adapter` is set,
both end in the same `write_j2534_data(output); return STATUS_SUCCESS;` — they
are byte-identical on the J2534 path
(`serial_port_actions_direct.cpp:904` and `:948`). On the direct-serial path
the echo variant additionally drains the local echo, byte by byte, with a
timeout.

`jtag` and `bdm` call the non-echo variant, and neither restricts itself to
OpenPort2. Routing them through the existing `write()` would therefore consume
bytes their own `read_serial_data` calls expect, on direct-serial adapters
only. This is a real behavior change, not a theoretical one, and it is why the
wave adds an explicit second write method rather than reusing `write()`.

### The VPP prompt is a dialog concern, not a transport capability

`unisia_jecs_m32r` prompts the operator to apply VPP mid-session
(`flash_ecu_subaru_unisia_jecs_m32r_operation.cpp:434`), guarded by
`get_use_openport2_adapter()` — the adapter can supply programming voltage
itself, so the prompt is for everyone else.

`ConfirmationSpec` already models exactly this. Its contract
(`flash_types.h:88`) is that confirmations are collected by the desktop dialog
*before* the executor starts, because a synchronous, dialog-free executor
cannot block mid-run for a human answer; presence in
`FlashPlan::confirmations()` means "granted". The adapter question is one the
dialog can answer, since the dialog owns `SerialPortActions`.

So this needs one new `ConfirmationSpec::Id`, no transport capability, and no
executor-side prompting. It is distinct from the existing
`requires_post_kernel_upload_delay()`, which is a Unix-only timing capability
that happens to read the same adapter flag.

### The TCU families share anatomy with their ECU siblings, not wire format

`FlashTcuSubaruHitachiM32rKlineOperation` has its own service set — `a0` block
read, `b8` byte read, `b0` block write, and two seed-key generators — and is
733 lines against the migrated ECU family's 1,244. The migrated wave-1 and
wave-3 families are a template for *structure*: plan shape, executor
decomposition, dialog rewrite. They are not a source of shareable constants.

This wave therefore plans no `*_common.h` extraction. Waves 4 and 5 both
measured the shared-substrate payoff and found it small; these nine share less
than either.

## Scope

### In scope

- Nine family migrations, one PR each, in three sub-waves ordered by port gap.
- Three port additions, each landed in the PR of its first real caller.
- One specified-but-unlanded port addition for wave 7.
- One new `ConfirmationSpec::Id`.
- A docs-only wave-close PR.

### Out of scope

- Hardware qualification. No family reaches `proven`; the project has no bench
  access, and the umbrella excludes it as a step-5 gate.
- `FlashEcuSubaruUnisiaJecsM32rBootMode` and package teardown — wave 7.
- Cluster factoring. Explicitly none; see the TCU finding above.
- `MainWindow` thin-shell work and `FileActions` deletion — step 6.

## Delivery Sequence

Ten PRs. Family is the PR unit; each sub-wave gets its own implementation plan.

### 6a — zero port gap (4 PRs, 4,466 lines)

| PR | Family | Lines | Transport |
|---|---|---|---|
| 6a-1 | `FlashTcuSubaruHitachiM32rKline` | 733 | K-Line |
| 6a-2 | `FlashTcuSubaruHitachiM32rCan` | 1,036 | ISO-15765 |
| 6a-3 | `FlashEcuSubaruHitachiSH72543rCan` | 1,201 | ISO-15765 |
| 6a-4 | `FlashEcuSubaruHitachiSH7058Can` | 1,496 | ISO-15765 |

Smallest first, so the two TCU families establish the ECU-to-TCU structural
template before the two larger Hitachi CAN families. No port additions, no ADR.

Every family in this sub-wave **must** register each new backend `cc_library`
by name in `PORTABLE_PACKAGES` (`bazel/portable_targets.bzl`): that map is keyed
by package but holds a list of individual target names, so a new target in an
already-listed package is silently never swept by `//:portable_closure` until it
is named. That `bazel/` edit is mandatory, not a deviation.

**Stop condition.** If a 6a family needs a port addition, the wave-6 premise is
wrong. Stop and re-spec; do not add the port inline. Proving that premise is
this sub-wave's purpose.

### 6b — one narrow port addition each (2 PRs, 1,969 lines)

| PR | Family | Lines | Adds |
|---|---|---|---|
| 6b-1 | `FlashEcuSubaruUnisiaJecs` | 233 | `KlineConfig` parity |
| 6b-2 | `FlashEcuSubaruDensoSH705xKline` | 1,736 | `IKlineFlashTransport::reset_connection()` |

6b-1 is read-only at 1953 baud with even parity — the cheapest possible proof
of a port addition, which is why it precedes the 1,736-line family that
supports read, test_write and write.

### 6c — novel seams (3 PRs, 1,942 lines)

| PR | Family | Lines | Adds |
|---|---|---|---|
| 6c-1 | `FlashEcuSubaruDensoMC68HC16Y5_02_BDM` | 480 | `write_raw()` |
| 6c-2 | `FlashEcuSubaruHitachiM32rJtag` | 715 | — consumes 6c-1's decision |
| 6c-3 | `FlashEcuSubaruUnisiaJecsM32r` | 747 | `ApplyProgrammingVoltage` confirmation |

BDM before JTAG: it is smaller and its byte stream is simpler — 19 reads and 8
writes, with no `set_add_iso14230_header`. 6c-3 is last because it is the only
PR touching `ConfirmationSpec` and the dialog's confirmation flow.

### 6d — wave close (1 PR, docs only)

Flip nine rows in the [flash qualification matrix](../../flash-qualification-matrix.md),
apply the documentation amendments below, and add wave-6 rows to the
[modularization plan](../../modularization-plan.md).

Drain after the wave: ten entries to one.

## Architecture

### Per-PR anatomy

Unchanged from the wave-0 template. Each PR delivers:

**Backend, portable:** one `FlashFamily` enum value; one `<Family>Plan` POD and
`FamilyPlan` variant alternative; `<family>_plan.{h,cpp}`, building and
validating with no irreversible I/O; `<family>_executor.{h,cpp}` implementing
`IKlineFlashExecutor` or `ICanFlashExecutor` — synchronous, bounded,
cancellable, dialog-free.

**Backend never sees `EcuCalDefStructure`.** The dialog performs the
translation, as in 5c and every wave since, so the drain adds no new dependency
on the god object step 6 deletes.

**UI:** rewrite `src/ui/desktop/flash/<scope>/<family>.cpp` from "construct the
legacy operation, connect `FlashOperationWorker` signals" to "build plan, run it
on `FlashWorker` with the portable executor and a transport adapter", plus a
`<family>_dialog_test.cpp`.

**Delete** `<family>_operation.{h,cpp}`, remove the `REMAINING` entry, flip the
matrix row. The legacy `BUILD.bazel` needs no edit — its `srcs` and `MOC_HDRS`
are globs.

### Port surface

Four additions, specified whole here and adopted one caller at a time.

**1. `KlineParity` on `KlineConfig`** — a portable enum `{None, Even, Odd}`,
mapped to `QSerialPort` parity values inside
`DesktopKlineFlashTransport::configure()`. Configure-time only; no portable
code sees a Qt type. *Landed 6b-1.*

**2. `IKlineFlashTransport::reset_connection()`** — mirrors the existing
`ICanFlashTransport::reset_connection()`, including its contract that every
transport must expose a real reset "so protocol-owned sequences cannot silently
degrade into configure/open only". *Landed 6b-2.*

**3. `write_raw(bytes::ByteView)` alongside `write()`** — a second explicit
method, not an `EchoPolicy` parameter and not a configure-time flag. A
defaulted parameter would let a family silently acquire the wrong echo
behavior, and the adapter already exposes semantically-named methods
(`disable_lec_lines()`, `pulse_lec_2_line()`) rather than pass-through
setters. `write()` keeps its current binding to
`write_serial_data_echo_check`; `write_raw()` binds `write_serial_data`. The
port's header comment records that the two are byte-identical on the J2534 path
and diverge only on direct-serial, so a later attempt to collapse them has the
reason in front of it. *Landed 6c-1.*

**4. `set_parity()` mid-session, and `enable_boot_mode_lines()`** (a fourth LEC
semantic: RTS-enabled with DTR-**enabled**) — bootmode's only remaining needs.
Specified here; **not landed in wave 6**, because no wave-6 family calls them
and the ratchet discipline gives speculative surface no safety net. Revising
this item before wave 7 costs a spec edit, not a migration.

Bootmode's other legacy calls need nothing new: `reset_connection` comes from
item 2, `change_port_speed` is `setBaud()`, `write_serial_data_echo_check` is
`write()`, `is_serial_port_open` is already a precondition inside every adapter
method, and its RTS/DTR getter pairs are what items 2 and 4 wrap.

### Confirmation

One new `ConfirmationSpec::Id::ApplyProgrammingVoltage`, collected up-front by
the `unisia_jecs_m32r` dialog and only when the adapter does not supply
programming voltage itself. The `get_use_openport2_adapter()` check moves from
the operation body to the dialog. An operator who declines causes the dialog to
never build a plan, matching the documented semantics of `EraseTrigger` and
`TopRegionBootstrap`.

This moves the prompt earlier in the operator's flow; it never removes it. The
matrix note for that row records the change.

## Testing

Three layers per PR, all package-owned and co-located.

**Plan tests.** Validation rejects every malformed shape. `transport_setup()`
returns the exact `KlineConfig` or `Iso15765Config` the legacy setters
produced, asserted field by field against the legacy source line that set each
one.

**Executor tests.** `ScriptedKlineFlashTransport` or
`ScriptedCanFlashTransport`, whose `expectWrite()` is already a byte-exact wire
assertion. Every request the legacy family emits gets a scripted exchange, and
cancellation is asserted at each checkpoint. 6b-2 additionally asserts
`reset_connection()` is called at the legacy call site, not merely implied by a
reconfigure.

**Desktop dialog tests.** Plan construction from dialog state, including the
negative path. 6c-3 asserts both confirmation branches: adapter supplies VPP,
so `ApplyProgrammingVoltage` is absent from `confirmations()`; adapter does
not, so it is present — and a declining operator yields no plan at all.

**Port-addition assertions.** 6b-1 asserts parity reaches
`QSerialPort::EvenParity` at 1953 baud, in
`desktop_kline_flash_transport_test.cpp`. 6c-1 and 6c-2 assert the executor
calls `write_raw()` and the adapter binds it to `write_serial_data`, so a
future refactor collapsing the two methods fails a test rather than silently
changing direct-serial behavior.

**Guards.** Each PR shrinks `REMAINING` by one; `//:portable_closure` and
`//:serial_compat_allowlist` are unchanged by every PR in this wave. No
allowlist entry is added — needing one would mean the PR took the legacy path.

## Behavior-Correction Rule

Inherited from wave 5 and unchanged. The migration is byte-preserving. Where
legacy behavior is demonstrably wrong — an ignored return, an unchecked length,
a swallowed timeout — the PR preserves the wire behavior and records the defect
in this spec's appendix. It does not fix it silently, because a wire change and
a structural migration cannot be reviewed in the same diff, and no family here
has hardware qualification to catch a regression.

## Risks and Mitigations

| Risk | Mitigation |
|---|---|
| A 6a family needs a port addition | Stop condition above: re-spec rather than add inline. |
| Echo-policy decision is wrong for direct-serial JTAG/BDM | `write_raw()` preserves the legacy byte path exactly; the divergence is documented at the port. Neither family is hardware-qualified, and neither reaches `proven` here. |
| Up-front VPP confirmation changes operator flow | The prompt moves earlier, never disappears. Both branches are dialog-tested and the matrix row notes it. |
| Wave 7 finds port item 4 mis-shaped | It is specified but unlanded by design. |
| Step 6 collides on `src/ui/desktop/flash/**` | Unchanged from the tail design: step 6 does not start on these dialogs until the drain finishes. |
| Nine PRs is a long wave | Three sub-waves, each with its own plan and its own stop condition; 6a alone moves the drain from ten to six. |

## Documentation and Completion

Carried by 6d:

- **Tail design amendment.** Its wave-6 rationale is correct in conclusion and
  wrong in premise — these families are not raw `write_serial_data` streams.
  Record why no new shared ports are needed: `write()` already *is*
  `write_serial_data_echo_check`. Its wave-7 note that six calls "exist on no
  current flash transport port" becomes two.
- **Modularization plan.** Wave 5's line changes from "complete on this branch"
  to merged as PR #341; wave-6 sub-wave rows are added.
- **Qualification matrix.** Nine rows to `portable=yes`,
  `hardware_status=experimental`, automated evidence only, with the
  per-family notes named above.

**No ADR.** Items 1 to 3 extend existing ports along their documented
contracts; none reverses a decision. If 6c-1's echo-policy discussion becomes a
general rule about adapter-path-dependent behavior, that earns an ADR then.

## Appendix: Preserved Legacy Defects

Populated per PR under the behavior-correction rule. Empty at spec time.

## Wave 6a-3 implementation note

The [SH72543R family spec](2026-09-20-step5-tail-wave6a3-hitachi-sh72543r-can-design.md)
records the approved exception to blanket behavior preservation: reject unsafe
TestWrite, correct bounds/read-integrity/cancellation/erase-response defects,
and keep other wire behavior. This family uses the current shared FlashWorkflow
and removes its old dialog rather than rewriting it.
