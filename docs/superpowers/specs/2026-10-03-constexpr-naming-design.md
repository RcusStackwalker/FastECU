# Identifier naming, slice 1: constexpr variables

Date: 2026-10-03

Issue: #49

## Problem

Identifier naming is enforced only by review, and it has drifted. Constexpr
variables are mostly `kCamelCase` (about 1,200 declarations), but 256 sites in
44 first-party files use something else: `lower_snake` test locals
(`cells`, `cases`), `UPPER_SNAKE` codec constants (`FRAME_LEN`), `camelBack`
(`dependentFalse`), trait members (`FamilyTraits<T>::family`), and legacy
tables (`fblocks_SH7058d`).

Issue #49 asks for consistent naming enforced by clang-tidy's
`readability-identifier-naming`. The target convention is Google C++ style,
adopted one identifier kind at a time. This slice is constexpr variables.

A second problem blocks enforcing any new check. CI's clang-tidy gate runs
`--changed`, which maps only changed C/C++ files to translation units. A PR
that edits `.clang-tidy` is analyzed only where it also touches source. A newly
enabled check's violations elsewhere reach master unseen. Because
`HeaderFilterRegex` covers every first-party header, they then fail whatever
unrelated PR next touches a file that includes the offending header. Every
naming slice edits `.clang-tidy`, so the gate has to cover that case.

## Goals

- Every constexpr variable follows Google style, and clang-tidy enforces it
  (`WarningsAsErrors: '*'` already makes findings fail the gate).
- A PR that changes a `.clang-tidy` file is analyzed across the whole tree in
  CI, on Linux and on Windows.
- The mechanism is reusable by later identifier kinds without redesign.

## Non-goals

- Other identifier kinds: constexpr functions, non-constexpr `const`
  globals, enumerators, types, functions, members. Each is a later slice with
  its own design. Constexpr functions follow whatever function rule a later
  slice adopts; Google has no separate rule for them.
- Google's exception for names "analogous to an existing C++ entity" (STL-like
  `_v` traits). The one instance, `family_requires_kernel_v`, is renamed
  instead.
- Widening the gate when `.clang-tidy-scope.toml` changes. The scope manifest
  has its own coverage guard.
- Renaming types or enumerators in the legacy headers this slice touches
  (`flashblock`, the `mcu_type` values).

## The rule

Constexpr variables of every storage duration are named `k` followed by
CamelCase: namespace scope, static data members, function locals, static
locals, and variable templates with their specializations. This is Google's
"Constant Names" rule. Google makes the prefix optional for automatic storage,
so requiring it everywhere stays compliant. It is also all clang-tidy can
express: the `ConstexprVariable` kind is matched before the storage-class
kinds and cannot tell them apart.

Underscores follow Google's exception ("where capitalization cannot be used for
separation"), encoded narrowly: an underscore must touch a digit on at least
one side.

```yaml
Checks: >
  ...
  readability-identifier-naming,
  ...
CheckOptions:
  - key: readability-identifier-naming.ConstexprVariableCase
    value: CamelCase
  - key: readability-identifier-naming.ConstexprVariablePrefix
    value: k
  # Google's digit-separator exception: an underscore may appear only next to
  # a digit (kFlashBlocksSH7058_1block). Matching skips the name entirely, so
  # the pattern itself enforces the k prefix and the CamelCase shape.
  - key: readability-identifier-naming.ConstexprVariableIgnoredRegexp
    value: '^k[A-Z]([A-Za-z0-9]|_?[0-9]_[A-Za-z]|_[0-9])*$'
```

`llvm::Regex` is POSIX ERE (no lookaround) and is matched against the full
identifier. Every underscore sits inside a token that also holds its digit
neighbour: `_[0-9]` (digit after) or `[0-9]_[A-Za-z]` (digit before, letter
after, optionally preceded by `_` so one digit can serve two underscores, as
in `_1_`). So no token can end on an underscore or produce `__`. Apart from the underscore, it is exactly as strict as `CamelCase`
with the `k` prefix: the first character after `k` must be an uppercase
letter. So a name it skips without an underscore would pass the usual check
anyway, and any name it rejects falls through to that check.

