# Step 5d-3 — Portable Definition Use Case — Design

**Status:** Approved 2026-07-26. Third sub-project of step 5d (see
`2026-07-24-step5d-fileactions-decomposition-design.md`). Depends on 5a and
5d-1, both implemented. 5d-2 is also implemented but is not a dependency.

## Goal

Replace the RomRaider and EcuFlash definition logic in
`file_defs_romraider.cpp`, `file_defs_ecuflash.cpp`, and the definition-related
parts of `file_actions.cpp` with portable, backend-owned definition use cases.
Both XML formats must parse into validated typed values, resolve inheritance
without partial mutation, and preserve the results of valid legacy definition
files. Definition creation and import rewriting also move behind the portable
boundary. Qt retains only file selection, form collection, dialogs, and
temporary adaptation to existing callers.

This sub-project establishes the parsed definition model consumed by 5d-4
(calibration) and 5d-6 (flash-definition glue). It does not implement either
consumer.

## Scope

### In scope

- Build typed RomRaider and EcuFlash definition catalogs.
- Recursively discover EcuFlash XML files and consume the configured RomRaider
  file list.
- Match ROM bytes against catalog calibration IDs in ASCII and hexadecimal
  form.
- Parse both XML formats with pugixml.
- Resolve RomRaider `base` and EcuFlash `include` inheritance.
- Merge tables, axes, scalings, selections, identity, and metadata according to
  explicit format-specific rules.
- Validate complete resolved definitions.
- Create new definition XML from UI-supplied values.
- Import an existing definition and rewrite its header without losing unrelated
  XML nodes.
- Atomically replace definition files through a new narrow port.
- Adapt successful typed results to the existing
  `ConfigValuesStructure`/`EcuCalDefStructure` shapes.
- Preserve `FileActions` public method signatures and `MainWindow` call sites
  during the transition.

### Out of scope

- Opening, saving, or editing ROM calibration data (5d-4).
- Map-value scaling/evaluation at runtime (5d-4); this slice parses and stores
  scaling definitions only.
- Building `FlashPlan` values from definitions (5d-6).
- Rewriting `MainWindow`, calibration widgets, or dialog presentation (step 6).
- Supporting additional definition formats.
- A generic schema/configuration framework for XML formats.
- Silently preserving malformed-input quirks. Valid-file results are the
  compatibility boundary; invalid input follows the strict policy below.

## Design decisions

### Strict and atomic parsing

All discovery, parsing, inheritance resolution, merging, and validation occurs
in temporary typed values. A caller-owned catalog or definition changes only
after the complete operation returns success. Malformed XML and invalid or
inconsistent definitions return an error rather than a partially populated
model.

For valid inputs, the resulting identity, metadata, tables, axes, scalings, and
selection values remain compatible with the legacy parsers. Compatibility does
not extend to accidental iteration order, unchecked out-of-bounds reads,
silently invalid booleans, partial list mutation, or recursion without cycle
detection.

### Format parsers plus a shared resolver

RomRaider and EcuFlash keep separate parsers because they have different XML
grammars. They share a resolver, validation model, catalog abstractions, and use
case facade. This avoids duplicating inheritance safety and orchestration while
keeping format-specific behavior visible and testable.

The rejected alternatives were:

1. two complete format pipelines, which duplicate lookup, recursion, merge,
   error, and validation behavior; and
2. one configurable generic XML engine, which introduces a speculative
   mini-framework for only two known formats.

## Package architecture

Portable code lives in a new `src/backend/definition/` package, never beside
the legacy `src/backend/definitions/` glob.

### `definition_model`

The portable model contains:

- `DefinitionFormat` (`RomRaider` or `EcuFlash`);
- `DefinitionIndexEntry`: definition ID, internal ID and address, ECU ID,
  source handle, format, and parent references;
- `DefinitionCatalog`: a validated collection of index entries with explicit
  lookup behavior;
- `RomIdentity` and `RomMetadata`;
- `RomDefinition`: resolved identity, metadata, maps, and source provenance;
- `CalibrationMap`, `AxisDefinition`, `Scaling`, and selection values.

