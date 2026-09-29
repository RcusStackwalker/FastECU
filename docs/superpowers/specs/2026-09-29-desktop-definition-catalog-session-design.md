# 6n-1: Desktop definition catalog and authoring ownership

Status: approved by the user on 2026-09-29; implementation plan pending review.

## Intent and scope

Replace the desktop consumers of FileActions catalog and authoring state with
one composition-owned session. Preserve ROM matching, definition inheritance,
authoring output, operator interactions, notices, and failure behavior.
The first slice removes the parallel definition indexes and FileActions
injection from production desktop consumers. Kernel model relocation and Qt
byte-helper relocation remain subsequent 6n slices. Android work is out of scope.

## Selected boundary

Add `DefinitionCatalogSession` under
`src/platform/desktop/common/definition`. It implements the existing portable
`fastecu::calibration::IDefinitionCatalogs` interface and exposes concrete
desktop methods for refreshing startup indexes and submitting authored files.
Its target has deliberate visibility for desktop composition and the UI
packages consuming it; it grants no access to serial implementation targets.

The class itself uses standard C++ records and injected ports. Its placement
reflects responsibility for desktop configuration and authoring coordination,
not a requirement to depend on Qt. Portable parsing, matching, inheritance,
and atomic writing remain in the existing DefinitionService. No new portable
port, parser, worker, or threading model is introduced.

DesktopComposition constructs one DefinitionService and lends it to both the
catalog session and RomOpenUseCase. The session also borrows ConfigSession,
IFileSystem, and IEventSink. Config changes are observed on subsequent scans,
as today. The workspace and opener are destroyed before the catalog session;
the catalog session is destroyed before its service, configuration, and ports.

## State and API

The session privately owns:

- Ordered typed startup index records, independently for each format.
- Sorted, deduplicated EcuFlash handles successfully authored this session.

A startup index record needs only definition ID and source. It must remain
separate from a validated DefinitionCatalog: immediate authoring appends can
contain duplicate IDs, whereas fresh catalog construction validates duplicate
content and may reject conflicts. Do not weaken DefinitionCatalog validation
to accommodate historical startup lookup behavior.

Methods:

- `catalog(format)` builds a fresh catalog using current configured sources
  and successful submission handles. It does not replace the startup index.
- `indexed_source(format, id)` returns the first matching retained source,
  treating an empty source as absent, matching the former QString lookup.
- `refresh_index(format)` scans and replaces only that format's startup index
  on success, preserving order and existing logs/notices.
- `submit_new_definition(destination, input, allow_overwrite)` and
  `submit_imported_definition(source, destination, input)` delegate the write,
  then register the handle and append the typed lookup record on success.
  The new-definition dialog passes overwrite authorization after the existing
  native save dialog. Callers cannot mutate index storage directly.

All fallible methods return existing Status/Result types. User decisions stay
in dialogs. Event sinks deliver existing logs and notices without requesting
decisions or duplicating UI error dialogs.

## Compatibility requirements

1. An empty configured source list or directory retains its existing startup
   index and emits the existing debug message.
2. A failed catalog scan preserves the previous startup index. A successful
   scan whose unusable files are skipped may replace it with an empty index.
3. RomRaider source order is preserved. EcuFlash discovery order remains
   governed by DefinitionService; this slice adds no sorting of catalog entries.
4. Startup duplicate-ID lookup returns the first retained record. Fresh
   catalogs retain their current duplicate-validation behavior.
5. Successful creation/import immediately appends a lookup record and records
   the destination for future scans, including destinations outside the
   configured directory. Repeated successful submissions append lookup records
   as today, while discovery handles remain sorted and deduplicated.
6. Failed writes, invalid inputs, and dialog cancellation do not register a
   destination or mutate retained lookup state.
7. Changing the configured directory drops previously discovered entries on
   the next successful refresh, while preserving successfully authored handles.
8. A file becoming unreadable after startup remains identifiable through
   indexed_source so RomOpenUseCase preserves its current load-error notice.
9. Native dialogs, XML suffix normalization, header forms, XML output,
   inheritance, overwrite interactions, and format selection remain unchanged.

## Consumer migration and removal

MainWindowServices exposes the new concrete session instead of FileActions.
MainWindow invokes refresh_index at the existing startup points and injects
the session into DefinitionAuthoringDialog. The dialog submits through the
session and removes record_definition and all parallel-list mutations.
RomOpenUseCase retains its existing interface and policy.

Composition and UI fixtures construct the same ownership graph as production.
Move catalog/submission characterization coverage to the new session; retain
real filesystem integration coverage where it proves discovery and XML output.
Remove FileActions and definition_indexes once their remaining live consumers
and useful tests have migrated. Delete unused FileActions-only formatting
wrappers and stale checks tied to its header after checking callers. Preserve
historical comments that provide useful compatibility provenance.

The legacy definitions package may remain solely for its portable kernel
models until the next slice. Removing its Qt visibility grant is appropriate
once no Qt target remains there; retaining the models is not a reason to keep
that grant. The algorithms byte shim remains until its separate relocation.

## Verification and acceptance

Package-owned tests cover index order, duplicate lookup, empty configuration,
failed refresh, successful empty refresh, directory changes, successful and
failed submissions, and sorted/deduplicated submitted handles. Tests exercise
public behavior through injected ports rather than expose mutable internals.

ROM-open integration tests prove authored destinations are discoverable and
missing indexed files still produce the existing notice. Authoring tests
retain form behavior and verify success/failure registration through the new
session. Composition/MainWindow tests prove shared session identity, startup
failure behavior, repeated construction, and destruction order.

Run focused tests first, then release build/test, changed-file static analysis,
formatting, and applicable build-generation checks. Inspect the target graph
to verify portable roots cannot reach the desktop session and production UI
still cannot reach serial facade headers. Require recorded platform CI and
packaging results before release. Existing hardware qualification remains
separately tracked; this change establishes no new bench evidence.

Acceptance: production desktop consumers have no FileActions or
DefinitionIndexes dependency; one composition-owned session supplies catalog
lookup and successful authoring registration; the listed behaviors are proven
by regression coverage. Kernel models and the byte shim remain explicitly
assigned to later slices.
