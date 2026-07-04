# Mitsu Colt CAN — vendor diagnostic-extension challenge algorithm

**Date:** 2026-07-04
**Status:** Approved
**Target ROM:** `47110032` (Colt CZT, Z37A) — specifically ROMs carrying a third-party
commercial tuning suite's diagnostic extension (identified via embedded plaintext
signature strings; not present in stock ROMs or in any ROM built by this project).

---

## Background

Reverse-engineering a bench-read ROM against the `47110032` stock reference (session
prior to this spec) found a diagnostic-dispatcher hijack that adds a memory read/write
channel and a session-control gate. The gate protects entry into the `0x85` ("bootload")
diagnostic session behind a custom challenge-response check, separate from and layered
in front of the ECU's own factory `SecurityAccess` (service `0x27`, sub-functions `5`/`6`,
already ported as `MitsuColtCan::seedKeyWord`/`seedKey`).

The challenge is a 4-round bit-mixing function at ROM offset `0x510b8`, reverse-engineered
by static disassembly only (no vendor source, no documentation). This spec covers
extracting that algorithm — both directions — into a pure, testable `.cpp` module in
FastECU, ready for a later session to wire into an actual handshake.

## Scope

**In scope:**
- Forward transform, ported verbatim from the ROM.
- Inverse transform, analytically derived and verified — this is the function a real
  client actually needs (see "Key finding" below for why).
- Byte-array convenience wrappers matching the wire's big-endian 4-byte seed/key layout.
- QtTest unit tests with hardcoded vectors, matching this codebase's existing style.

**Out of scope (deferred to the later integration session):**
- CAN/K-line framing, session-ID bytes, sub-function byte layout (`'A'`/`'B'`).
- Any session-state tracking or handshake orchestration.
- The ECU-side LCG accumulator that produces the *secret* in the first place
  (`RAM[0x807700] += RAM[0x8003d0] * 0x41c64e6d`) — that's ECU-internal state a client
  never computes; a client only ever receives the seed and answers with the inverse.
- Wiring this into `FlashEcuMitsuM32rCan` or any UI/orchestration code.
- Anything about the *other* hijacked call site (memory read/write) from the earlier
  session — unrelated to this challenge.

## Key finding: client needs the inverse, not the forward function

Tracing `requestSeed` (ROM `0x4fe10`) and `sendKey` (ROM `0x4fe94`):

- `requestSeed` computes a rolling LCG-style accumulator (the *secret*), saves it raw to
  `RAM[0x807704]`, and sends the client `transform(secret)` as the 4-byte seed.
- `sendKey` compares the client's submitted key directly against the **raw**
  `RAM[0x807704]` — i.e. against `secret`, not `transform(secret)`.

So: `seed = transform(secret)`, and the ECU expects `key = secret = transform⁻¹(seed)`.
A real client must implement the inverse of the ROM's function, not a forward
re-application of it.

## The forward transform (as it exists in ROM, at `0x510b8`)

4 rounds, `i = 0..3`. Each round: `x = P2(P1(x) + K[i])`, starting from `x = secret`;
final `x` after 4 rounds is the seed sent to the client.

- **P1(x)** — a bit permutation built from 4 disjoint masks (confirmed to partition all
  32 bits with zero overlap):
  ```
  P1(x) = ((x & 0x15555555) << 3)   // even bits 0..28  -> odd bits 3..31
        | ((x & 0xaaaaaaa8) >> 3)   // odd bits 3..31   -> even bits 0..28
        | ((x & 0x40000000) >> 29)  // bit 30           -> bit 1
        | ((x & 0x00000002) << 29)  // bit 1            -> bit 30
  ```
- **K[i]** — 4 round constants read from ROM flash at `0x5fddc + i*4` (big-endian u32),
  which are literally the first 16 bytes of the ASCII string embedded elsewhere in the
  same ROM, reinterpreted as words: `K0=0x45637554` ("EcuT"), `K1=0x656b2052` ("ek R"),
  `K2=0x4f4d2050` ("OM P"), `K3=0x61746368` ("atch"). Addition is mod 2³².
- **P2(x)** — nibble swap within each byte:
  ```
  P2(x) = ((x & 0x0f0f0f0f) << 4) | ((x & 0xf0f0f0f0) >> 4)
  ```

## Deriving the inverse

Both `P1` and `P2` are **involutions** (self-inverse):
- `P1` is built entirely from two disjoint bit-swaps — {even 0..28} ↔ {odd 3..31}, and
  bit 1 ↔ bit 30 — i.e. a permutation composed purely of 2-cycles, which is always its
  own inverse. `P1(P1(x)) == x` for all `x`.
- `P2` swaps the two nibbles of each byte; swapping twice returns the original.
  `P2(P2(x)) == x` for all `x`.

