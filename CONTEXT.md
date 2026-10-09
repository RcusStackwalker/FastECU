# FastECU calibration language

Terms used when viewing and editing calibration maps.

## Language

**Map body**:
The calibration values arranged along a map's axes, excluding the axis values
and labels themselves.
_Avoid_: Entire map table

**Scaled value**:
A numeric calibration value expressed in the units defined by its scaling.
_Avoid_: Display text

**Raw value**:
A numeric value represented by the ROM storage before scaling is applied.
_Avoid_: Scaled value

**Conversion expression**:
A formula over the single input `x` that turns a raw value into a scaled value
(or a scaled value back into a raw value). Used by calibration scaling and by
logging channels.
_Avoid_: Scaling expression, from-byte expression, formula string

**Invalid numeric cell**:
A calibration cell whose current ROM content cannot yield a valid scaled value.
Its current value is unavailable, but a replacement may still be encodable.
_Avoid_: Uneditable cell

**Structural map failure**:
A map whose layout does not establish valid storage locations for its cells.
_Avoid_: Invalid numeric cell

**Selectable map**:
A map whose body is one named blob chosen from a fixed list of selections,
rather than a grid of numeric values.
_Avoid_: Switch, option map

**Selection**:
One named blob value in a selectable map's list. Choosing a selection writes
its bytes to the map's address. Every selection in a list is the same width.
_Avoid_: Option, switch state

**Unchanged edit**:
An edit whose resulting bytes equal the bytes already in the session. It
reports no change and leaves the session clean.
_Avoid_: No-op write

## ROM memory

**ROM file**:
The bytes of a ROM exactly as stored on disk. Saving a session reproduces the
layout of the ROM file it was opened from.
_Avoid_: Dense buffer, raw image

**File offset**:
A byte position within a ROM file. Meaningful only when loading and saving.
_Avoid_: Address

**ECU address**:
A location in the ECU's own address space, as the ECU's CPU sees it. Map, axis
and checksum locations are ECU addresses.
_Avoid_: Offset, physical offset

**Memory map**:
The ordered set of memory blocks that places a ROM file's bytes at ECU
addresses. Two ROM files of one ECU can need different memory maps.
_Avoid_: Padding, sparse buffer, memory model

**Memory block**:
One contiguous range of ECU addresses in a memory map, backed either by a range
of the ROM file or by a fill byte, and carrying whether it is writable.
_Avoid_: Segment, region, flash block (an erase unit of the flash device)

**Writable**:
Of a memory block: the protocol can write it to the ECU by some route. Maps in
a block that is not writable cannot be edited.
_Avoid_: Protected, userspace

**Definition base**:
The ECU address that address 0 in a definition file stands for, declared by the
memory map. Zero for almost every ECU.
_Avoid_: Address offset, image base

**Fill block**:
A memory block with no ROM file bytes behind it; every address in it reads as
its fill byte.
_Avoid_: Gap, hole, synthetic bytes, padding

## Terminal scripts

**Terminal script**:
The ordered list of steps the Data Terminal runs against one connection.

**Message step**:
A step that sends one frame and reads its response.

**Delay step**:
A step that only pauses; it sends nothing, and consecutive delay steps add up.
_Avoid_: Response wait
