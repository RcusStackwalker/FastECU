# Definition use-case mergeability analysis

Date: 2026-07-30

Scope: the current diff from `origin/master` (`523a71f`) to
`step5d3-definition-use-case` (`2122dc2`). Previously extracted and merged
changes are deliberately excluded.

## Conclusion

Two parts of the current diff can be reviewed and merged independently on the
current master:

1. the definition inheritance resolver and its tests;
2. the EcuFlash definition writer, the shared hexadecimal text formatter, and
   their tests.

Both are substantial parts of the definition use case rather than unrelated
cleanup. Splitting them is technically sound and would reduce the final feature
PR, but it would also add two more review and merge cycles. If minimizing the
remaining PR is still the priority, split both. Otherwise, there are no
remaining small unrelated changes worth extracting.

The definition service, Qt legacy adapter, `FileActions` migration, UI
composition, and legacy integration tests should remain together. Their
interfaces and implementations form one dependency chain and collectively
deliver the use case.

## Dependency shape

```text
definition_resolver ────────┐
text_format + writer ───────┴─→ definition_service ──┐
                                                    ├─→ legacy_definition_adapter
legacy_definition_columns ──────────────────────────┘
                                                            ↓
                                                  FileActions migration
                                                    ├─ desktop composition
                                                    └─ integration tests
```

`definition_service` joins the two independently mergeable portable
components. The adapter then translates the service's typed values into the
parallel Qt containers still exposed by `FileActions`.

## Candidate assessment

| Candidate | Builds on current master | Related to main goal | Recommendation |
| --- | --- | --- | --- |
| Definition resolver | Yes | Yes | Independently mergeable |
| Definition writer plus `text_format` | Yes | Yes | Independently mergeable |
| Portable-boundary registration | Only with its corresponding target | Supporting constraint | Move with resolver/writer/service, not alone |
| Definition service | Not without resolver and writer | Central use case | Retain in main PR |
| `legacy_definition_columns` | Technically, but unused alone | Adapter implementation detail | Retain with adapter |
| Legacy definition adapter | Not without service and columns | Central compatibility boundary | Retain in main PR |
| `FileActions` source split and delegation | Not without service and adapter | Main integration | Retain in main PR |
| Desktop `QtAtomicFileWriter` injection | Not without the new constructor | Required composition | Retain in main PR |
| Legacy and integration test changes | Not without the migrated implementation | Verification of main integration | Retain with tested code |
| Test-suite dependency additions | Not without changed tests | Build metadata | Retain with changed tests |

## Independently mergeable part 1: definition resolver

Files:

- `src/backend/definition/definition_resolver.h`
- `src/backend/definition/definition_resolver.cpp`
- `src/backend/definition/definition_resolver_test.cpp`
- the `definition_resolver` library and test declarations in
  `src/backend/definition/BUILD.bazel`
- the resolver entries in `BUILD.bazel` and
  `scripts/check-portable-closure.py`

The production target depends only on the already-merged definition model and
portable result types. Its test target additionally uses the already-merged
RomRaider and EcuFlash parsers to construct realistic unresolved definitions.
It does not depend on the new service, writer, Qt adapter, `FileActions`, or
desktop code.

The resolver is therefore directly cherry-pickable onto current master. Its
public boundary is a loader callback:

```cpp
Result<RomDefinition> resolve_definition(
    UnresolvedDefinition root,
    const DefinitionLoader& loader);
```

That boundary keeps file discovery and persistence outside the resolver and
makes the component independently testable.

Suggested PR verification:

```text
//src/backend/definition:definition_resolver_test
//:portable_closure
```

## Independently mergeable part 2: definition writer

Files:

- `src/backend/definition/definition_writer.h`
- `src/backend/definition/definition_writer.cpp`
- `src/backend/definition/definition_writer_test.cpp`
- `src/backend/definition/text_format.h`
- the `text_format`, `definition_writer`, and `definition_writer_test`
  declarations in `src/backend/definition/BUILD.bazel`
- the writer entries in `BUILD.bazel` and
  `scripts/check-portable-closure.py`

