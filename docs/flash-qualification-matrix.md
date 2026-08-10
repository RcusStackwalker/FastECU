<!-- docs/flash-qualification-matrix.md -->
# Flash Qualification Matrix

## Schema

| Column | Meaning |
|---|---|
| `family_id` | Stable code identifier, one row per selectable operation family/transport variant |
| `scope` | ECU, TCU, EEPROM, JTAG, BDM, or bootmode |
| `transport` | K-Line, raw CAN, ISO-15765, SSM, JTAG, or BDM |
| `operations` | Actual supported `read`, `test_write`, `write` set; no inferred capability |
| `portable` | `yes` only when plan + executor are in enforced portable closure |
| `automated_evidence` | Golden/state-machine test labels and last qualifying revision |
| `hardware_status` | `unqualified`, `experimental`, or `proven` |
| `hardware_evidence` | Date, ECU/TCU/adapter identity, operation, operator/report reference; `—` if absent |
| `notes` | Known compatibility quirks, unsupported branches, and safety limitations |

## Rules

- Seed one row for every current backend flash/eeprom/jtag/bdm/bootmode family, not only the proving pair. No family silently disappears from migration scope.
- The proving pair has `portable=yes`, test labels, and `hardware_status=experimental`; it is never marked proven from unit tests.
- Unmigrated rows have `portable=no` and `hardware_status=unqualified` unless concrete historical hardware evidence is entered with a reference.
- `proven` requires a real read comparison and, where the family supports it, test-write/write/verify/recovery checks on named hardware. A future change records evidence; it does not infer proof from similarity to another family.
- Matrix status is documentation only. It does not drive UI badges, dialogs, or runtime feature flags in step 5.

## Families

<!-- One row per family, grouped by scope then stable family ID. Populate
     every column from the source enumeration performed at task start; do
     not leave any row's operations/transport column as a guess. -->

