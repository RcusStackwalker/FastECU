# Constexpr Naming Slice Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enforce Google-style `kCamelCase` constexpr variable names with clang-tidy, after making CI analyze the whole tree whenever a `.clang-tidy` file changes.

**Architecture:** Two stacked PRs.
- **PR 1** teaches `scripts/clang_tidy_runner.py --changed` to analyze every in-scope translation unit when a `.clang-tidy` file changed. It edits `.clang-tidy` itself (a comment), so its own CI makes the first whole-tree run under CI's clang-tidy. It opens early, so that run happens while PR 2 is being built.
- **PR 2** renames all 256 non-conforming constexpr variables, enables the check, and documents the rule. Names that need judgment are substituted by hand first; clang-tidy auto-fix handles the rest.

**Tech Stack:** Python 3 (`unittest`) for the runner; clang-tidy / run-clang-tidy (Homebrew LLVM 23 locally, clang-tidy 21.1.6 on CI Linux, LLVM 20.1.8 on CI Windows); Bazel (`--config=release`); C++23 sources; `prek`; `gh stack`.

**Spec:** [constexpr naming design](../specs/2026-10-03-constexpr-naming-design.md)

## Global Constraints

- The rule: constexpr variables of every storage duration are `k` + CamelCase.
- The `.clang-tidy` options are exactly these three. No other naming kind gets a style in this work.
  - `readability-identifier-naming.ConstexprVariableCase: CamelCase`
  - `readability-identifier-naming.ConstexprVariablePrefix: k`
  - `readability-identifier-naming.ConstexprVariableIgnoredRegexp: '^k[A-Z]([A-Za-z0-9]|_?[0-9]_[A-Za-z]|_[0-9])*$'`
- Every non-conforming site is renamed. No `NOLINT`, no ignore list, no per-directory config.
- Warnings are errors. Never add a `#pragma`, `-Wno-*`, or `-treat_warnings_as_errors` opt-out.
- **Local clang-tidy is not CI's clang-tidy.**
  - Local LLVM 23 reports 53 findings on today's tree that CI's 21.1.6/20.1.8 do not: 48 `bugprone-signed-bitwise`, 4 `readability-uppercase-literal-suffix` (literals inside `EXPECT_EQ` arguments in `qt_identify_launcher_test.cpp`), and 1 `bugprone-unhandled-code-paths`. They are out of scope.
  - Never "fix" them in this work. Never use `//:clang_tidy_fix` here, because it would apply them.
  - Local checks run the naming rule only. The full-config whole-tree gate is CI's.
- Branches:
  - `ci/tidy-config-widening` (PR 1) already exists, holding the spec commits.
  - `style/constexpr-naming` (PR 2) branches from it.
- PR titles: `ci: whole-tree clang-tidy when .clang-tidy changes (#49, 1/2)` and `style: enforce kCamelCase constexpr variables (#49, 2/2)`.
- Commit only on those branches (`prek` refuses `master`). End every commit message with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01WuhQPxAkEnfKWKGyaiajxH
  ```
- Old-name searches exclude `docs/superpowers/`. The spec and this plan cite old names on purpose, and Task 6 deletes both.
- clang-tidy on macOS needs Homebrew LLVM first on `PATH`: `export PATH="$(brew --prefix llvm)/bin:$PATH"`.
- Pushing branches and opening PRs needs the user's explicit go-ahead (Tasks 3 and 7).

## Review Focus

1. **Look-alike config names must not widen the gate.** `.clang-tidy-scope.toml`, `old.clang-tidy`, and `.clang-tidy.bak` must keep the normal changed-files behavior. Pinned in Task 1, `test_config_changed_ignores_look_alike_names`.
2. **A widened run on Windows must stay inside the Windows scope.** Analyzing the whole compile database there would time out and analyze code Linux already covers. Pinned in Task 1, `test_scope_os_with_changed_config_analyzes_the_whole_scope`.
3. **A renamed constant still referenced from code no local compile database contains.** The fix pass only sees TUs in the macOS database, and the Windows build would break. Pinned in Tasks 4 and 5 by the tree-wide old-name search over every tracked file.
4. **An auto-renamed local capturing an outer constant of the same new name.** For example, a local `cases` → `kCases` in a file that already uses a namespace-scope `kCases`, which silently changes what later code reads. Pinned in Task 5, Step 6, by the collision check.
5. **Docs and comments citing old names.** The flash qualification matrix and bench checklists are read by operators, and a stale table name there misdirects a bench check. Pinned in Task 4, Step 7, and Task 6, Step 5, by the old-name search including `docs/`.

---

## PR 1 — `ci/tidy-config-widening`

### Task 1: `--changed` analyzes the whole scope when a `.clang-tidy` file changed

**Files:**
- Modify: `scripts/clang_tidy_runner.py` (constants near line 23; `run_workflow` filter phase near lines 849–854)
- Test: `scripts/clang_tidy_runner_test.py` (`run_scoped` near line 500; new tests after `test_changed_mode_skips_analysis_when_nothing_matches`, near line 1540)
- Modify: `docs/coding-style.md` (Static analysis section, after the "In CI, clang-tidy runs as its own `clang-tidy` job" sentence near line 433)

**Interfaces:**
- Produces: `runner.CONFIG_FILE_NAME: str` (`".clang-tidy"`) and `runner.config_changed(changed: Sequence[Path]) -> bool`.
- Produces: the stdout line `clang-tidy: a .clang-tidy file changed; analyzing every translation unit in scope.` Tasks 3 and 7 look for it in CI logs.

- [ ] **Step 1: Make `run_scoped` accept the changed paths**

In `scripts/clang_tidy_runner_test.py`, replace the `run_scoped` signature and its `changed_files` patch:

```python
    def run_scoped(
        self,
        *,
        changed: bool = False,
        scope_os: str = "windows",
        changed_paths: list[Path] | None = None,
    ) -> tuple[list[list[str]], str]:
