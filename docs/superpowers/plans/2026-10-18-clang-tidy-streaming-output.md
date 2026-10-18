# clang-tidy Streaming Output Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `//:clang_tidy_report*` and `//:clang_tidy_fix*` print each finding as its translation unit finishes, in a readable console format, and with `--annotate` also emit GitHub annotations.

**Architecture:** A new pure module `scripts/clang_tidy_output.py` turns `run-clang-tidy` output lines into formatted findings (parse, drop noise, deduplicate, format for console and GitHub). The runner swaps its capture-everything `_run_quiet` for a streaming seam (`stream_lines`, `Popen`-backed) for both analysis passes and feeds each line to an `OutputStream`, keeping the full text for the profile parser.

**Tech Stack:** Python 3 (stdlib only for the new module), `unittest`, Bazel `py_test`, GitHub Actions workflow commands.

**Spec:** the agreed design is recorded below (grilling session, 2026-10-18). It has no separate spec file.

## Agreed design

- **Scope.** `clang_tidy_report`, `_report_changed`, `_fix`, `_fix_changed`. Both the upstream pass and the inline custom-check pass stream. Benchmark and fixture targets get no new features. The benchmark inherits streaming because it calls `run_workflow`, and its output goes to its log file.
- **Source.** Lines from `run-clang-tidy` stdout, with stderr merged in. The YAML in `-export-fixes` stays the authority for the finding count.
- **Console.** Per TU: `[n/N] src/foo.cpp`. Per finding: `<severity>: <path>:<line>:<col> [<check>]`, then the message indented 2 spaces, then clang's snippet and caret lines indented 2 more, then notes in the same shape, indented 2. Severity is whatever clang-tidy printed. The `,-warnings-as-errors` suffix is stripped. Colour only when stdout is a TTY.
- **Noise dropped:** `N warning(s) generated.` (any warning/error mix), `Suppressed N warnings…`, `Use -header-filter=…`, `N warning(s) treated as error(s)`, `Running clang-tidy in N threads…`, the profile tables, and blank lines. Every other unparsed line passes through live, unchanged.
- **Deduplication:** by (path, line, column, check). The summary shows the YAML count and the unique count.
- **No end-of-run dump** on any path, including failures.
- **Annotations:** only with `--annotate`, which `pr.yml` passes on `clang_tidy_report_changed`. Level comes from severity: `error`/`fatal error` → `::error`, `warning` → `::warning`, `remark` → `::notice`. The title is the check name. Notes are folded into the body as `%0A`-separated lines. Only workspace files are annotated, with repo-relative forward-slash paths; paths under `bazel-*` or `external/` are not annotated. No cap and no step-summary table.
- **Layout:** a new `scripts/clang_tidy_output.py` plus a `//:clang_tidy_output_test` target, and runner tests that fake the streaming seam. `docs/tooling.md` gets updated.

## Global Constraints

- The LLVM pin is 23.1.2 (`CLANG_TIDY_LLVM_VERSION` in `.github/workflows/pr.yml`). The progress echo format is `[<i>/<N>][<secs>s] <shlex.join(invocation)>`, with the source file as the invocation's last argument (`run-clang-tidy`, `get_tidy_invocation` line 159; echo at line 736).
- `.clang-tidy` sets `WarningsAsErrors: '*'`, so in practice findings print as `error: … [check,-warnings-as-errors]`.
- `run-clang-tidy` and `clang_tidy_adapter.py` are Python. Behind a pipe their stdout is block-buffered, so `stream_lines` must set `PYTHONUNBUFFERED=1` in the child environment.
- The runner's own prints must use `flush=True`, because in CI its stdout is a pipe.
- `clang_tidy_output.py` stays stdlib-only and pure, apart from the `write` callable it is given.
- FastECU git workflow: feature branch `clang-tidy-streaming-output`, commits there, PR to `master`.

## Review Focus

