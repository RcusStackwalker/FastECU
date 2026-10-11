# MUT/DMA free-form byte order

This reference records the static firmware evidence for the maintained
[free-form codec](../../src/algorithms/protocol/mut_dma/mut_dma_freeform.cpp).
It does not qualify an adapter, electrical path, ECU session, or another firmware.
Runtime verification belongs to the
[logging engine checklist](../checklists/logging-engine-bench-checklist.md).

## Primary source

The source is the annotated Colt Z27AG / 33520003 firmware disassembly
`z27ag.S`, in the parent research archive's `docs/33520003_z27ag_mt_2006`
directory. The artifact was preserved by archive commit
`71ed1b3629f73456892189dd2b3d19aaedbba664`. Flash addresses below identify the
instructions independently of annotation line numbers.

The older parent research titled "OEM K-Line DMA Logging — Activation Control
Flow & Wire Protocol" (2026-06-07) describes stream values as big-endian.
That prose disagrees with the instructions below; the byte loads and stores
establish the free-form contract.

## Request codes

At `0x10f50`–`0x10f58`, list setup computes the request-code offset as
`2 + ceil(count / 4)`, after the packed width descriptors. At
`0x10ff0`–`0x11004`, it stores a pointer directly into the receive buffer at
that offset, without a software byte swap.

At `0x115e4`–`0x115fc`, the fill routine captures the list base, initializes
the current request pointer, and sets the second-byte offset to one.
Instructions at `0x1164c`–`0x11660` load the first byte, shift the second
byte left eight bits, and combine them:

```text
request = first_byte | (second_byte << 8)
```

The loop advances both positions by two at `0x11810`–`0x11824`.
Thus request code `0x8123` is represented by bytes `23 81`.
The request code is a native MUT-table or compact RAM selector; it is not
an absolute RAM address.

## Stream values

The ECU stores native numeric values big-endian. The ordinary multi-byte
free-form gather paths reverse their source bytes:

| Source width | Instructions | Output order |
| --- | --- | --- |
| 1 byte | `0x116b4`–`0x116c4` | `src[0]` |
| 2 bytes | `0x11718`–`0x11738` | `src[1], src[0]` |
| 4 bytes | `0x117b4`–`0x117fc` | `src[3], src[2], src[1], src[0]` |

For example, a native numeric value `0x1234` stored as `12 34` is emitted
as `34 12`. One-byte-backed sources requested at wider widths emit the
source byte followed by zeros (`0x116f4`–`0x11714` and
`0x11770`–`0x117ac`), which also requires little-endian numeric decoding.

The send path at `0x11538`–`0x1156c` appends the checksum and `0x0D` trailer
and passes the buffer to the SIO0 transmitter. Its initializer at
`0x17ed0`–`0x17ef0` starts with buffer byte zero and sets the DMA source to
buffer byte one; it does not perform a software word swap.

## Scope

The maintained codec implements this free-form byte order directly. Its
previous big-endian encoding/decoding had no recorded working-hardware evidence;
there is no compatibility dialect selector. Fixed-format DMA logging, memory
write commands, and other MUT operations have separate layouts and are not
changed by this correction.

The firmware also has element/output bounds in its gather routine. Those
firmware-specific limits require a separate capacity correction; this change
does not generalize them to every ECU or alter the current count guard.
