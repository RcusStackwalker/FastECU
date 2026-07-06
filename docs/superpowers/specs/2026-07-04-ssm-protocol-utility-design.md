# SSM protocol math utility (phase 1 of protocol implementation generalization) — design

**Date:** 2026-07-04
**Status:** Approved for planning
**Predecessor:** `docs/protocol-generalization-opportunities.md` (survey written after the
FlashOperationWorker migration, identifying this cluster as the safest, highest-value
extraction target)

## Context and problem

`docs/protocol-generalization-opportunities.md` surveyed all 22+ Subaru ECU/TCU/EEPROM
`*_operation.cpp` files (post-FlashOperationWorker-migration) and found a cluster of
functions that are byte-for-byte identical across every module, differing only in the
constant lookup tables each ECU family passes in:

- `calculate_seed_key()` — the 16-round seed→key transform used by every K-Line/CAN
  bootloader handshake.
- `calculate_payload()` — the same round-transform shape, applied over a buffer instead of a
  single seed (kernel-upload encryption, TCU ROM decryption).
- `add_ssm_header()` / `calculate_checksum()` — the SSM wire-header byte layout (`0x80,
  target_id, tester_id, length, ...payload..., checksum`).
- `parse_message_to_hex()` — a byte-to-hex-string dumper (60 copies, zero variation).
- `crc32()` / `init_crc32_tab()` — a table-driven CRC32 with a fixed non-standard polynomial
  (`0x5AA5A55A`), used by flash modules that verify block CRC after write.

Counting occurrences: `parse_message_to_hex` (60 copies), `calculate_checksum` (40 copies),
the seed-key/payload/header/CRC cluster (22 modules, 44 files). Diffing
`calculate_seed_key()` across a Denso CAN module and a Hitachi CAN module confirmed the
round-transform loop itself is identical; only the two lookup tables
(`keytogenerateindex[16]`, `indextransformation[32]`) differ. Counting the actual table
literals across all 22+ modules found only ~15-18 distinct `keytogenerateindex[]` values in
total — several modules share a table outright, and many "encrypt" / "decrypt" pairs within
one module are just each other's table reversed.

`indextransformation[32]` is the *same* table in the overwhelming majority of call sites
(70 occurrences share one of two first-lines: 65 use the common table, tracked down further
below) — but it is **not** universal: `generate_ecutek_seed_key()` (an alternate seed-key
derivation present in the Denso SH7058-family modules, for compatibility with EcuTek-brand
tooling) uses a different `indextransformation`, and does so *within the same file* as calls
that use the common one — e.g. `flash_ecu_subaru_denso_sh7058_can_operation.cpp` has five
`indextransformation[]` declarations, four using the common table and one (inside
`generate_ecutek_seed_key`) using a different one. This was found during this spec's
self-review by grepping and diffing every occurrence rather than trusting the two-module
sample diff, and it changes the architecture below: both tables must stay explicit
parameters of the shared function, never a hardcoded shared constant.

