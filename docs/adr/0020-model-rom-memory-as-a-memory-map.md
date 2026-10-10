# ADR 0020: ROM Memory Is a Memory Map over the ROM File

## Status

Accepted and implemented ([#119](https://github.com/RcusStackwalker/FastECU/issues/119)).

## Context

A ROM file's byte positions and the ECU addresses those bytes occupy are
usually equal, but not always:
- The MC68HC16Y5 `_02` ROM file is 160 KiB. On the ECU it spans
  0x00000-0x2FFFF, with RAM at 0x20000-0x27FFF.
- 1N83M ROM files start at ECU address 0x08F9C000.
- Several M32R and SH families never read or write their bootloader prefix.

Both kinds of position were bare integers, and both kinds of buffer were bare
byte vectors. So the translation between them was written independently
wherever it was needed:
- calibration inserted 0x8000 bytes of padding at open;
- the HC16Y5 executor re-derived the physical layout from `FlashDevice` blocks,
  and the desktop flash workflow packed it back;
- checksum routes carried an address offset.

The copies drifted. One never ran, because it tested a spelling the alias
resolver had already replaced. Another padded a throwaway copy, and saving then
wrote the padded buffer, silently turning a 160 KiB file into a 192 KiB one.

## Decision

- **A ROM file is held exactly as loaded, together with a memory map.** The map
  is an ordered set of memory blocks. Each block covers an ECU address range,
  is backed by either a range of the ROM file or a fill byte, and records
  whether it is writable. This is the model Ghidra (memory blocks over
  `FileBytes`) and Binary Ninja (segments whose data length is shorter than
  their length) use.
- **Everything after loading addresses bytes by ECU address through the map.**
  Reads, edits, checksums and flash writes do. File offsets exist only when
  loading and saving, and saving reproduces the layout of the file that was
  opened.
- **Contiguous views are derived, never stored.** A consumer that needs
  contiguous bytes, such as a checksum algorithm or an executor that indexes by
  physical address, gets a view of an ECU address range rendered from the map.
  Anything it changes goes back through the map's write rules.
- **Protocols declare their memory maps.** Each protocol in the compiled
  catalog ([ADR 0019](0019-compile-the-protocol-catalog-into-the-backend.md))
  declares its memory maps and names its `FlashDevice`.
  - A ROM file selects the map whose size equals its own. A size matching no
    declared map is rejected, not padded.
  - A protocol that declares no map gets the identity map over the file.
  - Flash families keep their own address windows (ADR 0019); tests prove each
    declared map agrees with them and with the `FlashDevice`.
- **Bootloader regions a family never reads stay ROM-file-backed and
  non-writable.** Full-size community files and FastECU's own reads therefore
  share one map. Writable means "calibration may edit": exactly the addresses
  some write route of the protocol reaches.
- **Definition addresses are relative to a definition base that the memory map
  declares.**
  - The map is chosen after parsing, from the definition's flash method and the
    file's size. Definitions therefore keep definition addresses, which only the
    map converts to ECU addresses.
  - A definition's internal ID is matched by ECU address through the map the
    definition's flash method selects for the file.
  - The base is 0 everywhere except 1N83M (0x08F9C000):
    - Community WRX02 definitions already use ECU addresses at base 0
      (Merp/SubaruDefs: no address in the RAM range across 192 files).
    - No 1N83M definitions exist. The base preserves the file-relative
      convention FastECU's own 1N83M constants use.
- **Addresses are strong types tagged at compile time with their address
  space.**
  - Flash addresses are `Address<FlashSpace>`. RAM and EEPROM get their own
    space tags when logging and memory read/write adopt the model.
  - `FileOffset`, `DefinitionAddress` and `ByteCount` are distinct types.
    Offsets are 32-bit, arithmetic is checked, and a position becomes an integer
    only explicitly.
  - The types and the memory map live in the `algorithms` layer, so checksum
    algorithms can use them.

Rejected:

- **Tagging buffers as dense or padded, with wrapper integer types** (the shape
  #119 first proposed). It keeps two buffers that can diverge and leaves the
  translation duplicated; the tag records the bug rather than preventing it.
- **Converting to an address-indexed buffer at open and packing at save.** It
  invents bytes for regions the file does not hold, and makes every session
  carry the larger copy.
- **Runtime address spaces** (Ghidra's `AddressSpace` objects). ECU address
  spaces form a small, closed set known at compile time, so a mismatch should be
  a compile error; nothing in a definition creates a space at runtime.

## Consequences

- **Padding is not an operation.** A ROM file of a different size selects a
  different memory map, and supporting a new file layout means declaring a map,
  not writing a translation.
- **Placement errors are map failures.** A map whose cells or axes leave the
  memory map, touch a fill block, or span writable and non-writable blocks is a
  structural map failure, checked when the map is decoded.
- **Non-writable memory is enforced at every write.** Edits and checksum
  corrections that would change it are refused. Tests check every declared map
  against each flash family's write window and against each checksum family's
  store locations.
- **Checksum constants are ECU addresses**, and no checksum route carries an
  address offset.
- **Only the MC68HC16Y5 `_02` executor writes through the memory map.** Every
  other flash family's ROM files are already ECU-addressed, so it keeps indexing
  the file bytes. A test pins that each map those families declare keeps file
  offset = ECU address − definition base, so a map that breaks this fails a
  test instead of misflashing. Read results are ROM files, placed by their map
  when a session opens them.