| family_id | scope | transport | operations | portable | automated_evidence | hardware_status | hardware_evidence | notes |
|---|---|---|---|---|---|---|---|---|
| DensoSh705xEepromKline | EEPROM | K-Line | read | yes | `denso_sh705x_eeprom_common_test`, `denso_sh705x_eeprom_kline_executor_test` @ step 5c | experimental | — | write/test_write unsupported: legacy write call was already commented out; portable builder returns Unsupported rather than legitimizing a no-op write |
| DensoSh705xEepromCan | EEPROM | ISO-15765 | read | yes | `denso_sh705x_eeprom_common_test`, `denso_sh705x_eeprom_can_executor_test` @ step 5c | experimental | — | same write/test_write gap as the K-Line sibling; `_ecutek_racerom_alt` RAM-location read fixes a pre-existing legacy bug (read from `received`, not the unpopulated `response`) — see executor comment |
| FlashEcuMitsuM32rCan | ECU | ISO-15765 | read, write | yes | `mitsu_colt_m32r_can_plan_test`, `mitsu_colt_m32r_can_executor_test`, `test_flash_ecu_mitsu_m32r_can_dialog` @ step 5 tail wave 0 | experimental | — | Portable `MitsuColtM32rCanPlan`/`MitsuColtM32rCanExecutor` (`src/backend/flash/ecu/`) serves both `mitsu_ecu_m32r_can` and `mitsu_ecu_m32r_can_vendor_ext`; only the vendor authorization stage differs. Write preflight requires an exact 512 KiB image before confirmations or worker startup, preserves full-file absolute offsets, ignores the protected `0x0000`-`0x8000` bootloader prefix, and writes/verifies the aggregate `0x8000`-`0x80000` range (480 KiB). The normal `0x8000`-`0x60000` window uses the stock helpers; the conditional `0x60000`-`0x80000` window uses redirect helpers and is skipped when already equal. Both windows receive byte-for-byte read-back verification on write paths, with monotonic aggregate progress. `test_write` remains `Unsupported`. Both high-risk confirmations are collected before execution, and the warning calls out post-erase cancellation/recovery risk. A [bench-qualification checklist](colt_czt_47110032_can_bench_checklist.md) gates real-vehicle use, so `hardware_status` remains `experimental`. |
| FlashEcuSubaruDenso1N83M_1_5MCan | ECU | ISO-15765 | read, write | no | — | unqualified | — | Protocol `sub_ecu_denso_1n83m_1_5m_can`; cfg `test_write=no`. |
| FlashEcuSubaruDenso1N83M_4MCan | ECU | ISO-15765 | read, write | no | — | unqualified | — | Protocol `sub_ecu_denso_1n83m_4m_can`; cfg `test_write=no`. |
| FlashEcuSubaruDensoMC68HC16Y5_02 | ECU | K-Line | varies by protocol variant — see notes | no | — | unqualified | — | One class serves four protocol names via `mainwindow.cpp`'s `startsWith` prefix dispatch: `sub_ecu_denso_mc68hc16y5_02` / `_02_ecutek` (alias `wrx02`; cfg read/test_write/write = yes/yes/yes) and `sub_ecu_denso_mc68hc16y5_04` / `_04_ecutek` (alias `wrx04`; cfg read = `n/a`/`no`, test_write=no, write=no — i.e. cfg says this board revision isn't actually flashable through this path, yet the same class is wired up for it). Also catches `sub_ecu_denso_mc68hc16y5_02_tpu` by prefix even though that protocol's own `<mode>` is `BDM` and `<flash_transport>` is `K-Line` — it is routed here, not to the dedicated BDM class below. Weak/ambiguous historical-evidence note: commit `18fb9c83d53721cafde462e367e8a93087454b40` ("Some tests on bench setup and vbatt fix", 2025-03-10, upstream) added one line (`serial->read_serial_data(10);`, draining stale serial input before connect) to this file; the same commit made a substantive bootloader-retry rework to the sh7055_02 file below, so this file's one-line change looks like a mechanical follow-on rather than independent bench confirmation of this family — not counted as `hardware_evidence`. |
| FlashEcuSubaruDensoSH7055_02 | ECU | K-Line, raw CAN | read, test_write, write | no | — | unqualified | — | Protocols `sub_ecu_denso_sh7055_02` and `_02_ecutek` (alias `fxt02`); cfg read/test_write/write = yes/yes/yes for both. Historical hardware evidence found but not sufficient to promote `hardware_status`: commit `18fb9c83d53721cafde462e367e8a93087454b40` ("Some tests on bench setup and vbatt fix", Miika Syvänen, 2025-03-10) reworked `connect_bootloader()` into a 20-iteration handshake retry loop and reordered the countdown/pulse sequence — the kind of change that comes from observing real bootloader timing on a bench ECU. The commit carries no adapter identity, ECU serial, or pass/fail report, so it does not meet the `hardware_evidence` column's bar (needs date + identity + operator/report); recorded here so a future qualification pass can follow up with the original author. |
| FlashEcuSubaruDensoSH7058Can | ECU | ISO-15765 | read, test_write, write | no | — | unqualified | — | One class serves `sub_ecu_denso_sh7058_can`, `_can_ecutek`, `_can_ecutek_racerom`, `_can_ecutek_racerom_alt`, and `_can_cobb` (aliases `subarucan*`) via prefix dispatch; cfg read/test_write/write = yes/yes/yes uniformly across all five. |
| FlashEcuSubaruDensoSH7058CanDiesel | ECU | ISO-15765 | read, test_write, write | no | — | unqualified | — | Protocols `sub_ecu_denso_sh7058_can_diesel` and `sub_ecu_denso_sh7059_can_diesel` (alias `subarucand`) both dispatch to this one class; cfg yes/yes/yes for both. |
| FlashEcuSubaruDensoSH705xDensoCan | ECU | raw CAN | read, test_write, write | no | — | unqualified | — | Catches every protocol name ending in `_densocan` that does not also contain `eeprom` (the `mainwindow.cpp` `endsWith("_densocan")` branch is checked ahead of the eeprom/densocan branch): `sub_ecu_denso_sh7055_densocan`, `_sh7058_densocan`, `_sh7058s_densocan`, `_sh7058s_diesel_densocan`, `_sh7059_diesel_densocan`; cfg yes/yes/yes uniformly. Distinct proprietary "DensoCAN" bootloader framing, not ISO-15765 — cfg's `flash_transport` is `CAN` alone for this family vs `iso15765,CAN` for the ISO-TP families. |
| FlashEcuSubaruDensoSH705xKline | ECU | K-Line, raw CAN | varies by protocol variant — see notes | no | — | unqualified | — | Serves `sub_ecu_denso_sh7055_04` / `_04_ecutek` (yes/yes/yes) and `sub_ecu_denso_sh7058` / `_ecutek` (alias `sti05*`; yes/yes/yes), plus the Cobb-tuned variants `sub_ecu_denso_sh7055_04_cobb` and `sub_ecu_denso_sh7058_cobb`, which cfg declares read=no, test_write=yes, write=no — i.e. this same class is cfg-gated to test-write only for the Cobb variant. |
| FlashEcuSubaruDensoSH72531Can | ECU | ISO-15765 | read, write | no | — | unqualified | — | Protocol `sub_ecu_denso_sh72531_can`; cfg `test_write=no`. |
| FlashEcuSubaruDensoSH72543CanDiesel | ECU | ISO-15765 | read, write | no | — | unqualified | — | Protocol `sub_ecu_denso_sh72543_can_diesel`; cfg `test_write=no`. |
| FlashEcuSubaruHitachiM32rCan | ECU | ISO-15765 | read, write | no | — | unqualified | — | Protocol `sub_ecu_hitachi_m32r_can`; cfg `test_write=no`. |
| FlashEcuSubaruHitachiM32rKline | ECU | K-Line | read, write | no | — | unqualified | — | Protocols `sub_ecu_hitachi_m32r_kline` and `sub_ecu_hitachi_m32r_kline_recovery` both dispatch here by prefix; cfg read/write=yes/yes, test_write=no for both. |
| FlashEcuSubaruHitachiSH7058Can | ECU | ISO-15765 | read, write | no | — | unqualified | — | Protocol `sub_ecu_hitachi_sh7058_can`; cfg `test_write=no`. |
| FlashEcuSubaruHitachiSH72543rCan | ECU | ISO-15765 | read, write | no | — | unqualified | — | Protocols `sub_ecu_hitachi_sh72543r_can` and `_can_recovery`; cfg `test_write=no` for both. |
| FlashEcuSubaruMitsuM32rKline | ECU | K-Line | read, write | no | — | unqualified | — | Protocol `sub_ecu_mitsu_m32r_kline` — a Subaru-badged, Mitsubishi-sourced M32R ECU family, distinct from the standalone Mitsubishi Colt `mitsu_ecu_m32r_can`/`FlashEcuMitsuM32rCan` family above; cfg `test_write=no`. |
| FlashEcuSubaruUnisiaJecs | ECU | K-Line | read | no | — | unqualified | — | Protocols `sub_ecu_unisia_jecs_m3779x` and `_m3775x`; cfg `<mode>` is `SSM1` here (every other row in this table is `OBD2`). cfg read=yes, test_write=no, write=no — read-only family. |
| FlashEcuSubaruUnisiaJecsM32r | ECU | K-Line | varies by protocol variant — see notes | no | — | unqualified | — | Protocols `sub_ecu_unisia_jecs_20` and `_30` (cfg read/write=yes/yes) plus `_40` and `_70` (cfg read=yes, write=no) all dispatch to this one class; `test_write=no` throughout. |
| FlashTcuCvtSubaruHitachiM32rCan | TCU | ISO-15765 | read, write | no | — | unqualified | — | Protocol `sub_tcu_cvt_hitachi_m32r_can`; cfg `test_write=no`. |
| FlashTcuCvtSubaruMitsuMH8104Can | TCU | ISO-15765 | read, write | no | — | unqualified | — | Protocol `sub_tcu_cvt_mitsu_mh8104_can`; cfg `test_write=no`. |
| FlashTcuCvtSubaruMitsuMH8111Can | TCU | ISO-15765 | read, write | no | — | unqualified | — | Protocol `sub_tcu_cvt_mitsu_mh8111_can`; cfg `test_write=no`. |
| FlashTcuSubaruDensoSH705xCan | TCU | ISO-15765 | varies by protocol variant — see notes | no | — | unqualified | — | Protocols `sub_tcu_denso_sh7055_can` (cfg read=yes, write=no) and `sub_tcu_denso_sh7058_can` (cfg read=yes, write=yes) both dispatch to this one class; `test_write=no` for both. |
| FlashTcuSubaruHitachiM32rCan | TCU | ISO-15765 | read, write | no | — | unqualified | — | Protocol `sub_tcu_hitachi_m32r_can`; cfg `test_write=no`. |
| FlashTcuSubaruHitachiM32rKline | TCU | K-Line | read | no | — | unqualified | — | Protocol `sub_tcu_hitachi_m32r_kline`; cfg read=yes, test_write=no, write=no — read-only. |
| FlashEcuSubaruHitachiM32rJtag | JTAG | JTAG | read, test_write, write (inferred — see notes) | no | — | unqualified | — | **No corresponding `<protocol>` entry exists in `resources/shared/config/protocols.cfg`** — a case-insensitive search for "jtag" across the whole file returns zero matches. Yet `src/ui/desktop/mainwindow.cpp` (~line 1341) dispatches on `flash_protocol_selected_protocol_name.startsWith("sub_ecu_hitachi_m32r_jtag")` and instantiates `FlashEcuSubaruHitachiM32rJtag`, which wraps `FlashEcuSubaruHitachiM32rJtagOperation` (`src/platform/desktop/common/flash/legacy/jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.{h,cpp}`). This branch appears currently unreachable through the protocol dropdown since nothing in cfg can produce that protocol name; flagging explicitly per task instructions rather than omitting the row or guessing a cfg mapping. `operations` and `transport` here are inferred from the class's own members (`read_mem()`, `write_mem(bool test_write)`, a `test_write` flag, and JTAG IR/DR shift helpers) since there is no cfg entry to read capability flags from. |
| FlashEcuSubaruDensoMC68HC16Y5_02_BDM | BDM | BDM | read, write | no | — | unqualified | — | Protocol `sub_ecu_denso_mc68hc16y5_02_bdm`; cfg read=yes, test_write=no, write=yes. [README](../README.md)'s "Unbrick with FastECU" section lists this BDM path as "ROM implemented, TPU also coming" — a capability claim without a date, adapter identity, or operator, so it is not counted as `hardware_evidence` per this matrix's bar. |
| FlashEcuSubaruUnisiaJecsM32rBootMode | bootmode | K-Line | read, write | no | — | unqualified | — | Protocols `sub_ecu_unisia_jecs_20_bootmode` and `_30_bootmode`; cfg read=yes, test_write=no, write=yes for both. |
<!-- Append one row per remaining family found in resources/shared/config/protocols.cfg
     and src/platform/desktop/common/flash/legacy/{bdm,bootmode,ecu,jtag,tcu}/,
     each with portable=no, hardware_status=unqualified unless a specific
     historical report justifies otherwise. Do not invent family IDs --
     use the exact protocols.cfg <name> or the exact operation class name. -->

