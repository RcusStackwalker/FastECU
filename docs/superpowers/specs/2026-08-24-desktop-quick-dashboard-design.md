# Desktop Quick Application and Configurable CDBG Dashboard — Design

**Status:** Approved 2026-08-24.

## Context

FastECU currently ships one Qt Widgets desktop application from
`//apps/desktop:fastecu`. Its dependency flow is
`apps/desktop -> src/ui/desktop -> src/platform/desktop -> src/backend ->
src/algorithms`. The ongoing modularization work has already made the CDBG
logging protocol and logging use case portable, while the Qt desktop layer owns
adapter setup, worker threads, and GUI-thread event delivery.

This design introduces a second, general-purpose QtQuick desktop application.
The dashboard shown in the alternative-UI concept is its first feature, not the
identity or architectural boundary of the application. The new application
lives alongside the Widgets application and reuses the same backend and desktop
hardware adapters.

The product-facing name is **OmniHaste**, pending the repository-wide rename.
Existing `fastecu::` namespaces and core Bazel labels remain unchanged in this
project so that the branding change does not expand the implementation scope.

## Goals

- Add a separately buildable QtQuick application under `apps/desktop-quick`.
- Deliver a functional, read-only dashboard vertical slice using CDBG over CAN
  and a real local adapter.
- Show only channels present in the dashboard's embedded CDBG catalog.
- Let users add and remove cards, reorder them, and select numeric, sparkline,
  or horizontal-gauge presentation in a responsive grid.
- Store a complete operational dashboard profile in a portable `.ohd` file and
  reopen the last-used document on restart.
- Keep QML independent of transport APIs and portable backend internals.
- Keep the existing Widgets application buildable and behaviorally unchanged.

## Non-goals

- Replacing every existing Widgets workflow in the first release.
- DTC, actuator, calibration, flashing, or other write-capable QtQuick screens.
- Arbitrary free-form card positioning or card resizing.
- Persisting live sample history in the dashboard document.
- Renaming existing C++ namespaces, targets, or the repository to OmniHaste.
- Creating a separate hardware-service process or IPC protocol.
- Supporting SSM or MUT/DMA in the first QtQuick dashboard milestone.

## Chosen architecture

The new application is a native QtQuick shell over explicit C++ presentation
adapters. It does not embed, wrap, or invisibly instantiate the current Widgets
UI.

```text
apps/desktop-quick
    -> src/ui/desktop-quick
         -> src/platform/desktop
              -> src/backend
                   -> src/algorithms
```

The initial source layout is:

```text
apps/desktop-quick/                    composition root and executable
src/ui/desktop-quick/
    shell/                             navigation, window chrome, theme
    dashboard/                         screen, editor, cards, view models
    shared/                            reusable QtQuick controls
src/platform/desktop/common/
    logging/                           shared Qt logging runtime
    connection/                        adapter discovery and CDBG/CAN setup
src/backend/dashboard/                 portable document model and codec
```

The Bazel executable is
`//apps/desktop-quick:fastecu-desktop-quick`, exposed through the root alias
`//:fastecu-desktop-quick`. Its package and source-tree name describes the
technology-specific desktop application; Dashboard remains only a feature
package within it. Future Diagnostics, Programming, Logs, and Settings features
can join `src/ui/desktop-quick` without becoming dashboard components.

`apps/desktop-quick/main.cpp` is a small composition root. It creates a
`QGuiApplication`, the concrete Qt ports, connection and logging services, and
presentation controllers before loading the root QML component through
`QQmlApplicationEngine`.

QML does not receive `SerialPortActions`, transport objects, backend services,
or writable hardware APIs. It calls narrow `QObject` controllers and consumes
`QAbstractListModel` roles. The new UI therefore does not add another direct
consumer to the frozen `serial_qt_compat` allowlist. Any transitional serial
facade use remains internal to the desktop platform layer.

## Qt and Bazel integration gate

The repository pins Qt 6.8.3 and `rules_qt` 0.0.6, while the current shared
`QT_DEPS` set is Widgets-oriented and does not declare the QML, Quick, and Quick
Controls modules. The first implementation task is a small build spike that:

- introduces the new binary and a minimal QML window;
- identifies the exact `rules_qt` labels and resource-compilation mechanism for
  QML, Quick, and Quick Controls;
- keeps QtQuick dependencies in a separate dependency set instead of adding
  them to every existing Qt target;
- proves build and startup on Linux, macOS, and Windows; and
- proves that the required QML modules are collected by release packaging.

Failure of this gate stops feature work until the target graph or pinned
`rules_qt` integration is corrected. It does not justify a qmake or CMake side
graph; Bazel remains the sole build graph.

## Portable `.ohd` document

### Identity and format