```

and inside it:

```python
            mock.patch.object(
                runner,
                "changed_files",
                return_value=[self.root / "p.h"] if changed_paths is None else changed_paths,
            ),
```

- [ ] **Step 2: Write the failing tests**

Add after `test_changed_mode_skips_analysis_when_nothing_matches`, inside `ClangTidyRunnerTest`:

```python
    def run_changed_workflow(self, committed: list[str]) -> tuple[list[str], str]:
        """Report mode with --changed, where `committed` is the branch's diff.

        Returns the files handed to run-clang-tidy and the captured stdout.
        """
        analyzed_files: list[str] = []

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            if command == ["git", "merge-base", "HEAD", "origin/master"]:
                return subprocess.CompletedProcess(command, 0, stdout="abc123\n")
            if command == ["git", "diff", "--name-only", "abc123..HEAD"]:
                return subprocess.CompletedProcess(
                    command, 0, stdout="".join(f"{path}\n" for path in committed)
                )
            if command[:2] == ["git", "diff"] or command[:2] == ["git", "ls-files"]:
                return subprocess.CompletedProcess(command, 0, stdout="")
            if command == ["xcrun", "--show-sdk-path"]:
                return subprocess.CompletedProcess(command, 0, stdout="/SDK/MacOSX.sdk\n")
            if command[0] == _UNIX_TOOLS.run_clang_tidy:
                compdb_dir = Path(command[command.index("-p") + 1])
                database = json.loads((compdb_dir / "compile_commands.json").read_text())
                analyzed_files.extend(entry["file"] for entry in database)
            return subprocess.CompletedProcess(command, 0)

        output = StringIO()
        with (
            mock.patch.object(runner, "discover_tools", return_value=_UNIX_TOOLS),
            redirect_stdout(output),
        ):
            runner.run_workflow(
                mode="report",
                workspace=self.root,
                compdb_tool=_UNIX_COMPDB_TOOL,
                platform_name="darwin",
                environ={},
                command_runner=fake_run,
                changed=True,
            )
        return analyzed_files, output.getvalue()

    def two_sources(self) -> list[str]:
        first = self.root / "first.cpp"
        first.write_text("int first;\n")
        second = self.root / "second.cpp"
        second.write_text("int second;\n")
        self.write_database([first, second])
        return sorted([str(first), str(second)])

    def test_config_changed_matches_clang_tidy_files_at_any_depth(self) -> None:
        self.assertTrue(runner.config_changed([self.root / ".clang-tidy"]))
        self.assertTrue(runner.config_changed([self.root / "src" / "pkg" / ".clang-tidy"]))
        self.assertTrue(
            runner.config_changed([self.root / "a.cpp", self.root / ".clang-tidy"])
        )

    def test_config_changed_ignores_look_alike_names(self) -> None:
        self.assertFalse(runner.config_changed([]))
        self.assertFalse(
            runner.config_changed(
                [
                    self.root / ".clang-tidy-scope.toml",
                    self.root / "old.clang-tidy",
                    self.root / ".clang-tidy.bak",
                    self.root / "docs" / "clang-tidy.md",
                    self.root / _MAIN_CPP,
                ]
            )
        )

    def test_changed_root_config_analyzes_every_translation_unit(self) -> None:
        expected = self.two_sources()

        analyzed, output = self.run_changed_workflow([".clang-tidy"])

        self.assertEqual(expected, sorted(analyzed))
        self.assertIn("a .clang-tidy file changed", output)
        self.assertNotIn("no changed C/C++ translation units", output)

    def test_changed_nested_config_analyzes_every_translation_unit(self) -> None:
        expected = self.two_sources()
        (self.root / "pkg").mkdir()
        (self.root / "pkg" / ".clang-tidy").write_text("InheritParentConfig: true\n")

        analyzed, _ = self.run_changed_workflow(["pkg/.clang-tidy"])

        self.assertEqual(expected, sorted(analyzed))

    def test_changed_config_alongside_a_source_still_analyzes_everything(self) -> None:
        expected = self.two_sources()

        analyzed, _ = self.run_changed_workflow([".clang-tidy", "first.cpp"])

        self.assertEqual(expected, sorted(analyzed))

    def test_changed_scope_manifest_alone_does_not_widen(self) -> None:
        self.two_sources()
        self.write_manifest("[os.windows]\nprefixes = []\n")

        analyzed, output = self.run_changed_workflow([".clang-tidy-scope.toml"])

        self.assertEqual([], analyzed)
        self.assertIn("no changed C/C++ translation units", output)

    def test_scope_os_with_changed_config_analyzes_the_whole_scope(self) -> None:
        self.scoped_fixture()

        _, output = self.run_scoped(changed=True, changed_paths=[self.root / ".clang-tidy"])

        # win/w.cpp only: widened, but p.cpp stays outside the windows scope.
        self.assertIn("Analyzing 1 translation units", output)
        self.assertNotIn("no changed C/C++ translation units", output)
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `bazel test --config=release //:clang_tidy_runner_test --test_output=errors`
Expected: FAIL. The two `config_changed` tests fail with `AttributeError: module 'clang_tidy_runner' has no attribute 'config_changed'`. The four widening tests fail on their assertions, because only the changed source is analyzed, or analysis is skipped with the "no changed C/C++ translation units" message. `test_changed_scope_manifest_alone_does_not_widen` already passes.