### Enumeration notes

- `family_id` is the operation class name with its platform-specific suffix
  stripped (`...Operation` for the legacy `FlashOperationWorker` subclasses
  under `src/platform/desktop/common/flash/legacy/`, `...Executor` for the
  portable `src/backend/flash/eeprom/` classes) — the same convention the
  proving-pair rows already use. Several `protocols.cfg` `<protocol>` entries
  map to one class (e.g. five `*_densocan` protocol names all resolve to
  `FlashEcuSubaruDensoSH705xDensoCan` via `mainwindow.cpp`'s `startsWith`
  prefix dispatch); those are listed in the row's `notes`, not as separate
  rows, since the class — not the cfg alias — is the migratable unit.
- `resources/shared/config/protocols.cfg` has no top-level `<ECU>`/`<TCU>`/
  `<EEPROM>`/`<JTAG>`/`<BDM>`/`<bootmode>` XML sections; it is one flat
  `<protocols>` list of `<protocol name="...">` entries (line 3 to line
  1064, followed by an unrelated `<car_models>` section that only
  *references* protocol names). This matrix's `scope` grouping is derived
  from each protocol's `<mode>`/`<ecu>`/`<flash_transport>` fields and the
  legacy directory (`bdm`/`bootmode`/`ecu`/`jtag`/`tcu`) that implements it,
  not from a cfg section tag.
