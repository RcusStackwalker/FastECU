# Step 5 Tail — Wave 1 Hitachi M32R K-Line

## Scope and status

`FlashEcuSubaruHitachiM32rKline` moves from its Qt dialog/operation to the
portable flash seam. The accepted protocol identifiers are exactly
`sub_ecu_hitachi_m32r_kline` and
`sub_ecu_hitachi_m32r_kline_recovery`, both with
`M32R_512KB_1block`. Hardware status remains **experimental**; this work
claims automated equivalence, not bench qualification.

## Portable contract

`SubaruHitachiM32rKlinePlan` snapshots normal/recovery session mode, SSM
tester/target IDs `0xf0`/`0x10`, baud rates 4800/15625/38400, 128-byte
chunks, and the `0x100000` read-address bias. Plans are kernel-free. Reads
produce the logical `0x00000`–`0x80000` image; writes require exactly
`0x80000` bytes and erase/program the single full-ROM region. `test_write`
is rejected during plan construction, before transport configuration.

Reads first probe SID BF at 38400. A missing or malformed probe falls back
to BF at 4800, SID B8 baud selection, 38400, and a confirming BF. Recovery
does not affect reads.

Normal writes retain the 15625 active-OBK SID 34 probe. If inactive they
perform the 4800 BF/81/83/27/10 sequence. Recovery writes retain the bounded
1,000-attempt 0x81 wake loop followed by 83/27/10. Both paths then use the
legacy full-ROM SID 34/31/36 sequence and Hitachi encryption.

Required responses are checked with bounds-safe prefix parsing. Missing or
short SID 36 acknowledgements remain tolerated, as does any non-empty final
checksum response. Transport failures still propagate. Cancellation is
checked around every exchange and transfer boundary, including immediately
after erase, and returns `Cancelled`.

The existing common Begin prompt is the only prompt. The common workflow
propagates read bytes and ROM ID through `FlashDialogResult`; no new transport
or family-specific warning is introduced.

## Cluster factoring decision

The Hitachi and Mitsubishi implementations were compared for SSM framing,
optional exchange, and response-prefix parsing. Although their byte framing
is the same, those helpers currently sit inside materially different session
contracts: Mitsubishi treats the initial handshake as required and uses one
timeout policy, while Hitachi uses optional probes, a short recovery wake
timeout, fallback parsing, and deliberately tolerant write acknowledgements.
Extracting a useful common helper would therefore require exposing timeout
and tolerance policy at the helper boundary, creating the configurable state
machine this wave forbids. No cluster abstraction is introduced. Geometry,
keys, addressing, flow, response tolerance, logging, and progress remain
family-owned.

## Automated evidence

Plan tests pin exact protocol/mode mapping, geometry, all wire constants,
kernel-free behavior, and invalid input rejection. Byte-scripted executor
tests cover the direct 38400 read path, full biased traffic, bounded recovery,
and cancellation before I/O; the family suites and workflow/config/dialog
tests cover portable routing and result propagation. The legacy operation and
dialog are removed, and the drain ratchet contains 24 remaining families.