- [ ] **Step 4: Implement**

In `scripts/clang_tidy_runner.py`, after `HEADER_SUFFIXES` (line 24):

```python
CONFIG_FILE_NAME = ".clang-tidy"
```

After `filter_changed_entries` (before `_search_directories`):

```python
def config_changed(changed: Sequence[Path]) -> bool:
    """Whether a clang-tidy configuration file is among the changed paths.

    A config change can produce findings in files the change never touched,
    so the changed-files run analyzes everything in scope instead.
    """
    return any(path.name == CONFIG_FILE_NAME for path in changed)
```

In `run_workflow`, replace:

```python
        if changed:
            changed_paths = changed_files(workspace, command_runner)
            entries, notes = filter_changed_entries(entries, changed_paths, workspace.resolve())
            for note in notes:
                print(note)
```

with:

```python
        if changed:
            changed_paths = changed_files(workspace, command_runner)
            if config_changed(changed_paths):
                print(
                    f"clang-tidy: a {CONFIG_FILE_NAME} file changed; "
                    "analyzing every translation unit in scope."
                )
            else:
                entries, notes = filter_changed_entries(
                    entries, changed_paths, workspace.resolve()
                )
                for note in notes:
                    print(note)
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `bazel test --config=release //:clang_tidy_runner_test --test_output=errors`
Expected: PASS (all tests, including the pre-existing ones).

- [ ] **Step 6: Document the widening in the style guide**

In `docs/coding-style.md`, replace:

```markdown
  In CI, clang-tidy runs as its own `clang-tidy` job (Linux, plus Windows for
  Windows-exclusive code), in parallel with the Bazel build and test job.
```

with:

```markdown
  In CI, clang-tidy runs as its own `clang-tidy` job (Linux, plus Windows for
  Windows-exclusive code), in parallel with the Bazel build and test job.
  Changing any `.clang-tidy` file widens that changed-files run to every
  translation unit in the OS's scope, because a new or retuned check can fire
  in files the change never touched.
```

- [ ] **Step 7: Lint and commit**

Run: `prek run --files scripts/clang_tidy_runner.py scripts/clang_tidy_runner_test.py docs/coding-style.md`
Expected: all hooks pass. If ruff-format rewrote a file, re-run `bazel test --config=release //:clang_tidy_runner_test` and re-stage.

```bash
git add scripts/clang_tidy_runner.py scripts/clang_tidy_runner_test.py docs/coding-style.md
git commit -m "$(cat <<'EOF'
ci: analyze the whole scope when a .clang-tidy file changes (#49)

--changed maps only changed C/C++ files to translation units, so a PR
that enables or retunes a check was analyzed only where it also touched
source. Any changed file named .clang-tidy now widens the run to every
translation unit in the OS's scope.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WuhQPxAkEnfKWKGyaiajxH
EOF
)"
```

### Task 2: Budget for whole-tree runs and make PR 1 exercise its own gate

**Files:**
- Modify: `.clang-tidy` (prepend a comment)
- Modify: `.github/workflows/pr.yml:213`

**Interfaces:**
- Consumes: Task 1's widening, which this `.clang-tidy` edit triggers in PR 1's own CI.
- Produces: the measurement quoted in PR 1's description. A local whole-tree `bazel run --config=release //:clang_tidy_report -- --profile`, taken while planning on LLVM 23 with 15 cores, gave:
  - `prebuild` 133.6 s and `analysis` 653.0 s over 481 translation units;
  - about 41 minutes once scaled to 4 cores (`653 × 15 / 4 / 60`).

- [ ] **Step 1: Prepend the cost comment to `.clang-tidy`**

Insert as the first lines of `.clang-tidy`, above the existing `# bugprone-easily-swappable-parameters - ...` block:

```yaml
# Editing this file makes CI's changed-files clang-tidy run analyze every
# translation unit in scope instead of only the changed ones
# (config_changed in scripts/clang_tidy_runner.py): budget for a full run.
#
```

- [ ] **Step 2: Raise the clang-tidy job timeout**

In `.github/workflows/pr.yml`, change the `clang-tidy` job's timeout. That is the job named `clang-tidy (${{ matrix.os }})` at line 213, not the `gazelle` job at line 36. Replace:

```yaml
    runs-on: ${{ matrix.os }}
    timeout-minutes: 30
```

