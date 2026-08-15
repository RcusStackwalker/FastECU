# Step 5 Tail — Wave 3 Hitachi/Mitsu M32R CAN Cluster

## Scope and status

Four families move from legacy Qt flash operations to portable plans and
executors: `FlashEcuSubaruHitachiM32rCan`, `FlashTcuCvtSubaruHitachiM32rCan`,
`FlashTcuCvtSubaruMitsuMH8111Can`, `FlashTcuCvtSubaruMitsuMH8104Can`. This is
wave 3 of the
[step-5 tail design](2026-08-08-step5-tail-flash-drain-design.md)'s eight-wave
sequence — the first four-family cluster, and the first wave to cross the
ECU/TCU boundary within one cluster. Depends on wave 0's Colt CAN family
(portable ISO-15765/UDS precedent) and PR #191's UDS layer
(`UdsClient`/`IUdsChannel`/`CanFlashUdsChannel`), plus PR #197's per-family
`flash_types.h` split — both already merged as wave-3 groundwork. Hardware
status remains **experimental** for all four; this wave claims automated
equivalence, not bench qualification.

cfg protocol identifiers, all `mode=OBD2`, `flash_transport=iso15765`,
`test_write=no`:

| Protocol | Read/Write | Variant |
|---|---|---|
| `sub_ecu_hitachi_m32r_can` | yes/yes | `M32R_512KB_1block` |
| `sub_tcu_cvt_hitachi_m32r_can` | yes/yes | `M32R_512KB` |
| `sub_tcu_cvt_mitsu_mh8111_can` | yes/yes | `MH8111` |
| `sub_tcu_cvt_mitsu_mh8104_can` | yes/yes | `MH8104` |

## Portable contract

All four run over `configureIso15765Can` with the raw envelope
`00 00 07 <arbid>` + SID. Two CAN-ID pairs: `FlashEcuSubaruHitachiM32rCan`
uses request `0x7E0` / reply `0x7E8`; all three TCU families share request
`0x7E1` / reply `0x7E9`. Exchanges combine standard KWP2000/UDS SIDs
(`0x27` SecurityAccess, `0x34`/`0x35` RequestDownload/RequestUpload, `0x37`
RequestTransferExit, `0x31` RoutineControl for erase (`0x02 0x01`) and
checksum verify (`0x02 0x02 0x01`)) with proprietary SIDs: `0xB6` (write
block) / `0xB7` (read block) in place of standard TransferData `0x36`, plus
`0xAA`/`0xEA` (ECU ID) and `0xA8` (session-scope probe, ECU-only). Seed/key
and payload crypto reuse the already-portable
`SsmProtocol::calculateSeedKey`/`calculatePayload` and the same 32-byte
`indextransformation` table used by Colt and wave-1 Hitachi K-Line — only
each family's own key-index table is new; no new crypto plumbing is
introduced.

All four are kernel-free (`family_requires_kernel_v<...>` = false): each
jumps to the ECU's resident on-board kernel rather than uploading one, the
same shape as Colt CAN and wave 1/2.

Per-family geometry:

- **`FlashEcuSubaruHitachiM32rCan`** — 256-byte read and write chunks, full
  `0x00000`–`0x80000` image, `M32R_512KB_1block`. The bench path only (see
  below); `test_write` rejected before I/O.
- **`FlashTcuCvtSubaruHitachiM32rCan`** — 256-byte write / 128-byte
  `reflash_block` read page, `0x100000`-biased addressing floor-clamped to
  `0x8000`. Ports the real (currently-unreachable) `connect_bootloader` /
  `read_mem` / `write_mem` logic (see below), not the literal buggy entry
  point. Reads pad the unread low `0x8000` region with `0x00`.
- **`FlashTcuCvtSubaruMitsuMH8111Can`** — 256-byte write chunk, hardcoded
  read window `0x8000`–`0x80000` (`0x8000 + 0x78000`). Reads pad the low
  region with `0xFF`.
