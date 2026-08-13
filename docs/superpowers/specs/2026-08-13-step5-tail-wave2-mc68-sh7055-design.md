# Step 5 Tail — Wave 2 MC68HC16Y5_02 / SH7055_02

## Scope and status

`FlashEcuSubaruDensoMC68HC16Y5_02` and `FlashEcuSubaruDensoSH7055_02` move from
their Qt dialogs/operations to the portable flash seam. Both are K-Line
kernel-upload families and are the second measured clone cluster in the tail
design doc's sequencing table (0.78 whole-file overlap). Hardware status
remains **experimental**; this work claims automated equivalence, not bench
qualification.

Depends on wave 1 (merged, PRs #183/#184), which proved the K-Line
`IKlineFlashTransport` port and the per-family PR template. Wave 2 needs no
new port surface — both families are ordinary `IKlineFlashTransport`
singletons in the tail design's port-surface findings.

## Accepted protocol identifiers

**`FlashEcuSubaruDensoMC68HC16Y5_02`** serves four protocol names via
`mainwindow.cpp`'s prefix dispatch, all folded into plan fields rather than
new `FlashFamily` values, matching wave 0's precedent for
`MitsuColtM32rCan`:

- `sub_ecu_denso_mc68hc16y5_02` / `_02_ecutek` (alias `wrx02`; read/test_write/write = yes/yes/yes).
- `sub_ecu_denso_mc68hc16y5_04` / `_04_ecutek` (alias `wrx04`; cfg read =
  `n/a`/`no`, test_write = no, write = no — this board revision is not
  actually flashable through this path in `protocols.cfg`, even though the
  same class is wired up for it; preserved as-is, not fixed).
- `sub_ecu_denso_mc68hc16y5_02_tpu` is also prefix-caught here despite that
  protocol's own `<mode>` being `BDM`; **correction to an earlier draft of
  this spec: it is reachable**, not unreachable — `protocols.cfg` line 1144
  lists it in a car model's `<protocol>` set, so a real user selection can
  hit this exact quirk (a BDM-mode part flashed as a `wrx02`-shaped K-Line
  session). Preserved as-is, matching the existing matrix note; distinct
  from `FlashEcuSubaruHitachiM32rJtag`'s wave-1 unreachable case, which does
  not apply here.
- `sub_ecu_denso_mc68hc16y5_02_bdm` is a **different, already-dispatched
  legacy family** (`FlashEcuSubaruDensoMC68HC16Y5_02_BDM`, checked before the
  bare `_02` prefix in `mainwindow.cpp`'s current dispatch chain) and stays
  fully out of scope. Because `sub_ecu_denso_mc68hc16y5_02_bdm` textually
  starts with the bare `sub_ecu_denso_mc68hc16y5_02` string, the portable
  routing table must not register a plain `sub_ecu_denso_mc68hc16y5_02`
  prefix without also explicitly reserving `_02_bdm` ahead of it — a plain
  prefix registration would silently swallow BDM traffic into this family.
  The implementation plan spells out the exact routing-table fix.

**`FlashEcuSubaruDensoSH7055_02`** serves `sub_ecu_denso_sh7055_02` and
`_02_ecutek` (alias `fxt02`; read/test_write/write = yes/yes/yes for both).
`sub_ecu_denso_sh7055_04` and bare `sub_ecu_denso_sh7058` dispatch to the
already-tracked `FlashEcuSubaruDensoSH705xKline` (wave 6, `REMAINING`) and are
out of scope here.

**Matrix correction, not a code fix.** The flash-qualification-matrix lists
SH7055_02's transport as "K-Line, raw CAN," copied from `protocols.cfg`'s
`<flash_transport>K-Line,CAN</flash_transport>` tag on both protocol entries.
The class itself (`flash_ecu_subaru_denso_sh7055_02_operation.cpp`) always
calls `serial->set_is_can_connection(false)` and never performs CAN I/O — CAN
is aspirational in the cfg, not reachable through this code path. The ported
plan/executor are K-Line-only; the matrix row's `notes` column records the
cfg/code mismatch instead of silently adopting or silently dropping the CAN
claim.

## Portable contract

Both plans carry a `KernelImage` (id, load address, bytes) resolved through
`IFileRepository` at plan-build time — this is the first tail wave that is
**not** kernel-free, unlike wave 0/1's M32R families. `build_<family>_plan`
takes `(paths, protocol_name, cmd_type, file_repository)`, matching the
"backend never sees `EcuCalDefStructure`" rule 5c's EEPROM pair established;
the dialog performs the `EcuCalDefStructure` → plan-input translation, not the
backend.

Both share one wire skeleton — a `SUB_KERNEL_START_COMM` two-byte header,
16-bit length, one-byte opcode, payload, one-byte `checksum8` trailer — and
the same `SUB_KERNEL_*` opcode set (`READ_AREA`, `CRC`, `GET_MAX_MSG_SIZE`,
`GET_MAX_BLK_SIZE`, `FLASH_ENABLE`/`DISABLE`, `PROG_VOLT`, `BLANK_PAGE`,
`WRITE_FLASH_BUFFER`, `VALIDATE`/`COMMIT_FLASH_BUFFER`, `KERNEL_ID`). Both
implement the same phase sequence: connect/detect-alive-kernel → upload
kernel if not already running → read (paged 0x400 kernel reads) or write
(per-block CRC compare, blank-page erase, 0x200-chunked buffer writes,
0x1000-block commit/validate).

### MC68HC16Y5_02-specific

- `connect_bootloader` (`flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:100-180`):
  single WRX02 init request/response exchange at 9600 baud
  (`bootloader_init_request_wrx02`, one of three expected OK responses
  selected by `flash_method` suffix `_ecutek`/`_cobb`/stock), falling back to
  a kernel-ID probe over 62500 baud if the init response doesn't match.
- `upload_kernel` (`:182-318`): baud is 11700 for `_ecutek`, 9600 otherwise;
  payload XOR is `0x51`/`0x55` by the same suffix split, `+0x10` add, a
  2-byte magic word (`0x3940`/`0x3941`) patched into offset 2-3 of the
  encrypted payload, and a raw `SUB_UPLOAD_KERNEL`-opcode message (no SSM SID
  wrapper).
- Response-offset parsing in `init_flash_write` reads the length field at
  `received.at(5..8)` (`:756`, `:791`).

### SH7055_02-specific

- `connect_bootloader` (`flash_ecu_subaru_denso_sh7055_02_operation.cpp:97-231`):
  checks kernel-alive first via `request_kernel_id()`; for `cmd_type ==
  "read"` only, performs a read-only SSM SID 0xBF exchange
  (`send_sid_bf_ssm_init`, `:137-163`) to populate `ecuCalDef->RomId`; then
  shows the **mid-session** "cycle ignition" confirmation
  (`confirm(tr("ECU Operation"), "Connecting to bootloader, turn ign off /
  on and click OK to continue", ...)`, `:173`) before a 20-iteration WRX-init
  polling loop (`:201-224`) with `connect_bootloader_start_countdown`
  (`:1300-1315`, a 3-second countdown plus a "Switch Ignition ON!" log line)
  gating it.
- That confirmation maps to the existing `ConfirmationSpec::Id::CycleIgnition`
  — introduced for 5c's EEPROM K-Line family and reused here unmodified, not
  a new type. Per the umbrella's "obtain UI confirmation before irreversible
  I/O" rule, the plan surfaces `CycleIgnition` unconditionally and upfront;
  the legacy behavior only prompts when the kernel-alive check fails. This is
  the same divergence 5c already made once for the same confirmation kind,
  not a new precedent — recorded in the matrix `notes` column per the
  umbrella's "deliberate divergences are never silent" rule.
- `upload_kernel` (`:233-353`): fixed XOR `0x55`/`+0x10` (no ecutek baud or
  encryption branch), and wraps the payload in an SSM-shaped envelope with
  hardcoded SID bytes `0x31`/`0x61` at fixed offsets, checksummed twice
  (header checksum at output[7], then a second trailing checksum over the
  whole message) — structurally different from MC68's raw-opcode framing, not
  just different constants.
- Response-offset parsing in `init_flash_write` reads the length field at
  `received.at(6..9)` (`:791`, `:826`) — one byte higher than MC68's
  `:756`/`:791`. `check_romcrc` (`:645-750`) has its own additional
  length-prefixed unwrap step (`:704-717`) with no MC68 equivalent.
- `send_sid_27_request_seed`/`send_sid_27_send_seed_key`
  (`:1226-1264`) exist on the class but are never called from `execute()`,
  `connect_bootloader`, `upload_kernel`, `read_mem`, or `write_mem` — dead
  code, not ported.

### Shared, and explicitly not shared

- `checksum::checksum8`/`checksum::crc32` come from the portable checksum
  primitives already used across the drain; **do not** port either class's
  unused `CRC32` member (`0xEDB88320` in MC68, `0x5AA5A55A` in SH7055 —
  declared, never read in either `.cpp`). Dead members, not per-family
  constants.
- `read_mem`, `write_mem`, `get_changed_blocks`, `check_romcrc`,
  `init_flash_write`, `reflash_block`, `flash_block`, and `request_kernel_id`
  share the same opcode skeleton and page/block sizes (0x400 read page, 0x200
  write chunk, 0x1000 commit block) between the two families, but differ in
  the response-offset and framing details above. Each is ported and tested
  independently in this wave; no shared helper is written here.

## Cluster factoring decision

Deferred, not decided, in this wave — matching the tail design's
port-then-factor ordering. A follow-up factoring PR examines the two
independently-tested executors once both exist. Candidates worth checking
then: the `SUB_KERNEL_START_COMM` header-and-checksum framing helper (looks
identical in both) and the block-write/commit loop shape (same constants,
different response offsets). The offset differences documented above
(`init_flash_write`'s `.at(5..8)` vs `.at(6..9)`, `check_romcrc`'s extra
unwrap step) are exactly the kind of one-off a hand-comparison could miss;
factoring only after both sides are independently byte-tested is what
prevents that. The factoring PR may produce a narrow header/checksum helper,
a larger common, or nothing — the wave completes either way.

## Testing

Plan tests: invalid MCU rejection, `_04`/`_04_ecutek` write/test_write
rejection before I/O (cfg says `no`), kernel load-address/size bounds,
`FamilyPlan` variant round-trip. Executor tests via `ScriptedKlineFlashTransport`
covering the full `ErrorKind` taxonomy per family: success (read and write),
timeout, disconnect, malformed/negative response at each phase (bootloader
init, kernel upload, kernel-ID confirmation, per-block CRC, block write,
commit/validate), cancellation mid-transfer (including mid-kernel-upload and
mid-block-write), and unsupported operation. SH7055_02's tests additionally
cover: the `CycleIgnition` confirmation being surfaced (via `FlashPlan`'s
`ConfirmationSpec` list, not executor-side), the read-only SID 0xBF ECU-ID
path taken only for `cmd_type == "read"`, and the 20-iteration WRX-init retry
exhausting to `Error{Timeout}` or `Error{BadResponse}` as appropriate.
Dialog/workflow tests cover preflight rejection starting no worker,
confirmations answered before execution, and progress/log-line text preserved
verbatim, per the standing tail-wave template.

## Enforcement and wiring

- New `FlashFamily` values: `SubaruDensoMc68hc16y5_02`, `SubaruDensoSh7055_02`
  (appended to `flash_types.h`'s enum).
- New files: `src/backend/flash/subaru_denso_mc68hc16y5_02_{plan,executor}.{h,cpp}`,
  `src/backend/flash/subaru_denso_sh7055_02_{plan,executor}.{h,cpp}`, each
  registered in `PORTABLE_ROOTS` (`scripts/check-portable-closure.py`) as its
  PR lands.
- UI: rewrite `src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_mc68hc16y5_02.cpp`
  and the SH7055_02 equivalent from legacy-operation construction to
  `FlashWorker` + portable executor + transport adapter, per the EEPROM
  dialogs' template. Delete both `<family>_operation.{h,cpp}` pairs in the
  same PRs.
- `scripts/check-legacy-flash-drain.py`'s `REMAINING` shrinks by 2 (24 → 22),
  one entry per merged family PR, per the ratchet's existing shrink-only rule.
- Matrix: flip both rows' `portable` to `yes`, add wave-2 test labels to
  `automated_evidence`, keep `hardware_status=experimental`, add the
  cfg/code CAN-transport mismatch and the upfront-`CycleIgnition` divergence
  to the SH7055_02 row's `notes`.
- Gates, unchanged from wave 1:

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure //:legacy_flash_drain
```

Plus `>=80%` new-code coverage and the SonarCloud Quality Gate.

## Fidelity discipline

Unchanged from the tail design and wave 1: every exchange in a portable
executor carries a comment citing the legacy file and line it was
transcribed from. The legacy `.cpp` pair is the only source of truth for
correct bytes and is deleted in the same PR as its port. Documented quirks
(the `_04` no-op write path, the unreachable `_02_tpu` BDM-mode alias, the
dead `CRC32` members and dead SID-27 seed/key methods) are preserved or
dropped exactly as named above, not silently "corrected."
