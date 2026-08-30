# Portable `.ohd` Foundation — Design

**Status:** Approved 2026-08-24.

## Context

The first delivery step of the
[Desktop Quick Application and Configurable CDBG Dashboard design](2026-08-24-desktop-quick-dashboard-design.md)
established the separately buildable QtQuick application and its minimal shell.
The second step establishes the portable document boundary that later
connection and presentation work will consume.

FastECU already ships a CDBG channel catalog in a logger-shaped XML envelope.
That envelope is a legacy FastECU configuration detail: CDBG itself is not a
RomRaider protocol, and the new dashboard format must not expose RomRaider
concepts. Import is a one-time conversion into a self-contained `.ohd`
document.

## Goals

- Add a Qt-free dashboard document model under `src/backend/dashboard`.
- Parse, validate, and deterministically serialize format-version-1 `.ohd`
  XML.
- Import the existing legacy FastECU CDBG catalog atomically.
- Store a complete CDBG/CAN connection profile, embedded channel catalog, and
  ordered card configuration in one portable document.
- Load through `IFileRepository` and save through `IAtomicFileWriter`.
- Include the new package and tests in the portable-closure gates.

## Non-goals

- Supporting an `.ohd` version other than format version 1.
- Inventing a version 0 document or migration fixture.
- Adding QML, controllers, file dialogs, recent-document state, or dirty-state
  tracking.
- Discovering adapters, opening hardware, or building a logging session.
- Rendering, editing, or collecting live card data.
- Importing arbitrary RomRaider logger definitions or non-CDBG protocols.

## Architecture

The implementation is a pure transformation pipeline behind a thin stateless
persistence service:

```text
legacy FastECU CDBG catalog bytes
    -> strict legacy importer
    -> DashboardDocument

.ohd bytes
    -> root/version dispatch
    -> v1 decoder
    -> document validator
    -> DashboardDocument

DashboardDocument
    -> document validator
    -> deterministic v1 encoder
    -> atomic writer
```

`src/backend/dashboard` contains these focused units:

- `dashboard_document.h` defines plain document value types and enums.
- `dashboard_validation.{h,cpp}` validates a complete domain document.
- `dashboard_codec.{h,cpp}` decodes and encodes format-version-1 XML.
- `legacy_cdbg_catalog_importer.{h,cpp}` converts the existing FastECU
  logger-shaped CDBG catalog into a new document.
- `dashboard_document_service.{h,cpp}` coordinates repository reads and atomic
  writes without retaining mutable document state.

Parsing, validation, importing, and serialization remain independently
testable. The package depends on portable backend ports, the existing logging
conversion semantics, the CDBG protocol constants needed for defaults, and
`pugixml`. It does not depend on Qt, `src/backend/definitions`, either desktop
application, or platform adapters.

## Document model

`DashboardDocument` has five logical sections:

- `DocumentMetadata`: format version, name, and optional description.
- `CdbgConnectionProfile`: CDBG/raw-CAN identifiers and timing, logging retry
  policy, and optional portable adapter-matching hints.
- `DashboardChannel`: stable ID, descriptive text, address, byte length, raw
  assembly, and one or more conversions.
- `DashboardConversion`: channel-local stable ID, expression, unit, decimal
  precision, and default gauge bounds and step.
- `DashboardCard`: stable ID, channel/conversion references, presentation type,
  optional title and bounds overrides, order, and sparkline history duration.

Protocol fields use fixed-width integer types. IDs and descriptive fields use
strings. Closed value sets use enums instead of magic strings. Format version
1 accepts only CDBG over raw CAN, the supported CAN identifier widths, and
unsigned-integer-decimal raw assembly.

An empty card list and an absent preferred adapter are valid. The metadata
description, card title override, and card-specific gauge-bound overrides are
optional. All operational connection and retry fields are explicit.

## Format-version-1 XML

The root name and version are authoritative:

