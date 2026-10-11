# FastECU flash language

Terms used when describing which flash families share protocol behaviour.

## Language

**Flash family**:
One `FlashFamily` value: a plan builder and an executor for one ECU or TCU
model line over one transport.
_Avoid_: Wave, step, protocol (a protocol id such as `sub_ecu_denso_1n83m_1_5m_can`
names one flash family's catalog entry)

**Dialect**:
Two or more flash families that share wire-level behaviour, proven by shared
code and tests for named operations (seed/key, erase, an in-car sequence).
Each flash family is in at most one dialect. A flash family that shares nothing
beyond data tables is in no dialect.
_Avoid_: Cluster, wave, clone group

**Shared protocol data**:
Constants several flash families use, named by the artifact rather than by a
dialect: for example the Denso ISO-15765 seed/key and payload tables, which
the bootloader dialect and the kernel-upload dialect both use.
_Avoid_: Common dialect

**Denso ISO-15765 bootloader dialect**:
The N83M 1.5M, N83M 4M, SH72531 and SH72543 diesel flash families. They share
the level-0x61/0x62 seed/key exchange and the erase flow, and both N83M
families share the in-car exchange sequence. They talk to the ECU's resident
on-board kernel after a diagnostic-session jump and upload no kernel.
_Avoid_: Wave 4

**Denso BEEF CAN dialect**:
The SH7058 petrol, SH7058 diesel and TCU Denso SH705x flash families. They
share the BEEF-protocol helpers in `denso_beef_can_common.h`; connect, flash
block, kernel upload, write and read flows stay in each executor. The
DensoCAN SH705x family uses the same seed/key and payload tables but none of
those helpers, so it is in no dialect.
_Avoid_: Wave 5

**Bootloader**:
Code resident on the ECU that accepts a flash session over the diagnostic
protocol. The host uploads nothing to reach it.
_Avoid_: Boot mode (the ECU is forced into a state where the host must upload
a kernel; see the boot-mode kernel and program flash families)

**Bench arm** / **In-car arm**:
The two programming branches of a bootloader flash family: the ECU powered on
a bench harness versus installed in the vehicle with other modules on the bus.
The in-car arm quiets those modules first.
_Avoid_: Mode, variant

**Transfer progress**:
The speed (B/s) and estimated seconds remaining of a read or write loop,
reported as one log line per page. The estimate is clamped to 9999 s.
_Avoid_: Telemetry, throughput