1. **Windows paths** (`C:\src\foo.cpp:12:5: error: …`). They should parse, display as `src/foo.cpp` and annotate. Test: Task 1 `test_parses_windows_drive_letter_path`.
2. **A header finding repeated by many TUs**, each repeat carrying its own notes and snippet. It should print and annotate once, and `raw` should count every repeat. Test: Task 2 `test_header_finding_from_two_units_prints_once`.
3. **Messages containing `%`, `:`, `,` or newlines** in annotations. These should be escaped so GitHub doesn't truncate or misparse them. Test: Task 2 `test_annotation_escapes_data_and_properties`.
4. **A crash line arriving mid-finding** (`Stack dump:`, `…: terminated by signal 11`). The pending finding should print, then the crash line verbatim, with nothing swallowed. Test: Task 2 `test_unrecognised_line_flushes_pending_finding_and_passes_through`.
5. **Undecodable bytes or a slow child.** Output should arrive line by line as it is produced and never raise `UnicodeDecodeError`. Tests: Task 3 `test_stream_lines_delivers_lines_unbuffered_and_merged` and `test_stream_lines_replaces_undecodable_bytes`.

---

### Task 1: Line classification primitives

**Files:**
- Create: `scripts/clang_tidy_output.py`
- Create: `scripts/clang_tidy_output_test.py`
- Modify: `BUILD.bazel`: add `py_test(name = "clang_tidy_output_test", size = "small", srcs = ["scripts/clang_tidy_output.py", "scripts/clang_tidy_output_test.py"], imports = ["scripts"], main = "scripts/clang_tidy_output_test.py")` next to `clang_tidy_profile_test`

**Interfaces:**
- Produces:
  - `@dataclass(frozen=True) class Diagnostic: path: str; line: int; column: int; severity: str; message: str; check: str | None`. `severity` is one of `"fatal error"`, `"error"`, `"warning"`, `"note"`, `"remark"`. `check` has `,-warnings-as-errors` stripped, and is `None` when there is no `[...]` suffix.
  - `parse_diagnostic(line: str) -> Diagnostic | None`
  - `parse_progress(line: str) -> tuple[int, int, str] | None`, returning (index, total, source) where source is the last `shlex.split` token.
  - `is_detail(line: str) -> bool` for clang snippet and caret lines, matching `^\s*(\d+\s*)?\|`
  - `is_noise(line: str) -> bool`
  - `display_path(path: str, workspace: Path) -> str`: a repo-relative POSIX path when inside the workspace, otherwise `path` unchanged.
  - `annotation_path(path: str, workspace: Path) -> str | None`: like `display_path`, but `None` outside the workspace or when the first component starts with `bazel-` or is `external`.

- [ ] **Step 1: Write the failing tests** in `scripts/clang_tidy_output_test.py` (`unittest`, same style as `clang_tidy_profile_test.py`):