- **`FlashTcuCvtSubaruMitsuMH8104Can`** — 128-byte write chunk (not 256,
  despite the strong similarity to MH8111), same hardcoded read window as
  MH8111, `0xFF` padding. Response-check strictness differs from MH8111 (see
  legacy quirks below).

## Deliberate divergence: `FlashTcuCvtSubaruHitachiM32rCan` dead code

`FlashTcuCvtSubaruHitachiM32rCanOperation::execute()` calls `hack_words()` —
a brute-force seed-key search reading a hardcoded `default.bin` path — not
`connect_bootloader()`. `hack_words()` unconditionally returns
`STATUS_ERROR`. The family has never worked: it always fails immediately.
`connect_bootloader`/`read_mem`/`write_mem` exist in the same class with real
protocol logic but are unreachable from `execute()`.

This wave ports the real, unreachable logic — mirroring the wave-0/1
precedent for `FlashEcuSubaruHitachiM32rJtag`, whose class is unreachable
from UI dispatch entirely and is "ported as-is without wiring a new dispatch
path." Here the fix is internal rather than a dispatch gap, so it is called
out explicitly rather than folded in as a preserved quirk: **this is the
wave's one deliberate behavior change**, giving the family a working portable
path for the first time. The matrix `notes` column records it plainly, per
the umbrella's rule that deliberate divergences are named, never silent — the
same disclosure used for 5c's EEPROM write gap and the
`_ecutek_racerom_alt` read fix.

## Out of scope: on-car branch

`FlashEcuSubaruHitachiM32rCan` has an on-car programming branch that adds CAN
IDs `0xE0`/`0xDF`/`0xE1`/`0xB0` beyond the primary `0x7E0`/`0x7E8` pair.
`CanFlashUdsChannel` binds exactly one request/reply pair per instance, and
this wave introduces no new port surface to support more. Plan construction
rejects on-car mode with `Unsupported` rather than silently mishandling it —
the same move as 5c's EEPROM write gap, and the same category of decision
that pushed `bootmode`'s novel port needs to its own wave-7 spec in the
original tail design. Only the bench path is ported.

## UDS-layer reuse

`UdsClient`/`IUdsChannel`/`CanFlashUdsChannel` are protocol-agnostic byte
trampolines: the channel adds/strips the 4-byte arbitration-id envelope, the
client absorbs NRC `0x78` (responsePending) via `ExchangePolicy`, and the PDU
starts at SID. The proprietary SIDs (`0xB6`/`0xB7`, `0xAA`/`0xEA`, `0xA8`)
pass through `UdsClient::request()` unchanged — Colt CAN already sends
non-`buildRequest`-shaped exchanges this way. **No new port surface is
needed for this wave**, unlike wave 7's `bootmode`.

## Legacy quirks preserved, not fixed

- **MH8104's response checks are commented out.** `connect_bootloader` and
  `reflash_block` for `sub_tcu_cvt_mitsu_mh8104_can` log malformed/negative
  responses but proceed regardless, where MH8111's equivalent functions
  return an error. This tolerance is preserved as-is, matching wave-1's
  tolerant-response-parsing precedent for Hitachi K-Line.
- **Read padding differs by family**, `0x00` (TCU Hitachi CAN) vs `0xFF`
  (MH8111/MH8104) for the unread low region — a real per-family divergence,
  not a copy-paste artifact, preserved per-family rather than normalized.
- **Block-modified mask.** MH8111 and MH8104 share an identical
  block-skip mask (blocks 0–2 and 11–15 skipped) — preserved verbatim in both
  ported executors even though it is common between them, ahead of any
  factoring decision.

## Cluster factoring decision

