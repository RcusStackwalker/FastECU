# Logging selection and ECU support policy

Status: design confirmed on 2026-10-08; implementation authorized after plan
review. Enduring contracts are being extracted before final verification and review.
This proposal extends the behavior preserved by the portable logging preparation
migration; it is not part of that implementation's published PR.

## Agreed principles

Every measurement selected for a gauge, Digital display, or switch needs an
acquisition source. Repeated display positions for the same measurement share
acquisition rather than requesting it repeatedly.

Definition enablement and ECU support are distinct facts. ECU support can be
unknown. A definition's enabled flag is not proof that a connected ECU provides
its measurement. The [domain glossary](../../../CONTEXT.md#logging) owns these
terms.

## Agreed follow-up behavior

Preserve selected entries that are known unsupported or unresolved, show their
unavailable state, and prevent startup with actionable errors until the operator
removes or replaces them. Acquisition must not silently omit a selected value.

Share repeated measurement identities across display positions. Keep parameter
and switch namespaces distinct, and keep distinct definitions as separate
acquisitions until protocol-specific physical-read equivalence is established.

Include a working switch indicator/update path and acquisition/cache/CSV values
for gauge selections. Construction of a new gauge renderer is a separate
feature.

## User-authored MUT/DMA measurements

The operator may supply the request address/code or ID, scaling, and unit and
is trusted to know what those fields mean for the ECU. Do not require a
firmware-matched definition/profile or automatic capability discovery before
accepting that configuration. Unknown ECU support does not exclude an explicitly
selected user-authored MUT measurement.

Keep syntax, width, expression, and aggregate request validation. User trust does
not establish per-measurement capability evidence or change address-window guards.
Setup ACKs and returned values remain session/data observations, not proof of
measurement meaning. Definition enablement supplies default eligibility and is
separate from known or unknown ECU support.

## Active and pending selections

Acquisition, active display bindings, and CSV columns use the run snapshot for
the entire run. Editing selections saves choices for the next run, without
replacing the active display bindings or CSV schema. Show that a restart is
needed to apply pending choices.

## CSV lifetime

Every new logging run starts a new CSV file when file logging is enabled,
including a restart after Connect or any other worker/session end. Never append
a new run to the previous run's file, even if columns, protocol, target, and
selections are unchanged. The columns stay fixed for the run; pending selection
edits apply only to the next run's file.

Stopping a run ends that run's file ownership. Preserve its completed output;
create the next file using a collision-safe name rather than overwriting it.
Existing controls for enabling/disabling file logging retain their role.

## XML authoring scope

Use the existing logger XML file flow for this design. An in-app Add/Edit
measurement form is follow-up debt, not part of the acquisition/support change.
The [XML README](../../../resources/shared/config/README.md) owns practical file
creation, loading, restart, selection, and troubleshooting instructions.

Keep the existing definition schema: stable parameter ID, name, native MUT
16-bit request code in `address`, width 1/2/4, first arithmetic conversion of
`x`, units, and fixed display format. Do not implicitly translate or truncate
full absolute RAM addresses. Definitions contain measurement fields; `logger.cfg`
contains only saved selection IDs. Operator-authored XML supplies identity and
persistence for this scope.

## Baseline implementation and contract differences

The [logging contracts](../../reference/logging-contracts.md) describe the current
migration's behavior. Its [run preparation](../../../src/backend/logging/logging_run_snapshot.cpp)
uses only Digital/lower-panel IDs to add polling channels. Gauge and switch
selections are captured as metadata. The chooser hides entries based on model
support flags, while stored selections and displayed boxes can retain excluded
entries.

Model support flags initially copy parameter XML enablement. The production
capability-update path targets SSM; MUT/DMA has no equivalent discovery in this
implementation. Consequently, MUT/DMA's current support predicate represents
its definition enablement rather than confirmed ECU capability.

Switch definitions carry a byte and bit, but the current logging path has no
switch sample/cache update. Its switch widget displays the title without the
value. Gauge selection and CSV fields exist without a gauge renderer in the
current desktop sources. Acquiring the union of selected IDs alone does not
finish those display paths.

## Confirmation

The user confirmed the complete design on 2026-10-08. Operator policy decisions
are settled; implementation planning must include the engineering prerequisites.

Protocol read layout, capacity, and qualification are engineering prerequisites
below; unknown wire behavior is not a policy choice for the operator.

## Evidence and boundaries

UI [chooser filtering](../../../src/ui/desktop/widgets/logvalues.cpp) and backend
[session validation](../../../src/backend/logging/logging_session.cpp) remain
separate responsibilities. UI selection changes can leave invalid combinations;
backend startup validation is required regardless of UI assistance.

FastECU currently bounds MUT/DMA to 255 entries because its channel-count field
is one byte; widths are 1, 2, or 4. This is not established ECU capacity. The
analyzed 33520003 firmware bounds its fill loop to 96 elements and 96 output
bytes. SSM permits 84 requested address entries; CDBG packs variable-width reads
into eight frames with seven data bytes per frame. Display position count is not
universally the wire entry count. Protocol-specific equivalence and response layout must be
established before sharing raw reads or changing SSM response offsets.

Hardware-facing changes follow the [qualification matrix](../../flash-qualification-matrix.md)
and [logging composition checklist](../../checklists/logging-composition-bench-checklist.md).
No capability discovery or wire qualification is established by this proposal.

## MUT/DMA availability evidence

The OEM static research referenced by the
[logging evidence owner](../../reference/logging-contracts.md#mutdma-protocol-evidence)
traces `get_mut_pointer` in the annotated 33520003 Z27AG assembly. A free-form
request is a 16-bit code: low codes resolve through the ROM's MUT pointer table;
other codes resolve through a compact RAM-address mapping. The current XML
`address` field does not distinguish those meanings. Setup and channel-list ACKs
contain no per-measurement capability result, and a returned numeric value does
not establish that the requested bytes represent the intended measurement.

The same firmware's fill loop at flash `0x11600` checks element index and output
offset against 96. Its request assembly at `0x1164c`–`0x11660` also differs from
the maintained codec's big-endian ID interpretation. These are characterization
questions, not authorization to change framing or address-window guards. The
[technical debt owner](../../tech-debt.md#p1-resolve-known-correctness-gaps) records
the capacity and request-layout investigation.

## Engineering prerequisites

The [SSM protocol evidence](../../reference/logging-contracts.md#ssm-protocol-evidence)
establishes one data byte per requested three-byte address. Multi-byte values
need ordered byte-address requests and explicit mapping of returned bytes to logical values. The current slot-offset
implementation and fixtures do not establish that mapping after unioning or
filtering selections. SSM's 84-entry limit counts requested byte addresses.
Preserve decimal-byte-concatenation conversion separately from wire layout.

Normalize imported switch definitions with separate capability metadata and
sample-byte/sample-bit metadata. The current parser uses one bit field for both
roles and does not read standard nested switch addresses. Characterize legacy
and standard XML forms, reject conflicting metadata, and test the read-to-bit
mapping before adding switch values to the sample/cache/indicator path.

Characterize MUT request byte order and response capacity for the evidenced
firmware before changing its requests or budgets. No ROM-match requirement is
introduced for user-authored measurements. Do not convert unknown capability,
failed exchange, or an arbitrary returned value into a claim of known support.
Qualification remains owned by the existing logging checklists.

## Acceptance evidence

Prove that gauge-only and switch-only selections acquire and update their logical
values; repeated display identities share acquisition; parameter and switch
namespaces remain separate; and unsupported/unresolved selections fail with
useful errors. User-authored MUT measurements with unknown support must remain
selectable without a ROM-match requirement.

Pin physical request layouts and response-to-value mapping with independent
protocol fixtures, including multi-byte SSM values, missing-selection gaps,
switch sample/capability metadata, frame capacity, and partial replies. New-run
CSV rows must contain only that run's readings, without carrying cached values
from previous runs or fabricating missing samples as zero.

Cover pending edits without active display/CSV changes, and verify that every
new run gets a distinct CSV file with its own frozen columns, including identical
restarts and restart after Connect. Exercise rapid restarts and filename
collisions so completed output is never overwritten. Existing logging-worker
lifetime, cancellation, error, and stale-event guards remain required.

## Approved MUT dialect amendment

Approved by the user on 2026-10-08 during the wire evidence gate. Add an explicit
protocol XML attribute `dialect="oem-33520003"` for the analyzed OEM format;
omitted dialect or `dialect="legacy-be"` retains the maintained format. Unknown
explicit dialects fail selected MUT run preparation with an actionable error.
Dialect selection trusts the operator and requires no ROM matching.

The OEM dialect uses little-endian request codes and streamed two/four-byte
values, at most 96 request entries and 96 response data bytes. The maintained
format retains its big-endian order and 255-entry representation guard; no ECU
capacity claim follows from that guard. Both reject checksum-valid incomplete
payloads. Neither changes request-code meaning or address-window guards.
Source scope and firmware-specific expansion behavior are recorded in the
[wire evidence owner](../../reference/logging-wire-evidence.md).