with:

```yaml
    runs-on: ${{ matrix.os }}
    # 60, not 30: a .clang-tidy change makes the run whole-tree, which took
    # ~41 minutes when scaled to a 4-core runner (scripts/clang_tidy_runner.py).
    timeout-minutes: 60
```

Verify the right job changed: `git diff .github/workflows/pr.yml` shows exactly one `timeout-minutes` line changed, inside the `clang-tidy:` job.

- [ ] **Step 3: Lint and commit**

Run: `prek run --files .clang-tidy .github/workflows/pr.yml`
Expected: all hooks pass.

```bash
git add .clang-tidy .github/workflows/pr.yml
git commit -m "$(cat <<'EOF'
ci: budget 60 minutes for whole-tree clang-tidy runs (#49)

A whole-tree run analyzed 481 translation units in 653 s on 15 local
cores, about 41 minutes on a 4-core runner. The new .clang-tidy comment
tells editors what a config change costs, and makes this PR's own CI
take the whole-tree path under CI's clang-tidy.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WuhQPxAkEnfKWKGyaiajxH
EOF
)"
```

### Task 3: Open PR 1 and read its whole-tree run

**Files:** none, unless Step 4 finds something.

**Interfaces:**
- Consumes: Task 1's widening message; Task 2's measurement.
- Produces: an open PR 1 whose clang-tidy jobs ran whole-tree green. PR 2 is built on top of it.

- [ ] **Step 1: Get the go-ahead**

Ask the user to authorize pushing `ci/tidy-config-widening` and opening PR 1. Do not push without it.

- [ ] **Step 2: Push and open PR 1**

```bash
git push -u origin ci/tidy-config-widening
gh pr create --base master --head ci/tidy-config-widening \
  --title "ci: whole-tree clang-tidy when .clang-tidy changes (#49, 1/2)" --body-file - <<'EOF'
## What
- `--changed` analyzes every translation unit in the OS's scope when a `.clang-tidy` file changed (`config_changed` in `scripts/clang_tidy_runner.py`), with tests.
- `.clang-tidy` gains a comment saying what editing it costs; that edit also makes this PR's own CI take the whole-tree path.
- The clang-tidy job timeout goes from 30 to 60 minutes.
- Style guide: one sentence in Static analysis.
- Carries the design spec and plan for #49's first slice (removed again in 2/2).

## Why
`--changed` only maps changed C/C++ files to translation units. A PR that enables or retunes a check was analyzed only where it also touched source, so the new check's findings elsewhere reached master unseen.

## Verification
- `bazel test //:clang_tidy_runner_test` green.
- Local whole-tree report (LLVM 23, 15 cores): 481 translation units, analysis 653 s, about 41 min scaled to 4 cores.
- This PR's own Linux and Windows clang-tidy jobs run whole-tree and whole-scope under CI's clang-tidy (21.1.6 and 20.1.8).

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01WuhQPxAkEnfKWKGyaiajxH
EOF
```

- [ ] **Step 3: Confirm both clang-tidy jobs widened**

PR 2 work (Task 4 onward) can proceed while this runs.

```bash
gh pr checks ci/tidy-config-widening --watch
RUN_ID="$(gh run list --branch ci/tidy-config-widening --workflow pr.yml --limit 1 --json databaseId --jq '.[0].databaseId')"
gh run view "$RUN_ID" --log | grep -E "clang-tidy: (a .clang-tidy file changed|analysis took)|Analyzing [0-9]+ translation units"
```

Expected: the line `clang-tidy: a .clang-tidy file changed; analyzing every translation unit in scope.` appears in both the `clang-tidy (ubuntu-26.04)` and `clang-tidy (windows-latest)` jobs. Linux analyzes several hundred translation units, Windows its scope. Both jobs pass. Note the Linux `analysis took` figure for the PR description.

- [ ] **Step 4: Handle what the run reveals**

- **Job hit the 60-minute timeout:** raise `timeout-minutes` in the same `clang-tidy` job to `90`, update the comment's figure, commit on `ci/tidy-config-widening`, push, and re-check.
- **Findings under CI's clang-tidy:** these are latent findings the old gate never saw. List them for the user with per-check counts and wait for direction before fixing anything. The spec assigns their fixes to PR 1, but the count may change the plan.
- **`//tests:serial_backend_tests` failed on Windows:** re-run the job once before investigating; it is a known pre-existing flake.

Edit the PR description's Verification section with the CI analysis time.

---

## PR 2 — `style/constexpr-naming`

### Task 4: Hand-chosen renames

**Files:**
- Modify: `src/backend/flash/flash_types.h`, `src/backend/flash/flash_validation.cpp` (traits)
- Modify (by search-and-replace): every tracked file citing `family_requires_kernel_v`, `[frke]blocks_*`, `flashdevices`, the six mut_dma constants, or `dependentFalse`. Today that is about 45 files under `src/` and `tests/`, plus `docs/flash-qualification-matrix.md`.

