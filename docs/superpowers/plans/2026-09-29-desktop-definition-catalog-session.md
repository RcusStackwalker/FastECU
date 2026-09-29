# Desktop Definition Catalog Session Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Replace FileActions catalog and authoring consumers with one desktop-owned session and retire the Qt-typed legacy definition state.

**Architecture:** DesktopComposition owns a DefinitionCatalogSession implementing IDefinitionCatalogs. It shares one existing DefinitionService with RomOpenUseCase, retains ordered startup lookup records separately from fresh validated catalogs, and registers authored files only after successful writes.

**Tech Stack:** C++23, Bazel, existing injected file ports, GoogleTest, existing Qt desktop dialogs and composition tests.

**Spec:** [Approved design](../specs/2026-09-29-desktop-definition-catalog-session-design.md).

## Global Constraints

- Portable parsing, matching, inheritance, and atomic writing remain in the existing DefinitionService.
- No new portable port, parser, worker, or threading model is introduced.
- User decisions stay in dialogs.
- All fallible methods return existing Status/Result types.
- Kernel model relocation and Qt byte-helper relocation remain subsequent 6n slices. Android work is out of scope.
- Existing hardware qualification remains separately tracked; this change establishes no new bench evidence.
- Add no transitional qt_layer entry or portable-to-platform dependency. Designed UI adapter visibility belongs on its target.

## Review Focus

- Conflicting authored IDs: startup lookup keeps the first source, while fresh catalogs still reject conflicting content (Task 2).
- Overwriting an authored path with a different ID: retained old lookup survives until refresh; fresh scanning reflects disk content (Task 2).
- Fresh catalog reads after a file disappears: the previous startup lookup still produces the ROM-open missing-file notice (Task 3).
- Cancelled authoring and invalid header fields: no write or lookup registration occurs (Task 3).
- Configuration rejection and repeated desktop construction: no partially constructed session is published, and borrowed services outlive consumers (Task 3).

## File map

- Create `src/platform/desktop/common/definition/definition_catalog_session.h`, `.cpp`, and `_test.cpp`: session, private state, and public-behavior tests.
- Create `src/platform/desktop/common/definition/definition_catalog_session_integration_test.cpp`: real file discovery/output and ROM-open integration.
- Create `src/platform/desktop/common/definition/BUILD.bazel`: library `definition_catalog_session`, GoogleTest targets `definition_catalog_session_test` and `definition_catalog_session_integration_test`.
- Modify `apps/desktop/desktop_composition.{h,cpp}`, `desktop_composition_test.cpp`, and `BUILD.bazel`: construction, ownership, and wiring.
- Modify `src/ui/desktop/main_window_services.h`, `mainwindow.{h,cpp}`, `mainwindow_test.cpp`, and `BUILD.bazel`: session injection and startup refresh calls.
- Modify `src/ui/desktop/definition/definition_authoring_dialog.{h,cpp}`, `definition_authoring_dialog_test.cpp`, and `BUILD.bazel`: submissions through the session.
- Modify `src/backend/calibration/session/definition_catalogs.h`: current implementation comment only; portable interface stays unchanged.
- Remove `src/backend/definitions/file_actions.{h,cpp}`, `definition_indexes.h`, `file_actions_parsing_test.cpp`, and `ecuflash_definition_parsing_test.cpp` after coverage migration.
- Modify `src/backend/definitions/BUILD.bazel`, `src/platform/desktop/common/ports/BUILD.bazel`, `bazel/qt/BUILD.bazel`, and root `BUILD.bazel`: retire dead targets and grants.
- Modify `docs/modularization-plan.md` and `docs/design-notes.md`: record this slice and remaining 6n work.

## Task 1: Typed startup lookup and fresh catalog provider

**Interfaces:** namespace `fastecu::desktop::definition`. Class `DefinitionCatalogSession final : public fastecu::calibration::IDefinitionCatalogs`.

```cpp
DefinitionCatalogSession(fastecu::definition::DefinitionService& definitions,
                         fastecu::config::ConfigSession& config,
                         fastecu::IFileSystem& file_system,
                         fastecu::IEventSink& events);
fastecu::Result<fastecu::definition::DefinitionCatalog>
catalog(fastecu::definition::DefinitionFormat format) override;
std::optional<std::string>
indexed_source(fastecu::definition::DefinitionFormat format, std::string_view id) override;
fastecu::Status refresh_index(fastecu::definition::DefinitionFormat format);
```