The writer depends only on the existing model, EcuFlash parser, portable result
types, pugixml, and the small header-only text formatter. It does not depend on
the resolver, service, Qt adapter, or `FileActions`.

`text_format` should travel with this PR rather than become its own PR. It is
only 15 lines, the writer needs it immediately, and merging an unused library
target would not provide standalone value. The later adapter can reuse the
same formatter after the writer lands.

The writer exposes pure byte transformations:

```cpp
Result<std::vector<std::uint8_t>> create_ecuflash_xml(
    const DefinitionHeaderInput&);
Result<std::vector<std::uint8_t>> rewrite_ecuflash_xml(
    std::span<const std::uint8_t> source,
    const DefinitionHeaderInput&);
```

Atomic filesystem replacement is intentionally absent from this component and
belongs to the service.

Suggested PR verification:

```text
//src/backend/definition:definition_writer_test
//src/backend/definition:ecuflash_parser_test
//:portable_closure
```

## Parts that should remain in the main PR

### Definition service

`definition_service` is the use-case boundary. It combines catalog discovery,
ROM matching, inheritance resolution, XML generation, repository reads, and
atomic replacement. It directly depends on both independent candidates:

```text
definition_service
├── definition_resolver
├── definition_writer
├── ecuflash_parser
├── romraider_parser
├── file system and repository ports
└── atomic file writer port
```

It could be merged after the resolver and writer, but doing so as another
preparatory PR would extract the central behavior the feature branch exists to
deliver. It is not unrelated scope.

### Legacy adapter and column schema

`LegacyDefinitionAdapter` owns the transition from typed definitions to
`ConfigValuesStructure` and `EcuCalDefStructure`. The
`legacy_definition_columns` header gives the adapter and the remaining
`FileActions` code a shared definition of the parallel legacy row layout.

The column header can compile alone, but it has no consumer or independently
observable behavior on master. Separating it would create an unused target and
make the adapter harder to review. The adapter, column schema, and
`test_legacy_definition_adapter` should remain one unit.

### `FileActions` migration

The changes in `file_actions.cpp`, `file_actions_ecuflash.cpp`,
`file_actions_romraider.cpp`, and `file_actions.h` are not a mechanical file
split. They replace legacy parsing and write paths with calls through
`DefinitionService` and `LegacyDefinitionAdapter`, and change construction to
require `IAtomicFileWriter`.

The deletion of `file_defs_ecuflash.cpp` and `file_defs_romraider.cpp` must
therefore land with the replacement implementations. Splitting only the file
movement would either duplicate implementations temporarily or require
throwaway forwarding code.

### Desktop composition and legacy tests

`mainwindow` and `settings` create `QtAtomicFileWriter` instances solely to
satisfy the migrated `FileActions` constructor. Their changes cannot compile
before that constructor changes and should remain with it.

Likewise, changes to:

- `tests/test_ecuflash_definition_parsing.cpp`
- `tests/test_file_actions_parsing.cpp`
- `tests/test_rom_transformations.cpp`
- `tests/BUILD.bazel`
- `bazel/mut_dma_test_suites.bzl`

adapt existing integration coverage to the new constructor and verify the
delegated behavior. Their dependency additions are consequences of those test
changes, not standalone build cleanup.

## Build-boundary changes

The changes in the root `BUILD.bazel` and
`scripts/check-portable-closure.py` extend the existing portable dependency
guard to the resolver, service, and writer. These entries should move with the
target they protect.

The wording change from "flash target" to "backend target" is accurate once
definition targets participate in the closure. It is not worth a separate PR.
The incidental ordering change around `ecuflash_parser` should be normalized
when extracting the target-specific hunks so an extraction PR contains no
reordering noise.

## Recommended next state

If further splitting is desired:

1. merge the resolver PR;
2. merge the writer plus `text_format` PR;
3. update `step5d3-definition-use-case` from master;
4. keep all remaining service, adapter, `FileActions`, desktop, and integration
   changes in the final feature PR.

After those two extractions, no remaining current-diff seam is both independent
and valuable enough to justify another PR.