**Interfaces:**
- Produces, for Task 5 and later code: `FamilyTraits<T>::kFamily`, `FamilyTraits<T>::kTransport`, `kFamilyRequiresKernel<T>`, `kDependentFalse<T>`, `kFlashBlocks<MCU>`, `kRamBlocks<MCU>`, `kKernelBlocks<MCU>`, `kEepromBlocks<MCU>` (MCU suffix spelled exactly as the `mcu_type` enumerator, e.g. `kFlashBlocksSH7058_1block`), `kFlashDevices`, `mutdma::kFrameLen`, `kTrailerStd`, `kTrailerFreeform`, `kChecksumOffset`, `kTrailerOffset`, `kMaxWriteChunk`.

- [ ] **Step 1: Create the PR 2 branch**

```bash
git switch ci/tidy-config-widening
git switch -c style/constexpr-naming
```

- [ ] **Step 2: Rename the trait members**

`FamilyTraits<T>::family` is used through a dependent name inside a generic lambda (`src/backend/flash/flash_validation.cpp:43`), which clang-tidy's auto-fix does not rename.

```bash
perl -pi -e 's/static constexpr FlashFamily family =/static constexpr FlashFamily kFamily =/; s/static constexpr TransportKind transport =/static constexpr TransportKind kTransport =/' src/backend/flash/flash_types.h
perl -pi -e 's/FamilyTraits<T>::family\b/FamilyTraits<T>::kFamily/g; s/FamilyTraits<T>::transport\b/FamilyTraits<T>::kTransport/g' src/backend/flash/flash_validation.cpp
grep -c "static constexpr FlashFamily kFamily =" src/backend/flash/flash_types.h
grep -c "static constexpr TransportKind kTransport =" src/backend/flash/flash_types.h
git grep -n -P 'FamilyTraits<\w+>::(family|transport)\b'
```

Expected: `30`, `30`, and no output from the last command.

- [ ] **Step 3: Rename the kernel-requirement trait**

```bash
git grep -l -w family_requires_kernel_v -- ':!docs/superpowers' | xargs perl -pi -e 's/\bfamily_requires_kernel_v\b/kFamilyRequiresKernel/g'
grep -c "kFamilyRequiresKernel" src/backend/flash/flash_types.h
```

Expected: `23` (22 declarations plus one comment).

- [ ] **Step 4: Rename the kernel memory tables**

The struct fields `fblocks`, `rblocks`, `kblocks`, `eblocks` (no suffix, e.g. `flashdevices[index].fblocks[0]`) are not constexpr variables and must not change. The patterns require `_` plus a suffix, so they are left alone.

```bash
git grep -l -P '\b[frke]blocks_\w+|\bflashdevices\b' -- ':!docs/superpowers' | xargs perl -pi -e 's/\bfblocks_(\w+)/kFlashBlocks$1/g; s/\brblocks_(\w+)/kRamBlocks$1/g; s/\bkblocks_(\w+)/kKernelBlocks$1/g; s/\beblocks_(\w+)/kEepromBlocks$1/g; s/\bflashdevices\b/kFlashDevices/g'
git grep -c -P '\b[frke]blocks\b(?!_)' -- 'src/*' | head -3
```

Expected: the last command still lists files. The bare struct fields survive.

- [ ] **Step 5: Rename the mut_dma constants and `dependentFalse`**

```bash
git grep -l -w -E 'FRAME_LEN|TRAILER_STD|TRAILER_FREEFORM|CHECKSUM_OFFSET|TRAILER_OFFSET|MAX_WRITE_CHUNK|dependentFalse' -- ':!docs/superpowers' | xargs perl -pi -e 's/\bFRAME_LEN\b/kFrameLen/g; s/\bTRAILER_STD\b/kTrailerStd/g; s/\bTRAILER_FREEFORM\b/kTrailerFreeform/g; s/\bCHECKSUM_OFFSET\b/kChecksumOffset/g; s/\bTRAILER_OFFSET\b/kTrailerOffset/g; s/\bMAX_WRITE_CHUNK\b/kMaxWriteChunk/g; s/\bdependentFalse\b/kDependentFalse/g'
```

- [ ] **Step 6: Format the touched files**

Renamed identifiers are longer, so lines may now exceed the column limit.

Run: `prek run --files $(git diff --name-only)`
Expected: clang-format may rewrite files on the first run; a second run passes. lychee passes on `docs/flash-qualification-matrix.md`.

- [ ] **Step 7: Verify no old hand-renamed name survives anywhere**

```bash
git grep -n -P '\b([frke]blocks_\w+|flashdevices|family_requires_kernel_v|dependentFalse|FRAME_LEN|TRAILER_STD|TRAILER_FREEFORM|CHECKSUM_OFFSET|TRAILER_OFFSET|MAX_WRITE_CHUNK)\b' -- ':!docs/superpowers'
```

Expected: no output. This search covers Windows-only sources, docs, and comments that no compile database sees.

- [ ] **Step 8: Build and test**

Run: `bazel test --config=release //...`
Expected: PASS. (`//tests:serial_backend_tests` is a known pre-existing Windows-only flake; it does not run on macOS.)

- [ ] **Step 9: Commit**

