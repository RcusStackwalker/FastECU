# ADR 0020: ROM Memory Is a Memory Map over the ROM File

## Status

Accepted. Tracks [#119](https://github.com/RcusStackwalker/FastECU/issues/119);
slices 1-3 have landed: the memory types, and the catalog's memory maps with
agreement tests against every flash family.

## Context

A ROM file's byte positions and the ECU addresses those bytes occupy are
usually equal, but not always. The MC68HC16Y5 `_02` ROM file is 160 KiB; on the
ECU it spans 0x00000-0x2FFFF with RAM at 0x20000-0x27FFF. 1N83M ROM files
start at ECU address 0x08F9C000. Several M32R and SH families never read or
write their bootloader prefix.

Both kinds of position were bare integers, and both kinds of buffer were bare
byte vectors. The same translation was written independently in several places:
calibration padding inserted 0x8000 bytes at open, the HC16Y5 executor
re-derived physical layout from `FlashDevice` blocks, the desktop flash
workflow packed it back, and checksum routes carried an `address_offset`.
One copy, the edit-path `wrx02` relocation, tested a spelling the alias
resolver had already replaced and never ran. Another, a padding step on a
throwaway copy, was the regression PR #118's final review caught. Saving wrote
the padded buffer, silently turning a 160 KiB file into a 192 KiB one.

## Decision

- A session holds the ROM file's bytes exactly as loaded, plus a memory map: an
  ordered set of memory blocks, each covering an ECU address range and backed
  by either a ROM file range or a fill byte, and carrying whether it may be
  written. This is the model Ghidra (memory blocks over `FileBytes`) and
  Binary Ninja (segments with a data length shorter than their length) use.
- Reads, edits and checksums address bytes by ECU address through the memory
  map. File offsets exist only when loading and saving. Saving reproduces the
  layout of the ROM file that was opened.
- Each protocol in the compiled catalog ([ADR 0019](0019-compile-the-protocol-catalog-into-the-backend.md))
  declares its memory maps and names its `FlashDevice` for erase geometry.
  Following ADR 0019, flash families keep their own address windows, and tests
  prove the memory map agrees with them and with the `FlashDevice`.
- A ROM file selects a memory map by its exact size among those its protocol
  declares; a protocol declaring none gets the identity map over the file. A
  size matching no declared map is rejected, not padded. A definition's
  internal ID is checked by ECU address through the candidate's memory map.
- Definition files state addresses relative to a definition base that the
  memory map declares; parsing converts them to ECU addresses once. The base
  is 0 everywhere except 1N83M (0x08F9C000). Community WRX02 definitions
  already use ECU addresses at base 0 (Merp/SubaruDefs: no address in the RAM
  range across 192 files). No 1N83M definitions were found; the base preserves
  the file-relative convention FastECU's own 1N83M constants use.
- Regions a family never reads (bootloader prefixes) stay ROM-file-backed and
  non-writable, so full-size community files and FastECU reads share one
  memory map. Maps in non-writable blocks cannot be edited.
- Consumers that need contiguous bytes (checksum algorithms, executors that
  index by physical address) get a rendered view of an ECU address range. It
  is derived from the session and never stored back into it.
- Addresses are strong types tagged at compile time with their address space:
  `Address<FlashSpace>` now; RAM and EEPROM spaces when logging or memory
  read/write move onto the model. `FileOffset` and `ByteCount` are distinct
  types. Offsets are 32-bit, arithmetic is checked, and an address converts to
  an integer only explicitly. The types and the memory map live in the
  `algorithms` layer so checksum algorithms can use them.

Rejected:

- Tagging buffers as dense or padded, with wrapper integer types (the shape
  #119 first proposed). It keeps two buffers that can diverge and leaves the
  translation duplicated; the tag records the bug rather than preventing it.
- Converting to an address-indexed buffer at open and packing at save. It
  invents bytes for regions the file does not hold and makes every session
  carry the larger copy.
- Runtime address spaces (Ghidra's `AddressSpace` objects). ECU address spaces
  form a small closed set known at compile time, so a mismatch should be a
  compile error; nothing in a definition creates a space at runtime.

## Consequences

- "Padding" stops being an operation; a ROM file of a different size selects a
  different memory map.
- A map whose elements leave the memory map, touch a fill block, or span
  writable and non-writable blocks is a structural map failure. This replaces
  the check against the buffer length.
- Reserved bootloader regions become non-writable memory blocks, a fact the
  memory map can state and tests can check against each flash family's own
  windows.
