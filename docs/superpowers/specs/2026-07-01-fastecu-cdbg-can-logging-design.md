# Port Cdbg live-data logging from livemonitor into FastECU

## Goal

`externals/livemonitor` implements a raw-CAN "Cdbg" debug/logging protocol (`cdbgengine.cpp`, `logitemsmodel.cpp`) that lets the ECU stream live RAM values over CAN after a session/security handshake, with per-item RAM pointer + size configuration. This spec ports that capability into FastECU as a live-dashboard logging protocol, alongside the existing SSM and MUT/DMA logging paths.

## Target vehicle / ROM

`47110032` — Mitsubishi Colt CZT, chassis Z37A, MCU `M32176F3`, 5MT, EDM, 2004-2006. This is the same ECU that already has working CAN reflash in FastECU (`modules/ecu/flash_ecu_mitsu_m32r_can.cpp`, `protocol/mitsu_colt_can_protocol.cpp`), confirmed via `docs/superpowers/specs/2026-06-30-fastecu-colt-can-reflash-design.md`. Cdbg is confirmed (by the user, not independently verified on bench yet) to be a stock/built-in factory debug-logging feature already present in the stock ROM — no code injection or mmc-patches work is needed. This is purely a FastECU client-side feature.

## Protocol being ported (from `cdbgengine.cpp` + `logitemsmodel.cpp`)

Raw CAN, not ISO-TP: single 8-byte frames on CAN ID `0x630` (request) / `0x631` (reply), 500kbps, no flow control.

1. **Session/security handshake** (`SessionInitCommandSequence` in `cdbgengine.cpp`):
   - Command `{1, 1, ...}` — init.
   - Command `{18, 0, 2, ...}` ("log access") — seed request; reply carries a 4-byte big-endian seed at `rxbuf+4`.
   - `seed_to_key()` — a self-contained integer transform (bit rotation, byte permutation keyed on parity, two 16-bit multiply-accumulates) — ported byte-for-byte as pure math, no ROM-specific addresses involved.
   - Command `{19, 0, <key bytes>}` — key response. Reply's security-flags byte (`rxbuf[3]`) nonzero = access granted.

2. **Log-frame configuration** (`getLogFrameInitCommands`, `getLogInstanceResetCommand`):
   - Command `{20, 0, instance, 0, 0, 0, 0x06, 0x31}` — reset log instance.
   - Per item (≤7 items, ≤7 bytes total per frame — byte 0 of each streamed frame is reserved as a frame-index disambiguator): command `{21, 0, instance, frameIdx, itemIdx, ...}` selects the item slot, then `{22, 0, size, 0, <pointer, 4 bytes big-endian>}` sets its RAM pointer + size (1/2/4 bytes).

3. **Start logging** (`getLogStartCommand`): `{6, 0, 1, instance, frameCount, intervalUnit, <interval, 2 bytes big-endian>}`. Interval in ms, or in 10ms units (with a flag byte) if it exceeds 65535.

4. **Streamed decode** (`LogItemsModel::updateLogItems` in `logitemsmodel.cpp` — this is the part that makes livemonitor's implementation actually complete, not just command-building): the ECU pushes frames on CAN ID `0x631` at the configured interval, indistinguishable at the CAN-ID level from command replies. Frames are demultiplexed by `data[0]` (the frame index), then each configured item is decoded from its byte offset/bit-length within that frame, with unsigned/signed, big/little-endian, and multiplier/adder scaling all matching the schema definition. This decode path is generic — it does not need Cdbg-specific logic beyond the initial demux by frame index.

## What is explicitly not ported

- livemonitor's SID auto-discovery fallback (`sessionInitCallback`'s `m_cdbg->setSid(m_cdbg->sid() + 1)` retry loop scanning CAN IDs `0x630`-`0x780`). The target ECU and CAN ID are already known (`47110032`, `0x630`/`0x631`), so this discovery hack is dead weight.
- livemonitor's `schema.xml`/`czt_schema.xml` RAM pointer values, as real defaults. These were reverse-engineered against an uncertain/possibly-different ROM revision. This project's convention (established during the CAN reflash port) is to re-derive RAM addresses from `rom-analyzer` against the exact target ROM, never copy from a sibling reverse-engineering effort. Real channel addresses are a bench-driven follow-up once the wire protocol itself is confirmed working.
- CAN broadcast (`sid=`) log items (the non-Cdbg half of `logitemsmodel.cpp`'s generic decode, used for passive dash-CAN signals like `czt_schema.xml`'s ASC/yaw-rate messages). Out of scope for this port — Cdbg (RAM pointer) items only.