- One cfg protocol, `mitsu_ecu_m32r_kline_mut_dma` (line 771), is excluded
  from this matrix entirely: it declares `read=no`, `test_write=no`,
  `write=no` and is not reachable through the flash dispatch table in
  `mainwindow.cpp` at all — it selects the MUT/DMA *logging* protocol
  (`src/backend/logging/protocols/portable_mut_dma_logging_protocol.*`), not
  a flash operation family, despite living in the same `<protocols>` list.
- Transport-column vocabulary mapping from cfg's free-text
  `<flash_transport>` values: cfg `CAN` (the proprietary DensoCAN bootloader
  framing) → `raw CAN`; cfg `iso15765,CAN` → `ISO-15765`; cfg `K-Line,CAN`
  (protocols selectable over either wire) → `K-Line, raw CAN`; cfg `K-Line`
  and `BDM` map through unchanged.
- Automated evidence for every row in this appended set is `—`: none of
  `src/platform/desktop/common/flash/legacy/{bdm,bootmode,ecu,jtag,tcu}/`
  has a `cc_test` target in its `BUILD.bazel` (verified by grep), unlike the
  portable EEPROM executors' proving-pair rows.

## Hardware qualification checklist

For any family being moved from `experimental`/`unqualified` toward `proven`:

- [ ] Adapter setup confirmed (correct cable, correct COM/USB port, correct protocol selected in the UI)
- [ ] Battery voltage/power stability confirmed stable for the full operation duration
- [ ] Read performed; SHA-256/hash of the read compared against a known-good reference where one exists
- [ ] Test-write performed (if the family supports it) and verified via a subsequent read/hash comparison
- [ ] Write performed (if the family supports it; `n/a` if not — never silently treated as passed)
- [ ] Post-write read/hash comparison performed
- [ ] Ignition cycle (power-cycle) performed between attempts per the family's documented sequence
- [ ] Recovery path exercised or confirmed available (e.g. boot-mode recovery, backup ROM restore)
- [ ] Logs captured and attached to the hardware evidence reference
- [ ] Operator and date recorded in the `hardware_evidence` column