```python
def test_parses_finding_and_strips_warnings_as_errors(self):
    d = out.parse_diagnostic("/ws/src/a.cpp:12:5: error: use auto [modernize-use-auto,-warnings-as-errors]")
    self.assertEqual(out.Diagnostic("/ws/src/a.cpp", 12, 5, "error", "use auto", "modernize-use-auto"), d)

def test_parses_note_without_check(self):
    d = out.parse_diagnostic("/ws/src/a.h:3:1: note: declared here")
    self.assertEqual(("note", None), (d.severity, d.check))

def test_parses_fatal_error(self):
    self.assertEqual("fatal error", out.parse_diagnostic("/ws/a.cpp:1:10: fatal error: 'x.h' file not found [clang-diagnostic-error]").severity)

def test_parses_windows_drive_letter_path(self):
    d = out.parse_diagnostic(r"C:\ws\src\a.cpp:12:5: warning: msg [bugprone-x]")
    self.assertEqual((r"C:\ws\src\a.cpp", 12, 5), (d.path, d.line, d.column))

def test_line_without_column_is_not_a_diagnostic(self):
    self.assertIsNone(out.parse_diagnostic("main.cpp:1: warning: use Result matcher"))

def test_parses_progress_echo(self):
    line = "[ 3/12][0.4s] /llvm/bin/clang-tidy -checks=-* -export-fixes /tmp/f/x.yaml -p=/tmp/db /ws/src/a.cpp"
    self.assertEqual((3, 12, "/ws/src/a.cpp"), out.parse_progress(line))

def test_parses_progress_echo_with_quoted_windows_path(self):
    self.assertEqual("C:\\ws\\a.cpp", out.parse_progress("[1/1][0.1s] clang-tidy.exe 'C:\\ws\\a.cpp'")[2])

def test_detail_lines(self):
    self.assertTrue(out.is_detail("   12 |   foo();"))
    self.assertTrue(out.is_detail("      |   ^~~"))
    self.assertFalse(out.is_detail("Stack dump:"))

def test_noise(self):
    for line in ["1 warning generated.", "3 warnings and 1 error generated.", "2 errors generated.",
                 "Suppressed 40 warnings (40 in non-user code).",
                 "Use -header-filter=.* to display errors from all non-system headers. Use -system-headers to display errors from system headers as well.",
                 "1 warning treated as error", "Running clang-tidy in 8 threads for 3 files out of 9 in compilation database ...",
                 "===-------------------------------------------------------------------------===",
                 "                          clang-tidy checks profiling",
                 "  Total Execution Time: 1.2000 seconds (1.2100 wall clock)",
                 "   ---User Time---   --System Time--   --User+System--   ---Wall Time---  --- Name ---",
                 "   0.1000 ( 10.0%)   0.0100 (  5.0%)   0.1100 ( 9.0%)   0.1200 ( 9.5%)  bugprone-foo",
                 "", "   "]:
        self.assertTrue(out.is_noise(line), line)
    self.assertFalse(out.is_noise("/ws/a.cpp:1:1: error: x [y]"))
    self.assertFalse(out.is_noise("Stack dump:"))

def test_display_and_annotation_paths(self):
    ws = Path("/ws")
    self.assertEqual("src/a.cpp", out.display_path("/ws/src/a.cpp", ws))
    self.assertEqual("/usr/include/x.h", out.display_path("/usr/include/x.h", ws))
    self.assertIsNone(out.annotation_path("/usr/include/x.h", ws))
    self.assertIsNone(out.annotation_path("/ws/bazel-out/k8/bin/gen.h", ws))
    self.assertIsNone(out.annotation_path("/ws/external/qt/x.h", ws))
    self.assertEqual("src/a.cpp", out.annotation_path("/ws/src/a.cpp", ws))

def test_paths_through_a_symlinked_workspace(self):
    # tmp: real dir + symlink to it; diagnostic uses the symlink spelling, workspace the resolved one
    ...  # assert display_path == "src/a.cpp" and annotation_path == "src/a.cpp"
```

- [ ] **Step 2: Run the tests and confirm they fail**

Run: `bazel test //:clang_tidy_output_test --test_output=errors`
Expected: FAIL (`ModuleNotFoundError: clang_tidy_output`)

- [ ] **Step 3: Implement the primitives in `scripts/clang_tidy_output.py`**

- Diagnostic regex: `^(?P<path>.+?):(?P<line>\d+):(?P<column>\d+): (?P<severity>fatal error|error|warning|note|remark): (?P<message>.*?)(?: \[(?P<check>[^\]]+)\])?$`. The lazy path match handles drive letters.
- `is_noise` uses anchored regexes for the listed lines. A profile table row is any line whose first token matches `\d+(\.\d+)?` followed by a `(...%)` token.
- Path relativisation: try `PurePath(path).relative_to(workspace)`, then `Path(path).resolve().relative_to(workspace.resolve())`. On Windows, use `PureWindowsPath` when `path` has a drive letter. Return `.as_posix()`.

- [ ] **Step 4: Run the tests and confirm they pass**

Run: `bazel test //:clang_tidy_output_test --test_output=errors`
Expected: PASS

- [ ] **Step 5: Commit**: `git commit -m "feat: classify run-clang-tidy output lines"`

---

### Task 2: Findings, formatting and the output stream

**Files:**
- Modify: `scripts/clang_tidy_output.py`
- Modify: `scripts/clang_tidy_output_test.py`

**Interfaces:**
- Consumes: everything Task 1 produces.
- Produces:
  - `@dataclass(frozen=True) class Finding: diagnostic: Diagnostic; detail: tuple[str, ...] = (); notes: tuple["Finding", ...] = ()`
  - `format_finding(finding: Finding, *, workspace: Path, color: bool) -> str`, which returns multi-line text with no trailing newline.
  - `format_annotation(finding: Finding, *, workspace: Path) -> str | None`
  - `@dataclass(frozen=True) class StreamCounts: unique: int; raw: int`
  - `class OutputStream: __init__(self, *, workspace: Path, write: Callable[[str], None], color: bool, annotate: bool)`; `feed(self, line: str) -> None`; `finish(self) -> StreamCounts`