```bash
git add -A src tests docs/flash-qualification-matrix.md
git status --short   # only renamed sources and the matrix; nothing else
git commit -m "$(cat <<'EOF'
style: hand-rename constexpr traits, kernel tables and codec constants (#49)

Names clang-tidy's auto-fix would get wrong or miss: FamilyTraits members
used through dependent names, kFamilyRequiresKernel (no std-style _v),
the kernel block tables with spelled-out prefixes and the mcu_type
spelling kept as the suffix, and the mut_dma and bytes_compose constants
renamed tree-wide by word boundary.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WuhQPxAkEnfKWKGyaiajxH
EOF
)"
```

### Task 5: Enable the check and apply the auto-fixes

**Files:**
- Modify: `.clang-tidy` (`Checks` list; `CheckOptions`)
- Modify (by `run-clang-tidy -fix`): about 40 files with the remaining 99 sites, mostly test locals. The largest is `src/backend/calibration/map_edit_test.cpp` (32 `cells`).

**Interfaces:**
- Consumes: Task 4's renamed names, so the fix pass reports nothing for them.
- Produces: an enforced `.clang-tidy`. From here, every TU the gate analyzes is checked for the rule.

- [ ] **Step 1: Enable the check**

In `.clang-tidy`, insert `readability-identifier-naming,` into `Checks` between `readability-duplicate-include,` and `readability-redundant-member-init,`. Append to `CheckOptions`:

```yaml
  # Google C++ style, adopted one identifier kind at a time (#49); see the
  # Naming section of docs/coding-style.md.
  - key: readability-identifier-naming.ConstexprVariableCase
    value: CamelCase
  - key: readability-identifier-naming.ConstexprVariablePrefix
    value: k
  # Google's exception: an underscore may separate words only where
  # capitalization cannot, i.e. next to a digit (kFlashBlocksSH7058_1block).
  # Every underscore sits in a token holding its digit neighbour, so no name
  # can end on one or contain "__". A matching name skips the check entirely,
  # which is why the pattern itself spells out the k prefix and CamelCase.
  - key: readability-identifier-naming.ConstexprVariableIgnoredRegexp
    value: '^k[A-Z]([A-Za-z0-9]|_?[0-9]_[A-Za-z]|_[0-9])*$'
```

- [ ] **Step 2: Probe the rule with the committed config**

```bash
export PATH="$(brew --prefix llvm)/bin:$PATH"
PROBE="$(mktemp -d)"
cat > "$PROBE/probe.cpp" <<'EOF'
// Must pass.
constexpr int kMaxWireU24 = 1;
constexpr int kSubaruDensoMc68hc16y5_02BdmUploadChunk = 2;
constexpr int kFlashBlocksN83M_1_5MB = 3;
constexpr int kFlashBlocksSH7058_1block = 4;
constexpr int kFlashBlocksM32R_512KB_4blocks = 5;
constexpr int kFlashBlocksMC68HC16Y5_TPU = 6;
int f() { constexpr int kLocal = 7; static constexpr int kStaticLocal = 8; return kLocal + kStaticLocal; }
template <typename> inline constexpr bool kDependentFalse = false;
struct Traits { static constexpr int kFamily = 9; };
// Must be flagged.
constexpr int kFoo_Bar = 10;
constexpr int kFoo_ = 11;
constexpr int k_Foo = 12;
constexpr int kFoo__1 = 13;
constexpr int kFoo5__6 = 14;
constexpr int kFoo5_ = 15;
constexpr int k5_0 = 16;
constexpr int lower_case = 17;
constexpr int UPPER_CASE = 18;
int g() { constexpr int local = 19; return local; }
EOF
clang-tidy --config-file="$PWD/.clang-tidy" --checks='-*,readability-identifier-naming' "$PROBE/probe.cpp" -- -std=c++23 2>&1 \
  | grep -o "constexpr variable '[^']*'" | sed "s/constexpr variable //; s/'//g" | sort | tr '\n' ' '; echo
```

Expected, exactly: `UPPER_CASE k5_0 kFoo5_ kFoo5__6 kFoo_ kFoo_Bar kFoo__1 k_Foo local lower_case`

If any must-pass name appears, or a must-flag name is missing, stop: the regex in `.clang-tidy` differs from the spec.

- [ ] **Step 3: Refresh the compile database**

Run: `bazel build --config=release //... && bazel run --config=release //bazel/compile_commands:refresh`
Expected: both succeed. The build also materializes the generated headers clang-tidy needs, and `compile_commands.json` is rewritten at the workspace root.

- [ ] **Step 4: Apply the naming fixes across workspace sources**

`//:clang_tidy_fix` is deliberately not used, because under LLVM 23 it would also apply fixes for the 53 out-of-scope findings. The positional regex keeps generated `bazel-out` TUs (moc output) out, so nothing writes into Bazel's output tree.

```bash
run-clang-tidy -p . -config-file .clang-tidy -checks='-*,readability-identifier-naming' \
  -extra-arg=-Wno-error -extra-arg-before=-isysroot -extra-arg-before="$(xcrun --show-sdk-path)" \
  -quiet -fix "^$PWD/(src|apps|tests)/"
bazel build --config=release //...
```

Expected: `run-clang-tidy` exits non-zero, because `WarningsAsErrors: '*'` makes every naming diagnostic an error. It still prints `Applying fixes ...` and applies them with no conflict reported. Then the build passes.