Since the only non-involutive step per round is `+ K[i] mod 2^32`, the inverse of one
round is: `round⁻¹(y, K) = P1(P2(y) - K)` (using the *same* `P1`/`P2` as the forward
direction). The full inverse applies this 4 times with the round constants in **reverse**
order (`K3, K2, K1, K0`):

```
challengeInverseTransform(seed):
    x = seed
    for K in [K3, K2, K1, K0]:
        x = P1(P2(x) - K)   // subtraction mod 2^32
    return x   // == secret == the key the ECU expects
```

This closed-form result should be exhaustively verified (see Testing) before it's
trusted, but the structural argument above (disjoint 2-cycles ⇒ involution) is why we
expect it to hold for every input, not just spot-checked ones.

## File layout & API

New files, following the existing `protocol/*_protocol.h/.cpp` convention
(pure logic, no I/O — same shape as `MitsuColtCan` and `MitsuColtCanCdbg`):

- `protocol/mitsu_colt_can_vendor_ext_protocol.h`
- `protocol/mitsu_colt_can_vendor_ext_protocol.cpp`
- namespace `MitsuColtCanVendorExt`

Vendor-neutral naming: the third-party product name is documented factually in comments
(as "a commercially-tuned ROM's diagnostic extension, reverse-engineered by static
analysis") but not used as a code/file identifier.

```cpp
namespace MitsuColtCanVendorExt {

// Forward transform, ported verbatim from ROM 47110032 offset 0x510b8. This is
// what the ECU applies to its internal secret to produce the seed value it
// sends the client. Provided for documentation/completeness and as the basis
// for the exhaustive round-trip check in tests — not what a client calls.
quint32 challengeTransform(quint32 secret);

// Inverse of challengeTransform(), analytically derived (see design doc
// docs/superpowers/specs/2026-07-04-mitsu-colt-can-vendor-ext-challenge-design.md
// for the derivation) and verified against the forward function across the
// full 32-bit domain. THIS is what a real client calls: given the 4-byte seed
// the ECU sends, computes the key value the ECU will accept.
quint32 challengeInverseTransform(quint32 seed);

// Big-endian byte-array convenience wrappers matching the 4-byte wire layout,
// mirroring MitsuColtCan::seedKey's shape.
quint32 bytesToSeed(const QByteArray &seedBytes);  // expects exactly 4 bytes
QByteArray keyToBytes(quint32 key);                 // produces exactly 4 bytes

} // namespace MitsuColtCanVendorExt
```

`bytesToSeed`/`keyToBytes` are straightforward big-endian pack/unpack, no error
handling beyond what `MitsuColtCan::seedKey` already does for its analogous case
(caller is expected to pass exactly 4 bytes; this is internal protocol code, not a
public API needing defensive validation).

## Verification methodology

1. Exhaustive brute-force check, run once during implementation (throwaway
   tool/script, not shipped): for every `x` in `[0, 2^32)`, assert
   `challengeInverseTransform(challengeTransform(x)) == x`. This is the load-bearing
   proof — the structural involution argument above is why we expect it to pass, but
   a hand-derivation error in a shift amount or mask would only show up empirically.
   Record the result (pass/fail, wall-clock time) in the implementation notes.
2. A handful of the confirmed pairs from step 1 become permanent hardcoded vectors in
   the QtTest suite (step below) — the suite itself does not brute-force.

## Testing

New `tests/test_mitsu_colt_can_vendor_ext_protocol.cpp`, QtTest, matching
`test_mitsu_colt_can_protocol.cpp`'s existing shape:
- `challenge_transform_matches_known_vectors()` — a few forward vectors confirmed
  during derivation.
- `challenge_inverse_transform_matches_known_vectors()` — the corresponding inverse
  vectors.
- `challenge_inverse_round_trips_with_forward()` — `QCOMPARE(challengeInverseTransform(challengeTransform(x)), x)` for a small set of arbitrary `x` (e.g. `0`, `0xFFFFFFFF`, `0x12345678`, a couple of others) as a lightweight regression check standing in for the exhaustive one-time proof.
- `byte_wrappers_round_trip()` — `bytesToSeed`/`keyToBytes` pack/unpack correctness.

## Documentation / provenance

File header comment states: reverse-engineered from ROM `47110032` offset `0x510b8`
via static disassembly (not from vendor source or documentation); what it's for
(gating entry to the `0x85` bootload diagnostic session on ROMs carrying this vendor
extension); and that it's irrelevant to stock ROMs and to this project's own patches —
only ROMs with this specific third-party extension present use it.

## Out of scope / explicitly deferred

- Wiring this into `FlashEcuMitsuM32rCan` or any session/handshake orchestration.
- Frame builders for the request/response wire format (SID `0x27`, sub-function bytes).
- The LCG accumulator that produces the ECU-side secret (ECU-internal, not client logic).
- Any change to the existing `MitsuColtCan` or `MitsuColtCanCdbg` modules.
