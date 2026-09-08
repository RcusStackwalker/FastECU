# Task 4 report — route portable DensoCAN family

## Revision

- Base: `e3bee703daf26cbb2b4ac9ed265ec3cd4542c13e`
- Head: `HEAD` (`feat(flash): route portable DensoCAN family`; this commit)

## Delivered changes

- Added `KernelBackedCanFlashWorkflow` for typed kernel-backed CAN plans and
  instantiated it with `SubaruDensoSh705xDensoCanExecutor`,
  `build_subaru_denso_sh705x_densocan_plan`, and
  `DesktopMixedCanFlashTransport`.
- Added five exact DensoCAN workflow routes; existing routes retain prefix
  matching and the EEPROM rows remain ahead of DensoCAN.
- Removed the broad MainWindow `_densocan` dispatch and deleted the obsolete
  DensoCAN dialog and legacy operation pairs, including only their BUILD
  entries. The legacy drain now reports 13 remaining families.
- Registered DensoCAN plan/executor closure roots in root `BUILD.bazel` and
  its type/plan/executor targets in `check-portable-closure.py`. The latter
  now explicitly rejects a silently missing DensoCAN root registration.
- Updated the qualification matrix: portable=yes, experimental,
  automated-only, exact IDs, and raw CAN plus proprietary ISO-15765 BEEF
  transport (not UDS), retaining the accepted dynamic-log correction note.

## RED → GREEN workflow evidence

RED command:

```sh
bazel test --config=release //src/platform/desktop/common/flash:test_flash_workflow
```

The new three tests failed because `FlashWorkflowFactory::tryCreate()` returned
null for `sub_ecu_denso_sh7055_densocan`; the output reported 23 passing and
3 failing tests. No production DensoCAN route was registered at that point.

After implementing exact routes and `KernelBackedCanFlashWorkflow`, the same
test target passed. The regression coverage proves:

- exactly these five routes are owned:
  `sub_ecu_denso_sh7055_densocan`,
  `sub_ecu_denso_sh7058_densocan`,
  `sub_ecu_denso_sh7058s_densocan`,
  `sub_ecu_denso_sh7058s_diesel_densocan`, and
  `sub_ecu_denso_sh7059_diesel_densocan`;
- `sub_ecu_denso_sh7058_densocan_extra` and `future_densocan` are unrouted;
- kernel/catalog preflight occurs before `Begin`; accepted flow is `Begin`,
  `CycleIgnition`, then `FlashAttempt`; declines at either prompt yield
  `Cancelled` before an attempt is constructed;
- the resulting plan owns the catalog kernel bytes and declares
  `TransportKind::CanRawIso15765`;
- successful attempt results propagate both read bytes and ROM ID to
  `FlashCompletedStep`.

## Non-vacuous closure guard mutations

Mutations were made with `apply_patch`, tested, and restored before commit.

1. Removing `subaru_denso_sh705x_densocan_executor` from the root genquery
   `scope` made `bazel test --config=release //:portable_closure` fail during
   analysis: the executor was outside the query scope. Restoring it passed.
2. Removing the executor from `PORTABLE_ROOTS` made the same target fail with
   `FAIL: DensoCAN portable closure roots are missing`. Restoring it passed.

## Final verification

Passed:

```sh
prek run --files BUILD.bazel docs/flash-qualification-matrix.md \
  scripts/check-legacy-flash-drain.py scripts/check-portable-closure.py \
  src/platform/desktop/common/flash/BUILD.bazel \
  src/platform/desktop/common/flash/flash_workflow.cpp \
  src/platform/desktop/common/flash/flash_workflow_test.cpp \
  src/platform/desktop/common/flash/legacy/BUILD.bazel \
  src/ui/desktop/flash/ecu/BUILD.bazel src/ui/desktop/mainwindow.cpp \
  src/ui/desktop/mainwindow.h

bazel test --config=release \
  //src/backend/flash/ecu:subaru_denso_sh705x_densocan_plan_test \
  //src/backend/flash/ecu:subaru_denso_sh705x_densocan_executor_test \
  //src/platform/desktop/common/transport:test_desktop_mixed_can_flash_transport \
  //src/platform/desktop/common/flash:test_flash_workflow \
  //:portable_closure //:serial_compat_allowlist //:legacy_flash_drain \
  //:backend_no_widgets

bazel build --config=release //apps/desktop:fastecu
git diff --check
```

The eight requested tests/guards passed. The desktop build passed; it emitted
only pre-existing warnings in unrelated legacy/UI sources. A final
`rg -n 'FlashEcuSubaruDensoSH705xDensoCan' src scripts` produced no hits.

## Self-review and concerns

- No DensoCAN executor or plan protocol behavior was changed; Task 4 only
  composes the existing portable family into the desktop workflow.
- The qualification status remains experimental and automated-only. No bench
  or vehicle qualification is claimed.