- [ ] **Step 5: Check the rename spot-list and read the new names**

```bash
git diff --stat | tail -1
git grep -n -w -E 'API_VERSION|DLL_VERSION|biu_data_factors|can_data_factors|headerLength|checksumLength|hasDefinition|keepalive_interval|pings_sequently_missed_limit|singleton_children|first_page_prefix|last_page_prefix|cross_pairs|subaru_forester|parse_flags' -- ':!docs/superpowers'
git grep -n -w heartbeatInterval -- ':!docs/superpowers'
git diff -U0 | grep -E '^\+.*constexpr' | grep -o -E '\bk[A-Z][A-Za-z0-9_]*' | sort -u
```

Expected:
- About 40 files changed.
- The first search prints nothing.
- The second prints only `remote_serial_backend.h` (`const int heartbeatInterval = 0;`, a non-constexpr member outside this slice) and its use in `remote_serial_backend.cpp`.
- The new names are plain CamelCase of the old spellings: `kCells`, `kCases`, `kIndex`, `kApiVersion`, `kHeaderLength`, `kPingsSequentlyMissedLimit`, and so on. Single-letter test locals become `kN` and `kD`; that is accepted. Inspect anything else before continuing.

- [ ] **Step 6: Collision check**

A renamed local could start shadowing an outer constant of the same new name, which would change what later code in that scope reads. Flag every `k…` name the fix introduced into a file that already contained it:

```bash
python3 - <<'EOF'
import re, subprocess
def run(*args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout
token = re.compile(r"\bk[A-Z]\w*")
clashes = 0
for path in run("git", "diff", "--name-only").split():
    before = set(token.findall(run("git", "show", f"HEAD:{path}")))
    added, removed = set(), set()
    for line in run("git", "diff", "-U0", "--", path).splitlines():
        if line.startswith("+") and not line.startswith("+++"):
            added |= set(token.findall(line))
        elif line.startswith("-") and not line.startswith("---"):
            removed |= set(token.findall(line))
    clash = (added - removed) & before
    if clash:
        clashes += 1
        print(path, sorted(clash))
print("files with clashes:", clashes)
EOF
```

Expected: `files with clashes: 0`. For any file listed, read the renamed scope. If a later use now resolves to the renamed local instead of the outer constant, choose a different name for the local by hand.

- [ ] **Step 7: Whole tree is clean for the naming rule**

```bash
run-clang-tidy -p . -config-file .clang-tidy -checks='-*,readability-identifier-naming' \
  -extra-arg=-Wno-error -extra-arg-before=-isysroot -extra-arg-before="$(xcrun --show-sdk-path)" \
  -quiet "^$PWD/(src|apps|tests)/" 2>&1 | grep -c "readability-identifier-naming"
```

Expected: `0`.

- [ ] **Step 8: Test and lint**

Run: `bazel test --config=release //...` then `prek run --files .clang-tidy $(git diff --name-only)`
Expected: PASS; hooks pass (re-run once if clang-format rewrote lines).

- [ ] **Step 9: Commit**

```bash
git add .clang-tidy $(git diff --name-only)
git commit -m "$(cat <<'EOF'
style: enforce kCamelCase constexpr variables with clang-tidy (#49)

Enable readability-identifier-naming for constexpr variables only:
k prefix, CamelCase, and an IgnoredRegexp encoding Google's exception
for underscores next to digits. The remaining 99 sites, mostly test
locals, are renamed by run-clang-tidy -fix restricted to this check.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WuhQPxAkEnfKWKGyaiajxH
EOF
)"
```

### Task 6: Style guide Naming section; drop the spec and plan

**Files:**
- Modify: `docs/coding-style.md` (introduction lines 3–6 and 13–14; new `## Naming` section before `## Formatting and headers`, line 413)
- Delete: `docs/superpowers/specs/2026-10-03-constexpr-naming-design.md`, `docs/superpowers/plans/2026-10-03-constexpr-naming.md`

**Interfaces:**
- Consumes: the rule as enabled in Task 5.
- Produces: the `#naming` anchor in `docs/coding-style.md`.

- [ ] **Step 1: Make naming an exception to opportunistic conversion**

Replace:

```markdown
new and edited code; there is no proposed/accepted lifecycle. Pre-existing
sites that predate a rule are converted opportunistically as files are touched,
not in repo-wide sweeps.
```

with:

```markdown
new and edited code; there is no proposed/accepted lifecycle. Pre-existing
sites that predate a rule are converted opportunistically as files are touched,
not in repo-wide sweeps. [Naming](#naming) is the exception: clang-tidy
enforces each naming rule from the change that adopts it, so that change
renames every existing site.
```

- [ ] **Step 2: List clang-tidy naming among the mechanical checks**

Replace:

```markdown
Enforcement is PR review. Only a few of these rules have a mechanical check
(`prek` formatting, the `#pragma once` check); the rest do not, by design.
```

with:

```markdown
Enforcement is PR review. Only a few of these rules have a mechanical check
(`prek` formatting, the `#pragma once` check, clang-tidy for naming); the rest
do not, by design.
```

- [ ] **Step 3: Add the Naming section**

Insert immediately before `## Formatting and headers`:

````markdown
## Naming

Names follow the
[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html#Naming),
adopted one identifier kind at a time. Each kind listed here is enforced by
clang-tidy's `readability-identifier-naming`, and the change that adopted it
renamed every existing site, so a listed kind has no legacy exceptions. Kinds
not listed yet follow the surrounding code.

**Constexpr variables are `kCamelCase`** at every storage duration: namespace
scope, static data members, function locals, static locals, and variable
templates.

```cpp
inline constexpr std::size_t kReadPageSize = 0x100;
template <typename> inline constexpr bool kDependentFalse = false;
static constexpr auto kCells = std::to_array<std::string_view>({"10", "20"});
```

Google makes the `k` optional for function locals. Here it is required, because
the check cannot tell storage durations apart and one rule is simpler to follow.

**An underscore may separate words only where capitalization cannot**, which
means next to a digit: `kFlashBlocksSH7058_1block`,
`kSubaruDensoMc68hc16y5_02BdmUploadChunk`. `kFoo_Bar` is rejected.

````

- [ ] **Step 4: Remove the spec and plan**

```bash
git rm docs/superpowers/specs/2026-10-03-constexpr-naming-design.md docs/superpowers/plans/2026-10-03-constexpr-naming.md
```

- [ ] **Step 5: Final old-name sweep and lint**

```bash
git grep -n -P '\b([frke]blocks_\w+|flashdevices|family_requires_kernel_v|dependentFalse|FRAME_LEN|TRAILER_STD|TRAILER_FREEFORM|CHECKSUM_OFFSET|TRAILER_OFFSET|MAX_WRITE_CHUNK|API_VERSION|DLL_VERSION)\b'
prek run --all-files
```

Expected: no output from the search (no exclusion needed now that `docs/superpowers` is gone), and all hooks pass. If lychee rejects the `#Naming` fragment on Google's page, drop the fragment and link the page itself.

- [ ] **Step 6: Commit**

```bash
git add docs/coding-style.md
git commit -m "$(cat <<'EOF'
docs: add the Naming section; drop the constexpr naming spec and plan (#49)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WuhQPxAkEnfKWKGyaiajxH
EOF
)"
```

### Task 7: Open PR 2, link the stack, watch the widened gate

**Files:** none.

**Interfaces:**
- Consumes: PR 1 open (Task 3); Task 1's widening message.

- [ ] **Step 1: Get the go-ahead**

Ask the user to authorize pushing `style/constexpr-naming` and opening PR 2. Do not push without it.

- [ ] **Step 2: Push and open PR 2**

```bash
git push -u origin style/constexpr-naming
gh pr create --base ci/tidy-config-widening --head style/constexpr-naming \
  --title "style: enforce kCamelCase constexpr variables (#49, 2/2)" --body-file - <<'EOF'
## What
- `readability-identifier-naming` for constexpr variables only: `k` prefix, CamelCase, and an `IgnoredRegexp` encoding Google's exception for underscores next to digits.
- 256 sites renamed:
  - by hand: `FamilyTraits<T>::kFamily` and `kTransport`, `kFamilyRequiresKernel`, the kernel block tables (`kFlashBlocks…`, `kRamBlocks…`, `kKernelBlocks…`, `kEepromBlocks…` with the `mcu_type` spelling kept), `kFlashDevices`, the mut_dma constants, and `kDependentFalse`;
  - by `run-clang-tidy -fix`, restricted to this check: the remaining 99, mostly test locals.
- Style guide: a new Naming section; naming is the exception to opportunistic conversion.
- Removes the spec and plan added in 1/2.

## Verification
- A regex probe over passing and failing names.
- The collision check: no renamed local captures an outer constant.
- A naming-only whole-tree run shows 0 findings.
- An old-name search over every tracked file shows 0 hits.
- `bazel test //...` is green.
- This PR's clang-tidy jobs run whole-tree (Linux) and whole-scope (Windows) under CI's clang-tidy.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01WuhQPxAkEnfKWKGyaiajxH
EOF
```

- [ ] **Step 3: Link the stack**

```bash
gh stack init --base master ci/tidy-config-widening style/constexpr-naming
gh stack submit --auto
```

Expected: the stack adopts both existing PRs.

- [ ] **Step 4: Watch PR 2's clang-tidy jobs**

```bash
gh pr checks style/constexpr-naming --watch
RUN_ID="$(gh run list --branch style/constexpr-naming --workflow pr.yml --limit 1 --json databaseId --jq '.[0].databaseId')"
gh run view "$RUN_ID" --log | grep -E "clang-tidy: a .clang-tidy file changed|findings"
```

Expected: the widening line appears in both `clang-tidy` jobs, and both pass with no findings.
- A naming finding here means CI's clang-tidy 21/20 reads the regex or the rule differently from local 23. Report the finding to the user before changing anything.
- If `//tests:serial_backend_tests` fails on Windows, re-run the job once before investigating; it is a known pre-existing flake.

Hand-off note for the user: merge with `gh stack merge`, and use `gh stack sync` after any upstream change. Issue #49 stays open for the next naming slice. The 53 LLVM-23-only findings are a candidate follow-up issue for whenever CI's LLVM is upgraded.