Stable closed concepts use enums or booleans. Optional or format-extensible XML
values remain strings rather than creating speculative enums. Addresses and
dimensions are parsed into numeric types at the portable boundary; source text
is retained only where it is needed for compatible serialization or display.

The model uses one value per logical row. It does not expose parallel lists,
Qt types, raw owning pointers, or mutable parser state.

### `romraider_parser` and `ecuflash_parser`

Each parser accepts XML bytes plus source provenance and returns an unresolved
format document. A parser:

- checks XML syntax and expected structure;
- parses identity, metadata, tables, axes, scalings, selections, and parent
  references;
- applies local defaults defined by its format;
- validates individual attribute syntax; and
- reports node/attribute context on failure.

A parser does not read files, search catalogs, recurse into parents, mutate a
caller-owned definition, log through Qt, or display errors.

### `definition_resolver`

The resolver accepts an unresolved root document and a catalog-backed document
loader. It:

- resolves RomRaider `base` and EcuFlash `include` references;
- maintains a visited stack and reports the full chain on a cycle;
- memoizes parsed/resolved parents shared by multiple definitions;
- applies base values before child overrides;
- applies explicit format-specific table, axis, and scaling merge rules; and
- validates the final resolved `RomDefinition`.

RomRaider base-table enrichment and EcuFlash include/scaling behavior remain
separate named merge paths within the resolver. They are not forced through an
identical merge algorithm where their semantics differ.

For EcuFlash tables, an explicit `address` continues to take precedence over
`storageaddress`. Valid `swapxy`, `flipx`, and `flipy` attributes retain their
meaning; missing attributes receive the format default. A present value other
than `true` or `false` is invalid data rather than a warning followed by silent
coercion.

### `definition_service`

The service is the portable use-case facade. It receives `IFileSystem`,
`IFileRepository`, and `IAtomicFileWriter` references and owns no mutable
session state.

Its operations cover:

- building a RomRaider catalog from ordered configured handles;
- recursively discovering and building an EcuFlash catalog;
- matching ROM bytes to a catalog entry;
- loading and resolving a selected definition;
- creating a definition from validated submitted fields; and
- importing and rewriting an existing definition.

The service returns values through the existing `Result<T>`/`Status` types. It
does not receive UI callbacks or represent cancellation. A cancelled file
picker or form is handled before the service is called.

### `IAtomicFileWriter`

Add a narrow backend port whose single responsibility is atomic replacement of
one blob at an opaque destination handle. The desktop implementation uses
`QSaveFile`: it writes a temporary sibling and commits the replacement only
after the complete write succeeds.

This remains separate from `IFileRepository::write` so existing consumers and
test doubles do not acquire a stronger contract they do not need. Definition
creation/import fully constructs, validates, and serializes the XML before
calling the atomic writer. A failed replacement preserves the previous
destination.

### `legacy_definition_adapter`

The adapter is a Qt-linked transitional boundary, not part of the portable
package closure. It:

- converts typed catalog entries into the corresponding legacy config lists;
- converts a successful `RomDefinition` into the legacy calibration definition
  shape;
- converts submitted Qt dialog strings to portable creation/import requests;
  and
- converts portable results to the return conventions expected by
  `FileActions`.

The adapter builds a complete temporary legacy value and replaces the
definition-related fields only after every conversion succeeds. It never
incrementally appends to the caller's parallel lists.

To avoid a Bazel cycle, move `EcuCalDefStructure` unchanged from
`file_actions.h` into a narrow Qt-typed legacy model header/target.
`FileActions` re-exposes it with a `using EcuCalDefStructure = ...` alias, as
5d-1 already does for `ConfigValuesStructure`. Existing source-level spelling
and call sites remain unchanged.

## Data flow

### Catalog construction

1. The caller supplies configured RomRaider handles and the EcuFlash directory.
2. EcuFlash discovery recursively selects regular files with a case-insensitive
   `.xml` suffix and sorts complete handles lexically for deterministic
   processing. RomRaider preserves configured handle order.
3. The repository reads each selected file once for the catalog build.
4. The format parser extracts zero or more typed index entries.
5. The complete catalog validates IDs, addresses, source handles, and
   parent-reference syntax.
6. The service returns the new catalog.
7. Only after success does the adapter replace the matching legacy config
   lists.