| Name | Result |
|---|---|
| `kMaxWireU24` | passes (plain CamelCase) |
| `kSubaruDensoMc68hc16y5_02BdmUploadChunk` | passes (`5_0`) |
| `kFlashBlocksN83M_1_5MB` | passes (`M_1`, `1_5`) |
| `kFoo_Bar` | flagged (no digit next to `_`) |
| `kFlashBlocksM32R_512KB_4blocks`, `kFlashBlocksMC68HC16Y5_TPU` | pass |
| `kFoo_`, `k_Foo`, `kFoo__1`, `kFoo5__6`, `kFoo5_`, `k5_0` | flagged |

Only the `ConstexprVariable` options are set. `readability-identifier-naming`
reports nothing for kinds without a configured style, so enabling the check
changes nothing else.

## Part 1: whole-tree gate on config change

- **Runner.** In `scripts/clang_tidy_runner.py`, when `--changed` finds a
  file named `.clang-tidy` (in any directory) that was added or modified, it
  skips `filter_changed_entries` and analyzes every entry the whole-tree mode
  would, after the OS scope manifest has been applied. It prints one line
  naming the trigger. This holds in both report and fix modes.
- **Tests** in `scripts/clang_tidy_runner_test.py`:
  - root `.clang-tidy` changed → all entries;
  - nested `.clang-tidy` changed → all entries;
  - `.clang-tidy` changed alongside sources → all entries;
  - `.clang-tidy` changed under `--scope-os windows` → all in-scope entries,
    not the whole compile database;
  - no config change → unchanged behavior.
- **CI timeout.** The `clang-tidy` job has `timeout-minutes: 30`. PR 1
  times a local whole-tree `bazel run //:clang_tidy_report -- --profile` and
  records the analysis phase in its description. If that phase, scaled from
  the local core count to the runner's four, exceeds 20 minutes, PR 1 raises
  the job timeout to 60 minutes. Otherwise the timeout stays, and PR 2's CI run
  is the first real measurement.
- **Docs.** The style guide's Static analysis section gains one sentence:
  changing a `.clang-tidy` file makes the changed-files run analyze the whole
  tree.

## Part 2: the constexpr slice

**Inventory.** clang-tidy 23 over the macOS compile database, with only the
`ConstexprVariable` options above (before the underscore exception): 256
violations in 44 first-party files. Generated moc sources add 50 more
(`qt_meta_stringdata_*`), but the runner already skips `bazel-out` entries.

| File | Sites | Kind |
|---|---|---|
| `src/backend/flash/flash_types.h` | 82 | trait members `family`, `transport`; `family_requires_kernel_v` specializations |
| `src/backend/flash/kernel/kernelmemorymodels.h` | 74 | legacy block tables, `flashdevices` |
| `src/backend/calibration/map_edit_test.cpp` | 34 | `static constexpr auto cells` |
| `src/algorithms/protocol/mut_dma/` | 6 | `UPPER_SNAKE` constants |
| 39 other files | 60 | 34 of them in tests, mostly locals |

Windows-exclusive code is not in the macOS compile database. A source scan of
the Windows scope prefixes found one non-conforming site,
`tests/serial_pty_e2e_test.cpp`, which the macOS database also covers;
everything compiled only on Windows already conforms.

**Hand-chosen renames.** These get chosen names, either because auto-fix would
produce a worse one or because auto-fix misses them:

| Today | Becomes | Why |
|---|---|---|
| `FamilyTraits<T>::family`, `::transport` | `::kFamily`, `::kTransport` | Auto-fix does not rename uses through dependent names (`Traits::family` in templates) |
| `family_requires_kernel_v<T>` | `kFamilyRequiresKernel<T>` | Plain rule; no STL-analogy exception |
| `dependentFalse<T>` | `kDependentFalse<T>` | Plain rule |
| `fblocks_`, `rblocks_`, `kblocks_`, `eblocks_` + MCU | `kFlashBlocks`, `kRamBlocks`, `kKernelBlocks`, `kEepromBlocks` + MCU | Abbreviations spelled out. The MCU suffix keeps the `mcu_type` enumerator's exact spelling (`SH7058_1block`, `N83M_1_5MB`), so table and enumerator stay greppable together |
| `flashdevices` | `kFlashDevices` | Plain rule |
| `FRAME_LEN`, `TRAILER_STD`, ... | `kFrameLen`, `kTrailerStd`, ... | Plain rule |

