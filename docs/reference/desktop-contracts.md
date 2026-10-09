# Desktop contracts

Read the section relevant to the task. These are current compatibility and
lifetime contracts; architectural rationale is in the
[desktop design notes](../design-notes.md#desktop-composition).

## Composition lifetime

[DesktopComposition](../../apps/desktop/desktop_composition.h) is declared before
`MainWindow` inside the restart loop, so each restart rebuilds both. Borrowed
services outlive their consumers. Workspace/ROM opening stop before the definition
catalog, which stops before its borrowed service and configuration. Logging stops
before its clock/facade; remote services stop before serial release. The syslogger
thread is quit and joined after its dependents stop.

Composition links configuration and kernel resource registration explicitly.
The [serial factory](../../src/platform/desktop/common/serial/desktop_serial_factory.h)
exposes construction through an owner with a custom deleter and keeps the facade
incomplete to the application. Its string-based signal connections are checked
by the factory tests because connection failure is only visible at runtime.

An empty `--host` selects direct serial; a supplied host selects remote serial.
The facade receives a backend factory and owns no platform-selection policy.
The application binary selects the per-OS direct implementation. Each moc header
has one owner, including the shared direct-backend header; see
[ADR 0017](../adr/0017-generate-bazel-targets-with-gazelle.md).

Facade headers are platform-only; adapters forward-declare the facade and use
`implementation_deps` to hide its headers from production UI compile inputs.
Linux/macOS sandboxed builds enforce that header boundary; Windows alone cannot
prove it. UI test harnesses intentionally expose concrete fake backend headers.
The `STATUS_*` macro names collide with Windows SDK names if converted in place
to constexpr identifiers; any typed replacement needs collision-free names.

## Definition catalog lookup

[DefinitionCatalogSession](../../src/backend/definition/definition_catalog_session.h)
implements the portable catalog interface, shares `DefinitionService` with ROM
opening, and uses injected filesystem/event/configuration services. Dialogs own
operator decisions; successful writes alone register authored destinations.

Startup lookup provenance and fresh catalogs have separate lifetimes. A fresh
scan does not replace retained lookup: deletion of an indexed file after startup
must still produce the established ROM-open notice. Explicit refresh replaces
one format's lookup only on success. Empty configured sources retain it; a
successful scan skipping every unusable file clears it.

Retained lookup uses ordered ID/source records with first-match behavior, rather
than `DefinitionCatalog`, whose validation rejects conflicting duplicate identities.
Authoring appends a lookup record and remembers destinations outside the configured
directory. A directory change drops prior discovery on refresh while preserving
authored handles.

## Configuration session

[ConfigSession](../../src/backend/config/config_session.h) initializes before
other composition services. Its saved vehicle ID is the only selection state;
make, model, MCU, checksum support, capabilities, and description are derived from
the selected catalog vehicle. Catalog order is retained; choosers sort presentation
only. Alias resolution takes the first match and ROM-open selection the last, so
new vehicle entries append. Protocol-name selection takes the last matching row.

Provisioned paths are fixed for the run. Effective paths override only calibration
and datalog directories from settings. Defaults provision through resource bytes
and repository ports, preserving existing user files.

Provisioning failure or unreadable/malformed `fastecu.cfg` reports path and reason
and exits before the window, syslog thread, or ECU I/O. A failed rewrite after a
successful load is a nonfatal startup warning. An unknown/missing vehicle ID
selects nothing; the startup gate asks before constructing the window, and Cancel
exits with code 0. The datalog directory's non-round-tripping key mismatch remains
an unresolved item in [technical debt](../tech-debt.md).

Catalog facts are checked independently against plans, checksum routes, resources,
and workflow capabilities; see [ADR 0019](../adr/0019-compile-the-protocol-catalog-into-the-backend.md)
and the [consistency test](../../tests/catalog_consistency_test.cpp).

## Connection and identification

[ConnectionCoordinator](../../src/ui/desktop/connection/connection_coordinator.h)
owns generation fences, per-attempt continuations, and control lock/unlock order.
The launcher supplies the worker; the window owns the port-open preamble,
disconnect, refresh, and logging continuation. Identification uses the facade
exclusively: Connect first joins logging; controls and ECU/TCU selection stay
locked through capability parsing/logging continuation, and battery reads skip
sampling. Other facade entry points cancel and join identification first.

Cancelled identification unlocks the port selector while leaving the port open
without an identified ECU. Its pending continuation receives `false`
synchronously before the cancelling caller uses the facade; it must not start
a new connection synchronously. A pending logging start reports "Unable to
connect to ECU". Stale queued completions are dropped by generation. Nested
notices that start/cancel another attempt invalidate the older continuation;
an older completion cannot unlock a newer running attempt.

Opening a port, identifying a unit, and reporting callback success are distinct.
When identification fails, the presentation disconnects logically and resets UI
state. The callback still reports `true` for that failed-identification outcome
because the port opened; it does not assert that an ECU was identified. Explicit
cancellation reports `false`. Non-Subaru/raw-CAN paths skip SSM identification.

Refresh clears link flags but leaves parity; disconnect resets baud/parity and
retains flags so the next CAN connect does not accidentally open as K-Line.
ISO-15765 selects 29-bit IDs and raw CAN 11-bit IDs. SSM2 validates header `0x80`,
tester `0xF0`, requested target, `0xFF`, and checksum, then ignores bytes beyond
declared length. SSM1 retains length-only validation, bounds its trailing drain
to 100 reads, and ignores trailing frames too short for an ID. Writes are
echo-checked. K-Line opens set parity explicitly. CAN uses UDS `22 F1 82` without
capability bits; SSM `AA` work is tracked in [technical debt](../tech-debt.md#p2-identify-subaru-can-ecus-with-ssm-aa).

The [connection checklist](../checklists/connection-bench-checklist.md) owns
qualification of these paths, including BIU port persistence and empty-port warnings.

## Diagnostic tools

`IDiagnosticLink` leaves framing to the facade/caller. It exposes `uses_j2534()`
because response interpretation and `read` versus `read_obd` differ by adapter;
only the equivalent P1-max effect is hidden. See
[OBD frame helpers](../../src/backend/diagnostics/obd_frames.h).

BIU and DataTerminal perform bounded synchronous exchanges (800 ms and 200 ms
reads); their keep-alive/timer ownership stays desktop-side. DTC uses a worker for
its multi-exchange sequence. Cancellation is best-effort inside sleep/read calls:
an already-cancelled run still performs one open/init exchange, and dialog close
joins the worker. Fast-init open failure still falls back to five-baud; a run
ends disconnected if that path's reopen also fails. Each run starts clean, and a
failed run logs one error. DataTerminal resets flags before opening; BIU opens
at 10400 and saves the selected port.

K-Line unframing retains total-length heuristics: data replies under 7 bytes keep
the last byte, under 10 drop 5, and others drop 6. DTC-list replies under 7 keep
the last byte and others drop 4. The ISO-15765 init NRC description starts at
offset 3; other NRC descriptions use response index 4. Five-baud ASCII comparison
and terminal delay parsing are unresolved [technical debt](../tech-debt.md).
Qualification is recorded in the [diagnostics checklist](../checklists/diagnostics-bench-checklist.md).

## UI channels

UI-owned `LogChannel` and `RemotePeer` expose signals; composition wires platform
services to them. Timestamp/linefeed flags remain part of the desktop log contract.
Signals retain the names `logE`, `logW`, `logI`, and `logD`: the logger reads
the delivering signal's name to choose level. UI signals relay signal-to-signal
through the long-lived channel so logs survive destruction of short-lived dialogs.

`RemotePeer::wait_for_source` connects to the remote utility with
`Qt::DirectConnection`, retaining its blocking behavior. Headless composition
tests verify the connection, not completion of a wait requiring a live peer.
