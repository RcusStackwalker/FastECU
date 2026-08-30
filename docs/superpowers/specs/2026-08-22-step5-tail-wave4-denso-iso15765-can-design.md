# Step 5 Tail — Wave 4 Denso ISO-15765 Cluster

## Scope and status

Four families move from legacy Qt flash operations to portable plans and
executors: `FlashEcuSubaruDenso1N83M_1_5MCan`, `FlashEcuSubaruDenso1N83M_4MCan`,
`FlashEcuSubaruDensoSH72531Can`, `FlashEcuSubaruDensoSH72543CanDiesel`. This is
wave 4 of the
[step-5 tail design](2026-08-08-step5-tail-flash-drain-design.md)'s eight-wave
sequence, following wave 3's Hitachi/Mitsu M32R CAN cluster. It takes
`//:legacy_flash_drain` from 18 remaining families to 14. Hardware status
remains **experimental** for all four; this wave claims automated equivalence,
not bench qualification.

The four legacy sources are 6,178 lines as of `577ce33`. The tail design's
5,971 was exact at its own measurement commit `04fce81`; the difference is
intervening reformatting (the 120-column change in PR #214 and the warnings
cleanup in PR #217), not a change in scope.

cfg protocol identifiers, all `mode=OBD2`, `test_write=no`, `checksum=yes`:

| Protocol | MCU | `flash_transport` | Read window | Write blocks |
|---|---|---|---|---|
| `sub_ecu_denso_1n83m_1_5m_can` | `N83M_1_5MB` | `iso15765` | `fblocks[1]` = `0x08FAC000` + `0x173F00` | block 1 |
| `sub_ecu_denso_1n83m_4m_can` | `N83M_4MB` | `iso15765` | `fblocks[1]` = `0x08FAC000` + `0x3D3F00` | block 1 |
| `sub_ecu_denso_sh72531_can` | `SH72531` | `iso15765` | `fblocks[1]` = `0x8000` + `0x137F00` | block 1 |
| `sub_ecu_denso_sh72543_can_diesel` | `SH72543d` | `iso15765,CAN` | `fblocks[0]` = `0x8000` + `0x1F7F00` | block 0 |

## Portable contract

All four run over `configureIso15765Can` at 500 kbit with the raw envelope
`00 00 07 E0` + SID, on the single CAN-ID pair request `0x7E0` / reply `0x7E8`.
Exchanges combine standard KWP2000/UDS SIDs — `0x10` DiagnosticSessionControl
(subfunctions `0x5F`, `0x03`, `0x43`, `0x62`, `0x63`), `0x27` SecurityAccess
(`0x61` seed, `0x62` key),
`0x22` ReadDataByIdentifier, `0x34` RequestDownload, `0x37`
RequestTransferExit, `0x31` RoutineControl, `0x09` vehicle information — with
the proprietary `0xB6` (write block) and `0xB7` (read block) in place of
standard TransferData `0x36`, plus `0xAA`/`0xEA` for ECU ID on three of the
four.

`connect_bootloader` selects between two programming paths at runtime on byte 7
of the `0x22 0x10 0x1D` reply — `0xFF` selects bench, anything else selects
in-car. Both are ported; see
[Both programming branches are ported](#both-programming-branches-are-ported).

All four are kernel-free (`family_requires_kernel_v<...>` = `false`): each
jumps to the ECU's resident on-board kernel via `0x10 0x62` rather than
uploading an image, the same shape as waves 0–3. `ecuCalDef->Kernel` is read
and logged but never transferred. Note that
`sub_ecu_denso_sh72543_can_diesel` is the one cfg entry in this cluster
declaring `<kernel>ssmk_can_tp_sh72543d_euro6.bin</kernel>` and
`<kernel_addr>0xFFF80000</kernel_addr>`; the legacy operation ignores both, and
this port preserves that. The cfg declaration is recorded in the matrix rather
than acted on.

Seed/key and payload crypto reuse the already-portable
`SsmProtocol::calculateSeedKey` / `SsmProtocol::calculatePayload`. Unlike every
prior wave, **all four families share byte-identical tables**: the same
16-entry seed index (`0x78B1, 0x4625, ...`), the same encrypt key
`{0xC85B, 0x32C0, 0xE282, 0x92A0}` with its reverse for decrypt, and the same
32-byte `indextransformation` already used by Colt CAN, wave 1, and wave 3. No
new crypto plumbing is introduced, and no per-family key table exists.

## UDS-layer reuse

`UdsClient` / `IUdsChannel` / `CanFlashUdsChannel` cover this cluster with no
new port surface, as in wave 3. `uds_client_exchange_common`'s `non_fatal_query`
was extracted for "legacy's non-fatal identity-query blocks (ECU/TCU ID, VIN,
CAL ID, CVN, ...)", which is exactly how all four wave-4 `connect_bootloader`
functions open; `fatal_request` and `fatal_query` cover the standard SID+0x40
exchanges. The proprietary `0xB6`/`0xB7`/`0xAA` SIDs pass through
`UdsClient::request()` unchanged, as Colt CAN and wave 3 already do.
`TransportKind::CanIso15765` covers all four; no new kind is needed.

## The substrate is smaller than whole-file similarity suggests

The tail design sequenced this cluster as the "highest clone ratio in the tree,
so the largest substrate payoff", from whole-file line-overlap of 0.81–0.94.
Re-measured at function granularity against `577ce33`, that estimate does not
survive.

Whole-file identity is even higher than the original measurement: normalizing
the class name away, 92–94% of every file's lines are common to all four, and
pairwise `difflib` sequence similarity is 0.87–0.95.

Function-level identity is the opposite:

| Function | Lines (`1_5M`) | 4-way identical | Min pairwise similarity |
|---|---|---|---|
| `connect_bootloader` | 623 | no | 0.928 |
| `read_memory` | 219 | no | 0.811 |
| `reflash_block` | 165 | no | 0.720 |
| `erase_memory` | 95 | no | 0.840 |
| `write_memory` | 72 | no | 0.952 |
| `execute` | 62 | no | 0.912 |
| `generate_can_seed_key` | 21 | **yes** | 1.000 |
| `decrypt_payload` | 15 | **yes** | 1.000 |
| `encrypt_payload` | 10 | **yes** | 1.000 |

**Not one protocol function is four-way identical.** The only identical
functions are the three crypto wrappers, which are already thin shims over
`SsmProtocol`. The divergences are interleaved through the wire sequences
rather than isolated at their ends, so high line identity does not translate
into factorable code.

The divergence axes, in full:

| Axis | `1n83m_1_5m` | `1n83m_4m` | `sh72531` | `sh72543d` |
|---|---|---|---|---|
| ECU-ID exchange | `0xAA` → `0xEA` | `0xAA` → `0xEA` | `0xAA` → `0xEA` | `0x22 F1 82` → `0x62 F1 82` |
| ID reply trim | `remove(0,8)`, keep 5 | same | same | `remove(0,7)`, keep all |
| `0x34` addressing | hardcoded bytes | hardcoded bytes | hardcoded bytes | computed from region |
| Timeouts | named constants | bare literals `200`/`500` | named constants | `serial_read_timeout` where siblings use `serial_read_short_timeout` |
| Response tolerance | strict | 4 commented-out returns (`read_memory`; 3 more in `connect_bootloader` match every sibling's own no-return behavior) | strict | 1 commented-out return |
| Leading pad | `0x10000` | `0x10000` | `0x8000` | `0x8000` |
| `block_modified` | `{0, 1, 0}` | `{0, 1, 0}` | `{0, 1, 0}` | `{1}` |
| `erase_memory` signature | no-arg | no-arg | no-arg | `(fdt, blockno)` |
| Reads after jump-to-kernel | 1 | 2 | 1 | 1 |

## Cluster factoring decision

Per the tail design's port-then-factor ordering, the decision is made in the
factoring PR against ported and tested code, not here. The measured hypothesis
going in, from the table above: the only four-way-identical artifact is the
crypto key-table set, so the factoring PR is expected to land
`denso_iso15765_can_common.h` holding those three tables and nothing else — or
to record "no common" and land only documentation. Both are sanctioned by the
tail design's explicit allowance that "a factoring PR is allowed to produce no
common at all." The expectation is stated here so a near-empty closing PR is
not read as a failed wave.

**Guardrail.** Factor only across a *data* difference — region, pad size, block
mask, key table. Never factor across a *control-flow or tolerance* difference.
Parameterizing `1n83m_4m`'s four tolerated checks or `sh72543d`'s erase
signature into a shared executor would produce exactly the configurable state
machine the [umbrella](2026-07-22-step5-backend-portable-design.md) and the
[protocol generalization notes](../../protocol-generalization-opportunities.md)
forbid.

### Outcome

The factoring pass ran against the four ported and tested executors. The
measured hypothesis held: **the crypto key-table set is the only four-way
byte-identical artifact that is pure data.** It is now
`src/backend/flash/ecu/denso_iso15765_can_common.h` — a header-only
`cc_library` holding four `inline constexpr` arrays (`kDensoIso15765SeedKeyTable`,
`kDensoIso15765EncryptTable`, `kDensoIso15765DecryptTable`,
`kDensoIso15765IndexTransformation`) and no code. All four executors include it;
`denso_iso15765_can_common_test.cpp` pins both the table bytes and the seed-key
and payload vectors they produce through `SsmProtocol`.

The four executors' own suites keep their independently transcribed copies of
these tables, so a wrong entry in the shared header still fails those suites
rather than passing silently. That is what makes the four suites a real guard
that the factoring changed no behavior, and they pass unchanged.

The index transformation is included per the factoring brief even though it is
not cluster-specific: the same 32-entry table appears verbatim in
`subaru_hitachi_m32r_can`, `subaru_tcu_cvt_hitachi_m32r_can`,
`subaru_tcu_cvt_mitsu_mh8104_can` and `subaru_tcu_cvt_mitsu_mh8111_can`, which
keep their own copies. It is an `SsmProtocol`-level constant wearing a
cluster-level name. Promoting it to `src/algorithms/protocol/ssm` and retiring
all eight copies is a separate package-wide change, out of this wave's scope;
the header says so at the definition. (Wave 3's
`subaru_tcu_cvt_mitsu_can_common.h` reached the opposite conclusion for the
same reason and left its copy per-file — the two decisions differ only in
whether the shared header is the right temporary home, not in the underlying
fact that the table is broader than either cluster.)

Everything else that reads as common was inspected and **not** factored:

| Candidate | Four-way identical? | Decision |
|---|---|---|
| Read timeouts `200`/`500`/`2000` and their three `ExchangePolicy` values | yes, as declarations | **Keep per-file.** The values coincide; the *sites they are applied at* do not — `sh72543d` reads with `serial_read_timeout` where its siblings read short, `1n83m_4m` spells them as bare literals. A shared policy set would invite a later "just use the shared one" at a site whose family reads differently. Data that happens to coincide, not data that is the same. |
| Session/SID/routine/format constants, in-car CAN ids | mostly | **Keep per-file.** `1n83m_4m` needs two extra reply ids, `sh72543d` needs a DID pair its siblings do not. Each file's constant block documents that family's own legacy line numbers; sharing it would strand those citations. |
| `Ctx`, `info`/`error`, `kRejectionPrefix`, `exchange_context`, `fatal_request`, `fatal_query`, `non_fatal_query` | yes | **Keep per-file.** The shared logic is already extracted into `uds_client_exchange_common.h`; what remains is a per-file currying adapter. The identical shape spans nine executors in this package, not four — this cluster is not the right place to re-cut it. |
| `setup_pdu` | yes | **Keep per-file.** Three lines wrapping `composeBe`; `sh72543d` computes its region where the others hardcode. Below the threshold where a shared target pays. |
| `tolerant_probe` | yes, as a shape | **Keep per-file.** Implements legacy's abort-on-absent/continue-past-a-wrong-reply probe pattern, common to all four families at their connect-side identity checks — not a divergence; the genuine tolerance difference is `tolerant_setup`'s four `read_memory` checks, below. `1n83m_4m` names it because the same shape repeats three times in its own `connect_bootloader`; `sh72543d`'s equivalent reads with a different timeout. Below the threshold where a shared cluster-wide target pays. |
| `tolerant_setup` (`1n83m_4m` only) | no | **Guardrail: tolerance difference.** Exists in one family precisely because four returns are commented out there and live in the others. |
| `jump_to_kernel` | no | **Guardrail: control-flow difference.** Three different signatures across the four (`discard_first_reply`, `duplicate_pre_loop_read`, `loop_timeout_ms`) encoding genuinely different read counts and timings. |
| `security_access`, `fire_and_forget`, `connect_in_car`, `connect_bench`, `connect_bootloader` | no | **Guardrail.** Per-family timeouts, a different fire-and-forget PDU in `sh72531`, per-exchange timeouts in `sh72543d`, differing log strings. |
| `read_memory`, `erase_memory`, `reflash_block`, `write_memory`, `execute` | no | **Guardrail.** `sh72543d`'s erase signature, `1n83m_4m`'s tolerated setup, differing image bases, block counts, pacing sleeps and checksum policies. |

No shared executor, base class, policy struct or template was created, and none
should be. Under-factoring here is safe; over-factoring is not.

### Two shared-shape issues resolved in the same pass

Both existed identically in all four executors, were raised by the per-task
reviews, and were deferred to the factoring pass because a one-executor fix
would have left the cluster inconsistent.

**1. The post-connect `TestWrite` re-validation guard is unreachable — kept.**
It cannot fire: `validate_<family>_plan` at the top of `execute()` rejects
`TestWrite` before any I/O, and the `Read` branch has already returned, so
`FlashOperation` has no third value left to reach the guard. It stays in all
four anyway. It costs three lines and no runtime work, no compiler or
clang-tidy check flags it, and it is the last thing standing between a
non-`Write` operation and a real erase-and-write of an ECU should the entry
validation ever be relaxed or the enum gain a fourth value — the exact hazard
the section above says this wave closes. Deleting a hardware guard because the
current call graph makes it redundant is the wrong trade in a flash path. Each
site now carries a comment saying it is unreachable, that this was reviewed,
and that it is not dead code to be swept.

**2. The close-block log line's hex payload — restored.** All four legacy
sources log `"Closed succesfully: " + toHex(received)` (`1n83m_1_5m` line 1285,
`sh72531` 1289, `1n83m_4m` 1299, `sh72543d` 1307); all four ports dropped the
`": <hex>"` suffix entirely. Since the wave's standing constraint is to
preserve legacy log strings, dropping operator-visible content is the
divergence, and restoring it is the fix. No test pins the string, so nothing
was asserted into a new shape; the change is log-only and touches no wire byte,
timeout or loop bound.

The restored hex is the envelope-stripped PDU (`77`) where legacy's was the raw
frame (`000007e877`). That residual gap is **accepted, not fixed**: rebuilding
the 4-byte CAN envelope would mean the executor synthesizing framing bytes the
portable layer deliberately does not own, and would need a new
`CanFlashUdsChannel` accessor to do it. The three families that also log
`"Stop request response: "` already carry exactly this envelope-stripped shape
and shipped with it through review, so the close line now matches its own
sibling line rather than being the odd one out. `1n83m_4m` has no stop-response
line to match because its legacy source logs only a blank continuation there —
that per-family difference is preserved.

## Deliberate divergence: `test_write` rejected before I/O

`test_write` is threaded from `execute()` through `write_memory(bool)` into
`reflash_block(..., bool test_write_arg)` in all four families and **never
consulted in any of the four bodies**. A `test_write` run therefore performs a
real erase and a real `0xB6` flash write, byte-identical to `write`. All four
cfg entries declare `test_write=no`, so the UI does not currently offer it, but
the code path is live if reached.

Plan construction rejects `test_write` with `Unsupported` before any I/O. This
is the wave's one deliberate behavior change — an earlier draft of this design
named a second, `sh72543d`'s write image base, but further verification found
that one was not a divergence at all; see
[below](#write-base-equivalence-no-sh72543d-divergence). It follows wave 3's
precedent — all four of its families reject `test_write` before I/O — and here
additionally closes a live-write hazard rather than merely declining an
unsupported mode. The matrix `notes` column records it for each family, per
the umbrella's rule that deliberate divergences are named, never silent.

## Both programming branches are ported

`connect_bootloader` runs a `0x22 0x10 0x1D` ReadDataByIdentifier query and
branches on byte 7 of the reply:

- **`== 0xFF` — bench.** `0x10 0x43` session, seed/key on `0x27 0x61`/`0x27
  0x62`, jump to kernel via `0x10 0x62`, then a bounded wait for `0x50 0x62`.
  Every exchange is on the primary `0x7E0`/`0x7E8` pair.
- **`!= 0xFF` — in-car.** A longer sequence that additionally addresses CAN ids
  `0x7A2`, `0x7DF`, `0x7E1`, and `0x7B0` before converging on the same
  `0x10 0x62` kernel jump and wait loop.

Both are ported. This differs from wave 3's on-car scope decision for
`FlashEcuSubaruHitachiM32rCan`, and the reason is that the two situations are
not alike: wave 3's on-car mode "is not exposed by any current `protocols.cfg`
entry", making it a paper boundary, whereas wave 4's branch is selected at
runtime from the ECU's own reply. For the 2014–2021 Forester, XV, Legacy, and
Outback targets these families serve, programming with the ECU still in the
vehicle is a normal use rather than an edge case, so rejecting it would remove
a working path from real users instead of declining an unreachable one.

**No new port surface is required.** `CanFlashUdsChannel` is a byte trampoline
that prepends a request id and validates a reply id over an
already-configured transport; `Iso15765Config` is applied once per session.
Addressing a second id pair is therefore an additional channel instance over
the same `ICanFlashTransport`, which is exactly what legacy does — one
`configureIso15765Can(serial, "500000", 0x7E0, 0x7E8)` in `execute()`, then raw
writes whose envelope carries a different id.

Two fidelity constraints govern the in-car transcription:

- **Fire-and-forget exchanges stay raw.** Most in-car exchanges write a request
  and read a reply that is never inspected. These cannot go through
  `UdsClient`, which enforces the SID+0x40 convention and decodes NRCs, so they
  go through the channel — or the transport — directly, in the manner
  `subaru_hitachi_m32r_can_executor.cpp` already uses for its OBK and
  session-scope probes.
- **Legacy does not check the reply id.** It reads whichever frame arrives
  next, regardless of arbitration id. A per-id `CanFlashUdsChannel` would
  impose a reply-id check legacy does not have, so the unchecked reads are
  taken from the transport directly rather than through a channel bound to the
  id just written.

## Write-base equivalence: no `sh72543d` divergence

The four families index the write image as follows:

| Family | Legacy indexing | `newdata` argument (`write_memory`'s call site) | Composed image base | Read image base |
|---|---|---|---|---|
| `1n83m_1_5m` | `newdata[i + blockaddr - fblocks[0].start]` | `&data_array[0]` (line 1141) | `0x08F9C000` | `0x08F9C000` |
| `1n83m_4m` | `newdata[i + blockaddr - fblocks[0].start]` | `&data_array[0]` (line 1154) | `0x08F9C000` | `0x08F9C000` |
| `sh72531` | `newdata[i + blockaddr]` | `&data_array[0]` | `0x0` | `0x0` |
| `sh72543d` | `newdata[i + blockctr * blocksize]` | `&data_array[fblocks[0].start]` (line 1129) | `0x0` | `0x0` |

The indexing formula alone is not the composed address `reflash_block` writes
to — `newdata` is a pointer `write_memory` computes at the call site, and the
formula is relative to wherever that pointer points. `sh72531`'s form is
equivalent to its siblings' because its `fblocks[0].start` is `0`, so the
distinction between an offset and an unoffset buffer does not show up there.
`sh72543d`'s form is *also* equivalent, once composed with the buffer it is
actually handed: `write_memory` calls
`reflash_block(&data_array[flashdevices[...].fblocks->start], ...)` (legacy
line 1129) — the buffer is already pre-offset by `fblocks[0].start` = `0x8000`
— and `reflash_block` then indexes `newdata[i + blockctr * blocksize]` (legacy
line 1217), which is block-relative to that pre-offset buffer. Composed:
`data_array[fblocks[0].start + i + blockctr * blocksize]` =
`data_array[blockaddr + i]` — the absolute flash address, image based at `0`,
matching `read_memory`'s own image base and the three sibling families.

**There is no write-base divergence.** An earlier draft of this section
derived that same composition correctly in the paragraph above and then
asserted the opposite conclusion — that legacy wrote a full ROM `0x8000` low
and that the port needed a deliberate `0x8000` correction to fix it. That
conclusion does not follow from the arithmetic; it was an error, caught during
implementation and verified twice against the legacy source.
`subaru_denso_sh72543_can_diesel_executor.cpp`'s `reflash_block` carries the
same derivation inline, with an explicit note not to reintroduce a `0x8000`
shift on the strength of the old text. Legacy was already correct here, and
the port is byte-identical to it — nothing about which bytes reach the ECU
changed.

The same check was run for `1n83m_4m`: `write_memory` hands `reflash_block`
the unoffset `&data_array[0]`, so its block-relative-minus-start formula
composes to `data_array[absolute address − 0x08F9C000]` — also byte-identical
to legacy. Neither family carries a deliberate behavior change on this axis,
and there is nothing here for a bench operator to verify beyond the wave's
ordinary fidelity discipline.

## Structural changes with byte-identical wire output

These are not behavior divergences. They are recorded so a reviewer comparing
the port against legacy is not surprised by the shape change.

- **`read_memory`'s discarded arguments become a plan field.** Legacy computes
  `start_addr` and `length` in `execute()`, passes them to `read_memory`, and
  then overwrites both with hardcoded constants. Those constants are not
  arbitrary: each equals the family's own main flash block exactly
  (`fblocks[1]` for three families, `fblocks[0]` for `sh72543d`, whose table is
  shifted because its `0x0` entry is commented out). The `// hack for testing`
  comment beside them describes the hardcoding, not a wrong window. The
  portable plan carries the window as a `MemoryRegion` and the executor uses
  it, producing the same bytes.
- **`0x34` addressing converges.** `sh72543d` already computes the
  RequestDownload address and length bytes from its region; the other three
  emit the identical bytes as literals. Routing all four through the region
  field converges them without changing a byte.

## Read-image layout is already correct

All four families synthesize the unread leading region as `0xFF` at offset 0
(`padBytes.fill(0xFF, ...)`, `insert(0, padBytes)`) and append a `0x100` `0xFF`
tail. `1n83m_4m` and `sh72531` write their tail as `insert(0x3E3F00, ...)` /
`insert(0x13FF00, ...)`, which are inserts at exactly the then-current size —
appends, not past-end inserts. Resulting images:

| Family | Read data | Leading pad | Tail | Image | `romsize` |
|---|---|---|---|---|---|
| `1n83m_1_5m` | `0x173F00` | `0x10000` | `0x100` | `0x184000` | `0x174000` |
| `1n83m_4m` | `0x3D3F00` | `0x10000` | `0x100` | `0x3E4000` | `0x3E4000` |
| `sh72531` | `0x137F00` | `0x8000` | `0x100` | `0x140000` | `0x140000` |
| `sh72543d` | `0x1F7F00` | `0x8000` | `0x100` | `0x200000` | `0x200000` |

Every image is offset-correct: byte 0 is address 0 in all four. No divergence
is needed on the read path, and no Qt `insert`-past-end fill semantics are
relied on.

Round-trip consistency with `reflash_block` holds for all four — see
[write-base equivalence](#write-base-equivalence-no-sh72543d-divergence)
above.

`N83M_1_5MB` is the one inconsistency: its `romsize` field (`0x174000`) does not
equal its own `fblocks` sum (`0x184000`), so this family's image is `0x10000`
larger than its declared `romsize`. Nothing consumes `romsize` on the read path
— `read_memory` discards the `length` argument derived from it — so this is
recorded as a flash-table observation and left untouched.

## Legacy quirks preserved, not fixed

- **`1n83m_4m`'s four commented-out `return STATUS_ERROR` statements in
  `read_memory`'s two RequestDownload/RequestUpload checks** (legacy lines
  876, 883, 917, 924). This family logs malformed and negative responses and
  proceeds where its three siblings abort at all four. Preserved exactly,
  matching wave 3's MH8104 tolerance precedent. `sh72543d` carries one such
  check; `1n83m_1_5m` and `sh72531` carry none. (Legacy also commented out
  three more `return STATUS_ERROR` statements in `connect_bootloader`, lines
  305/335/369, but those are not a divergence: every sibling already proceeds
  rather than aborts at the equivalent check, so the commented-out lines are
  this family's own record that the strictness there was never present, not
  evidence it was removed.)
- **`1n83m_4m`'s duplicate read after jump-to-kernel** — a second
  `read_serial_data` its siblings do not issue. Preserved.
- **Per-family timeout constants**, including `1n83m_4m`'s bare `200`/`500`
  literals where siblings use named constants, and `sh72543d`'s use of the
  2000 ms `serial_read_timeout` where siblings use the 200 ms
  `serial_read_short_timeout`. These are wire timing, not style; preserved
  per-family rather than normalized.
- **Per-family `block_modified` masks and erase signatures**, which are correct
  for each family's own flash table and are not copy-paste artifacts.

## FamilyPlan and types-header integration

Following the shape established by PR #197 and used by wave 3: `flash_types.h`
remains assembly-only. `FlashFamily` gains four values; `FamilyPlan` grows from
eleven to fifteen alternatives, each with a `family_requires_kernel_v<...>`
specialization set to `false` per the kernel-free note above. Each family gets
its own leaf `<family>_plan.{h,cpp}`, `<family>_executor.{h,cpp}`, and
`<family>_types.h` under `src/backend/flash/ecu/`, with no shared header until
the factoring PR (if any) lands.

## Sequence

Four family PRs in ascending divergence order, so the strict siblings establish
the shape before the tolerant outlier lands against it:

1. `sub_ecu_denso_1n83m_1_5m_can` — strict, hardcoded addressing; the cluster's
   reference port.
2. `sub_ecu_denso_sh72531_can` — strict; differs from (1) only in region and
   pad size.
3. `sub_ecu_denso_sh72543_can_diesel` — computed addressing, `0x22 F1 82`
   identity exchange, `(fdt, blockno)` erase signature.
4. `sub_ecu_denso_1n83m_4m_can` — the tolerant outlier, last.

Then one cluster-factoring PR per the decision above.

Each family PR follows the established per-family anatomy: the backend plan,
executor, and types headers; scripted executor and plan tests; the UI dialog in
`src/ui/desktop/flash/ecu/<family>.cpp` rewritten from legacy-operation
construction onto `FlashWorkflowFactory` registration plus the common
`FlashDialog`; deletion of the legacy `<family>_operation.{h,cpp}`; the matrix
row flip; and the `//:legacy_flash_drain` ratchet shrink.
`PORTABLE_ROOTS` in `scripts/check-portable-closure.py` gains each new backend
flash target as its PR lands, never in bulk, each extension proven non-vacuous
per the umbrella rule.

## Testing

Each executor test covers the full `ErrorKind` taxonomy: success path, timeout,
disconnect, negative or malformed response, cancellation mid-transfer, and
unsupported operation. Plan tests assert rejection before any I/O: invalid
protocol or mode, `test_write` for all four, and image/region size mismatch.
Every exchange in each executor carries a comment citing the legacy file and
line it was transcribed from. Tests use `ScriptedCanFlashTransport`, whose
`expectWrite()` gives byte-exact wire assertions, consistent with every prior
wave.

Two family-specific obligations, named because they are easy to get backwards:

- **`1n83m_4m`'s tolerance must be asserted positively at its four genuine
  divergence points.** Its tests feed a malformed or negative response to each
  of the four `read_memory` checks and assert the executor *proceeds*; the
  three strict siblings abort at the same four exchanges and need the
  mirror-image assertion there. The three `connect_bootloader` checks are also
  commented out and also worth a positive proceeds-assertion, but they are not
  a divergence — every sibling already proceeds at the equivalent exchange —
  so no mirror-image abort assertion is owed on those.
- **`test_write` rejection must be asserted before any transport call**, not
  merely as a returned error, since the legacy path would otherwise perform a
  real write.
- **Both programming branches need scripted coverage per family.** One test
  scripts the `0x22 0x10 0x1D` reply with byte 7 `== 0xFF` and asserts the
  bench sequence; another scripts it `!= 0xFF` and asserts the in-car sequence,
  including the writes addressed to `0x7A2`, `0x7DF`, `0x7E1`, and `0x7B0`.
  A branch with no test is a branch transcribed blind.

Gates per PR, unchanged from prior waves:

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure //:legacy_flash_drain
```

Plus ≥80% new-code coverage and the SonarCloud Quality Gate.

## Matrix updates

Each family PR flips its row in the
[flash qualification matrix](../../flash-qualification-matrix.md): `portable`
`no` to `yes`, `automated_evidence` gains the new test labels and "Wave 4",
`hardware_status` `unqualified` to `experimental` — never `proven` from unit
tests. The `notes` column records, per family:

- the `test_write` `Unsupported` rejection and the live-write hazard it closes
  (all four);
- that both the bench and in-car programming branches are ported, including the
  additional CAN ids the in-car path addresses (all four);
- `1n83m_4m`'s four genuine tolerated `read_memory` checks (plus three
  commented-out `connect_bootloader` checks that match every sibling's own
  behavior and are not a divergence) and its duplicate post-jump read;
- `sh72543d`'s write-base equivalence — an earlier draft of this design
  misread the composed indexing as a divergence; the matrix corrects that and
  still directs a bench operator's first check to be reading back a written
  ROM and confirming the bytes land where they came from, since the family
  has no bench access;
- `sh72543d`'s cfg `<kernel>` / `<kernel_addr>` declaration that the family
  does not use;
- `N83M_1_5MB`'s `romsize` inconsistency with its own `fblocks` sum.

## Risks

| Risk | Mitigation |
|---|---|
| Third-largest wave by volume (6,178 lines, behind waves 6 and 5) hand-transcribed with no bench access | Per-exchange legacy line citations; byte-exact `expectWrite` scripts; `experimental` never upgraded by unit tests |
| `1n83m_4m`'s tolerance is asserted backwards, silently converting a tolerant family into a strict one | Positive tolerance assertions required per the testing section; its three strict siblings carry the mirror assertion on the same four `read_memory` exchanges |
| Over-factoring a cluster that is 92–94% line-identical but has zero four-way-identical protocol functions | Port-then-factor ordering; the data-vs-control-flow guardrail above; expected near-empty factoring PR stated in advance |
| An earlier draft of this design misdiagnosed `sh72543d`'s write base as a divergence needing a deliberate `0x8000` correction | Caught during implementation and re-derived correctly — see [write-base equivalence](#write-base-equivalence-no-sh72543d-divergence); the port is byte-identical to legacy, verified twice against the legacy source, and no image-base correction was applied |
| The in-car branch is transcribed with no bench and no in-vehicle test rig, and most of its exchanges ignore their replies, so a wrong byte there is invisible until someone flashes a car | Per-exchange legacy line citations and byte-exact `expectWrite` scripts cover it exactly as they cover the bench path; both branches are separately scripted per family; `experimental` is never upgraded by unit tests |
| `test_write` rejection turns out to be reachable from some path and regresses a user workflow | All four cfg entries declare `test_write=no`; plan tests assert rejection before I/O, and the dialog rewrite is in the same PR |

## Amendments to the [step-5 tail design](2026-08-08-step5-tail-flash-drain-design.md)

1. **Wave 4's substrate payoff is small, not "the largest in the tree."** The
   original estimate came from whole-file line overlap (0.81–0.94).
   Function-level measurement shows zero of six protocol functions are
   four-way identical; only the three crypto wrapper functions are, and those
   already delegate to `SsmProtocol`. The porting cost stands as sequenced;
   the factoring payoff does not — this is not in tension with the
   [factoring pass's outcome](#outcome), which did land
   `denso_iso15765_can_common.h`: what it shares is the crypto key-table
   *data* those wrapper functions read, not a protocol function, and it is
   the only four-way byte-identical artifact in the cluster.