OmniHaste Dashboard documents use the `.ohd` extension and human-readable XML.
XML reuses the repository's existing portable `pugixml` dependency. The root
element and version marker distinguish the format from unrelated historical
uses of the same extension:

```xml
<omnihaste-dashboard format-version="1">
    <!-- metadata, connection, channel catalog, and cards -->
</omnihaste-dashboard>
```

Packaging registers the file type as `OmniHaste.Dashboard` on Windows and
`com.omnihaste.dashboard` on macOS. A package-specific OmniHaste XML media type
may be registered when Linux desktop integration is added. File recognition
must not depend on the extension alone; the root element and supported format
version are authoritative.

There is prior art for `.ohd`: NISTIR 7739 documents the extension for OMCE
histogram output. That use is unrelated and sufficiently niche that the short,
product-specific extension remains acceptable. The distinct XML root and OS
file-type identifiers prevent content ambiguity.

### Model

`src/backend/dashboard` owns a Qt-free `DashboardDocument` with these logical
parts:

- `DocumentMetadata`: format version, dashboard name, and optional description.
- `CdbgConnectionProfile`: protocol profile, CAN bitrate, identifier width,
  request and reply identifiers, stream instance, sampling interval, logging
  retry policy, and preferred adapter descriptor.
- `DashboardChannel`: stable ID, display name, description, address, byte
  length, raw assembly, and one or more named conversions.
- `DashboardConversion`: stable ID within its channel, expression, unit,
  decimal precision, and default gauge bounds and step.
- `DashboardCard`: stable card ID, channel ID, conversion ID, display type,
  optional title override, order, sparkline history duration, and
  display-specific bounds.

Format version 1 accepts only CDBG/CAN and validates the currently supported
identifier width, byte lengths, address ranges, policy bounds, unique channel
and card IDs, conversion expressions, card types, and card references. Protocol
values that are constants in today's CDBG driver are nevertheless serialized
explicitly so the document is self-contained and a future protocol profile can
vary them deliberately.

Format version 1 permits a channel to appear on at most one card. A card chooses
one of that channel's embedded conversions. This maps directly to the current
`LoggingChannel` contract and prevents the same ECU address from being added to
the wire request more than once. Supporting multiple simultaneous views or
conversions of one raw source requires a later acquisition/conversion split and
therefore a later document version.

The file contains all settings required to reproduce dashboard behavior on
another PC. It never contains secrets, credentials, absolute driver paths, or
live sample history. The preferred adapter descriptor uses portable matching
hints such as adapter kind, vendor, and display name. If it does not resolve on
the current PC, the user chooses an installed adapter. That choice remains a
session override until the document is explicitly saved.

Only machine-local shell state lives outside the `.ohd`: the last-opened
document path and window geometry. These values use `ISettings`/`QSettings` and
do not affect the dashboard's operational behavior.

### Codec and lifecycle

`DashboardDocumentService` parses, validates, migrates, and serializes the
model. Reads use `IFileRepository`; saves use `IAtomicFileWriter` so a failed or
interrupted save cannot replace a valid file with a partial document.

Unsupported newer schema versions are rejected without modification. Supported
older versions migrate in memory and become dirty only after a successful
load. Invalid fields produce `ErrorKind::InvalidConfig` with a stable field path
and actionable detail. Unknown or malformed input is never silently rewritten.

A new document can import an existing RomRaider-format CDBG logger XML once.
Channel definitions and all of their conversions are copied into the `.ohd`;
the user chooses the active conversion when adding a card. The original XML is
no longer required after save, making the document self-contained. In the first
release, "available channel" means a valid channel in this embedded catalog;
the CDBG protocol has no runtime capability discovery for arbitrary ECU memory
addresses.

## Shared logging and connection components

### Generic Qt logging runtime

The current `LoggingEngine` couples its otherwise reusable Qt worker lifecycle
to `DesktopLoggingSnapshot` and the legacy Widgets adapter. The implementation
extracts a generic target/API that starts from a portable `LoggingSession` and
an owned `LoggingProtocol` (or a factory producing that pair). It continues to
own:

- `LoggingWorker` creation and teardown;
- cancellation and joined shutdown;
- GUI-thread sample and state delivery;
- mapping terminal backend errors to session-end reasons; and
- delivery of diagnostic events.

The existing Widgets path retains its snapshot-building and sample-application
adapters. A thin compatibility caller converts the snapshot into the generic
start input, so existing behavior is preserved without making the new QtQuick
target depend on `src/backend/definitions` or legacy parallel-list models.

### Dashboard session builder

`DashboardSessionBuilder` converts the document into a `LoggingSession`. It
selects only channels referenced by cards and applies each card's chosen
conversion, preserving document order for deterministic CDBG frame construction.
Unused embedded channels do not consume CDBG frame capacity.

The builder validates the logging policy and channel conversion inputs through
backend-owned factories. It reports duplicate or missing IDs before hardware is
opened.