## New files / changes (FastECU repo)

- **`protocol/mitsu_colt_can_cdbg_protocol.{h,cpp}`** (new) — `CdbgLogDriver` class, modeled on `protocol/mut_dma_freeform.h`'s `MutDmaDriver` shape:
  - `CdbgChannel { uint32_t pointer; uint8_t size; }` (mirrors `CdbgLogItemDescription`).
  - `bool startFreeFormLog(QVector<CdbgChannel> channels, unsigned instance = 0, unsigned intervalMs = 10)` — runs session init, seed/key handshake, frame-config commands (batching channels into ≤7-byte frames the same way `LogItemsModel::loadSchema` does), and the start command.
  - `bool isStreaming() const`.
  - `QVector<quint32> pollOnce(int timeoutMs)` — reads pending raw CAN frames via the transport, demuxes by frame-index byte, returns one decoded raw value per configured channel (scaling/expression conversion happens at the `MainWindow::cdbg_poll()` layer, matching how `mitsubishi_dma_poll()` separates driver-level raw values from display-level conversion).
  - Uses FastECU's raw-CAN transport (`serial->set_is_can_connection(true)`, 500kbps, arbitration ID `0x630`/`0x631`), following the exact pattern already proven in `modules/ecu/flash_ecu_subaru_denso_sh705x_densocan.cpp` — no new CAN transport plumbing needed.

- **`log_operations_mitsubishi_cdbg.cpp`** (new, `MainWindow::` methods, structurally identical to `log_operations_mitsubishi.cpp`):
  - `cdbg_start_logging()` — builds the channel list from `logValues` filtered on `log_value_protocol == "CDBG"` and `log_value_enabled == "1"` (same filter shape as `mut_channels_from_logvalues()`), constructs the CAN transport + `CdbgLogDriver`, calls `startFreeFormLog()`. On failure, `emit LOG_E(...)` and clean up (mirrors `mitsubishi_dma_start_logging`'s failure path).
  - `cdbg_poll()` — connected to the existing shared `logparams_poll_timer` (10ms). Calls `pollOnce()`, and for each returned value applies the same RomRaider-style expression conversion (`log_value_units` split into from-byte expr + format) that `mitsubishi_dma_poll()` already uses, writing into `logValues->log_value`.
  - `cdbg_stop_logging()` — disconnects the timer, tears down driver/transport.

- **`menu_actions.cpp`** (`toggle_realtime()`): add `else if (configValues->flash_protocol_selected_log_protocol == "CDBG")` branches (start and stop) alongside the existing `"MUT_DMA"` branches.

- **`mainwindow.cpp`** destructor: add the same `"CDBG"` branch to the existing `"MUT_DMA"` cleanup check.

- **`config/protocols.cfg`**: change the existing `mitsu_ecu_m32r_can` protocol block's `<log_protocol>` from `SSM` → `CDBG` (currently a placeholder left over from when that block only covered flash read/write, per the CAN reflash spec).

- **`config/logger_cdbg_example.xml`** (new) — mirrors `config/logger_mut_dma_example.xml`'s structure and doc-comment convention: `<protocol id="CDBG">` with one or two placeholder parameters, addresses clearly marked fake/needing bench + `rom-analyzer` confirmation against `47110032` before real use.

## Safety / risk

Read-only in effect (no flash write path involved), but the seed/key security-access step is the same class of ECU interaction as the reflash work, so treat handshake failures conservatively (don't retry aggressively; log and stop). The first bench session is also the first real-world confirmation of the "stock/built-in feature" assumption — if the ECU doesn't respond to the session-init handshake at all, that assumption needs revisiting before any further work here.

## Testing

No CI coverage is realistic — needs real J2534 hardware and a bench `47110032` ECU, same situation as the K-Line MUT/DMA work and the CAN reflash port. Bench verification sequence:

1. Raw CAN connect + session-init handshake succeeds (confirms the ECU responds to Cdbg at all).
2. Seed/key security access granted (nonzero security-flags byte in the reply).
3. Configure one placeholder channel, start logging, confirm frames arrive on `0x631` at the requested interval and decode to plausible (even if meaningless-placeholder) values.
4. Only then invest in deriving real channel addresses via `rom-analyzer` against `47110032` for a first useful default channel set — a follow-up, not part of this spec.