`OutputStream.feed` rules, in order:
1. A diagnostic that is a note while a finding is pending: attach it to that finding. Detail lines that follow go to the note.
2. Any other diagnostic: flush the pending finding, then start a new one.
3. `is_detail` while a finding is pending: append to the current finding or note.
4. Otherwise flush, then:
   - a progress line: `write("[i/N] <display_path>")`;
   - noise: drop it;
   - anything else, including a note with no pending finding: `write(line)` unchanged.

Flushing does `raw += 1`. If the key (path, line, column, check or `""`) is new, it does `unique += 1`, writes `format_finding` and, when `annotate` is set, writes the annotation (if not `None`). `finish()` flushes and returns the counts.

- [ ] **Step 1: Write the failing tests**

```python
WS = Path("/ws")
F = out.Finding(out.Diagnostic("/ws/src/a.cpp", 12, 5, "error", "use auto", "modernize-use-auto"),
                detail=("   12 |   int x = f();", "      |   ^"),
                notes=(out.Finding(out.Diagnostic("/ws/src/a.h", 3, 1, "note", "declared here", None)),))

def test_console_format_plain(self):
    self.assertEqual(
        "error: src/a.cpp:12:5 [modernize-use-auto]\n"
        "  use auto\n"
        "       12 |   int x = f();\n"
        "          |   ^\n"
        "  note: src/a.h:3:1 declared here",
        out.format_finding(F, workspace=WS, color=False))

def test_console_format_colour_wraps_severity_and_check(self):
    text = out.format_finding(F, workspace=WS, color=True)
    self.assertTrue(text.startswith("\x1b[1;31merror\x1b[0m: src/a.cpp:12:5 \x1b[2m[modernize-use-auto]\x1b[0m"))

def test_annotation_error_with_folded_note(self):
    self.assertEqual(
        "::error file=src/a.cpp,line=12,col=5,title=modernize-use-auto::use auto%0Anote: src/a.h:3:1 declared here",
        out.format_annotation(F, workspace=WS))

def test_annotation_levels(self):  # warning -> ::warning, remark -> ::notice, fatal error -> ::error

def test_annotation_escapes_data_and_properties(self):
    f = out.Finding(out.Diagnostic("/ws/src/a,b.cpp", 1, 1, "warning", "100% bad: x\ny", "c"))
    self.assertEqual("::warning file=src/a%2Cb.cpp,line=1,col=1,title=c::100%25 bad: x%0Ay",
                     out.format_annotation(f, workspace=WS))

def test_annotation_skipped_outside_workspace(self):  # /usr/include/x.h -> None

def test_stream_prints_progress_finding_and_drops_noise(self):
    lines = []; s = out.OutputStream(workspace=WS, write=lines.append, color=False, annotate=False)
    for l in ["[1/2][0.3s] clang-tidy -p=/db /ws/src/a.cpp",
              "/ws/src/a.cpp:12:5: error: use auto [modernize-use-auto,-warnings-as-errors]",
              "   12 |   int x = f();", "      |   ^", "1 warning generated.", ""]:
        s.feed(l)
    self.assertEqual(out.StreamCounts(unique=1, raw=1), s.finish())
    self.assertEqual("[1/2] src/a.cpp", lines[0])
    self.assertTrue(lines[1].startswith("error: src/a.cpp:12:5 [modernize-use-auto]"))
    self.assertEqual(2, len(lines))

def test_finding_is_written_when_the_next_line_arrives_not_at_finish(self):
    # feed diagnostic, then progress line for next TU -> finding already in `lines` before finish()

def test_header_finding_from_two_units_prints_once(self):
    # same /ws/src/a.h:3:1 [c] finding fed twice under two progress lines, with different notes
    # -> exactly one formatted finding in output; finish() == StreamCounts(unique=1, raw=2)

def test_annotate_writes_annotation_after_console_block(self):
    # annotate=True -> lines[-1].startswith("::error file=src/a.cpp")

def test_unrecognised_line_flushes_pending_finding_and_passes_through(self):
    # feed diagnostic, "Stack dump:", "0.  Program arguments: clang-tidy ..."
    # -> output == [<formatted finding>, "Stack dump:", "0.  Program arguments: clang-tidy ..."]

def test_orphan_note_passes_through_verbatim(self):
    # a note with no pending finding is written unchanged; counts (0, 0)
```

- [ ] **Step 2: Run the tests and confirm they fail**