### Desktop connection service

`DesktopConnectionService` is the only new component that knows about local
adapter discovery and CDBG/CAN setup. It:

1. resolves the preferred adapter descriptor or requests a local selection;
2. configures raw CAN, identifier width, bitrate, and reply filtering from the
   document profile;
3. opens the adapter;
4. constructs the existing `FastEcuCanTransport`; and
5. constructs `CdbgLoggingProtocol` from the selected channels and CDBG profile.

The CDBG driver gains an explicit validated protocol configuration where it
currently reads compile-time request/reply IDs and stream timing. Defaults
remain byte-for-byte compatible with the existing Mitsubishi Colt CDBG path.
No write, flash, diagnostic, or actuator service is exposed to QML.

## QtQuick presentation components

`src/ui/desktop-quick/shell` owns the general application window, sidebar,
navigation, theme tokens, status banner, and bottom connection bar. Sidebar
destinations not implemented in the first milestone are absent or visibly
disabled; they do not lead to placeholder operational screens.

`DashboardController` owns:

- new/open/save/save-as and recent-document restoration;
- the current document, path, and dirty state;
- adapter resolution, connect, disconnect, and session status;
- the transition between view and edit modes; and
- translation of backend errors into presentation state.

`AvailableChannelsModel` exposes the embedded, validated channel catalog.
`DashboardCardModel` exposes card identity, ordering, display configuration,
latest value, unit, stale state, and sparkline data through explicit roles.
Neither model exposes backend pointers to QML.

The initial reusable QML components are:

- `NumericCard` for a labeled value and unit;
- `SparklineCard` for a labeled value plus bounded recent history;
- `HorizontalGaugeCard` for a labeled value and range;
- `DashboardEditor` for channel selection, add/remove, reorder, display type,
  conversion selection, and title override; and
- shared shell controls, dialogs, status indicators, and empty/error states.

The dashboard uses a responsive ordered grid. Users do not set pixel positions,
row/column coordinates, or arbitrary spans in format version 1. This keeps a
single `.ohd` useful across different window and display sizes.

## Runtime and user flow

### Startup

1. Read the last-opened `.ohd` path from local settings.
2. Parse and validate the document without opening hardware.
3. Populate the channel and card models.
4. If the path is missing or invalid, show the welcome/open screen and preserve
   the original setting and file for user recovery.

### Connect and stream

```text
open .ohd
    -> resolve preferred local adapter
    -> configure CDBG/raw CAN
    -> build session from channels used by visible cards
    -> create CdbgLoggingProtocol
    -> run through the shared Qt logging worker
    -> deliver sample batches on the GUI thread
    -> update affected card-model rows
    -> QML redraws affected cards
```

The presentation layer retains the latest value for every active channel and
coalesces model notifications to a bounded UI refresh rate. Backend polling is
not throttled by rendering. Sparkline cards maintain bounded in-memory ring
buffers, timestamped with a monotonic platform clock. Their history is cleared
when a new session begins and is never saved in the document.

### Edit and document transitions

The editor is disabled while connected in format version 1 because adding or
removing a channel changes the CDBG stream configuration. Users disconnect,
edit, and reconnect. All changes mutate one in-memory document and set its dirty
flag.

Opening another document first resolves unsaved changes, then stops and joins
any active session, loads the new document, and records its path only after a
successful open. Application shutdown follows the same unsaved-change and
orderly-disconnect sequence.

## Errors, recovery, and safety

The presentation layer maps the existing backend taxonomy consistently:

| Error/state | Presentation behavior |
|---|---|
| `InvalidConfig` | Keep the document open in edit mode, identify the invalid field, and prevent connection. |
| `Disconnected` | End the session, retain visibly stale readings, and offer reconnect or adapter selection. |
| ECU silence | Show `Car not responding`, dim cards, and allow the existing policy to retry. |
| `BadResponse` | Retry transient poll failures; treat a failed handshake as a failed connection attempt. |
| `Timeout` | End the session and offer an explicit retry. |
| `Unsupported` | Identify the unsupported document feature or channel setting. |
| `Internal` | Stop safely, retain diagnostics, and show a concise summary with expandable technical detail. |
| `Cancelled` | Treat user disconnect and shutdown as normal completion. |

Loss of communication never overwrites readings with zero. Cards retain the
last value, display a stale indicator, and show the age of the last valid
sample. Recovery removes stale state when a valid batch arrives.

Diagnostic events feed an in-memory application log with export-to-file. No
connection or parsing failure silently changes the `.ohd` document. The first
milestone is read-only at the product level and provides no path from QML to ECU
write operations.

## Testing

### Portable backend

- Model validation for every field and cross-reference.
- Valid format-version-1 parsing and deterministic serialization.
- Parse/serialize/parse semantic round trips.
- Rejection of malformed XML and unsupported newer versions.
- In-memory migration fixtures for every retained older schema after migrations
  exist.
