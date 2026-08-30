# Desktop Quick Editor and Persistence Design

## Purpose

Add the first editable document workflow to the Qt Quick desktop application.
Users can import a CDBG logger definition, configure and reorder dashboard
cards in a persistent side panel, preview changes immediately, save `.ohd`
documents atomically, and restore the last successfully opened document after
restart.

This is delivery-sequence item 7 from the
[desktop Quick dashboard design](2026-08-24-desktop-quick-dashboard-design.md).
Operating-system `.ohd` file associations remain deferred to the packaging and
hardening phase.

## Scope

This slice includes:

- import-first document creation from a CDBG logger definition;
- Open, Save, Save As, and recent-document restoration;
- a persistent editor panel beside the live dashboard;
- adding cards only from imported channels and their embedded conversions;
- changing card title, conversion, display type, and type-specific settings;
- removing cards and reordering them with Move Up and Move Down controls;
- immediate in-memory preview with explicit persistence;
- authoritative dirty-state and unsaved-change handling; and
- disabling document replacement and mutation while connected.

This slice excludes:

- blank dashboard creation;
- drag-and-drop reordering;
- editing channel definitions or conversions;
- live reconfiguration of an active logging session;
- operating-system file associations and packaging integration;
- diagnostic export, hardware qualification, and other hardening work; and
- any ECU write, flash, or actuator behavior.

## Architecture

### Dashboard document controller

Introduce a `DashboardDocumentController` in the desktop Quick dashboard
package. It owns the authoritative in-memory `DashboardDocument`, optional
file path, dirty flag, selected card identity, and document-transition state.
It coordinates import, open, save, save-as, close, and startup restoration
through existing portable dashboard services and injected ports.

The controller exposes typed commands and observable state to QML. QML never
owns or directly mutates the domain document. The controller does not own
transport, acquisition, conversion, or logging-session policy.

The last successfully opened or saved document path is persisted through
`ISettings`. File reads use `IFileRepository`; writes continue through
`DashboardDocumentService` and `IAtomicFileWriter`.

### Dashboard editor model

Introduce a `DashboardEditorModel` that projects the controller-owned document
into editor choices and commands. It exposes:

- imported channels in document order;
- each channel's embedded conversions;
- the selected card and its editable fields;
- the three portable display types;
- type-specific gauge and sparkline settings; and
- whether Add, Remove, Move Up, and Move Down are currently available.

Every edit is expressed as a typed C++ operation. An operation builds a
candidate document, validates and prepares it, and commits it only on success.
The editor model uses stable card IDs for selection, so selection follows a
card when it moves. Removing the selected card chooses the next card at the
same index, or the preceding card when the removed card was last.

### Presentation integration

The existing presentation/card model remains the preview surface. After a
successful mutation, the controller rebuilds the prepared dashboard session
and refreshes presentation state from the committed document. This preserves
one authoritative document while keeping editing rules outside QML.

Document replacement and mutation commands are disabled whenever the
connection state is not disconnected. The user must disconnect before
editing, importing, or opening another document. No active logging session is
reconfigured in place.

## Document lifecycle

### Import

Import is the only version-1 New Dashboard workflow. The user selects a CDBG
logger definition, and the existing portable importer creates an unsaved
document containing the imported channels and conversions. The imported
document has no file path and is dirty immediately.

Importing while another document is dirty first enters the shared
unsaved-change flow. A failed or cancelled import leaves the current document,
path, dirty state, selection, and preview unchanged.

### Open

Open reads, parses, validates, and prepares an `.ohd` before replacing current
state. A successful open installs the document and path, clears dirty state,
selects the first card when present, refreshes the preview, and records the
path as the recent document. A failed or cancelled open changes none of those
values.

### Save and Save As

Save writes the current document to its existing path. An untitled imported
document routes Save through Save As. Save As chooses a path and writes the
same in-memory document without changing its identity or selection.

The controller clears dirty state and records the recent path only after the
atomic write succeeds. Cancellation or failure preserves the previous path
and dirty state. A failed Save As does not adopt the requested path.

### Recent-document restoration

At startup, the application attempts to open the last successfully opened or
saved path. Successful restoration behaves like a clean Open. A missing,
unreadable, invalid, or unusable recent document produces a recoverable UI
error and leaves the application in its empty Import/Open state. The stored
path remains available for diagnostics but is not retried during the same
process unless the user explicitly opens it.

### Unsaved-change state machine

Open, Import, and application Exit use one continuation-based unsaved-change
flow:

- **Save:** persist the current document, using Save As when it has no path;
  continue the original action only after a successful save.
- **Discard:** continue the original action without persisting the current
  document.
- **Cancel:** abandon the original action and retain current state.

Only this unsaved-change decision is a blocking dialog. File pickers may be
native modal dialogs, but ordinary validation and I/O errors are presented as
non-destructive actionable notifications.

## Editing behavior

