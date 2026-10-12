# Logging rewrite implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Deliver three independently reviewable prerequisite PRs and rewrite #562 according to the confirmed interview.

**Architecture:** Portable import resolves definition metadata; protocols own wire decoding. Prepared runs own compiled conversions and immutable identity bindings; explicit unavailable samples clear individual readings without losing healthy samples.

**Tech Stack:** C++23, Qt 6 at desktop boundaries, Bazel, GoogleTest.

**Spec:** [Confirmed interview](../specs/2026-10-11-logging-selection-rewrite.md).

## Global Constraints

- No Qt, owned threads, direct filesystem I/O or platform dependencies in backend/algorithms.
- Check Result/Status with .has_value(); no new ErrorKind without an ADR.
- Warnings are errors; preserve layer visibility and regenerate Gazelle BUILD files.
- Preserve master response framing, deadlines, cancellation and worker generation fencing.
- No migration modes, physical-read deduplication, new gauge renderer or unit-selector UI.
- Preserve completed design originals in separate delivered Git history before deletion.

## Review Focus

- Nested XML length must govern acquisition, including bundled two-byte examples.
- Signed, floating and endian-sensitive SSM values must decode before conversion.
- Valid expressions whose domains exclude 1, 16 and 1616 must prepare successfully.
- Failed conversion after a good sample must clear that reading rather than export stale data.
- Pending edits and file-output toggles must never change an active schema or overwrite output.

### Task 1: Canonical parameter width prerequisite PR

**Files:** src/backend/logging/logger_definition_parser.cpp and logger_definition_parser_test.cpp; resources/shared/config/logger_mut_dma_example.xml and logger_cdbg_example.xml; docs/reference/logging-contracts.md and relevant example docs.

**Interfaces:** Keep ParseLoggerDefinition(bytes::ByteView, std::string_view) and model types unchanged. Width comes solely from address/@length, default 1 if absent; parameter/@length supplies no fallback. No migration-specific handling.

- [ ] Add parser regressions for nested width 2 and ignored parent width, including both declarations with differing values; run focused test to observe expected failure.
- [ ] Read nested width only; move bundled width declarations onto address nodes and correct fixtures/docs to canonical syntax.
- [ ] Run focused package tests, Gazelle, prek, changed-source static analysis, full release tests and application build; stage named paths and commit feature branch.
- [ ] Review fixed diff and publish independent PR against current master.

### Task 2: Numeric SSM decoding prerequisite PR

**Files:** backend logging definition model/parser, logging_types.h, channel preparation/session validation, portable SSM read plan/protocol and corresponding tests; relevant desktop adapter fixtures; docs/reference/logging-contracts.md.

**Interfaces:** Retain protocol transport framing and deadline guards. Establish correct contiguous SSM byte-address requests and complete-response mapping alongside numeric decoding; reuse portable read-plan concepts from the existing PR rather than preserve Digital-slot offsets. Add portable typed storage/endian metadata to prepared channels; parse first conversion's storagetype/endian. No decimal-byte concatenation mode. Upstream default uses big-endian unsigned integer semantics when storage is omitted; support evidenced signed/unsigned 1/2/4-byte and float representations, reject incompatible selected fields.

- [ ] Add independently literal regressions: 01 02 supplies 258, signed negative, explicit little endian, IEEE float, and incomplete data rejected. Observe failures before implementation.
- [ ] Decode numeric binary bytes before checked scaling; propagate definition metadata through preparation, retaining finite numeric errors.
- [ ] Replace decimal-concatenation fixtures and owning contracts, preserving framing/timeout coverage. Verify focused and applicable full gates, commit.
- [ ] Review and publish separate PR; preserve a documented branch base for integration.

### Task 3: Compiled expressions prerequisite PR

**Files:** src/algorithms/expression/expression.h/.cpp and expression_test.cpp; backend logging session/types/conversion and tests; generated BUILD files and contracts.

**Interfaces:** Add expression::Compile(std::string_view) -> std::expected<CompiledExpression, EvaluationError>; immutable owned CompiledExpression::Evaluate(double) const -> std::expected<double, EvaluationError>. Keep EvaluateChecked as compile/evaluate wrapper for existing callers. LoggingSession owns compiled conversions, and polling uses them without reparsing. Preserve double arithmetic, syntax/nesting guards and finite/divide-by-zero checks.

- [ ] Add syntax-only compilation and repeated-evaluation regressions; valid 1/((x-1)*(x-16)*(x-1616)) must compile and prepare, while runtime domain errors remain checked. Observe failures.
- [ ] Implement owned reusable compiled representation; remove arbitrary trial evaluation and route conversion through run-owned compiled values.
- [ ] Test existing expression/calibration callers, runtime domain failures and logging preparation; regenerate BUILD, run applicable gates, commit.
- [ ] Review and publish independent prerequisite PR.

### Task 4: Rewrite #562 atop prerequisites and current master

**Files:** existing #562 backend logging model/run snapshot/CSV/use case/types; protocol read plans/drivers; desktop logging adapter/cache/file runtime; UI logging selections/indicators; package-owned tests; owning docs.

**Interfaces:** Retain public port boundaries. Add explicit per-measurement availability to delivered samples and resolved cache updates. Unknown support allows explicit selections, known unsupported/unresolved selected identities block preparation. CSV identity is (kind, protocol, ID), first occurrence in gauge/Digital/switch order. Distinct definitions remain separate acquisitions.

- [ ] Reconcile prerequisite commits into old reviewed feature history on a feature branch; remove dialect enum/attribute/default path and keep firmware-derived little-endian codec plus 96-entry/96-byte guards. Remove width fallback/duplicate fields and nested switch alias; retain RomRaider switch byte/bit, separate internal support/sample roles and ordered SSM address plans.
- [ ] Add failing regressions for unique CSV headers/no trailing empty column, good-to-unavailable-to-good measurements, healthy peer survival, run snapshot/edit isolation and conversion compilation integration. Observe expected failures before each behavior change.
- [ ] Implement unique export, explicit unavailable updates/cache clearing, bounded diagnostics, next-run edits, first conversion default, exclusive per-run/re-enabled files and nonfatal file output failure. Preserve acquisition union, identity-only sharing, current-master lifecycle guards.
- [ ] Update enduring contracts and remove superseded completed proposal artifacts only after originals are preserved in delivered history. Rewrite PR description around final scope and accurate validation.
- [ ] Run full release tests, application/macOS packaging, Gazelle, prek and changed-source static analysis; review full fixed diff on standards/spec axes, fix findings and verify. Push nonforce when authorized; do not merge master.