- Import of CDBG logger XML, including conversions and duplicate IDs.
- Session building from only referenced card channels.
- Atomic-save success and failure using in-memory port implementations.
- Inclusion of the new portable targets in `//:portable_closure`.

### Desktop platform

- Adapter matching, no-match fallback, and session-local overrides with fake
  backends.
- CDBG connection configuration, open failure, disconnect, timeout, and
  cancellation.
- Generic logging-engine startup, sample delivery, error mapping, stop, and
  joined teardown using `ScriptedLoggingProtocol`.
- Regression tests proving the Widgets snapshot adapter still drives the same
  generic runtime.

### Presentation and QML

- Model-role, ordering, dirty-state, stale-value, notification-coalescing, and
  last-opened restoration tests.
- Card component tests for numeric, sparkline, and gauge states.
- Editor tests for add/remove/reorder and display-type changes.
- Unsaved-change, invalid-document, unavailable-adapter, and reconnect flows.
- An offscreen smoke test that loads the complete root QML component with a
  fixture `.ohd` and fake services.

Pixel-perfect screenshot tests are not a primary correctness gate. Small visual
fixtures may cover theme regressions, while functional QML and model contracts
remain the stable tests.

### Regression, packaging, and hardware

- Build and test both `//:fastecu` and `//:fastecu-desktop-quick` on all CI
  platforms.
- Verify macOS and Windows packages include required QML modules and `.ohd`
  associations.
- Run the normal portable-closure, formatting, clang-tidy, and repository-wide
  test gates.
- Add a CDBG/CAN bench checklist and record adapter, ECU, connection profile,
  sustained sample delivery, disconnect, and reconnect results before calling
  the hardware path qualified.

## Delivery sequence

1. **QtQuick build spike:** prove the minimal cross-platform application,
   resources, runtime modules, and packaging inputs.
2. **Portable `.ohd` foundation:** add model, codec, validation, import,
   migration framework, and portable-closure coverage.
3. **Shared logging runtime:** introduce the generic session/protocol API and
   migrate the Widgets caller through its compatibility adapter.
4. **Connection integration:** add adapter resolution, configurable CDBG/CAN
   setup, and a fake-backed end-to-end session.
5. **Functional dashboard:** add shell, controller, models, numeric cards,
   connection state, and batched live values.
6. **Remaining visualizations:** add horizontal gauge and bounded sparkline
   cards with responsive layout.
7. **Editor and persistence:** add document actions, card configuration, dirty
   state, atomic saving, recent-document restoration, and `.ohd` associations.
8. **Hardening and qualification:** finish recovery states, diagnostic export,
   accessibility, packaging, cross-platform gates, and real CDBG/CAN bench
   qualification.

Each phase leaves the Widgets application buildable. Changes to the portable
backend are tested without Qt, and changes to shared desktop runtime include
regression coverage for the existing UI before the QtQuick caller is added.

## Implementation-planning boundary

This document is the shared architectural design, not one monolithic execution
plan. Implementation is split into five focused plans and review checkpoints:

1. QtQuick/Bazel foundation and minimal `desktop-quick` shell.
2. Portable `.ohd` model, codec, import, and persistence.
3. Generic logging runtime, configurable CDBG profile, and desktop connection
   service.
4. Dashboard presentation, visualizations, editor, and document workflows.
5. Packaging, file associations, hardening, and hardware qualification.

Each plan must leave both application targets buildable and satisfy its focused
tests before the next begins. The first implementation plan covers only item 1;
later plans inherit this approved design and are written after the preceding
checkpoint passes.

## Acceptance criteria

The first milestone is complete when all of the following hold:

- `//:fastecu` and `//:fastecu-desktop-quick` build in the supported CI matrix.
- A user can create an `.ohd` by importing a CDBG logger definition, add only
  imported channels as cards, choose an embedded conversion and any supported
  card type, reorder cards, and save atomically.
- Copying that `.ohd` to another supported PC requires no logger-definition file
  or absolute path; the user can resolve a local adapter and connect.
- A real CDBG/CAN session updates configured cards while keeping the GUI thread
  responsive.
- Missing channels never create placeholder cards.
- Disconnect and ECU-silence states retain and mark stale values, and recovery
  restores live state.
- Restarting OmniHaste reopens the last successfully opened dashboard.
- No QtQuick component can reach an ECU write, flash, or actuator operation.
- The CDBG/CAN bench checklist has a recorded successful qualification run.

## References

- [Portable backend workflows design](2026-07-22-step5-backend-portable-design.md)
- [FastECU technical-debt roadmap](../../tech-debt.md)
- [NISTIR 7739 — Open Monte Carlo Engine](https://nvlpubs.nist.gov/nistpubs/Legacy/IR/nistir7739.pdf)