**Auto-fixed renames.** Everything else, mostly test locals (`cells` →
`kCells`, `cases` → `kCases`), is renamed by `bazel run //:clang_tidy_fix`
once the check is enabled. The runner rebuilds the analyzed targets after
applying fixes. Before committing, each auto-renamed `x` → `kX` is checked
against existing `kX` names in the same file, so a rename cannot silently start
shadowing another constant.

**Outside the compiler's view.** Fix-its do not touch comments, docs, or
macro bodies. Every old name is searched for across the whole tree, docs
included, and updated. Known sites: the
[flash qualification matrix](../../flash-qualification-matrix.md) cites
`fblocks_SH72543d[0]`, and comments in `flash_plan.h` and
`flash_validation_test.cpp` cite `family_requires_kernel_v`.

**Style guide.** The [coding style guide](../../coding-style.md) gains a
`## Naming` section:

- Constexpr variables are `kCamelCase` at every storage duration, citing
  Google's "Constant Names".
- An underscore may appear only next to a digit, with an example.
- clang-tidy enforces naming. Further identifier kinds are added one change at
  a time, and each change fixes every existing site in the same PR.

The guide's introduction changes in two places. Naming becomes an explicit
exception to "pre-existing sites are converted opportunistically". It also
joins the short list of rules with a mechanical check.

## Verification

1. **Regex.** Before touching code, run clang-tidy over a scratch translation
   unit holding the names in the table above. Confirm each passes or is flagged
   as listed. The scratch file is not committed.
2. **Whole tree, locally.** `bazel run //:clang_tidy_report` with the final
   config reports zero findings. `bazel build //...`, `bazel test //...`, and
   `prek run --all-files` pass.
3. **Old names.** A tree-wide search for every old name returns no hits.
4. **CI.** PR 2 edits `.clang-tidy`, so with Part 1 in place the Linux job
   analyzes the whole tree and the Windows job its whole scope. That is the
   cross-version check (CI's LLVM differs from the local 23) and the Windows
   check for code the macOS database cannot see.

## Slicing

A two-PR stack, built with `gh stack`:

1. **`ci/tidy-config-widening`** — "ci: whole-tree clang-tidy when .clang-tidy
   changes (#49, 1/2)". Runner change, tests, CI timeout if needed, style-guide
   sentence. Carries this spec and its plan.
2. **`style/constexpr-naming`** — "style: enforce kCamelCase constexpr
   variables (#49, 2/2)". Three commits so it reviews in layers:
   1. hand-chosen renames;
   2. enable the check in `.clang-tidy` and apply the auto-fixes;
   3. style-guide section, comment and doc updates, and removal of this spec
      and its plan.

Issue #49 stays open for later slices.

## Risks

- **Whole-tree CI duration.** Only PRs that change `.clang-tidy` pay for it,
  and the clang-tidy job runs in parallel with build and test. The first real
  measurement is PR 2's CI run. If it exceeds the timeout, raise the timeout in
  PR 2.
- **clang-tidy version skew.** Local LLVM 23; CI uses Ubuntu's packaged
  clang-tidy and Chocolatey's LLVM on Windows. `ConstexprVariable` and its
  `IgnoredRegexp` option are long established. PR 2's whole-tree CI run is the
  check.
- **Auto-fix misses.** Dependent names and macro bodies are not renamed. The
  former are hand-renamed first; the runner's post-fix build and the old-name
  search catch the rest.

## Success criteria

- `.clang-tidy` enables `readability-identifier-naming` with exactly the three
  `ConstexprVariable` options.
- PR 2's CI clang-tidy jobs run whole-tree (Linux) and whole-scope (Windows)
  with zero findings.
- No old name remains in code, comments, or docs.
- `bazel test //...` passes on all three OSes, and `prek` is clean.
- The style guide documents the rule and its enforcement policy.