```xml
<omnihaste-dashboard format-version="1">
    <metadata name="Colt Dashboard" description="Example"/>
    <connection protocol="cdbg" transport="raw-can"
                bitrate="500000" identifier-width="11"
                request-id="0x630" reply-id="0x631"
                stream-instance="0" sampling-interval-ms="50">
        <retry poll-timeout-ms="100" silence-threshold="3"
               reconnect-attempts="3" reconnect-period-ms="250"/>
    </connection>
    <channels>
        <channel id="CDBG_ENGINE_RPM" name="Engine RPM"
                 description="engine_rpm uint16" address="0x804cfc"
                 length="2" raw-assembly="unsigned-integer-decimal">
            <conversion id="conversion-1" expression="x*1000/256"
                        unit="rpm" precision="0" gauge-min="0"
                        gauge-max="8000" gauge-step="500"/>
        </channel>
    </channels>
    <cards/>
</omnihaste-dashboard>
```

When present, `<preferred-adapter>` is a child of `<connection>` and contains
portable matching hints: adapter kind, vendor, and display name. It contains no
driver path, credential, or machine-local identifier.

The encoder emits elements and attributes in one documented order, lowercase
enum spellings, ordinary integers in decimal, CAN identifiers and addresses in
canonical lowercase `0x` hexadecimal, four-space indentation, UTF-8, and one
trailing newline. Deterministic output makes reviews and source-control diffs
stable; semantic equality, not byte identity with hand-authored input, defines
round-trip correctness.

Unknown elements and attributes are rejected in version 1. This catches
misspellings and prevents a read-modify-save cycle from silently discarding
data. Optional elements may be absent only where this design identifies them
as optional.

## Validation

Decoding constructs a candidate document and then runs the same complete
validator used before saving. Validation returns the first error as
`ErrorKind::InvalidConfig`. Its detail begins with a stable field path, such as
`channels[CDBG_ENGINE_RPM].conversions[conversion-1].expression`, followed by
an actionable explanation.

The validator enforces:

- the supported protocol, transport, identifier width, and identifier ranges;
- positive bounded connection and retry-policy values;
- nonempty, unique channel IDs;
- 32-bit addresses and channel lengths of 1, 2, or 4 bytes;
- unsigned-integer-decimal raw assembly;
- at least one conversion per channel;
- nonempty channel-local unique conversion IDs;
- expressions and precision accepted by the existing logging conversion
  semantics;
- finite gauge values, `minimum < maximum`, and a positive step;
- nonempty unique card IDs and contiguous unique orders beginning at zero;
- one card at most per channel;
- card references that resolve to an embedded channel and one of its
  conversions;
- card-type-specific fields only on the applicable display type;
- valid card-specific gauge bounds when overrides are present; and
- an integer sparkline history duration from 1 through 300 seconds, inclusive.

The validator does not require cards to exist. Wire-frame capacity is not a
whole-catalog constraint because only channels referenced by cards enter a
logging session. The later session builder validates the selected subset
against CDBG frame capacity.

## Legacy FastECU CDBG catalog import

The importer consumes the existing FastECU logger-shaped XML envelope, selects
exactly one `<protocol id="CDBG">` section, and ignores non-CDBG protocol
sections and switches. This is not general RomRaider import support.

Import creates a new unsaved document with:

- the current Mitsubishi Colt CDBG/raw-CAN constants serialized explicitly as
  connection defaults;
- the supplied default sampling and retry policy;
- every valid CDBG parameter and all of its conversions;
- deterministic channel-local conversion IDs (`conversion-1`,
  `conversion-2`, and so on, in source order);
- no cards; and
- no preferred adapter.

The import is all-or-nothing. Duplicate CDBG sections, no CDBG channels,
missing or placeholder IDs, duplicate channel IDs, invalid addresses or
lengths, a channel without conversions, invalid conversion fields, or any
other document-validation error returns no document. The original catalog is
never changed. Once the imported document is saved, it no longer depends on
the catalog.