This means the real per-ECU-family "secret" is two constant tables, not an algorithm — the
current structure obscures that by re-deriving the same algorithm 20+ times, at real cost to
anyone who needs to touch this logic (a change or bugfix must be manually propagated to every
copy, and has not been, historically — there is no way to know today whether all copies are
actually still identical without diffing them, which this design's survey had to do by hand).

This is phase 1 of a three-phase generalization effort identified in the survey doc; phases 2
(retry/response-check idiom) and 3 (block-transfer-with-progress loop) are separate, future
specs, deliberately excluded from this one (see "Scope decisions" below).

## Scope decisions (from brainstorming)

- **Cluster boundary:** this spec covers only the pure-math cluster listed above — functions
  whose entire behavior is determined by their explicit parameters, with no protocol
  sequencing or per-family wire semantics baked in. The retry/response-check idiom and the
  block-transfer loop are real duplication too, but both encode per-family protocol
  knowledge (response op-codes, byte offsets, block sizes) and were explicitly deferred to
  their own future specs rather than bundled in, to keep this phase's risk and review surface
  small.
- **Verification bar:** every module's behavior must be pinned by a characterization test
  before its private implementation is deleted — not just a clean build. In practice (see
  "Testing" below) this is one golden-value test per distinct table pair in a single shared
  test suite, not 22 separate per-module test files, since the existing Operation test
  pattern (`test_flash_ecu_mitsu_m32r_can_operation.cpp`) exercises modules through a scripted
  `FakeBackend` and full `execute()`, which would make isolating just the seed-key math
  expensive to set up per module for no extra coverage value.
- **Rollout order:** write `SsmProtocol` and its test suite first, pilot the refactor on two
  modules (one Denso-family, one Hitachi-family, to prove the generalization holds for both
  table shapes actually in use), then apply the same mechanical recipe to the remaining ~20
  modules one at a time, mirroring the per-module task shape used by the
  FlashOperationWorker migration (`docs/superpowers/plans/2026-07-03-flash-operation-worker-migration.md`).
- **Utility shape:** free functions in a `namespace SsmProtocol`, not a static-method class.
  (A static-method class matching `FileActions::parse_nrc_message()`'s existing convention
  was the recommended default; the namespace form was chosen instead.)

## Architecture

New files: `modules/ssm_protocol.h` / `modules/ssm_protocol.cpp`.

```cpp
namespace SsmProtocol {
    // 16-round seed->key transform. table1 is the 16-entry key table, table2 the
    // 32-entry byte-substitution table -- both stay required parameters, since
    // table2 is not actually universal (see "generate_ecutek_seed_key" above).
    QByteArray calculateSeedKey(const QByteArray &seed, const uint16_t *table1,
                                const uint8_t *table2);

    // Same transform applied over a buffer (kernel upload encryption / ROM decryption).
    QByteArray calculatePayload(const QByteArray &buf, uint32_t len,
                                const uint16_t *table1, const uint8_t *table2);

    // SSM checksum: sum of bytes, optionally negated from 0x100.
    uint8_t checksum(const QByteArray &data, bool dec0x100);

    // Wraps payload in the SSM wire header (0x80, target, tester, length, ..., checksum).
    QByteArray addHeader(QByteArray payload, uint8_t testerId, uint8_t targetId, bool dec0x100);

    // Space-separated uppercase hex dump, e.g. "80 F1 12 05 67 ...".
    QString toHex(const QByteArray &data);

    // Table-driven CRC32, fixed polynomial 0x5AA5A55A. Table is a function-local
    // static const array (computed once, thread-safe per C++11 magic statics) --
    // not a lazily-set instance flag.
    uint32_t crc32(const uint8_t *buf, uint32_t len);
}
```

Each module keeps its own `keytogenerateindex[]` and `indextransformation[]` (or equivalent)
as local arrays at their existing call sites, unchanged — this is the real per-call-site data,
and it stays exactly where a reader would look for it (the module implementing that ECU
family or alternate derivation), not hidden inside a shared file or assumed constant. Only the
round-transform loop itself, checksum, header-wrapping, hex-dump, and CRC move out — no table
data moves anywhere.

## Call-site changes

Per module, the private method declarations and bodies for `calculate_seed_key`,
`calculate_payload`, `add_ssm_header`, `calculate_checksum`, `parse_message_to_hex`,
`crc32`, and `init_crc32_tab` are deleted from both the `.h` and `.cpp`. Call sites change
from (existing shape):

```cpp
seed_key = calculate_seed_key(seed, keytogenerateindex, indextransformation);
```

to:

```cpp
seed_key = SsmProtocol::calculateSeedKey(seed, keytogenerateindex, indextransformation);
```

with the equivalent renaming for `calculatePayload`, `checksum`, `addHeader`, `toHex`, and
`crc32`. No other line in any module's `execute()` or protocol methods changes — this is a
pure find-and-replace at the small number of call sites per module (typically 3-8), not a
restructuring of any module's control flow.

## Concurrency correctness note

Today, `crc_tab32` / `crc_tab32_init` are per-`Operation`-instance members, lazily computed
on first use. This is safe only because each `Operation` runs on its own worker thread and
never shares that state with another instance. Moving `crc32()` into a shared, file-scope
implementation removes that isolation: if two flash operations ever run concurrently on
different threads (e.g. a user opens both an ECU flash dialog and a TCU flash dialog at the
same time — possible today, not hypothetical), a naive shared lazy-init guarded by a plain
`bool` would be a genuine data race. The fix is to compute the CRC table via a function-local
`static const` array, which C++11 guarantees is initialized exactly once even under
concurrent first-touch ("magic statics"). This is a deliberate, scoped correctness
improvement over the status quo — not a change to the CRC algorithm, polynomial, or any wire
byte.

## Testing

New file: `tests/test_ssm_protocol.cpp`, added to `tests/tests.pro`.

- **Golden-value characterization cases:** one case per distinct `(keytogenerateindex[],
  indextransformation[])` *pair* actually in use — not per `keytogenerateindex[]` alone, since
  `indextransformation[]` is not universal (see the `generate_ecutek_seed_key()` finding
  above). This is more cases than originally estimated from the two-module sample diff; the
  implementation task must re-grep every `calculate_seed_key`/`calculate_payload` call site
  across all 22+ modules to enumerate the actual distinct pairs before writing golden values,
  rather than assuming the earlier ~15-18 estimate. Each golden value is captured by running
  the *existing* module's private method once (via a small throwaway harness or a temporary
  `friend` test on that one module) before its implementation is deleted, then hard-coded as
  the expected value in the permanent test. This suite is what "pins" every module's behavior
  — not 22 separate per-module test files.