Run: `bazel test //:clang_tidy_output_test --test_output=errors`
Expected: FAIL (`AttributeError: module 'clang_tidy_output' has no attribute 'Finding'`)

- [ ] **Step 3: Implement `Finding`, `format_finding`, `format_annotation`, `StreamCounts` and `OutputStream`**

- Colours: `error`/`fatal error` `\x1b[1;31m`, `warning` `\x1b[1;33m`, `note`/`remark` `\x1b[1;36m`, check `\x1b[2m`, reset `\x1b[0m`.
- Notes are formatted as `note: <path>:<line>:<col> <message>`, indented 2, with their detail indented 4.
- Annotation escaping: data `%`→`%25`, `\r`→`%0D`, `\n`→`%0A`. Properties do the same plus `:`→`%3A` and `,`→`%2C`.
- When `check` is `None`, the title is the severity.
- Snippets are not included in annotations.

- [ ] **Step 4: Run the tests and confirm they pass**

Run: `bazel test //:clang_tidy_output_test --test_output=errors`
Expected: PASS

- [ ] **Step 5: Commit**: `git commit -m "feat: format clang-tidy findings for the console and GitHub annotations"`

---

### Task 3: Stream analysis output through the runner

**Files:**
- Modify: `scripts/clang_tidy_runner.py`: `_run_quiet` call sites at `:954` and `:1031`, dumps at `:973`, `:978`, `:985` and `:1035`, summaries at `:986`, `:988` and `:1044`, `run_workflow` signature `:801`, and `main` `:1053`.
- Modify: `scripts/clang_tidy_runner_test.py`
- Modify: `bazel/clang_tidy.bzl`: add `"//:scripts/clang_tidy_output.py"` to `srcs`.
- Modify: `BUILD.bazel`: add `"scripts/clang_tidy_output.py"` to the `srcs` of `clang_tidy_check_fixtures`, `clang_tidy_plugin_fixtures`, `clang_tidy_benchmark_test` and `clang_tidy_runner_test`.

**Interfaces:**
- Consumes: `OutputStream`, `StreamCounts` (Task 2).
- Produces:
  - `StreamRunner = Callable[[list[str], Path, Callable[[str], None]], subprocess.CompletedProcess[str]]`
  - `stream_lines(command: list[str], cwd: Path, on_line: Callable[[str], None]) -> subprocess.CompletedProcess[str]` uses `Popen` with `stdout=PIPE`, `stderr=STDOUT`, `text=True`, `encoding="utf-8"`, `errors="replace"` and `bufsize=1`. The child environment is `{**os.environ, "PYTHONUNBUFFERED": "1"}`. It calls `on_line` with each line, minus `\r\n`, as it is read, and returns the full text as `stdout`.
  - `run_workflow(..., stream_runner: StreamRunner = stream_lines, annotate: bool = False, color: bool | None = None)`. When `color` is `None`, use `sys.stdout.isatty()`. Each pass creates an `OutputStream` whose `write` is `lambda text: print(text, flush=True)`. An `OSError` from `stream_runner` becomes `WorkflowError(f"could not execute {command[0]}: …")`, as `_run_quiet` does.
  - Summary lines: `clang-tidy: {n} files analyzed, {finding_count} findings ({unique} unique) (advisory)`, and inline `clang-tidy: {custom_count} inline findings ({unique} unique)`. `files clean, 0 findings` and every `WorkflowError` detail stay unchanged.
  - `main`: `--annotate` (`store_true`) is forwarded as `annotate=`.

- [ ] **Step 1: Write the failing tests**

Add a module helper that adapts the existing `CompletedProcess` fakes:

```python
def _streaming(fake_run):
    def stream(command, cwd, on_line):
        result = fake_run(command, cwd=cwd, check=False)
        for line in (result.stdout or "").splitlines():
            on_line(line)
        return result
    return stream
```

Pass `stream_runner=_streaming(fake_run)` in every existing test that reaches analysis (those not stopping before `run-clang-tidy`). New tests:

```python
def test_findings_print_while_analysis_is_still_running(self):
    # stream_runner fake: on_line(diagnostic for main.cpp); snapshot = output.getvalue(); return CompletedProcess(…, 0)
    # assert "error: main.cpp:1:1 [c]" in snapshot

def test_failed_analysis_prints_findings_once_and_no_raw_dump(self):
    # run-clang-tidy fake returns 1 with stdout "<diag line>\n1 warning generated.\n"
    # assertRaises WorkflowError; output contains the formatted header exactly once, no "1 warning generated"

def test_inline_pass_streams_too(self):
    # adapter command (contains clang_tidy_adapter.py) emits a diagnostic -> formatted header printed

def test_annotate_prints_github_annotation(self):
    # annotate=True -> "::error file=main.cpp,line=1,col=1,title=c::" in output; annotate=False -> absent

def test_summary_reports_unique_count(self):
    # YAML has 2 diagnostics, stream sees the same finding twice -> "2 findings (1 unique)"

def test_main_forwards_annotate(self):
    # patch runner.run_workflow and workspace_root; main([... "report", "--annotate", "--compdb-tool", "x"]) -> kwargs["annotate"] is True

def test_stream_lines_delivers_lines_unbuffered_and_merged(self):
    # real stream_lines on [sys.executable, "-c", "import os,sys,time; print(os.environ['PYTHONUNBUFFERED']); print('err', file=sys.stderr); time.sleep(0.2); print('late')"]
    # on_line records (line, time.monotonic()); assert lines == ["1", "err", "late"] and the "1" arrived >=0.15s before "late"

def test_stream_lines_replaces_undecodable_bytes(self):
    # child writes b"bad \xff byte\n" to stdout.buffer -> one line containing "\ufffd", no exception
```

The existing `test_non_error_findings_are_visible_without_failing` (`main.cpp:1: warning: …`, no column) must still pass through verbatim. `test_analysis_success_prints_terse_summary` must still hide `1 warning generated`.

- [ ] **Step 2: Run the tests and confirm they fail**

Run: `bazel test //:clang_tidy_runner_test --test_output=errors`
Expected: FAIL (`TypeError: run_workflow() got an unexpected keyword argument 'stream_runner'`)

- [ ] **Step 3: Implement the changes**

- Add `stream_lines` and the `StreamRunner` seam.
- Route both analysis passes through it, with one `OutputStream` per pass. Profile parsing still reads `result.stdout`.
- Delete every `print(tidy_result.stdout…)` and `print(custom_result.stdout…)`.
- Make the remaining runner prints in `run_workflow` `flush=True`.
- Delete `_run_quiet` if it has no other callers.

- [ ] **Step 4: Run the tests and confirm they pass**

Run: `bazel test //:clang_tidy_runner_test //:clang_tidy_output_test //:clang_tidy_benchmark_test //:clang_tidy_adapter_test --test_output=errors`
Expected: PASS

- [ ] **Step 5: Commit**: `git commit -m "feat: stream clang-tidy findings as each translation unit finishes"`

---

### Task 4: CI wiring, docs and a real-LLVM check

**Files:**
- Modify: `.github/workflows/pr.yml:313`: `bazel run --config=release //:clang_tidy_report_changed -- --profile --annotate ${{ matrix.scope }}`
- Modify: `docs/tooling.md` (`### clang-tidy`): add a bullet saying that findings stream per TU in the console format and that `--annotate` emits GitHub annotations (CI passes it). Add `bazel run //:clang_tidy_report_changed -- --annotate` to the command block.

- [ ] **Step 1: Run the real runner against LLVM 23 locally.** Make a throwaway edit that triggers a finding (e.g. a C-style cast in a changed `src/` file), then run:
`PATH=/opt/homebrew/opt/llvm/bin:$PATH bazel run --config=release //:clang_tidy_report_changed -- --annotate`
Expected:
- `[i/N] src/…` progress lines appear as the run goes, not all at the end;
- the finding prints in the agreed format, with no `N warnings generated` or command echoes;
- a `::error file=src/…` line follows it;
- the run ends with the `WorkflowError` summary (WarningsAsErrors).
If the format differs from the Task 1 fixtures, add the real line as a fixture test before fixing anything. Revert the throwaway edit.

- [ ] **Step 2: Run the fixture suites.**
Run: `bazel run --config=release //:clang_tidy_check_fixtures`
Expected: PASS

- [ ] **Step 3: Commit**: `git commit -m "ci: annotate clang-tidy findings on pull requests"`

- [ ] **Step 4: After the PR's CI run, check** that the clang-tidy job log shows streamed progress and that any findings show up as annotations on the PR diff.