Conversion IDs need only be stable within the imported document. Import
creates no cards, so source reordering cannot invalidate a pre-existing card
reference.

## Service API and failure behavior

`DashboardDocumentService` exposes three operations:

- `load(handle)` reads, decodes, and validates an `.ohd` document.
- `save(handle, document)` validates, encodes, and atomically replaces the
  destination.
- `import_legacy_cdbg_catalog(handle, defaults)` reads and strictly converts a
  legacy catalog, returning an unsaved document.

The service owns no current path, dirty state, recent document list, or
in-memory document. Import never saves automatically.

Repository and writer errors pass through unchanged. Malformed XML, the wrong
root, missing or unknown fields, invalid values, and broken references return
`InvalidConfig`. A well-formed `.ohd` whose numeric format version is not 1
returns `Unsupported` and is never rewritten.

Saving completes validation and serialization before calling
`IAtomicFileWriter::replace`. An invalid document therefore performs zero
writes, and an interrupted or failed replacement cannot overwrite a valid
destination with partial content. Loading and importing similarly return no
partially populated document.

## Versioning

The codec has an explicit root/version dispatch point and recognizes only
format version 1. There is no v0 type, migration registry, migration fixture,
or unused migration interface.

A future format can add a decoder for its own schema and an explicit mapping
into the then-current domain model. Unsupported versions continue to fail
without modification. This preserves a clean extension point without
maintaining fictional legacy behavior now.

## Testing

### Model validation

- Cover every scalar bound and closed enum.
- Cover unique channel, conversion, and card IDs.
- Cover card references, contiguous ordering, and one card per channel.
- Cover conversion expression/precision and gauge invariants.
- Cover the inclusive 1–300-second sparkline range.
- Prove that an empty card list and absent preferred adapter are valid.

### Codec

- Parse a canonical version-1 fixture.
- Reject malformed XML, the wrong root, missing fields, and unknown fields.
- Return `Unsupported` for well-formed non-v1 documents.
- Verify deterministic serialization.
- Verify parse/serialize/parse semantic equality.

### Importer

- Import the bundled legacy FastECU CDBG catalog.
- Ignore non-CDBG protocols and switches.
- Verify current connection defaults and deterministic conversion IDs.
- Reject duplicate CDBG sections, empty CDBG catalogs, duplicate IDs, invalid
  addresses and lengths, missing conversions, and invalid expressions.
- Prove every failure returns no partial document.

### Service and architecture gates

- Propagate repository read failures.
- Cover load success and validation failure.
- Cover atomic replacement success and failure.
- Prove invalid documents perform zero writes.
- Add the new production and test targets to `//:portable_closure` and the
  repository's portable dependency checks.
- Prove the package has no Qt dependency.

The bundled catalog is used as an integration fixture where practical. Small
inline XML fixtures isolate individual validation failures.

## Delivery boundary

This step ends with a portable document API, canonical `.ohd` fixtures, and a
strict path from the existing CDBG catalog to an unsaved document. It does not
integrate with the QtQuick shell or hardware.

The next delivery step may consume this model while introducing the generic
logging runtime, configurable CDBG profile, and desktop connection service.
Both desktop applications remain buildable throughout.

## Acceptance criteria

- A valid format-version-1 `.ohd` parses into the complete portable model.
- Serializing the same document twice produces identical bytes.
- Parse/serialize/parse preserves document semantics.
- Invalid or unknown fields identify a stable field path.
- Non-v1 documents return `Unsupported` without being rewritten.
- The legacy FastECU CDBG catalog imports all-or-nothing into a document with
  explicit current connection defaults, all conversions, no cards, and no
  preferred adapter.
- Invalid documents never reach the atomic writer.
- The dashboard backend remains Qt-free and is covered by
  `//:portable_closure`.