Not decided in this spec — decided in the factoring PR, against ported and
tested code, per the tail design's port-then-factor ordering. The measured
hypothesis going in: MH8111 and MH8104's `connect_bootloader`/`reflash_block`/
erase bodies are near line-for-line identical — same SIDs, same seed/key
tables, same block-modified mask — differing only in write-chunk size (256B
vs 128B) and response-check strictness, matching the tail design's 0.79–0.86
measured similarity for this pair. `FlashEcuSubaruHitachiM32rCan` and
`FlashTcuCvtSubaruHitachiM32rCan` are comparatively outliers: different SIDs
around jump-to-kernel, different chunk/addressing shape, and (for the TCU
family) logic transcribed from a previously-unreachable path with no sibling
in this cluster to compare against. The factoring PR is allowed to produce a
common core for MH8111/MH8104 alone, or none at all, per the tail design's
explicit allowance that "a factoring PR is allowed to produce no common at
all."

## FamilyPlan and types-header integration

Following PR #197's shape: `flash_types.h` remains assembly-only. `FlashFamily`
gains four values; `FamilyPlan` gains four variant alternatives, each with a
`family_requires_kernel_v<...>` specialization (all `false`, per the
kernel-free note above). Each family gets its own leaf
`<family>_plan.{h,cpp}`, `<family>_executor.{h,cpp}`, and `<family>_types.h`
under `src/backend/flash/ecu/` — matching wave 1/2's per-family layout, with
no shared header until the factoring PR (if any) lands.

## Testing

Each executor test covers the full `ErrorKind` taxonomy: success path,
timeout, disconnect, negative/malformed response, cancellation mid-transfer,
unsupported operation. Plan tests assert rejection before any I/O: invalid
protocol/mode, on-car mode for `FlashEcuSubaruHitachiM32rCan`, `test_write`
requests for all four, and image/region size mismatch. Every exchange in
each executor carries a comment citing the legacy file and line it was
transcribed from, including — for `FlashTcuCvtSubaruHitachiM32rCan` — a note
that the source is the unreachable `connect_bootloader`/`read_mem`/`write_mem`
path, not `execute()`'s `hack_words()` entry point. Tests use
`ScriptedCanFlashTransport`, whose `expectWrite()` gives byte-exact wire
assertions, consistent with every prior wave.

## Sequence

Four family PRs, one per family, each following the established per-family
anatomy (`<family>_plan.{h,cpp}`, `<family>_executor.{h,cpp}`, `<family>_types.h`,
scripted tests, UI dialog rewrite, legacy operation deletion, matrix row
flip, `//:legacy_flash_drain` ratchet shrink), then one cluster-factoring PR
per the decision above. `PORTABLE_ROOTS` in `scripts/check-portable-closure.py`
gains each new backend flash target as its PR lands.

Gates per PR, unchanged from prior waves:

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure //:legacy_flash_drain
```

Plus ≥80% new-code coverage and the SonarCloud Quality Gate.

## Matrix updates

Each family PR flips its row in the
[flash qualification matrix](../../flash-qualification-matrix.md):
`portable` `no` to `yes`, `automated_evidence` gains the new test labels and
"Wave 3", `hardware_status` stays `unqualified` to `experimental` — never
`proven` from unit tests. `FlashEcuSubaruHitachiM32rCan`'s notes column
records the on-car `Unsupported` scope decision.
`FlashTcuCvtSubaruHitachiM32rCan`'s notes column records the dead-code fix
plainly, per the umbrella's "deliberate divergences are named in the matrix
notes column, never silent" rule.

## Risks

| Risk | Mitigation |
|---|---|
| `FlashTcuCvtSubaruHitachiM32rCan`'s real logic was never exercised by any code path, so it may itself contain latent bugs | Same fidelity discipline as every other family: byte-exact `expectWrite` scripts against the legacy source, `experimental` never upgraded by unit tests, dead-code origin flagged explicitly in the matrix |
| MH8111/MH8104 near-identity does not survive porting | Port-then-factor ordering; factoring PR may produce no common core |
| On-car scope decision proves too narrow later | Revisit as its own spec if a user needs on-car programming — same precedent as deferring `bootmode` |
