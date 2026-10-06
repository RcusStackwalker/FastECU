# Header Cleanup Stack Implementation Plan

Goal: Five focused, behaviour-preserving PRs based on merged #508.
Architecture: Keep portable definition policies. Fixed XML names are C-string literals; Unicode trim implementation is out-of-line; structure validation shares one loop; ParsedRomHeader owns identity and borrows its ROM node; typed drafts retain textual addresses until submission and named form editors replace runtime field-name lookup.
Spec: User approved all cleanup recommendations and the stronger typed authoring/shared ParsedRomHeader designs in this conversation.
Execution: Inline, one whole-stack independent review. Each PR is based on the preceding branch. Do not merge PRs into master.

Constraints: Preserve direct text/CDATA concatenation, notes/plain markup/whitespace, Unicode scalar trim, uint64 numeric policy, partial drafts, encoding autodetection, nested-markup errors, table text semantics, catalog canonical IDs, and selected-record validation timing. No dependencies or ErrorKind additions. Keep existing main checkout and unrelated files untouched.

Review focus: Notes through form/writer, raw invalid addresses editable until submit, borrowed DOM lifetime, root versus romid field selection, per-PR dependency/build integrity.

1. refactor/header-fixed-xml-names: metadata_fields.h uses const char* xml_name; writer passes it directly. Run definition tests. Commit.
2. refactor/header-trim-implementation: move whitespace table + trim body from text_format.h to text_format.cpp; leave declaration. Regenerate BUILD, run definition tests and release build. Commit.
3. refactor/header-structure-validation: share root/romid field validation loop with a parent + span-of-field-names helper; preserve known-field/error behavior. Run parser and draft tests. Commit.
4. refactor/header-parsed-rom-result: ParsedRomHeader stores rom + identity; derive romid. Remove EcuFlash ParsedHeader and return shared result from its loader wrapper. Update all parser/tests and borrowed-node expectations; run all definition tests. Commit.
5. refactor/header-typed-authoring: replace DefinitionHeaderFields with DefinitionHeaderDraft named strings (xml_id, internal_id, ecu_id, internal_id_address_text, include, notes) plus RomMetadata. Import returns draft, submission parses address + normalizes input. Form takes draft and returns named editors, with typed metadata editor bindings. Remove backend field-name vector lookups, flattened QStringList, parallel value lists, objectName-driven submission, and dead labels/names/lookup APIs. Preserve UI order/labels, notes lifetime, cancellation/errors, and logs. Migrate existing semantic regression tests; run portable/UI and full release tests, Gazelle, formatting, clang-tidy, dependency queries. Commit.

After whole-stack review and corrections: push five branches; create PRs sequentially with explicit preceding bases; link full stack in each description; verify remote heads/base relationships. Record per-layer production/test/build stats.


## Execution Results

All five layers implemented and published. Each PR is based on the immediately
preceding branch, with the bottom PR based on master. Remote heads and base
relationships were verified. The PR worktree remains available and clean.

- [510: refactor: use fixed XML name literals for metadata](https://github.com/RcusStackwalker/FastECU/pull/510) — `68c207ca`; production -1 lines.
- [511: refactor: compile Unicode header trimming out of line](https://github.com/RcusStackwalker/FastECU/pull/511) — `4e631647`; production +14 lines.
- [512: refactor: share header structure validation traversal](https://github.com/RcusStackwalker/FastECU/pull/512) — `d8737d21`; production -11 lines.
- [513: refactor: share one parsed ROM header result](https://github.com/RcusStackwalker/FastECU/pull/513) — `d046d4dc`; production -16 lines.
- [514: refactor: use typed drafts and editors for definition authoring](https://github.com/RcusStackwalker/FastECU/pull/514) — `ceabf706`; production -126 lines.

Verification: all 13 definition/authoring targets passed on each layer; complete
release build passed; final full release suite passed 251 applicable targets,
with seven Windows-only targets skipped locally. Final changed-file clang-tidy:
13 translation units clean, zero findings. Formatting, Gazelle/Buildifier, and
portable dependency checks passed. Independent whole-stack review found no
Critical, Important, Minor, or declined findings.

The typed editor mapping regression failed before implementation (changing a
widget objectName erased its ECU ID), and passes with named editor references.
Existing tests were migrated to the typed API while retaining partial drafts,
malformed address text, Unicode normalization, notes/CDATA/entity semantics,
UTF-16 import, strict-loader agreement, form labels/placement, widget lifetime,
cancellation, failed imports, and submission/catalog registration coverage.

Rulings: use existing semantic tests for pure reversible refactors; preserve the
nine editable metadata fields and keep unfinished address text until submission;
reuse the existing isolated worktree and preserve unrelated main-checkout files.
A constexpr naming gate was fixed in its owning unpublished validation commit;
the two descendants were rebased before publication. No behavior changed in that
fix. No deferred minors.

Total diff against merged #508: production -140, tests -201, build -1,
documentation +2, overall -340 lines. After merging each predecessor, retarget
and rebase its next layer onto master as needed for the repository's squash-merge
workflow.