Catalog lookup precedence is explicit:

- an exact definition ID match is required;
- if an ID appears more than once in the same format, the first entry in the
  deterministic source order wins, matching the effective legacy
  first-match behavior;
- every duplicate produces a catalog diagnostic containing both sources; and
- duplicates with conflicting internal IDs or parent relationships make the
  catalog invalid rather than choosing silently.

This limited duplicate rule preserves real-world definition overlays without
making lookup depend on filesystem iterator order.

### ROM ID matching

ROM matching is a pure operation over a byte span and catalog entries. For each
entry it:

1. validates and converts the configured address;
2. determines candidate length from the internal ID representation;
3. bounds-checks the complete half-open byte range;
4. compares bytes using the entry's explicit ASCII or hexadecimal
   representation; and
5. returns the first exact match in catalog precedence order.

No-match is a normal `NotFound` result. An invalid address or representation is
`InvalidData`. The unused legacy `is_ascii` parameter remains only on the
compatibility wrapper; portable matching derives representation from the
catalog entry.

### Definition loading

1. The service locates the exact catalog entry.
2. It reads and parses the selected document into an unresolved value.
3. The resolver loads and resolves the complete parent chain.
4. It merges base-to-child and validates the completed graph.
5. It returns one `RomDefinition`.
6. The adapter constructs a complete temporary legacy definition.
7. On success, the wrapper commits it to the caller-owned
   `EcuCalDefStructure`.

Read, parse, resolution, validation, or adaptation failure leaves the
caller-owned definition unchanged.

### Definition creation and import

Qt remains responsible for presenting fields, choosing handles, and deciding
whether the user wants to retry after cancellation. Once the user submits:

- creation receives a typed header request and produces a new minimal
  EcuFlash-compatible `<rom>` document;
- import reads the source XML, updates the intended `romid`, `include`, and
  `notes` nodes structurally, and preserves unrelated elements, comments, and
  tables in the parsed tree; and
- both operations validate the resulting document by parsing it before
  serialization and atomic replacement.

Serialization uses UTF-8, an XML declaration, two-space indentation, and a
terminal newline. Tests compare semantic round trips rather than depending on
attribute order or insignificant whitespace.

After a successful write, the service returns the new index entry. The caller
may rebuild the catalog through the normal catalog operation; the service does
not mutate a long-lived hidden catalog.

## Validation rules

A resolved definition is rejected when any of the following applies:

- malformed XML or an unexpected required root structure;
- missing or empty required definition identity;
- invalid numeric address or dimension;
- an address range that overflows its numeric representation;
- duplicate/conflicting definitions outside the catalog rule above;
- missing parent, inheritance cycle, or cross-format parent reference;
- unresolved or duplicate/conflicting scaling references;
- a present strict boolean other than `true` or `false`;
- a table or axis whose required shape is incomplete;
- zero dimensions where the format requires a populated dimension; or
- contradictory storage, endian, or selection metadata that prevents a
  complete typed record.

Optional metadata remains optional. The parsers must not promote a field to
"required" solely because a legacy parallel list stored a placeholder for it.
Golden fixtures determine which omissions are valid in each format.

## Error handling

Use the existing error taxonomy:

- `InvalidData`: XML, field, catalog, inheritance, merge, and validation
  failures;
- `NotFound`: missing selected definition, parent, source, or ROM ID match;
- `Io`: discovery, read, or atomic replacement failure.

Errors identify the format, source handle, definition ID when known, and
relevant XML element/attribute. Recursive failures include the inheritance
chain. A parser may collect independent validation diagnostics from one
document, but the public operation returns one concise combined failure and no
partial value.

`FileActions` temporarily translates errors into existing logs and
caller-owned warning dialogs. Portable components do not emit Qt signals or
construct UI.

## Legacy integration and removal

`FileActions` keeps its public definition method signatures for step 5d-3.
Their bodies become calls to the service/adapter or UI-only orchestration.
`MainWindow` continues invoking `create_ecuflash_def_id_list` and
`create_romraider_def_id_list` unchanged.

Delete the legacy parsing implementations in `file_defs_romraider.cpp` and
`file_defs_ecuflash.cpp`; do not keep a second parser for compatibility. Drain
definition matching, creation/import XML handling, and parsing helpers from
`file_actions.cpp`. Dialog construction and retry/cancel loops remain there
until step 6.

