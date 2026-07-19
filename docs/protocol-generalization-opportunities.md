# Protocol implementation generalization opportunities

This document records the current safe boundary for sharing code across the 29
flash/eeprom/jtag/bdm operation families. It originated during the
`FlashOperationWorker` migration; the first wave of low-risk duplication has
since been removed.

## Consolidated foundations

The following are shared today and are no longer open extraction work:

- `src/algorithms/protocol/ssm/ssm_protocol.*` owns SSM headers/checksums, seed-key and payload
  transforms, the non-standard CRC, frame validation, and byte formatting.
- `modules/flash_utils.*` owns common byte stuffing and ISO-15765 flash setup.
- `src/algorithms/protocol/bytes.h` and `src/algorithms/protocol/qt_bytes.h` provide the C++20 byte boundary
  and explicit Qt conversions.
- `FlashOperationWorker` owns logging signals, prompt injection, progress,
  cancellation, and worker-thread plumbing.

The former per-module copies of `calculate_seed_key`, `calculate_payload`,
`add_ssm_header`, CRC helpers, and hex formatting should not be reintroduced.
Family-specific lookup tables remain at the call sites because they are
protocol data, not duplicate algorithms.

## Remaining opportunities

### Response validation

Many operation classes still repeat the same broad exchange shape:

```cpp
serial->write_serial_data_echo_check(output);
received = serial->read_serial_data(timeout);
if (received.length() <= minimumLength || received.at(responseOffset) != expected)
{
    // log timeout or negative response and abort
}
```

Extract family-aware response validators only when their length, addressing,
and negative-response rules are explicit. A validator should consume
`bytes::ByteView` and return a structured result rather than log, display UI,
or choose retry policy itself.

### Block planning and transfer loops

Read/write operations repeatedly compute block boundaries, retry transfers,
emit progress, and calculate throughput. Pure block planning is a good
candidate for extraction. The transfer loop should remain family-specific
until scripted tests prove that erase rules, address translation, response
opcodes, and retry semantics are genuinely identical.

### Transport boundaries

All operation headers still retain `SerialPortActions*`. New or modified
families should move protocol sessions behind `IKlineTransport`,
`ICanTransport`, or `ISsmTransport`, with port configuration and adapter
diagnostics supplied separately. This makes handshake, timeout, rejection, and
cancellation paths scriptable without a real adapter.

## Where not to generalize

Do not force all ECU families into one configurable state machine. Handshake
order, service identifiers, field offsets, erase behavior, and recovery
semantics differ across K-Line/CAN and Denso/Hitachi/Mitsubishi families. A
shared abstraction is justified only after byte-level tests demonstrate a
stable behavioral contract across concrete families.

The intended boundary is:

- Share pure byte algorithms, framing, validation primitives, block planning,
  and worker plumbing.
- Keep protocol sequence and safety policy readable within each verified
  family unless repeated behavior is proven equivalent.
