# FastECU calibration language

Terms used when viewing and editing calibration maps.

## Language

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
