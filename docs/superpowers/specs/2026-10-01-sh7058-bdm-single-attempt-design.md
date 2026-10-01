# SH7058 and BDM join SingleAttemptFlashWorkflow — design

Date: 2026-10-01. Status: approved in brainstorming; dropped from the change
before merge, as with the earlier consolidation specs.

## Goal

Close the last single-attempt action under "P1: Consolidate flash workflow
orchestration" in the [tech-debt roadmap](../../tech-debt.md): run the Hitachi
SH7058 and Denso MC68HC16Y5 BDM families through `SingleAttemptFlashWorkflow`
and delete `SubaruHitachiSh7058Workflow` and
`SubaruDensoMc68hc16y5_02BdmWorkflow` from
`src/platform/desktop/common/flash/flash_workflow.cpp`.

Success means:

- identical prompt kinds, prompt order, cancellation and result behavior for
  both families;
- identical plan-preparation timing (SH7058 at construction; BDM on the first
  `next()`, before Begin, catalog read only for Write);
- characterization tests landed first and passing unmodified across the
  refactor, as in #455 and #460.

## Decision

Both extra prompts become plan-owned `ConfirmationSpec`s. Rule 1 of the
[design notes](../../design-notes.md#operator-steps-never-block-an-executor) already
says permission before starting is a `ConfirmationSpec` whose presence in the
plan means "granted"; these two prompts are the remaining exceptions. The
rejected alternatives were workflow-side "extra prompt" and "binding by
operation" policies, each serving one family, and keeping both workflows
hand-written.

## Backend: plan-owned confirmations (`src/backend/flash/`)

### New ids

Add to `ConfirmationSpec::Id` in `flash_types.h`, after
`ApplyBootModeVoltages`, with a comment stating the existing contract
(collected by the desktop before the executor starts; presence means granted):

- `StartKlineRead` — Hitachi SH7058: the operator agreed to open the adapter
  and start the K-Line ROM read.
- `KernelBootstrap` — Denso MC68HC16Y5 BDM: the operator agreed to upload the
  kernel into RAM and start it; the ROM is not written.

Both carry no arguments.

### Builders

- `build_subaru_hitachi_sh7058_plan` sets `confirmations` to
  `{ConfirmationSpec{.id = StartKlineRead}}` for Read and `{}` for Write.
- `build_subaru_denso_mc68hc16y5_02_bdm_plan` sets `confirmations` to
  `{ConfirmationSpec{.id = KernelBootstrap}}` for Write and `{}` for Read.

TestWrite is rejected by both builders before any plan is built, unchanged.

### Validators

Each validator removes `!plan.confirmations().empty()` from its existing
shape/geometry condition and adds a separate confirmation check with its own
message, in the style of `validate_subaru_denso_sh7055_02_plan`:

- SH7058: Read requires exactly one confirmation, `StartKlineRead`, with empty
  arguments; Write requires none.
- BDM: Write requires exactly one confirmation, `KernelBootstrap`, with empty
  arguments; Read requires none.

Executors are unchanged. Both SH7058 executors and the BDM executor already
call these validators from `transport_setup` (and BDM again from `execute`),
so a plan missing its consent is rejected before the adapter is configured.

### Plan tests

- `subaru_hitachi_sh7058_plan_test.cpp` and
  `subaru_denso_mc68hc16y5_02_bdm_plan_test.cpp`: the hand-built
  `FlashPlanFields` fixtures gain the confirmation where the operation
  requires it; builder tests assert the emitted set per operation.
- New negative cases per family: the confirmation missing; present on the
  other operation; replaced by a different id; carrying arguments.
- Executor tests build plans through the builders and need no change.

## Desktop: join the shared workflow (`src/platform/desktop/common/flash/`)

### Prompt mapping

`confirmationPrompt` gains two cases, arguments forwarded unchanged:

- `StartKlineRead` → `FlashPromptKind::ConfirmSh7058Read`
- `KernelBootstrap` → `FlashPromptKind::ConfirmBdmKernelBootstrap`

`FlashPromptKind`, `flash_workflow.h` and `FlashDialog` do not change.

### Hitachi SH7058

Delete `SubaruHitachiSh7058Workflow`. Add:

```cpp
// Read runs over K-Line and Write over CAN: the plan builder chooses the
// transport by operation and the factory chooses the matching executor.
using SubaruHitachiSh7058KlineWorkflow =
    KernelFreeKlineWorkflow<SubaruHitachiSh7058KlineExecutor, &build_subaru_hitachi_sh7058_plan>;
using SubaruHitachiSh7058CanWorkflow =
    KernelFreeCanWorkflow<SubaruHitachiSh7058CanExecutor, &build_subaru_hitachi_sh7058_plan>;
```

The factory's `SubaruHitachiSh7058` case returns the K-Line alias for
`FlashOperation::Read` and the CAN alias otherwise; TestWrite still fails at
preflight with `Unsupported` from the builder. The route table is unchanged.
If the factory and builder ever disagreed, the bound executor's
`transport_setup` would reject the plan with `Unsupported` ("SH7058 K-Line
supports read only" or "SH7058 CAN supports write only") before configure.

### Denso MC68HC16Y5 BDM

Delete `SubaruDensoMc68hc16y5_02BdmWorkflow`. Add a preparation function that
reproduces its `buildPlan()`:

- Write: resolve the catalog kernel through `QtFileRepository` and
  `resolveKernel`, then call the builder with `rom_image = std::nullopt` and
  the kernel.
- Read and TestWrite: call the builder with no ROM image and no kernel; the
  catalog is never read.
- The operator's ROM (`request.image`) is never forwarded; keep the existing
  comment saying so.

The alias is
`SingleAttemptFlashWorkflow<SubaruDensoMc68hc16y5_02BdmExecutor, DesktopKlineFlashTransport, BdmKernelPlan>`,
keeping the name `SubaruDensoMc68hc16y5_02BdmWorkflow` for the factory case.

### Fold the lazy preparation policies

`CachedKernelPlan`, `Mc68KernelPlan` and the BDM policy would be three copies
of the same "prepare on the first `next()`, cache the result" class. Replace
them with one template:

```cpp
using PlanPreparation = Result<FlashPlan> (*)(FlashWorkflowRequest&);

// Prepares the plan on the first next(), before Begin, and keeps the result:
// a preparation failure is reported before any prompt, and the attempt
// carries the snapshot taken then even if files change afterward.
template <PlanPreparation Prepare> class LazyPlan
{
  public:
    explicit LazyPlan(const FlashWorkflowRequest&) {}

    Result<FlashPlan>& plan(FlashWorkflowRequest& request)
    {
        if (!plan_.has_value())
        {
            plan_ = Prepare(request);
        }
        return *plan_;
    }

  private:
    std::optional<Result<FlashPlan>> plan_;
};

template <KernelBackedPlanBuilder Build> using CachedKernelPlan = LazyPlan<&prepareKernelBacked<Build>>;
using Mc68KernelPlan = LazyPlan<&prepareMc68>;
using BdmKernelPlan = LazyPlan<&prepareBdm>;
```

The preparation functions keep their current bodies and comments. `EagerPlan`
is unchanged.

## Commits

Each commit builds and passes `bazel test --config=release //...` on its own.

1. **`test:` characterization tests only**, in `flash_workflow_test.cpp`,
   reusing `request`, `catalogPaths`, `recordingSerial` and
   `expectNoBackendIo`. They pass against the current code.
   - SH7058 Read and Write: running the bound attempt with an
     already-cancelled token yields `ErrorKind::Cancelled` (not the
     mismatched executor's `Unsupported`) with no backend I/O, proving the
     bound executor accepts the plan for that operation.
   - SH7058: TestWrite fails with `Unsupported` and a wrong MCU with
     `InvalidConfig`, both before any prompt; an attempt result is propagated
     for success with read bytes, failure and cancellation.
   - BDM: a Write whose catalog has no kernel fails before Begin; an attempt
     result is propagated for Read success with read bytes and for Write
     failure.
2. **`feat(flash):` plan-owned confirmations** — the backend section above,
   tests first. The workflow suite passes unchanged because the hand-written
   workflows ignore plan confirmations.
3. **`refactor:` SH7058 and BDM join `SingleAttemptFlashWorkflow`** — the
   desktop section above except the fold. Commit 1's tests pass without
   modification. Before committing, confirm they catch breakage: swapping the
   two SH7058 aliases in the factory, and removing either new
   `confirmationPrompt` case, must each fail tests.
4. **`refactor:` fold the lazy preparation policies into `LazyPlan`** — no
   behavior change; covered by the kernel-backed and MC68 suites.

## Documentation

In commit 3:

- [Tech-debt roadmap](../../tech-debt.md): remove the "Decide whether the last
  two hand-rolled single-attempt workflows…" action and reword the next
  bullet, which refers to "that consolidation".
- [Design notes](../../design-notes.md): in rule 1, add the BDM kernel
  bootstrap and SH7058 read as examples; in "Keep `FlashOperation` to Read,
  TestWrite and Write", say the difference is made explicit with a dedicated
  `ConfirmationSpec`.

No bench checklist or qualification-matrix change: wire behavior and prompt
text are identical, and nothing is marked qualified.

## Verification

- `bazel test --config=release //...`
- `prek run --all-files`
- `bazel run //:clang_tidy_report_changed`
- `scripts/android-cross-compile.sh` (backend sources change)
- `python3 scripts/gazelle_check.py` (no BUILD changes expected)

## Behavior notes

- No observable change for any plan a builder produces: same prompt kinds,
  same order, same kernel-resolution timing, same failures before Begin.
- New: a hand-built SH7058 Read plan or BDM Write plan without its consent is
  rejected by the validator, and therefore by the executor before configure.

## Out of scope

The EEPROM, Unisia Jecs M32R K-Line (programming voltage) and boot-mode
workflows; `nonfatal_query` convergence; renaming `FlashPromptKind` values or
changing dialog text; executor or wire changes.