- [x] Write failing GoogleTests in `definition_catalog_session_test.cpp` using the existing ConfigSession fixture and package-owned file-port fakes. Seed lookup through refresh, never mutable test access. Pin: RomRaider handles `b.xml`, `a.xml` with IDs `ZZZ_FIRST`, `AAA_SECOND` retain their corresponding sources; unknown IDs and wrong-format lookups return `std::nullopt`; a fresh catalog call does not replace retained lookup.
- [x] Run `bazel test --config=release //src/platform/desktop/common/definition:definition_catalog_session_test`. Confirm failure comes from the missing class/API before implementing it.
- [x] Implement the constructor, catalog, indexed_source, and refresh_index. Store independent vectors of private `{std::string definition_id; std::string source;}` records for each format. Build a replacement vector before publishing it. Empty configured sources are a successful no-op. Failed scans return their error without replacing state. Retain the exact existing FileActions scan log strings/counts and RomRaider missing-file notice.
- [x] Add tests proving: empty configuration retains prior lookup; an injected directory-list error retains lookup and returns that error; successfully skipping a malformed sole definition clears lookup; a successful directory change drops old discovery; refresh of one format leaves the other untouched. For log assertions, preserve the exact messages currently asserted in `file_actions_parsing_test.cpp`.
- [x] Run the session tests again. Expected: PASS. Use no Qt dependency in the production library. Library visibility is `//bazel/layers:apps`, plus commented literal exceptions `//src/ui/desktop:__pkg__` and `//src/ui/desktop/definition:__pkg__`; package-owned tests have access to the library. Do not register this desktop target as a portable root.
- [x] Commit the tested provider and its BUILD file: `refactor(desktop): add typed definition catalog session`.

## Task 2: Successful authoring registration

**Consumes:** Task 1 constructor and IDefinitionCatalogs methods.

**Produces:**

```cpp
fastecu::Status submit_new_definition(
    std::string_view destination,
    const fastecu::definition::DefinitionHeaderInput& input,
    bool allow_overwrite);
fastecu::Status submit_imported_definition(
    std::string_view source, std::string_view destination,
    const fastecu::definition::DefinitionHeaderInput& input);
```

- [x] Write failing tests for successful create/import and failed writes. Use XML ID `NEW_XML`, internal ID `A1B2C3`, ECU ID `ECU-42`, and internal ID address `0x1A0` from the existing submission fixture. Assert a successful write makes indexed_source return the destination immediately; an injected writer error returns the exact error and retains lookup/catalog state. Invalid headers do not register a destination. Creating an existing file with `allow_overwrite=false` rejects it; `true` preserves the confirmed overwrite behavior.
- [x] Run the session test target and confirm the new cases fail before implementation.
- [x] Implement both submission methods by delegating to the shared DefinitionService. After success, insert the destination into a sorted, deduplicated vector of submitted handles and append `{input.xml_id, destination}` to the EcuFlash startup lookup. Preserve existing submission error log text. No rescan or new failure point follows a successful write.
- [x] Add real-file integration tests in `definition_catalog_session_integration_test.cpp`, migrating useful cases from both legacy definition suites. Assert: create/import round trips preserve serialized fields; imported maps/inheritance content survives; files authored outside the configured directory appear in fresh catalogs; directory changes retain submitted handles. Use Qt file ports and QTemporaryDir here, not in the production library.
- [x] Add duplicate-ID tests: submit ID `NEW_XML` to `outside/first.xml` then `outside/second.xml`; indexed_source stays `outside/first.xml`, while conflicting fresh catalog content returns InvalidConfig. Submit a changed ID to the same destination and assert old lookup remains before refresh, new catalog content reflects the write, and successful refresh drops the old lookup. Repeat same-handle submissions and assert fresh catalog contains one discovered entry, not duplicated content. Prove sorted handle processing via resulting catalog order with an empty discovered directory.
- [x] Run `bazel test --config=release //src/platform/desktop/common/definition:all`. Expected: PASS. Fakes with independent repository/writer storage must explicitly synchronize successful written bytes before fresh scans; use real ports for end-to-end output tests.
- [x] Commit the authoring methods and coverage: `refactor(desktop): register authored definitions through catalog session`.

## Task 3: Migrate composition, ROM opening, and dialogs

**Consumes:** Task 1 and Task 2 interfaces.

**Produces:** `MainWindowServices::definition_catalogs` is a `fastecu::desktop::definition::DefinitionCatalogSession&`; `DesktopComposition::definition_catalogs_` owns that session. DefinitionAuthoringDialog takes the session by reference in place of FileActions.

- [x] Update tests first: replace FileActions fixtures in `desktop_composition_test.cpp` and `mainwindow_test.cpp` with the new session sharing their DefinitionService. Assert repeated services() calls expose the same definition_catalogs reference, rejected startup constructs neither service nor session, and repeated composition construction/destruction preserves current lifecycle checks. Add real-session ROM-open cases to the new integration suite: authored file lookup succeeds; deleting an indexed file after refresh retains the missing-file notice currently covered by portable RomOpen tests.
- [x] Replace the parallel-list authoring test with tests using the session's submission and indexed_source behavior. Retain existing form labels, names, header-field extraction, dialog parent lifetime, and output assertions. Add event-loop-driven create/import cancellation and invalid-header cases; assert the writer is untouched and indexed_source remains absent. Use the existing offscreen QApplication test setup.
- [x] Run `bazel test --config=release //apps/desktop:desktop_composition_test //src/ui/desktop:test_mainwindow //src/ui/desktop/definition:definition_authoring_dialog_test //src/platform/desktop/common/definition:all`. Confirm expected compile/assertion failures before production migration.
- [x] Update composition to construct DefinitionService first, then DefinitionCatalogSession, then RomOpenUseCase and CalibrationWorkspace. Destruction order is workspace, opener, catalog session, service. Publish the session in MainWindowServices. Preserve failed-start behavior and startup refresh timing.
- [x] Update MainWindow and dialogs to consume the session. Replace startup list builders with refresh_index calls, checking returned Status without emitting duplicate errors already logged/noticed by the session. Dialog creation passes `allow_overwrite=true` following existing save confirmation. Remove record_definition and its declaration; neither dialog mutates session internals. Remove the obsolete MainWindow FileActions pointer. Update hand-owned moc target dependencies explicitly.
- [x] Run the previous focused command plus `bazel test --config=release //src/backend/calibration/session:all`. Expected: PASS. Add only target-level UI visibility for the designed adapter; do not expose the platform package generally to UI.
- [x] Commit the consumer migration: `refactor(desktop): use definition catalog session in composition and dialogs`.

