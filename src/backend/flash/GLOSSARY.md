# FastECU flash language

Terms used when describing which flash families share protocol behaviour.

## Language

**Flash transport source**:
The connected adapter as the flash workflow sees it: it provides unopened
K-Line, CAN or mixed-CAN flash transports and reports whether it supplies
programming voltage itself.
_Avoid_: Transport factory, flash adapter

**Flash family**:
One `FlashFamily` value: a plan builder and an executor for one ECU or TCU
model line over one transport.
_Avoid_: Wave, step, protocol (a protocol id such as `sub_ecu_denso_1n83m_1_5m_can`
names one flash family's catalog entry)

**Flash route**:
The exact protocol ids that select one flash workflow, and how that workflow is
created. Every protocol the catalog can flash (one that reads, test-writes or
writes) has exactly one flash route; a protocol id is never matched by prefix.
_Avoid_: Route pattern, prefix route, dispatch entry

**Plan admission**:
The checks an executor makes before any family code runs: the plan belongs
to this family, passes its family validator, and the run has not been
cancelled.
_Avoid_: Preamble, executor shell

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
The N83M 1.5M, N83M 4M, SH72531 and SH72543 diesel flash families. They talk
to the ECU's resident on-board kernel after a kernel jump and upload no
kernel. Their wire behaviour from connect through close is shared operation by
operation where it is proven identical; each family's remaining deviations are
its own, stated in its dialect profile.
_Avoid_: Wave 4

**Dialect profile**:
The per-family facts a dialect runs with: the family's timings, log text,
reply strictness and image placement. Every deviation a family has from its
dialect siblings is a value in its profile; the order of operations belongs to
the dialect.
_Avoid_: Family config, variant, settings

**Kernel jump**:
The diagnostic-session request that hands a bootloader flash family's ECU over
to its resident on-board kernel, ending the connect.
_Avoid_: Kernel upload (that sends a kernel; a kernel jump sends none)

**Jump continuation**:
The bounded wait after a kernel jump for the ECU to acknowledge it. Running
out of attempts is logged but does not fail the connect.
_Avoid_: Retry loop, init wait

**Tolerant probe**:
An exchange whose wrong reply is logged and stepped over, but whose absent or
too-short reply aborts the operation.
_Avoid_: Non-fatal query (that never aborts), soft check

**Tolerant reply**:
An exchange whose wrong or absent reply is logged and stepped over; only a
failure of the transport itself aborts the operation.
_Avoid_: Tolerant probe (an absent reply aborts that), non-fatal query (that
also steps over transport failures)

**Required reply**:
An exchange whose absent reply is a timeout and whose short or wrong reply is a
bad-response error, worded per peer (ECU or TCU). A present empty reply is
still a reply. The counterpart of a Tolerant reply; the M32R SSM kind lives in
[m32r_ssm_exchange.h](ecu/m32r_ssm_exchange.h).
_Avoid_: Strict reply, mandatory exchange

**Fire-and-forget run**:
The in-car arm's opening writes to the other modules on the bus, whose replies
are read and discarded without inspection.
_Avoid_: Quiet sequence, broadcast

**Denso BEEF CAN dialect**:
The SH7058 petrol, SH7058 diesel and TCU Denso SH705x flash families. They share
the warm-kernel owner `BeefCanKernel`. Petrol and diesel share the fixed ECU
cold-path owner `RunDensoBeefCanEcu`; the TCU executor owns its separate cold
path. The [sharing contract](../../../docs/reference/flash-contracts.md#denso-beef-can-kernel)
owns their exact shared operations, deviations and transcript evidence.
Dialect membership does not assert one complete cold-path order. DensoCAN
SH705x uses the same seed/key and payload tables but none of the BEEF CAN
conversation, so it is in no dialect.
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

**CVT B6 chunk dialect**:
The Subaru TCU CVT Hitachi M32R CAN and Mitsubishi MH8111 CAN flash families.
They share only the B6/U24 encrypted-chunk exchange in
[cvt_b6_chunk_exchange.h](ecu/cvt_b6_chunk_exchange.h): one send and one
2000 ms receive, accepting silence or any framed reply. Geometry, encryption,
cancellation checkpoints, progress, setup, close and checksum stay family-owned.
Mitsubishi CVT crypto sharing alone does not define this dialect.

**Guarded transport**:
A flash transport boundary that refuses new I/O when it observes cancellation.
Results of accepted operations retain their meaning; each flash family keeps
its own protocol success, failure, and continuation policies.
_Avoid_: Session, session wrapper (a session is a UDS diagnostic session)

**Critical section**:
A span a flash family declares in which cancellation is deferred, because
interrupting it would leave the ECU unbootable. Cancellation is reported when
the span ends.
_Avoid_: Atomic step, high-risk step

**Expected silence**:
An exchange a flash family declares as allowed to get no reply. Silence there
is not a timeout.
_Avoid_: Tolerated timeout

**Wire transcript**:
A recorded file of the frames a flash family writes and reads for one
operation, with timing where the timing is part of the wire contract.
Replaying it is equivalence evidence, not hardware evidence.
_Avoid_: Trace, log, script

**Plan golden**:
The expected flash plan for a family and protocol, stated as literal data in
a test.
_Avoid_: Snapshot
