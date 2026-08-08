# Step 5 Tail — Per-Family Flash Drain — Design

**Status:** Approved 2026-08-08. The `tail` row of the
[step-5 umbrella design](2026-07-22-step5-backend-portable-design.md), and the
last work in step 5. Depends on 5c (merged, PR #79) and 5e (merged, PR #161),
which unblocked it by moving the `serial_qt_compat` allowlist entry onto the
package that actually owns the debt.

**Goal:** migrate the 27 legacy Qt flash operation classes to portable plans and
executors, retire the `:qt_compat` shims that exist only to serve them, and
close step 5.

## Completion criterion

Exact and machine-checked:

- `scripts/check-legacy-flash-drain.py`'s `REMAINING` set is empty and
  `src/platform/desktop/common/flash/legacy/` is deleted.
- `//src/platform/desktop/common/flash/legacy:__pkg__` is out of
  `scripts/check-serial-compat-allowlist.py`'s `FROZEN` set, leaving only UI
  entries for step 6 and the adjudicated `remote_utility` carve-out.
- `//src/algorithms/protocol/ssm:qt_compat`,
  `//src/algorithms/protocol/colt:qt_compat`,
  `//src/algorithms/protocol/mut_dma:qt_compat`,
  `//src/algorithms/menu:qt_compat`, `//src/algorithms/crypto:qt_compat`, and
  `//src/algorithms/expression:qt_compat` are deleted.
- `//src/algorithms/protocol:qt_compat` is **retained** — see
  [The shim that stays](#the-shim-that-stays).
- All 27 rows in the [flash qualification matrix](../../flash-qualification-matrix.md)
  read `portable=yes` with `hardware_status=experimental`.

## Findings that shaped the scope

Four measurements, all taken against `master` at `04fce81`, materially changed
the slice. They are recorded with their method so a future reader can re-run
them rather than trust them.

### There is no global substrate

Hashing every candidate shared method body across all families, whitespace
stripped, finds **zero** exact duplicates. Eighteen `reflash_block`
definitions, eighteen distinct. Twenty `read_mem`, twenty distinct. Fifteen
`encrypt_payload`, fifteen distinct. Only
`connect_bootloader_start_countdown` is defined once at all.

Near-duplication, measured as median pairwise `difflib` line-sequence
similarity across every pair of definitions:

| Method | defs | median sim | pairs >=0.90 | pairs >=0.75 | of |
|---|---|---|---|---|---|
| `reflash_block` | 18 | 0.32 | 3 | 8 | 153 |
| `read_mem` | 20 | 0.26 | 0 | 1 | 190 |
| `write_mem` | 19 | 0.28 | 0 | 1 | 171 |
| `connect_bootloader` | 19 | 0.33 | 6 | 9 | 171 |
| `encrypt_payload` | 15 | 0.33 | 3 | 7 | 105 |
| `check_romcrc` | 7 | 0.53 | 0 | 1 | 21 |
| `get_changed_blocks` | 7 | 0.49 | 0 | 1 | 21 |

A median of 0.26-0.33 means any substrate spanning all 27 families would be a
configurable state machine — precisely what the umbrella and the
[protocol generalization notes](../../protocol-generalization-opportunities.md)
forbid. **The shared method *names* are misleading:** 18 classes share the
`execute` / `cmd_type` / `bootloader_start_countdown` / `kernel_alive`
skeleton and 14 share `connect_bootloader` + `reflash_block`, but the bodies
behind those names are per-family wire sequences, not copies.

### But the >=0.90 tail is real, and it clusters

Whole-file line-overlap ratio identifies five clone clusters, topping out at a
0.98 `connect_bootloader` pair:

| Cluster | Families | Overlap |
|---|---|---|
| Denso ISO-15765 | `1n83m_1_5m_can`, `1n83m_4m_can`, `sh72531_can`, `sh72543_can_diesel` | 0.81-0.94 |
| Denso SH705x | `sh7058_can`, `sh7058_can_diesel`, `tcu_sh705x_can`, `sh705x_densocan` | 0.73-0.90 |
| Hitachi/Mitsu M32R CAN | `hitachi_m32r_can`, `cvt_hitachi_m32r_can`, `cvt_mh8111_can`, `cvt_mh8104_can` | 0.79-0.86 |
| M32R K-Line | `hitachi_m32r_kline`, `mitsu_m32r_kline` | 0.79 |
| MC68 / SH7055_02 | `mc68hc16y5_02`, `sh7055_02` | 0.78 |

**The substrate is cluster-local, and 5c already set the precedent:**
`src/backend/flash/eeprom/denso_sh705x_eeprom_common.{h,cpp}` is a shared core
for exactly two sibling families, not a universal one. This design generalizes
that shape to five clusters; the remaining 11 families are singletons and get
no common.

### JTAG and BDM need no new transport port; `bootmode` does

The JTAG and BDM operations reach the serial layer only through plain
`write_serial_data` / `read_serial_data`, with every connection flag set false
— a raw byte stream that `IKlineFlashTransport` already covers. They are
ordinary singletons.

`bootmode` is the exception. It calls `set_lec_lines`,
`set_serial_port_parity`, `change_port_speed` mid-session, `reset_connection`,
`get_request`, and `get_data` — none of which exist on any current flash
transport port. It needs port-surface design and therefore goes last, with its
own spec, rather than being discovered mid-wave.

### The shim that stays

`//src/algorithms/protocol:qt_compat` is nothing but `qt_bytes.h`, and
[ADR 0004](../../adr/0004-limit-qbytearray-to-qt-boundaries.md) designates it
the permanent, sanctioned Qt-to-portable conversion point: "Keep Qt conversion
explicit through `qt_bytes.h`." It has 22 non-algorithms callers across
transport, serial, UI, and backend adapters. It is a boundary, not debt.

The umbrella's note that "step 5 should drain them and delete the shims" is
amended accordingly — see [Amendments](#amendments-to-the-step-5-umbrella).

## Scope

Two streams, one dominant.

**1. The drain** — 27 legacy classes, 31,704 lines, migrated to portable plan +
executor pairs until the legacy package is deleted. Roughly 95% of the effort.

**2. `:qt_compat` retirement** — mostly a byproduct of the drain:

| Shim | Non-algorithms callers | Disposition |
|---|---|---|
| `ssm:qt_compat` | 27, all legacy flash | dies with the last family (wave 7) |
| `colt:qt_compat` | 2, both `mitsu_m32r_can` | dies in wave 0 |
| `mut_dma:qt_compat` | **0** | already dead; deleted in wave 0 |
| `menu:qt_compat` | 1 (`src/ui/desktop`) | independent PR, any time |
| `crypto:qt_compat` | 1 (`src/ui/desktop`) | independent PR, any time |
| `expression:qt_compat` | 1 (`src/backend/definitions`) | independent PR, any time |
| `protocol:qt_compat` | 22 | **retained** (ADR 0004) |

### Out of scope

- Hardware qualification. No family reaches `proven` in this work; the project
  has no bench access, and the umbrella already excludes it as a step-5 gate.
- The `MainWindow` thin-shell rewrite and `FileActions` deletion — step 6.
- Any universal flashing abstraction. Sharing is cluster-local and only where
  demonstrated identical.

## Unit of work

**A wave is the unit of design** — one spec plus one plan each, in the manner
of 5a through 5e. **A family is the unit of PR**, plus one factoring PR closing
each cluster. A four-family cluster is 4.5-7.3k lines and does not fit one PR.

## Sequence

| Wave | Families | Lines | Rationale |
|---|---|---|---|
| **0** | `FlashEcuMitsuM32rCan` | 476 | Smallest ECU family and the only legacy class with existing wire-assertion tests (`flash_ecu_mitsu_m32r_can_operation_test.cpp`, 255 lines). Validates the per-family template end to end before any cluster work. Installs the drain ratchet; frees `colt:qt_compat` and the dead `mut_dma:qt_compat`. |
| **1** | `FlashEcuSubaruHitachiM32rKline`, `FlashEcuSubaruMitsuM32rKline` | 2,155 | First cluster: the smallest, on K-Line, where the transport adapter is already proven by the 5c pair. Proves port-then-factor. |
| **2** | `FlashEcuSubaruDensoMC68HC16Y5_02`, `FlashEcuSubaruDensoSH7055_02` | 2,518 | Second pair; the pattern is routine by now. |
| **3** | `FlashEcuSubaruHitachiM32rCan`, `FlashTcuCvtSubaruHitachiM32rCan`, `FlashTcuCvtSubaruMitsuMH8111Can`, `FlashTcuCvtSubaruMitsuMH8104Can` | 4,506 | First four-family cluster; first crossing of the ECU/TCU boundary within one cluster. |
| **4** | `FlashEcuSubaruDenso1N83M_1_5MCan`, `FlashEcuSubaruDenso1N83M_4MCan`, `FlashEcuSubaruDensoSH72531Can`, `FlashEcuSubaruDensoSH72543CanDiesel` | 5,971 | Highest clone ratio in the tree, so the largest substrate payoff. |
| **5** | `FlashEcuSubaruDensoSH7058Can`, `FlashEcuSubaruDensoSH7058CanDiesel`, `FlashTcuSubaruDensoSH705xCan`, `FlashEcuSubaruDensoSH705xDensoCan` | 7,305 | Largest by volume; taken once the pattern has settled. Introduces `TransportKind::CanRaw`. |
| **6** | `FlashEcuSubaruDensoSH705xKline`, `FlashEcuSubaruHitachiSH7058Can`, `FlashEcuSubaruHitachiSH72543rCan`, `FlashEcuSubaruUnisiaJecs`, `FlashEcuSubaruUnisiaJecsM32r`, `FlashTcuSubaruHitachiM32rCan`, `FlashTcuSubaruHitachiM32rKline`, `FlashEcuSubaruHitachiM32rJtag`, `FlashEcuSubaruDensoMC68HC16Y5_02_BDM` | 8,118 | Nine singletons; no common, 5c-style ports. |
| **7** | `FlashEcuSubaruUnisiaJecsM32rBootMode` + teardown | 655 | The only family needing new port surface. Also deletes `FlashOperationWorker`, `legacy_flash_utils`, the package, its allowlist entry, the drain ratchet, and `ssm:qt_compat`. |

Wave line counts sum to 31,704, matching the package total exactly — no family
is unaccounted for.

## Per-family PR anatomy

The template is inherited from 5c's EEPROM pair, which is the working
reference for every element below.

**Backend, portable:**

- one `FlashFamily` enum value, one `<Family>Plan` POD, one `FamilyPlan`
  variant alternative;
- `<family>_plan.{h,cpp}` — build and validate with no irreversible I/O;
- `<family>_executor.{h,cpp}` — an `IFlashExecutor`: synchronous, bounded,
  cancellable, dialog-free;
- tests driving `ScriptedKlineFlashTransport` or `ScriptedCanFlashTransport`,
  whose `expectWrite()` is already a byte-exact wire assertion.

**Backend never sees `EcuCalDefStructure`.** 5c's
`build_eeprom_read_plan(paths, protocol_name, mode, file_repository)` takes
none; the dialog performs the translation. Every family follows that, so the
drain adds no new dependency on the god object step 6 deletes.

**UI:** rewrite `src/ui/desktop/flash/<scope>/<family>.cpp` from "construct the
legacy operation, connect `FlashOperationWorker` signals" to "build plan, run
it on `FlashWorker` with the portable executor and a transport adapter". The
EEPROM dialogs are the exact template. Expect roughly 110 lines to become 330,
plus a `<family>_dialog_test.cpp`.

**Delete** `<family>_operation.{h,cpp}` in the same PR, shrink the ratchet, and
flip the matrix row. The legacy `BUILD.bazel` needs no edit per family — its
`srcs` and `MOC_HDRS` are globs.

## Cluster factoring PR

Closes each cluster. Factors the provably identical parts of the now-tested
portable executors into `<cluster>_common.{h,cpp}`. No behavior change; the
executor tests written in the preceding PRs are the guard. Adds
`<cluster>_common_test.cpp`.

**Port-then-factor is the ordering, deliberately.** The alternative —
extracting a shared core from a hand-comparison of the legacy siblings, then
porting each family onto it — is cheaper when the reading is right, but the
core would be written from untested Qt code with no bench to check it against,
and a single misread constant becomes a wrong byte in every family in the
cluster at once. Factoring already-tested portable code makes any drift a test
failure instead. This is also how `denso_sh705x_eeprom_common` came to exist in
5c.

**A factoring PR is allowed to produce no common at all.** The clusters are a
sequencing hint derived from similarity measurement, not a commitment; if a
cluster does not survive porting, the wave still completes.

## Enforcement

### A new ratchet

The `serial_qt_compat` allowlist entry is package-level, so it cannot move
until the 27th family lands and shows no progress before then. A second guard
supplies the per-PR ratchet, mirroring the `FROZEN` pattern exactly:

- **`//:legacy_flash_drain`** / `scripts/check-legacy-flash-drain.py`, holding
  a `REMAINING` set of exactly the 27 legacy operation sources. Thirty files in
  the package include `serial_port_actions.h` today; the other three —
  `legacy_flash_utils.cpp` and two tests — are not family migrations and are
  excluded from the set, dying with the package in wave 7.
- **The set may only shrink.** A file in the package that includes
  `serial_port_actions.h` and is not in `REMAINING` fails as growth, which also
  blocks anyone adding a 28th legacy family.
- Wave 7 deletes the ratchet, the package, and the allowlist entry together.

### Existing guards

- `PORTABLE_ROOTS` in `scripts/check-portable-closure.py` gains each new
  backend flash target as its PR lands, never in bulk.
- Every guard change is proven **non-vacuous** per the umbrella rule — verified
  to fail when the target is absent as well as when it is non-conforming, as
  was done for `//:bazel_openssl_wiring`.

### Scaling items, named now rather than discovered mid-wave

1. **`FamilyPlan` grows to a 29-alternative `std::variant`** in a header every
   flash target includes. Keep the variant — compile-time exhaustiveness is
   exactly what 29 hardware-critical families want — but split the plan structs
   into per-cluster headers that `flash_types.h` assembles, so each cluster's
   types live beside its executor. Decide at wave 3, where it first bites.
2. **`TransportKind` needs `CanRaw`** for `sh705x_densocan` in wave 5. The
   proprietary DensoCAN bootloader framing is not ISO-15765; the matrix already
   records the distinction.

## Testing

Each executor test must cover the full `ErrorKind` taxonomy the umbrella names:
success path, timeout, disconnect, negative or malformed response, cancellation
mid-transfer, and unsupported operation. Plan tests assert rejection *before*
any I/O — invalid MCU, out-of-range region, image and region size mismatch.
Dialog tests assert that preflight rejection starts no worker, that
confirmations are answered, that progress ordering holds, and that log-line
text is preserved verbatim.

Gates per PR, unchanged from 5c onward:

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure //:legacy_flash_drain
```

Plus `>=80%` new-code coverage and the SonarCloud Quality Gate. Coverage is
budgeted during execution, not treated as a final check — roughly 35k lines of
new portable source is the largest test-writing commitment in the
modularization, and deferring it would strand a wave.

## Fidelity discipline

This is what substitutes for hardware. The legacy `.cpp` is the only source of
truth for correct bytes, and it is deleted in the same PR as the port.

- **Every exchange in a portable executor carries a comment citing the legacy
  file and line it was transcribed from.** 5c's executors already do this; here
  it is a rule, because it is the only way a reviewer can check a port without
  a bench.
- **Deliberate divergences are named in the matrix `notes` column, never
  silent.** 5c set both precedents: the EEPROM write gap, which returns
  `Unsupported` rather than legitimizing a no-op write, and the
  `_ecutek_racerom_alt` read-from-`received` bug fix.
- **Documented legacy quirks are preserved, not fixed.** The matrix already
  records several that a well-meaning port would "correct":
  `FlashEcuMitsuM32rCan`'s `test_write` performs the basic handshake only and
  returns success without planning a write; `FlashEcuSubaruDensoMC68HC16Y5_02`
  catches `sub_ecu_denso_mc68hc16y5_02_tpu` by prefix although that protocol's
  own mode is BDM; `FlashEcuSubaruHitachiM32rJtag` is unreachable from the UI
  because no `protocols.cfg` entry can produce its protocol name, and is ported
  as-is without wiring a new dispatch path.

## The matrix is the ledger

Each family PR flips its row in the
[flash qualification matrix](../../flash-qualification-matrix.md): `portable`
`no` to `yes`, `automated_evidence` gains the new test labels and the wave
number, `hardware_status` `unqualified` to `experimental`. Nothing reaches
`proven` from unit tests. When the drain closes, all 27 rows read
`portable=yes` and `experimental`, and the matrix becomes the standing list of
what still needs a bench.

## Risks

| Risk | Mitigation |
|---|---|
| Hand-transcribing ~32k lines of wire logic with no hardware produces wrong bytes | Per-exchange legacy line citations; byte-exact `expectWrite` scripts; divergences named in the matrix; `experimental` never upgraded by unit tests |
| Substrate extracted across siblings that only *look* alike | Port-then-factor ordering; factor only what is identical in already-tested portable code; the >=0.90 similarity data names which pairs are candidates at all |
| A cluster does not survive porting | The factoring PR may produce no common and the wave still completes |
| Wave 7 needs port surface nobody has designed | `bootmode` sequenced last, with its own spec |
| Step 6 collides with the drain | Both rewrite `src/ui/desktop/flash/**`. Step 6 does not start on those dialogs until the drain finishes |
| A long drain lets the legacy package grow behind the ratchet's back | `REMAINING` fails on unrecognized files, not only on a larger count |
| `>=80%` new-code coverage on ~35k new lines | Budgeted per PR during execution, as since 5c |

## Amendments to the [step-5 umbrella](2026-07-22-step5-backend-portable-design.md)

1. **The tail's unit of work is the wave, not the family.** The umbrella's
   `tail` row reads "~28 remaining flash families, one to a few per PR". The
   count is 27, and the sequencing is five measured clone clusters (16
   families) plus 11 singletons, in eight waves, each with its own spec and
   plan.
2. **`//src/algorithms/protocol:qt_compat` is retained, not drained.** The
   umbrella's step-4 note that "step 5 should drain them and delete the shims"
   is scoped to the six shims that wrap portable logic. `qt_bytes.h` is the
   permanent Qt boundary designated by
   [ADR 0004](../../adr/0004-limit-qbytearray-to-qt-boundaries.md), and
   retiring it would mean converting 22 files at the boundary the ADR exists to
   define.
3. **A second ratchet, `//:legacy_flash_drain`, supplements the allowlist.**
   The allowlist entry is package-level and cannot record per-family progress;
   the new guard does, and is deleted together with the package in wave 7.

## Amendments to the [modularization plan](../../modularization-plan.md)

Step 5's status list gains the tail's eight waves after 5e. The plan's
statement that the tail "has also not started" is superseded on the first wave-0
merge. Step 5 closes when the [completion criterion](#completion-criterion)
holds.

## Doc fixes carried by wave 0

The [protocol generalization notes](../../protocol-generalization-opportunities.md)
still list `src/backend/flash/flash_utils.*` under "Consolidated foundations".
5e deleted that file, splitting it between
`src/platform/desktop/common/flash/legacy/legacy_flash_utils.{h,cpp}` and
`//src/algorithms/checksum`. Correct it in wave 0, and record there that the
document's own unlock condition — "a shared abstraction is justified only after
byte-level tests demonstrate a stable behavioral contract across concrete
families" — is what the port-then-factor ordering implements.

## Follow-ups

To be filed as issues, not carried inside the tail:

- **Retire the `findFlashDeviceIndex` Qt shim.** Inherited from 5e. Each family
  PR converts its own call sites to the portable `std::string_view` form; the
  shim dies with `legacy_flash_utils` in wave 7. Tracking it here closes 5e's
  open follow-up.
- **Bench-qualify the migrated families.** All 27 land `experimental`. The
  matrix's hardware qualification checklist is the entry point, and this is
  where the project's lack of bench access becomes the only remaining blocker
  to `proven`.