The persistent side panel sits beside the dashboard so every accepted edit is
visible immediately. It contains document actions and a selected-card editor.

Add Card requires an imported channel and one of that channel's embedded
conversions. It creates a new stable card ID and appends the card after the
current last card. Missing channels never produce placeholder cards.

The editor permits:

- selecting an imported channel and one of its conversions;
- editing the card title;
- selecting Numeric, Sparkline, or Horizontal Gauge;
- editing gauge minimum, maximum, and step overrides;
- editing sparkline history duration; and
- removing or moving the selected card by one position.

Changing a card's channel resets its conversion to the first conversion on
that channel. Changing display type retains portable configuration fields in
the document so switching back restores prior valid settings; fields
irrelevant to the active type are hidden in the panel but remain validated.

Every accepted mutation marks the document dirty, including a mutation that
returns a field to its prior value through a later command. Commands that
would make no semantic change are no-ops and do not change dirty state.

Invalid candidate edits return an actionable error and leave the authoritative
document, dirty flag, selection, and preview unchanged.

## QML interface

The dashboard screen gains a persistent editor side panel with:

- Import, Open, Save, and Save As actions;
- dirty and current-file indicators;
- a card list with stable selection;
- Add Card and Remove Card actions;
- keyboard-accessible Move Up and Move Down actions; and
- fields for common and display-specific card configuration.

The dashboard stays visible and responsive while editing. The panel remains
visible while connected, but all document replacement and mutation controls
are disabled with explanatory text. Save is also disabled while connected so
the on-disk document cannot diverge from the active session through an editor
workflow.

The UI requests file paths and unsaved decisions, then returns those decisions
to controller continuations. It does not duplicate validation, dirty-state,
or transition rules in JavaScript.

## Error handling

Portable and port errors retain their structured kind and detail through the
controller. User-visible messages identify the failed operation and preserve
the underlying actionable detail.

All replacement and mutation operations are transactional at the controller
boundary. Failures never partially update document state or preview state.
Atomic writer failure never replaces a previously valid file with partial
content.

Startup restoration errors are recoverable. Save failures block a pending
Open, Import, or Exit continuation until the user retries, discards, or
cancels. Disconnection during an attempted editor action leaves the action
rejected; the user may retry after the disconnected state is observed.

## Testing

### Controller tests

- import success, cancellation, and failure;
- open success and transactional failure;
- Save and Save As success, cancellation, and atomic-write failure;
- exact clean/dirty/path transitions;
- recent-path updates only after successful open or save;
- successful and failed startup restoration;
- Save, Discard, and Cancel continuations for Open, Import, and Exit; and
- rejection of all document mutations and replacements while connected.

### Editor model tests

- only imported channels and embedded conversions are offered;
- add, remove, and stable-ID selection behavior;
- Move Up and Move Down ordering and boundary availability;
- channel/conversion, title, and display-type changes;
- gauge and sparkline setting changes;
- semantic no-op behavior;
- invalid candidate rollback; and
- prepared-session refresh after each accepted mutation.

### QML and integration tests

- persistent side-panel bindings and immediate preview;
- document action enablement and dirty/current-path indicators;
- common and display-specific fields;
- keyboard-accessible reorder controls;
- connected-state disabling and explanation;
- unsaved-change dialog continuations;
- recoverable error notifications;
- startup restoration through fake ports; and
- an offscreen flow from import through card edits, Save As, reload, and the
  same card order and configuration.

The implementation gate runs the affected portable and desktop Quick tests,
the repository-wide Bazel suite, portable-closure and formatting checks, and
builds both `//:fastecu` and `//:fastecu-desktop-quick`.

## Delivery boundaries

Implement this design in independently reviewable tasks that keep both desktop
applications buildable. A practical sequence is:

1. document controller state and transactional open/save/restoration;
2. unsaved-change continuations and application-exit integration;
3. editor model mutations and stable selection;
4. persistent side panel and immediate preview integration; and
5. end-to-end import/edit/save/reload coverage and phase gates.

File associations, packaging changes, diagnostic export, recovery hardening,
accessibility audit, and real hardware qualification remain subsequent work.

## Acceptance criteria

- Importing a valid CDBG definition creates an unsaved, dirty dashboard.
- Cards can use only imported channels and their embedded conversions.
- Users can add, remove, configure, and reorder all three card types from a
  persistent side panel while previewing the result.
- Accepted edits update the preview immediately and set dirty state.
- Editing, importing, opening, and saving are disabled while connected.
- Open, Import, and Exit resolve dirty state through Save, Discard, or Cancel.
- Failed and cancelled operations preserve the prior authoritative state.
- Save and Save As use atomic persistence; dirty and recent-path state update
  only after success.
- Restart restores the last successfully opened or saved dashboard, with
  recoverable handling for an unavailable or invalid file.
- Missing channels never produce placeholder cards.
- QML contains no domain validation or document mutation policy.
- Both desktop applications remain buildable and the repository verification
  gates pass.