- **Direct unit tests** for `checksum`, `addHeader`, `toHex`, and `crc32` against synthetic
  inputs with hand-computed expected outputs (these are simple enough not to need
  characterization from existing code).
- **Concurrency test** for `crc32()`: call it from two threads simultaneously on first use
  and assert both get the correct, identical result (regression test for the "Concurrency
  correctness note" above).

Per-pilot-module verification: after refactoring each of the two pilot modules, the existing
full-suite pattern (`qmake6 FastECU.pro && make -j4`, plus the existing
`test_flash_ecu_mitsu_m32r_can_operation.cpp`-style Operation tests where they exist) must
stay green. No new per-module Operation tests are required by this spec — the shared utility
suite is the regression safety net for the math; each module's own existing tests (where
present) continue to cover its handshake sequencing.

## Rollout

1. Write `SsmProtocol` (`modules/ssm_protocol.h/.cpp`) and `tests/test_ssm_protocol.cpp`,
   capturing golden values from current code before any module is touched.
2. Pilot: refactor one Denso-family module and one Hitachi-family module end to end. Build
   clean, existing tests green. This step validates the recipe and the golden-value approach
   before touching the remaining 20.
3. Apply the same mechanical recipe to the remaining ~20 modules, one per task, in the same
   style as `docs/superpowers/plans/2026-07-03-flash-operation-worker-migration.md`'s
   per-module tasks.
4. Final full regression run: `tests.pro`, `serial_backend_tests.pro`,
   `serial_crash_tests.pro`, `mut_dma_integration_tests.pro`, and the full `FastECU.pro`
   build.

## Out of scope

- **Retry/response-check idiom** (phase 2, future spec): collapsing the repeated
  `write; read; check received.length()/byte; log NRC on failure` pattern into a helper.
  Deferred because it touches dozens of call sites per module with per-family timeout/offset
  variation, and doesn't share this phase's "pure function of explicit parameters" safety
  property.
- **Block-transfer-with-progress loop** (phase 3, future spec): the highest-payoff and
  highest-risk cluster identified in the survey doc, since block sizes, addresses, and
  response op-codes genuinely differ per ECU family. Not touched here.
- **Protocol semantics/state-machine generalization**: `connect_bootloader()`'s handshake
  order and SID meanings differ enough between K-Line/CAN and Denso/Hitachi/Mitsubishi that
  the survey doc explicitly recommends against forcing them into one shared implementation.
  Not proposed here or in any future phase without a concrete new reason.

## Risks

- **Golden-value capture error:** if a golden value is captured incorrectly (e.g. from the
  wrong module, or after an unrelated edit), the characterization test would silently pin
  wrong behavior. Mitigated by capturing each golden value directly from the specific
  module's still-in-place private method immediately before deleting it, in the same task.
- **A table assumed shared turns out to differ.** Already caught once during this spec's own
  self-review — `indextransformation[]` was assumed universal from a two-module sample diff,
  then found to vary within a single file once every occurrence was grepped and compared.
  This is exactly the failure mode the golden-value suite exists to catch, and confirms the
  implementation step must re-enumerate every call site's actual table arguments (not reuse
  this spec's estimates) before finalizing the test list.
- **Missed alternate derivation paths.** `generate_ecutek_seed_key()` and similar
  less-common/alternate methods (present but not necessarily invoked by every module's default
  code path) are easy to miss with a cursory grep for the "main" seed-key function name.
  Mitigated by grepping for all call sites of `calculate_seed_key`/`calculate_payload` by
  their actual invocation, not by function name pattern alone, during implementation.