The exact source-file deletion and compatibility-wrapper placement are plan
details, but the final dependency direction is fixed:

```text
FileActions UI wrappers
        |
        v
LegacyDefinitionAdapter (Qt-linked)
        |
        v
DefinitionService -> parsers/resolver/model -> pugixml
        |
        +-> IFileSystem / IFileRepository / IAtomicFileWriter
```

The portable packages never depend back on the adapter, legacy model, or
`FileActions`.

## Testing

### Parser tests

Use in-memory XML fixtures for every supported identity, metadata, table, axis,
scaling, selection, and format-specific attribute. Include comments,
insignificant whitespace, multiple definitions per RomRaider document, and
representative EcuFlash layouts.

### Golden compatibility tests

Characterize valid legacy behavior before replacement. Golden typed-model
tests cover:

- RomRaider base enrichment and child overrides;
- EcuFlash multi-level includes and scaling application;
- `address` over `storageaddress`;
- valid strict booleans and missing-boolean defaults;
- table dimensions, categories, descriptions, user levels, storage/endian
  values, expressions, formatting, and selections; and
- definition identity and metadata used by current UI/flash consumers.

### Resolver and catalog tests

Cover missing parents, self-cycles, multi-node cycles, shared memoized bases,
duplicate IDs, conflicting duplicates, deterministic source precedence,
cross-format references, recursive discovery ordering, and partial read
failure.

### ROM matching tests

Cover ASCII and hexadecimal IDs, lowercase/uppercase hex input, invalid
addresses, exact-end ranges, one-byte-short ROMs, ambiguous matches, empty IDs,
and no match.

### Strictness and atomicity tests

Seed destination catalogs and legacy definitions, fail each discovery, read,
parse, resolution, validation, and adaptation stage, and assert the original
values are unchanged. Invalid fixtures cover malformed XML, invalid booleans,
invalid numbers, incomplete records, unresolved scalings, inconsistent
dimensions, missing parents, and cycles.

### Creation/import and writer tests

- Parse generated XML back into the semantic model.
- Verify imported unrelated nodes, comments, tables, and scalings survive.
- Verify deterministic UTF-8 serialization.
- Verify a successful atomic replacement.
- Inject open, write, and commit failures and prove the old destination
  remains intact.

### Adapter and Qt integration tests

Adapter tests map every typed field to the correct legacy field and assert that
all row-oriented legacy lists remain aligned. A small Qt integration layer
covers cancellation, portable-error translation, and successful submitted
forms. Parser behavior is not retested through widgets.

## Build and enforcement

- Add the model, parsers, resolver, service, and `IAtomicFileWriter` interface
  targets to `//:portable_closure`.
- Ensure their transitive closure contains no Qt target.
- Keep the desktop atomic-writer implementation and legacy adapter outside the
  portable closure.
- Use explicit source lists or the new package boundary so the legacy
  `definitions` glob cannot absorb portable files.
- Remove only definition-related `serial_qt_compat` allowlist entries verified
  to disappear from the live dependency graph.
- Keep `docs/coverage-baseline.txt` absent.
- Meet the existing >=80% new-code coverage and SonarCloud Quality Gate.

Required final gates:

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

The implementation plan must also run focused definition/parser/adapter tests
during development rather than waiting for the full final gate.

## Success criteria

- Both formats produce typed, compatible results for valid characterized
  fixtures.
- Malformed or inconsistent inputs fail without changing caller-owned catalog
  or definition state.
- Inheritance has deterministic precedence, missing-parent errors, and cycle
  detection.
- ROM matching performs validated bounds-safe ASCII/hex comparisons.
- Creation and import contain no backend Qt XML or direct file access, and a
  failed replacement preserves the old destination.
- The old RomRaider/EcuFlash parser implementations are removed rather than
  retained alongside the new code.
- `FileActions` public signatures and `MainWindow` call sites remain compatible.
- 5d-4 and 5d-6 can consume `RomDefinition` without depending on Qt or legacy
  parallel lists.
- Portable closure, serial compatibility, release build/test, coverage, and
  quality gates pass.