## Task 4: Retire legacy targets and verify closure

**Consumes:** All desktop consumers and migrated coverage from Tasks 1–3.

**Produces:** No live FileActions/DefinitionIndexes consumers or Qt targets under backend/definitions; portable kernel `:models` remains there for the next slice.

- [x] Check all production references with `rg -n 'FileActions|DefinitionIndexes|file_actions\.h|definition_indexes\.h' apps src scripts BUILD.bazel`. Distinguish useful historical comments from live references. Check formatting wrappers and unused fields have no callers before deleting them.
- [x] Delete the five legacy files listed in the file map and replace `src/backend/definitions/BUILD.bazel` with the retained portable models target and its current visibility. Remove stale production/test dependencies, the backend/definitions qt_layer entry, and the test-only ports visibility exception. Remove the file_actions.h export and root windows_preprocessor_guards data entry; keep the guard and its surviving serial-header input.
- [x] Update the IDefinitionCatalogs implementation comment and the roadmap/design notes with ownership, compatibility rules, implemented slice status, remaining kernel/shim moves, and unchanged hardware qualification status.
- [x] Run `python3 scripts/gazelle_check.py --fix`, inspect the diff, then `python3 scripts/gazelle_check.py`. Expected: generated targets agree. Respect current managed scope; do not add an unrelated Gazelle expansion. Run `prek run --all-files` and resolve failures attributable to the change.
- [x] Run `bazel build --config=release //:portable_closure //apps/desktop:fastecu` and the focused tests from Task 3 plus `//:windows_preprocessor_guards`. Expected: successful build and PASS.
- [x] Inspect `bazel query 'deps(//src/ui/desktop:desktop)'` and relevant serial `implementation_deps`. Session headers are deliberately reachable; serial facade headers remain hidden from UI compile inputs. Validate that boundary with the sandboxed macOS build; require the existing Linux CI result as well.
- [x] Run `bazel build -k --config=release //...`, `bazel test -k --config=release //...`, and `bazel run --config=release //:clang_tidy_report_changed`. Record actual results and distinguish tool/environment blockers from failures. Require Windows/macOS/Linux CI and Windows/macOS packaging results before release; do not claim them from local checks.
- [x] Review the full diff against the approved spec, then commit: `refactor(desktop): retire FileActions and legacy definition indexes`. Report files changed, verification evidence, and remaining 6n work. Branch integration follows the user's selected workflow; no merge or publication is implied.

## Execution results — 2026-09-29

Native execution completed all four tasks. The release build covered 769 targets;
the complete release test suite passed 237 tests, with seven Windows-only tests
skipped on macOS. Focused coverage includes 13 session cases, nine real-file/ROM
integration cases, offscreen cancellation and invalid-header flows, 75 MainWindow
cases, and desktop composition lifecycle tests.

Gazelle generation/check and all-file prek checks passed. Changed-file clang-tidy
reported nine translation units clean with zero findings. Local macOS packaging with the Bazel-pinned Qt 6.8.3 deployment tool produced
an app zip without deployment errors. Strict bundle signature verification failed
with "code has no resources but signature indicates they must be present";
macOS packaging remains unqualified. The initial run with Homebrew Qt 6.11.1
had unresolved library paths despite returning zero, so its emitted zip is not
verification evidence. The packaging script is unchanged by this slice. Portable closure and sandboxed macOS
UI compilation passed; serial facade implementation dependencies remain intact.
Windows/Linux CI, Windows packaging, and hardware qualification remain release
gates to be recorded separately.

Implementation decisions: use the existing branch after sandbox-blocked worktree
creation; grant this package narrow access to the configuration test fixture;
keep header metadata notes and document notes distinct; explicitly register bundle
resources in composition and provisioning test fixtures; explicitly discard
already-reported nonfatal startup scan errors.

Independent final review found no Critical or Important issues. Its sole Minor
finding was the premature packaging PASS record, corrected above after inspecting
the deploy logs and verifying the resulting bundle. No implementation fix was
required. Hardware behavior, Windows/Linux runtime qualification, macOS artifact
deployability, kernel-model relocation and byte-helper relocation remain outside
this slice's completion claim.
