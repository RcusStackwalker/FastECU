# Logging wire evidence

This document owns physical read layout and dialect boundaries. It complements
[logging contracts](logging-contracts.md) and the
[composition qualification checklist](../checklists/logging-composition-bench-checklist.md).
Static evidence is not hardware qualification or per-measurement support evidence.

## SSM ordered byte reads

Primary sources are pinned to RomRaider commit
`dafe0c36c1a68efadbeedb2825f3855463fdbc35`:

- [Protocol constants](https://github.com/RomRaider/RomRaider/blob/dafe0c36c1a68efadbeedb2825f3855463fdbc35/src/main/java/com/romraider/io/protocol/ssm/iso9141/SSMProtocol.java#L57): three-byte addresses and one returned byte per address.
- [Address expansion](https://github.com/RomRaider/RomRaider/blob/dafe0c36c1a68efadbeedb2825f3855463fdbc35/src/main/java/com/romraider/logger/ecu/definition/EcuAddressImpl.java#L78): ordered base-plus-length expansion; multiple explicit addresses when the list exceeds the declared expansion length.
- [Independent packet example](https://github.com/RomRaider/RomRaider/blob/dafe0c36c1a68efadbeedb2825f3855463fdbc35/docs/ssm_info.txt#L108): request `80 10 f0 08 a8 01 00 00 08 00 00 1c 55`, response `80 f0 10 03 e8 7d b1 99`.
- [Definition handler](https://github.com/RomRaider/RomRaider/blob/dafe0c36c1a68efadbeedb2825f3855463fdbc35/src/main/java/com/romraider/logger/ecu/definition/xml/LoggerDefinitionHandler.java): capability metadata and nested sample address/bit are separate.

Addresses `0x000008` and `0x00001c` correspond to response positions 0 and 1.
Display slots never determine response positions. Each requested address consumes
one of the maintained 84 request entries. Multi-byte measurements require ordered
byte requests. Historical decimal-byte concatenation remains a conversion rule:
bytes `01 02` supply raw expression input `"12"` independently of physical mapping.

Continuous SSM responses can arrive consecutively or coalesced in a serial read.
The plain transport path consumes one declared frame at a time, retains subsequent
bytes, and checks that frame against the captured read plan and checksum.

## MUT/DMA: maintained format and analyzed OEM firmware

The [maintained free-form codec](../../src/algorithms/protocol/mut_dma/mut_dma_freeform.cpp)
encodes request codes and decodes multi-byte stream values in big-endian order.
Its one-byte count field can represent 255 entries; that is a representation
limit, not demonstrated ECU capacity.

The parent OEM research is titled **OEM K-Line DMA Logging — Activation Control
Flow & Wire Protocol**, dated 2026-06-07. Its primary extraction is the annotated
33520003 Z27AG firmware assembly, parent research path
`externals/my-old/docs/33520003_z27ag_mt_2006/z27ag.S`. The instruction addresses
below identify the evidence without treating an external checkout as a portable
Markdown link.

| Firmware addresses | Observed contract |
| --- | --- |
| `0x10f50`–`0x10f78` | Packed width descriptors start at RX byte 2; request words follow `ceil(count/4)` descriptor bytes. |
| `0x11004` | Request pointer directly references the received buffer after the descriptors. No intermediate byte swap is shown. |
| `0x1164c`–`0x11660` | Request code is `buf[i+1] << 8 | buf[i]`: code `0x1234` requires bytes `34 12`, unlike maintained `12 34`. |
| `0x11844` onward | Low request codes resolve through the MUT pointer table; other codes use compact RAM mapping with clamping. Returned data does not validate measurement meaning. |
| `0x11600`–`0x11620` | Fill loop bounds element index and output offset below 96. |
| `0x116dc`–`0x116e4`, `0x11758`–`0x11760` | Two-byte output starts below offset 95; four-byte output starts below offset 93. Thus aggregate complete output is at most 96 bytes. |
| `0x11718`–`0x11738`, `0x117b4`–`0x117fc` | Normal two/four-byte values emit memory bytes in reverse order: `src[1],src[0]` or `src[3],src[2],src[1],src[0]`. Request order and stream order both need characterization. |
| `0x10f34`, `0x10fec` | RX buffers have 244-byte stride. Maintained list framing `ceil(n/4)+2n+28` reaches 244 bytes at n=96. This corroborates a scoped request limit, not every MUT firmware's capacity. |

Special pointer-table sources can follow firmware single-byte expansion branches;
choosing a width does not prove the source's meaning. Do not generalize these
properties to another firmware or silently flip the maintained generic format.
Protocol XML `dialect="oem-33520003"` selects these scoped properties.
Omitted dialect or `dialect="legacy-be"` selects the maintained format; unknown
explicit dialect names fail selected MUT preparation. The validated session
carries that choice through the desktop factory to request encoding and decoding.
No ROM matching or automatic capability discovery is established.

## Complete response integrity

The [MUT driver](../../src/backend/protocol/mut_dma_driver.cpp) compares complete
stream data length with selected widths before decoding. The free-form decoder
also returns no values for incomplete payloads, preventing the historical
checksum-valid short-frame path from fabricating zero readings.

Both protocols validate the complete expected response shape before
publishing any values from that poll. A malformed/truncated response is
`BadResponse`; absent data retains existing polling/retry policy. Conversion
failure also publishes none of the batch. Literal independent fixtures and
boundary tests prove host behavior; bench evidence remains required for adapter,
electrical, firmware, and end-to-end qualification.
