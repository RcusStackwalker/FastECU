# Step 5 Tail Wave 6b-2 — Denso SH705x K-Line — Design

**Status:** design approved in brainstorming; written spec awaiting review.
**Parent:** [wave 6 singletons](2026-09-19-step5-tail-wave6-singletons-design.md).
**Predecessors:** 6b transport foundation (#351) and 6b-1 Unisia Jecs (#352), both merged.
**Source baseline:** `1026933e`.

## Intent and success criteria

Migrate `FlashEcuSubaruDensoSH705xKlineOperation` (1,736 lines; read,
test_write and write) to a portable plan and `IKlineFlashExecutor`, routed
through the existing desktop `FlashWorkflow`. Land the parent spec's port item
2, `IKlineFlashTransport::reset_connection()`, in a foundation PR ahead of the
family. Reduce `//:legacy_flash_drain` from five entries to four. Hardware
status stays `experimental`.

Done means:

- `ecu/flash_ecu_subaru_denso_sh705x_kline_operation.cpp` is out of
  `REMAINING`; the legacy operation and its dialog
  (`src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh705x_kline.{h,cpp}`) are
  deleted, and the two `MainWindow` dispatch branches are gone.
- All six protocol names below reach the common `FlashDialog` through a
  `FlashWorkflow` registration.
- The matrix row reads `portable=yes`, `hardware_status=experimental`, and a
  bench checklist exists.

## Decisions

Three decisions were taken with the user during brainstorming; they override
the parent spec where they differ from it.

1. **Behavior-correction policy: the 6a-3/6a-4 exception, not the parent's
   blanket preservation rule.** Wire sequence and bytes are preserved.
   Bounds, read-integrity and reply-gating defects are corrected so that a
   malformed reply or failure stops all later commands, and every correction is
   named in the matrix notes. The parent spec's
   [behavior-correction rule](2026-09-19-step5-tail-wave6-singletons-design.md#behavior-correction-rule)
   and its "Preserved Legacy Defects" appendix do not govern this family.
2. **Shared substrate: identical data only.** The seed-key and kernel-payload
   encryption tables, which are byte-identical to the already-ported
   `DensoSh705xEepromKlineExecutor`, move into one shared header both executors
   use. Protocol functions stay per-family. This amends the parent's "no
   cluster factoring" scope in the narrow way wave 4 did for its crypto tables.
3. **Delivery: two PRs, foundation first,** mirroring #351 → #352.

## Findings that shaped the design

### The reset fits an existing seam

Legacy calls `serial->reset_connection()` before any setter and before
`open_serial_port()` (legacy :67). That is the interval
`IKlineFlashExecutor::before_transport_configure()` exists for, and
`SubaruDensoSh7058CanExecutor::before_transport_configure()` already uses the
CAN port's `reset_connection()` exactly this way. The port addition is therefore
the K-Line mirror of `ICanFlashTransport::reset_connection()`, not a new
lifecycle concept; ADR 0015's caller-owns-lifetime rule is unchanged.

### The EEPROM sibling shares data, not protocol

Function-by-function comparison against the deleted legacy
`EepromEcuSubaruDensoSH705xKlineOperation` (log lines excluded):

| Function | Similarity | Nature of the difference |
|---|---|---|
| `generate_seed_key`, `generate_ecutek_seed_key`, `encrypt_payload` | 0.28-0.43 textually | **identical table data**, reformatted only |
| `connect_bootloader` | 0.64 | same wire sequence; this family checks replies more strictly (`67 01`, `67 02`, length) |
| `request_kernel_id` | 0.88 | same |
| `upload_kernel` | 0.46 | **different wire**: in-payload `0x5AA5` balance and one upload, vs. a separate 4-byte checksum-bypass block and a 10-try alive poll |
| `send_sid_bf_ssm_init` | 0.67 | different read timeout |

Only the tables are provably identical. `decrypt_payload` is never called by
this family and is not ported.

### TestWrite is a kernel-side dry run

Legacy TestWrite sends `SUB_KERNEL_FLASH_DISABLE` (0x21) during init
(legacy :977-1030) and `VALIDATE_FLASH_BUFFER` (0x23) instead of
`COMMIT_FLASH_BUFFER` (0x24) at each 0x1000 boundary (:1297-1307). It still
sends `BLANK_PAGE` and `WRITE_FLASH_BUFFER`, relying on the kernel to honour the
disable. Unlike 6a-4, whose legacy TestWrite erased live, this family's
TestWrite is kept.

### The matrix transport column is wrong

The row reads "K-Line, raw CAN" because cfg declares
`<flash_transport>K-Line,CAN</flash_transport>`. This class only speaks K-Line
(`set_is_can_connection(false)`, legacy :70); SH7058 CAN is a separate,
already-migrated family.

## Scope

**In:** the port addition; the shared-table header and the EEPROM executor's
adoption of it; the family plan, executor, workflow registration and tests;
legacy and dialog deletion; ratchet, matrix, bench checklist, parent-spec note.

**Out:** hardware qualification; the other four legacy families; any change to
EEPROM wire behavior; `MainWindow` work beyond deleting the two branches.

## PR 6b-2a — foundation (no behavior change)

**`IKlineFlashTransport::reset_connection()`** — pure virtual, carrying the CAN
sibling's contract comment that every transport exposes a real reset so
protocol-owned sequences cannot silently degrade into configure/open only.

- `DesktopKlineFlashTransport` forwards to `SerialPortActions::reset_connection()`.
  Legacy returns `true` unconditionally, so the adapter fails only when it holds
  no port object.
- `ScriptedKlineFlashTransport` records the call in its event log and gains
  `expectReset()`, so tests pin its position relative to `configure()`.
- The EEPROM K-Line executor does not reset today and is not changed to.

**`src/backend/flash/ecu/denso_sh705x_kline_common.h`** — header-only, in the
shape of `denso_iso15765_can_common.h`:

- `kDensoSh705xKlineSeedKeyTable` (16 × u16, `0x53DA…0x88F0`) and
  `kDensoSh705xKlineEncryptTable` (`0x7856, 0xCE22, 0xF513, 0x6E86`);
- `denso_sh705x_kline_stock_seed_key()`, `denso_sh705x_kline_ecutek_seed_key()`,
  `denso_sh705x_kline_encrypt_payload()` — thin wrappers over
  `SsmProtocol::calculateSeedKey` / `calculatePayload` with the stock or ECUTEK
  index transformation.

`DensoSh705xEepromKlineExecutor` replaces its three local functions with these
calls. Its test suite is unchanged and is the guard; its transcribed table
literals stay in the test so the suite does not read the helper back.

**Guards:** the new target is named in `PORTABLE_PACKAGES`. `REMAINING`, the
allowlist and `//:portable_closure` are otherwise unchanged.

## PR 6b-2b — the family

### Plan

Exactly six protocol/MCU pairs route to one executor:

| Protocol | MCU | Seed key | Operations |
|---|---|---|---|
| `sub_ecu_denso_sh7055_04` | SH7055 | stock | Read, TestWrite, Write |
| `sub_ecu_denso_sh7055_04_ecutek` | SH7055 | ECUTEK | Read, TestWrite, Write |
| `sub_ecu_denso_sh7055_04_cobb` | SH7055 | stock | TestWrite only |
| `sub_ecu_denso_sh7058` | SH7058 | stock | Read, TestWrite, Write |
| `sub_ecu_denso_sh7058_ecutek` | SH7058 | ECUTEK | Read, TestWrite, Write |
| `sub_ecu_denso_sh7058_cobb` | SH7058 | stock | TestWrite only |

The legacy `startsWith("sub_ecu_denso_sh7055_04")` prefix match reaches exactly
the first three today; the plan lists them by name, so a future prefix
lookalike is rejected rather than captured.

Validation, all before I/O:

- cross-paired protocol/MCU, unknown names and unknown MCUs are
  `InvalidConfig`;
- **Cobb Read and Write are `Unsupported`.** Legacy relied on the UI honouring
  cfg `read=no`/`write=no`; the plan now enforces it for every caller;
- a missing or empty kernel is `InvalidConfig`;
- for Write and TestWrite, an image whose size is not the device `romsize` is
  `InvalidConfig` (legacy indexed `FullRomData` unchecked);
- every flash block length must be a multiple of 0x1000, and the device's first
  block must start at 0. Both SH7055 and SH7058 tables already satisfy this; the
  check guards the legacy `remain -= blocksize` loop and `src[address]`
  indexing against a future table.

Seed-key variant: legacy selects ECUTEK when the flash method
`endsWith("_ecutek")` (legacy :269). The plan computes this once into a
`SeedKeyVariant` enum; the executor never sees a protocol string.

`SubaruDensoSh705xKlinePlan` carries `initial_baud` 4800, `tester_id` 0xF0,
`target_id` 0x10 (legacy :73-76), the seed variant, the kernel bytes and load
address (cfg `kernel_addr`, `0xFFFF6004`), the device block table from
`find_flash_device()`, and the image for writes. `transport_setup()` returns
`non_iso14230_kline_config_from(plan)`. The probe (62500) and kernel-upload
(15625) bauds are executor constants with citations.

### Executor

`SubaruDensoSh705xKlineExecutor` implements `IKlineFlashExecutor`.
`before_transport_configure()` checks cancellation, calls
`reset_connection()`, and checks cancellation again. It never configures, opens
or closes the transport. Every exchange carries a legacy line citation.

1. **Kernel probe** (:127-157). `setBaud(62500)`, 100 ms, `request_kernel_id`.
   A valid `BEEF … 0x41` reply means the kernel is already running: skip 2 and
   3. Any other reply falls through, as in legacy.
2. **Bootloader handshake** (:161-330). `setBaud(4800)`, 100 ms, then SID `BF`
   (ECU ID → `FlashExecutionResult::rom_id`, Read only, as legacy), `81`, `83`,
   `27 01`, `27 02` with the plan's seed variant, `10`. Every reply is
   length-checked before any byte is indexed.
3. **Kernel upload** (:354-500). `setBaud(15625)`. The payload transform is a
   pure, separately tested function: append `00 00`, pad to a multiple of 4,
   drop the last two bytes, append the big-endian `0x5AA5 − Σ(u32 words)`
   balance, encrypt. Then `34` → `74`, `36` → `76`, `31 01 01` → `71`, 100 ms,
   `setBaud(62500)` — whose result legacy ignored and the port checks — and a
   kernel ID that must reply `0x41`.
4. **Read** (:503-640). `READ_AREA` over the device ROM in 0x400 pages with a
   24-bit address. **Correction:** each reply must be exactly `5 + 0x400 + 1`
   bytes with a valid `sum8` before its page is accepted; legacy appended
   whatever arrived. The result is exactly `romsize` bytes.
5. **Write and TestWrite** (:641-1366).
   - Pre-compare: `SUB_KERNEL_CRC` per block against `crc32` of the image,
     keeping each trailing short-timeout flush read. Nothing differs → success
     with the legacy "no flashing needed" line.
   - Init: `GET_MAX_MSG_SIZE` and `GET_MAX_BLK_SIZE` are sent and logged; legacy
     then hard-codes block 0x1000 and chunk 0x200, which is preserved. Then
     `FLASH_DISABLE` (TestWrite) or `FLASH_ENABLE` (Write). **Correction:** its
     ack must match exactly before any erase is sent.
   - Per changed block: `PROG_VOLT` (voltage logged), `BLANK_PAGE`,
     `WRITE_FLASH_BUFFER` in 0x200 chunks, and at each 0x1000 boundary
     `VALIDATE` (TestWrite) or `COMMIT` (Write) with the image CRC32. Every ack
     is gated; failure or cancellation stops all later commands.
   - Post-compare: the same CRC pass. **Correction:** a Write that still shows
     differing blocks returns `BadResponse`; legacy logged "ERROR IN FLASH
     PROCESS" and returned success. TestWrite returns success with the legacy
     PASS line, as nothing was committed.
   - **Correction:** image bytes are indexed by `address − 0`, justified by the
     plan's first-block-at-zero check, instead of legacy's
     `&data[fblocks->start]` offset combined with absolute `src[i]` indexing.

**Framing.** Kernel requests are `beef_request(op, payload)` from
`denso_beef_can_common.h` plus a K-Line `sum8` trailer. SSM requests use
`SsmProtocol::addHeader`. Executor tests transcribe wire bytes independently of
both helpers, as the BEEF common's header requires.

**Transport use.** All writes use `write()` (echo-checked), matching legacy's
`write_serial_data_echo_check`; `*_raw` is never used. Cancellation is checked
before every write and every `clock.sleep`. `Timeout`, `Disconnected` and
`BadResponse` propagate unchanged. No `ErrorKind` is added.

**Events.** Legacy log strings are preserved verbatim through `IEventSink`.
Progress uses `flash_phase_progress.h`; the legacy B/s and seconds-left
arithmetic is display-only and not ported.

### Workflow and UI

`SubaruDensoSh705xKlineWorkflow` in `flash_workflow.cpp` resolves the kernel
with `resolveKernel()` and takes `request_.image` for writes. If it is
identical to `KernelBackedCanFlashWorkflow` apart from executor type, the
implementation plan generalizes that template instead of adding a class. The
factory registers the six names. The two `MainWindow` branches, the old dialog
and the legacy operation are deleted.

## Testing

Package-owned and co-located.

- **Plan:** six pairs and their seed variants; Cobb Read/Write `Unsupported`;
  cross-pairs, prefix lookalikes, unknown MCU, missing kernel, wrong image size
  and bad block geometry rejected before I/O; `transport_setup()` field by
  field against legacy :67-76.
- **Executor** (`ScriptedKlineFlashTransport`, byte-exact `expectWrite`):
  reset before configure; kernel-alive shortcut; handshake per seed variant;
  upload transform against a golden vector; full read, plus short and
  bad-checksum pages rejected; Write with zero, one and several changed blocks;
  TestWrite emits `FLASH_DISABLE` and `VALIDATE` and never `FLASH_ENABLE` or
  `COMMIT`; a bad `FLASH_DISABLE` ack sends no `BLANK_PAGE`; post-compare
  mismatch fails Write and passes TestWrite; cancellation at every checkpoint;
  `Timeout`, `Disconnected`, `BadResponse`.
- **Mutation checks:** each correction above and the TestWrite opcode selection
  are mutation-checked — flip the production branch or value, observe the named
  test fail, restore a byte-identical tree.
- **Adapter (6b-2a):** `reset_connection()` reaches
  `SerialPortActions::reset_connection()`.
- **Workflow:** plan construction for one stock, one ECUTEK and one Cobb
  protocol, including Cobb Read rejection.

Gates per PR: `bazel test --config=release //...`, `bazel build
--config=release //:fastecu //:portable_closure`, `prek run --all-files`,
`bazel run //:clang_tidy_report_changed`, ≥80% new-code coverage and the
SonarCloud Quality Gate.

## Documentation

- **Qualification matrix:** `portable=yes`, `experimental`; transport corrected
  to K-Line only; notes list the Cobb plan gate, the `FLASH_DISABLE` ack gate,
  page integrity on read, post-write verify failure, image indexing, and the
  progress-arithmetic change.
- **Bench checklist** `docs/denso-sh705x-kline-bench-checklist.md`, with a STOP
  header and separate SH7055 and SH7058 records. First item: confirm from a
  trace and a post-run read that `BLANK_PAGE` under `FLASH_DISABLE` erases
  nothing. Then kernel-alive re-entry, full read against a trusted image, an
  ECUTEK seed, and a one-block Write with recovery.
- **Parent spec:** a "Wave 6b-2 implementation note" linking here and recording
  decisions 1 and 2.

## Risks

| Risk | Mitigation |
|---|---|
| TestWrite safety rests on the kernel honouring `FLASH_DISABLE` for `BLANK_PAGE` | Legacy bytes preserved; the ack gate added; first bench-checklist item; row stays `experimental` |
| Shared-table extraction silently changes EEPROM behavior | EEPROM suite unchanged and keeps its own transcribed literals |
| A correction masks a real legacy wire dependency | Corrections change only whether later commands are sent, never the bytes of those that are; each is named in the matrix |
| Workflow duplication with the CAN kernel-backed template | The implementation plan decides between a new class and generalizing the template |
