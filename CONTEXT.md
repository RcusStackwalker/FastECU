# FastECU domain language

Terms used when viewing and editing calibration maps, running terminal scripts,
and logging vehicle measurements.

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

## Terminal scripts

**Terminal script**:
The ordered list of steps the Data Terminal runs against one connection.

**Message step**:
A step that sends one frame and reads its response.

**Delay step**:
A step that only pauses; it sends nothing, and consecutive delay steps add up.
_Avoid_: Response wait

## Logging

**Logging run snapshot**:
The fixed choices and measurement definitions captured for one logging run,
including its protocol, ECU/TCU target, selected channels, and support status.
Later edits apply to subsequent runs.
_Avoid_: Live logger configuration
