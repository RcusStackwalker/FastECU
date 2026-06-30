# Port Colt CAN reflash from livemonitor into FastECU

## Goal

`externals/livemonitor` is an old Qt4-era, Linux-only tool that already implements a working CAN-based reflash protocol for a Mitsubishi Colt M32R ECU (read, write, security access, the works). It is not actively maintained and isn't cross-platform. `externals/FastECU` is the actively maintained, cross-platform (including macOS) tool this project is already building on for K-Line MUT/DMA logging. This spec ports livemonitor's CAN reflash capability into FastECU so FastECU becomes the one tool used for reading and writing this Colt ECU, over CAN, superseding livemonitor for that purpose.

## Target vehicle / ROM

The protocol's RAM-resident helper routines (see below) are baked into `livemonitor/colt_flasher.xml` at fixed addresses, which are only valid for whichever ROM revision they were built against. Cross-referencing those addresses against `mmc-definitions/roms/m32r/47110032/rom.toml` confirms the target:

- ROM `47110032` — Mitsubishi Colt CZT, chassis **Z37A**, MCU `M32176F3`, 5MT, EDM, 2004-2006.
- `rom.toml`'s `flash_kb = 384` (`= 0x60000`) matches `colt_flasher.xml`'s `<userspace start="8000" end="60000">` and `<crc start="0" end="60000">` exactly.
- This is a **different chassis platform from Z27AG** (the project's primary ROM) despite the similar "Colt" naming — `rom.toml` says so explicitly. Don't assume Z27AG-specific addresses, security keys, or RAM layout apply.

This port is scoped to `47110032` only. The erase/write-page blobs are not regenerated or verified for any other registered ROM (Z27AG, Z27A, Evo X, etc.) as part of this work.

## Key architectural finding: no CAN framing code needs to be ported

`livemonitor`'s `CanEngine`/`ObdEngine` (`canengine.cpp`, `obdengine.cpp`) hand-roll the entire ISO-TP (ISO15765) framing state machine in C++: single/first/consecutive-frame detection, flow-control frames, multi-packet send and receive (`ObdEngine::slotOnNewCanMessage`, `ObdEngine::multipacketTranmission`). This exists because livemonitor talks to the J2534 adapter in raw `CAN` sniff mode (`caninterface.cpp`).

FastECU does not need any of this. `serial_port_actions_direct.cpp` configures the J2534 channel with `protocol = ISO15765` directly (confirmed around `serial_port_actions_direct.cpp:1322`), so the J2534 driver itself handles ISO-TP segmentation, reassembly, and flow control. Existing FastECU CAN modules (e.g. `modules/ecu/flash_ecu_subaru_hitachi_m32r_can.cpp`) just call `serial->write_serial_data_echo_check(QByteArray)` / `serial->read_serial_data(timeout)` with the full payload, however long, and get a fully reassembled reply back.

**Consequence**: the port is upper-layer protocol logic only — the equivalent of `obdengine.cpp`'s service-ID sequencing and `obdsessionwidget.cpp`'s read/write orchestration, re-expressed as blocking calls against FastECU's existing transport. No new CAN/ISO-TP transport code is needed.

## Protocol being ported (from `obdengine.cpp` + `obdsessionwidget.cpp`)

1. **Session/security handshake**:
   - `ServiceDiagnosticSession` (0x10), bootload session id `0x85`.
   - `ServiceSecurityAccess` (0x27) seed request (subfunction 5, "for reflash"), then seed-key answer (subfunction 6).
   - Seed-key algorithm is the Mitsubishi-specific linear one in `ObdSessionInitCommandSequence::messageCallbackCustomAction`: split the 4-byte seed into two 16-bit halves `pk1`, `pk2`; compute `sk = pk * 135 + 1542` for each half; send both back big-endian. This is a different, much simpler algorithm than the table-based seed-key routines already in `flash_ecu_subaru_mitsu_m32r_kline.cpp` / `flash_ecu_subaru_hitachi_m32r_can.cpp` — do not reuse those tables, they're for unrelated ECUs.

2. **RAM-resident helper routines**, carried over verbatim as embedded byte arrays (not recompiled) from `colt_flasher.xml`:
   - `erasepage` blob → uploaded to RAM address `0x805568`.
   - `writepage` blob → uploaded to RAM address `0x8054ac`.
   - Both uploaded via the same `RequestDownload`(0x34) + `TransferData`(0x36) mechanism used for the ROM payload itself (`ObdFlashWriteCommandSequence` in `obdengine.cpp`), then invoked by the ECU's own flash control flow once present in RAM.

3. **ROM write** (`ObdSessionWidget::on_writeFlashButton_clicked`, `ObdFlashWriteCommandSequence`):
   - Upload `erasepage` blob to `0x805568`, trigger it.
   - Upload `writepage` blob to `0x8054ac`.
   - `RequestDownload` + `TransferData` the patched ROM bytes for the userspace region (`0x8000`–`0x60000`), in 256-byte chunks.
   - `RequestDownload` + `TransferData` a 2-byte checksum (`get_crc`: plain 16-bit byte-sum over the transferred range, not a real CRC) to fixed address `0x200000`.
   - `ServiceRoutineControl`(0x31), routine `225`, to make the ECU check the checksum and commit the write — this is what livemonitor calls the "reflash trigger" (`ServiceRoutineControl` subfunction `224` for the erase-page step, `225` for the final check/commit).

4. **ROM read** (`requestReadFlashBlock`): `ServiceReadMemoryByAddress`(0x23), 192-byte (`FLASH_BLOCK_SIZE`) blocks, sequential. No security session required for read.

5. **Error handling**: every reply's first byte checked against `request_sid + 0x40` (positive response); `0x7F` = negative response, byte 2 of the reply is the NRC. Match this against FastECU's existing `FileActions::parse_nrc_message` convention used by sibling modules.

## What is explicitly not ported

- The manual ISO-TP/CAN framing state machine — superseded by FastECU's J2534 `ISO15765` protocol mode (see above).
- A working `test_write` dry-run. Checked every M32R protocol currently in FastECU (`sub_ecu_hitachi_m32r_can`, `sub_ecu_hitachi_m32r_kline`, `sub_ecu_mitsu_m32r_kline`): all have `<test_write>no</test_write>`, and where the parameter is even threaded through (`reflash_block(..., bool test_write)` in the Hitachi CAN module) it's unused — dead code, not a real feature anywhere in this codebase. This port matches that convention: read + write only, no dry-run.
- Generalizing the erase/write-page blobs, addresses, or security keys to any other Colt-family ROM. Future work, not this spec.
- mmc-patches/manifest.toml SHA-256 verification or any ROM-pack workflow — FastECU is a flashing client, not the build/patch pipeline; it has no business validating against `manifest.toml`.

## New files / changes (FastECU repo)

- **`modules/ecu/flash_ecu_mitsu_m32r_can.{h,cpp}`** — new module, class `FlashEcuMitsuM32rCan : public QDialog`. Structurally modeled on `flash_ecu_subaru_hitachi_m32r_can.cpp` (CAN/ISO15765 setup via `serial->set_is_can_connection(false)` + `set_is_iso15765_connection(true)`, `connect_bootloader()`/`read_mem()`/`write_mem()`/`reflash_block()` shape, blocking `write_serial_data_echo_check`/`read_serial_data` calls), with Mitsubishi Colt protocol internals (seed-key formula, SID sequence, embedded blobs) from livemonitor as described above. The ISO15765 source/destination CAN addresses need confirming against `colt_flasher.xml`/livemonitor's `m_sid` setup (`ObdEngine` is constructed with an `id` — the request CAN ID; reply is `id + 8`) before first bench test.
- **`kernelmemorymodels.h`/`.cpp`** — new `flashdev_t` entry for this MCU: single block `0x8000`–`0x60000` (352KB writable), matching `rom.toml`'s `flash_kb = 384` and `colt_flasher.xml`'s `userspace` bounds directly — no guessing required.
- **`config/protocols.cfg`**:
  - New `<protocol name="mitsu_ecu_m32r_can">` block: `<read>yes</read>`, `<test_write>no</test_write>`, `<write>yes</write>`, `<flash_transport>iso15765</flash_transport>`, `<mcu>` pointing at the new flashdev entry.
  - New `<car_model>` entry: `<make>Mitsubishi</make><model>Colt CZT</model><version>Z37A 5MT</version><type>4G15T</type>`, `<protocol>mitsu_ecu_m32r_can</protocol>`. This is the first non-`Subaru` `<make>` in this fork's config — confirmed the vehicle-select UI (`vehicle_select.cpp`/`protocol_select.cpp`) has no hardcoded Subaru filtering, so this is just data.
- **`mainwindow.cpp`** — one new `else if (configValues->flash_protocol_selected_protocol_name.startsWith("mitsu_ecu_m32r_can"))` branch instantiating `FlashEcuMitsuM32rCan`, alongside the existing dispatch chain.

## Safety / risk

Flash-write to a real ECU is inherently destructive if something is wrong (no working dry-run exists anywhere in this codebase to de-risk it, per above). This inherits the project's existing bench-qualification convention — same gate already applied to the K-Line MUT/DMA work (`m32r/39670016_z27a_mt_audm/mode23-bench-notes.md`): verify on a bench/spare ECU (read ECU ID, read full ROM, compare against known-good) before ever attempting a write, and verify writes on a bench/spare ECU before any real-vehicle use.

**Known landmine in the source material**: the erase-trigger step in `obdsessionwidget.cpp:173-191` (`writePageUploadedCallback`) sends a 12-byte `ServiceRequestReflash`(0x3b) command — `{0x3b, 154, 1, 1, 'R','c','u','s','0','0', 0, 1}` — followed by `ServiceRoutineControl`(0x31) subfunction `224`. The original livemonitor author left the comment `//caused bootloader lockup` directly on that 12-byte payload, meaning it locked up the bootloader during their own testing. This is ported verbatim (it's the only known-attempted implementation) but must be treated as the single highest-risk step: loudly flagged in code comments, called out explicitly in the bench-test checklist as a step to attempt only with a recovery path available (see `boot-talk`, this project's bricked-ECU recovery utility, per the repo map in `CLAUDE.md`), and gated behind an explicit confirmation in the UI before it's ever sent.

## Testing

No CI coverage is realistic here — it needs real J2534 hardware and a bench ECU, same situation as the existing MUT/DMA work. Verification is bench-only:

1. Connect, read ECU ID via the session handshake, confirm it matches `47110032`.
2. Full ROM read over CAN, compare against a known-good reference dump of the same ROM.
3. Write-path bench test on a spare/bench Z37A ECU (not a running vehicle) — erase-page blob upload + trigger, write-page blob upload, small-scale write, checksum verify, full write, then read back and diff.
