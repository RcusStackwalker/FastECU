# Protocol implementation generalization opportunities

Notes surfaced while migrating all 29 flash/eeprom dialog modules to `FlashOperationWorker`
(see `docs/superpowers/specs/2026-07-03-flash-operation-worker-design.md`). The migration was
mechanical relocation only — no protocol behavior was changed — but reading every module's full
protocol implementation start to finish surfaced duplication worth recording. Nothing here is
proposed as a next task; it's a map for whoever next touches this code.

## What's duplicated, and how much

Grepping across `modules/` after the migration:

| Function | Copies | Modules |
|---|---|---|
| `parse_message_to_hex(QByteArray)` | 60 | nearly every ECU/TCU/EEPROM module |
| `calculate_checksum(QByteArray, bool)` | 40 | every SSM-header-based module |
| `calculate_seed_key`/`calculate_payload`/`add_ssm_header`/CRC cluster | 22 modules (44 files) | every Subaru Denso/Hitachi ECU+TCU+EEPROM module, plus `flash_ecu_subaru_unisia_jecs_m32r_bootmode` |
| `init_crc32_tab`/`crc32` (poly `0x5AA5A55A`) | 14 | flash modules that verify block CRC after write |

## The seed-key algorithm is one function, not many

The biggest finding: `calculate_seed_key()`'s core round loop —

```cpp
for (ki = 15; ki >= 0; ki--) {
    wordtogenerateindex = seed;
    wordtobeencrypted = seed >> 16;
    index = wordtogenerateindex ^ keytogenerateindex[ki];
    index += index << 16;
    encryptionkey = 0;
    for (n = 0; n < 4; n++)
        encryptionkey += indextransformation[(index >> (n * 4)) & 0x1F] << (n * 4);
    encryptionkey = (encryptionkey >> 3) + (encryptionkey << 13);
    seed = (encryptionkey ^ wordtobeencrypted) + (wordtogenerateindex << 16);
}
```

is byte-for-byte identical across Denso CAN, Denso K-Line, Hitachi CAN, Hitachi K-Line, and
Mitsubishi CAN variants (confirmed by diffing `flash_ecu_subaru_denso_sh7058_can_operation.cpp`
against `flash_tcu_subaru_hitachi_m32r_can_operation.cpp` — the only differences are stray debug
`emit LOG_I` calls and comments). Only the two lookup tables (`keytogenerateindex[16]`,
`indextransformation[32]`) differ per ECU family, and `indextransformation[]` is itself identical
in every module that has it. `calculate_payload()` (used for kernel-upload encryption and TCU
ROM decryption) is the same shape again: identical round loop, table-driven, operating over a
buffer instead of a single seed.

This means the actual per-family "secret" is just two constant tables, not an algorithm — the
current structure obscures that by re-deriving the algorithm 20+ times. A shared
`calculate_seed_key(seed, table1, table2)` / `calculate_payload(buf, len, table1, table2)` free
function (or a small `SsmCipher` header-only utility) called with each module's own tables would
delete on the order of 800-1000 lines without touching any wire behavior, since the call sites
already pass their tables as parameters — the function bodies are the copy-paste, not the call
sites.

## Other good extraction candidates, roughly in order of payoff/risk

1. **`add_ssm_header()` + `calculate_checksum()`** — trivially identical everywhere (header byte
   layout `0x80, target_id, tester_id, length, ...payload..., checksum`). Same case as the seed-key
   cluster: zero behavioral risk, pure duplication.
2. **`parse_message_to_hex()`** — a one-line-per-byte hex dumper, 60 copies, zero parameters vary.
   Easiest possible win, also the least valuable per-copy.
3. **`crc32()`/`init_crc32_tab()`** — same non-standard polynomial (`0x5AA5A55A`) and table-gen
   loop everywhere it appears. `crc_tab32_init`/`crc_tab32` are currently per-instance state
   (recomputed once per `Operation` object); a shared implementation could compute the table once
   at static/namespace scope instead of once per flash session.
4. **Retry-loop-with-response-check idiom.** Nearly every wire exchange in every module is:
   ```cpp
   serial->write_serial_data_echo_check(output);
   received = serial->read_serial_data(timeout);
   if (received.length() > N) {
       if (received.at(4) != expected) { emit LOG_E(...parse_nrc_message...); return STATUS_ERROR; }
   } else {
       emit LOG_E("No valid response from ECU", ...); return STATUS_ERROR;
   }
   ```
   `FileActions::parse_nrc_message()` is already shared (good prior art), but the length-check +
   early-return wrapper around it is re-typed at every call site — dozens of times per module.
   A small helper (`expectResponse(received, minLen, offset, expectedByte)`) could collapse this to
   one line per exchange, but see the caveat below before generalizing it.
5. **Block read/write loops with progress reporting.** The shape (compute block boundaries, loop
   emitting `progressChanged`, retry N times, track bytes/sec and time-remaining for the log
   message) recurs in every `read_mem`/`write_mem`/`flash_block`/`reflash_block`. Structurally
   templatable with a send/parse callback per iteration, but the per-family block sizes, addresses,
   and response op-codes genuinely differ — this is the highest-payoff *and* highest-risk item on
   the list.

## Where not to generalize (yet)

The actual protocol semantics — which SID means what, which byte offset holds which field, the
`connect_bootloader()` handshake sequence order — differ enough between K-Line/CAN and
Denso/Hitachi/Mitsubishi that forcing them into one shared state machine would trade duplicated-
but-legible code for a parameterized abstraction nobody can safely modify without touching every
ECU family at once. These protocols were reverse-engineered piecemeal and are sparsely documented;
per-module copies that can be read top-to-bottom and modified in isolation are arguably a feature
here, not just debt. The `FlashOperationWorker` base class extracted during this migration
(`LOG_E/W/I/D`, `confirm()`, `progressChanged`, `stopRequested()`, `delay()`) is the right
altitude for shared infrastructure — mechanical/plumbing concerns, not protocol semantics. The
seed-key/checksum/CRC/hex-dump cluster above sits at the same altitude: pure math with the
per-family data passed in as parameters, no protocol knowledge baked in. That's the boundary to
generalize up to, and no further, without a concrete reason (e.g., a new ECU family whose protocol
turns out to need one of these look-alike-but-not-identical loops to actually be identical).
