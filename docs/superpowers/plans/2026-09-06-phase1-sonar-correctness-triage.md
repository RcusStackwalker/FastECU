# Phase 1 SonarCloud Correctness Triage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resolve Phase 1 of the SonarCloud backlog paydown plan — `cpp:S1117` (declaration shadows outer variable), `cpp:S5276` (implicit narrowing conversion), and `cpp:S5025` (raw `new`/`delete`) — by separating scanner artifacts from real bugs and fixing only the real ones.

**Architecture:** Investigation ahead of this plan (see "Findings" below) showed most `S1117` findings are not real shadowing at all, so the plan front-loads two triage scripts that use ground truth (a real compiler, and the project's own signal declarations) to split "confirmed real" from "scanner artifact" *before* any source file is touched. Confirmed artifacts are resolved as SonarCloud false positives via the API, not by changing code. Confirmed real findings get fixed with tests. `S5276`/`S5025` don't have an equivalent single root cause, so they're triaged per-file using a documented decision rule instead.

**Tech Stack:** Python 3.14 (repo's pinned `rules_python` toolchain) for triage scripts, run with `pytest`; `sonar` CLI (SonarQube CLI v1.4.0, already authenticated) for reading/writing SonarCloud issues; the existing `compile_commands.json` (regenerate via `bazel run //bazel/compile_commands:refresh_sonar` if stale) plus system `clang++` for ground-truth compiler checks; Bazel/`ctest`-style `bazel test` for the actual C++ fixes.

**Spec:** [docs/tech-debt.md](../../tech-debt.md), section "P2: Pay down the SonarCloud code-smell backlog", Phase 1.

## Findings (context for every task below)

Before writing this plan, the `cpp:S1117` backlog (550 issues) was cross-checked against ground truth instead of hand-triaged line by line:

- **466 of 550 (85%)** shadow one of `LOG_I`, `LOG_D`, `progressChanged`, or `externalLoggerMessage` — all Qt signals declared under `signals:` in the corresponding class header. Every flagged line is `emit SomeSignal(...)`, a normal signal emission, not a declaration. SonarCloud's C-family parser is misreading Qt's `emit` keyword-macro idiom. This class is a pure scanner artifact — the code is correct as written.
- Of the remaining 84, running each through `clang++ <same flags as compile_commands.json> -Wshadow-all -fsyntax-only` on the current source tree found:
  - **17 are confirmed by a real compiler** (7 files): `src/ui/desktop/ecu_operations.cpp` (4, `test_write`), `src/ui/desktop/vehicle_select.cpp` (6, `item_local`), `src/ui/desktop/settings.cpp` (2, `configValues`), `src/ui/desktop/mainwindow.cpp` (2, `ecuCalDef`), `src/ui/desktop/menu_actions.cpp` (1, `ecuCalDef`), `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_unisia_jecs_operation.cpp` (1, `parent`), `src/ui/desktop/flash/ecu/flash_ecu_subaru_unisia_jecs.cpp` (1, `cmd_type`).
  - **54 are NOT confirmed by clang** (mostly single-letter loop variables `i`/`r` and `result`/`parent` reused across sibling, non-nested scopes — a MISRA-style "don't reuse an identifier anywhere" hygiene rule stricter than real C++ shadowing, matching the issue's `based-on-misra`/`cert` tags).
  - **13 live in headers with no direct `compile_commands.json` entry** (`car_model_catalog.h`, `definition_model.h`, `transport_legacy_compat.h`, `serial_port_actions.h`) and need a manual read.
- `cpp:S5276` (implicit narrowing) showed no equivalent artifact: `clang++ -Wconversion` independently flags truncations in the same functions (off by one line in a few cases, consistent with Sonar reporting the first line of a multi-line statement). Treat `S5276` as genuinely worth a per-instance fix.
- `cpp:S5025` (raw `new`/`delete`) is a real mix: `src/ui/desktop/settings.cpp:50` (`fileActions = new FileActions(...)`) is a confirmed leak — `FileActions` does not derive from `QObject`/`QWidget`, the pointer is never deleted, and grepping the file shows it's used nowhere else. Other `new` call sites in the same file (e.g. `new QGroupBox(...)`, `new QListWidgetItem(text, list)`) are Qt-parented or container-owned and are Sonar false positives, since Sonar's C-family engine doesn't model Qt's parent-child ownership transfer.

## Global Constraints

- Backend/UI code changes must build under `bazel build --config=release //:fastecu` and pass `bazel test --config=release //...` before each commit.
- A fix that changes runtime behavior (not just naming or type) needs a test proving the old behavior was wrong and the new behavior is correct, per this repo's TDD convention — don't fix a "confirmed real" shadow without a test showing what was broken.
- Files under `src/platform/desktop/common/flash/legacy/` and `src/platform/desktop/unix/j2534/` are hardware-facing (K-Line/CAN/J2534). None of this plan's fixes change wire bytes or protocol timing — they are renames, type changes (raw pointer → smart pointer), and explicit casts — so no bench re-qualification is required. If any step is found to require a behavior change to the actual bytes sent to hardware, stop and flag it instead of proceeding.
- Every marking of a SonarCloud issue as a false positive must carry a `comment` explaining the root cause (this plan's Findings section), so it's auditable later and doesn't get silently reopened without context.
- `prek run --all-files` (clang-format, ruff, pragma-once, lychee) must pass before each commit.
- New Python scripts under `scripts/` follow the existing convention: flat files, a sibling `<name>_test.py`, run with `pytest`, linted with `ruff` (see `scripts/clang_tidy_runner.py` / `scripts/clang_tidy_runner_test.py`).

---

### Task 1: Reusable SonarCloud issue fetch/resolve helper

**Files:**
- Create: `scripts/sonar_issues.py`
- Test: `scripts/sonar_issues_test.py`

**Interfaces:**
- Produces: `Issue` dataclass (`key: str`, `rule: str`, `component: str`, `line: int | None`, `message: str`, property `file_path: str`), `fetch_open_issues(rules: list[str]) -> list[Issue]`, `resolve_false_positive(issue_keys: list[str], comment: str) -> None`. Every later task imports these.

- [ ] **Step 1: Write the failing tests**

```python
# scripts/sonar_issues_test.py
from __future__ import annotations

import json
from unittest.mock import MagicMock, patch

from scripts.sonar_issues import Issue, fetch_open_issues, resolve_false_positive


def _page(issues, total):
    return json.dumps({"issues": issues, "paging": {"total": total}})


def test_fetch_open_issues_filters_by_rule_and_paginates():
    page1 = _page(
        [
            {"key": "k1", "rule": "cpp:S1117", "component": "P:a.cpp", "line": 1, "message": "m1"},
            {"key": "k2", "rule": "cpp:S9999", "component": "P:b.cpp", "line": 2, "message": "m2"},
        ],
        total=3,
    )
    page2 = _page(
        [{"key": "k3", "rule": "cpp:S1117", "component": "P:c.cpp", "line": 3, "message": "m3"}],
        total=3,
    )
    with patch("scripts.sonar_issues.PAGE_SIZE", 2), patch("subprocess.run") as run:
        run.side_effect = [MagicMock(stdout=page1), MagicMock(stdout=page2)]
        issues = fetch_open_issues(["cpp:S1117"])

    assert issues == [
        Issue(key="k1", rule="cpp:S1117", component="P:a.cpp", line=1, message="m1"),
        Issue(key="k3", rule="cpp:S1117", component="P:c.cpp", line=3, message="m3"),
    ]
    assert issues[0].file_path == "a.cpp"


def test_resolve_false_positive_batches_at_page_size():
    keys = [f"k{i}" for i in range(5)]
    with patch("scripts.sonar_issues.PAGE_SIZE", 2), patch("subprocess.run") as run:
        run.return_value = MagicMock(returncode=0)
        resolve_false_positive(keys, comment="artifact of X")

    assert run.call_count == 3  # batches of 2, 2, 1
    for call in run.call_args_list:
        args = call.args[0]
        assert args[:4] == ["sonar", "api", "post", "/api/issues/bulk_change"]
        payload = json.loads(args[-1])
        assert payload["do_transition"] == "falsepositive"
        assert payload["comment"] == "artifact of X"
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `pytest scripts/sonar_issues_test.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'scripts.sonar_issues'`

- [ ] **Step 3: Write the implementation**

```python
# scripts/sonar_issues.py
"""Fetch and bulk-resolve SonarCloud issues via the `sonar` CLI.

Shared by the Phase 1 backlog triage (docs/tech-debt.md, "Pay down the
SonarCloud code-smell backlog") so each triage script doesn't reimplement
pagination and subprocess plumbing.
"""
from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass

PROJECT = "RcusStackwalker_FastECU"
PAGE_SIZE = 500


@dataclass(frozen=True)
class Issue:
    key: str
    rule: str
    component: str
    line: int | None
    message: str

    @property
    def file_path(self) -> str:
        return self.component.split(":", 1)[-1]


def fetch_open_issues(rules: list[str]) -> list[Issue]:
    """Fetch every OPEN/CONFIRMED issue for the given rule keys, paginated."""
    issues: list[Issue] = []
    page = 1
    while True:
        result = subprocess.run(
            [
                "sonar", "list", "issues",
                "--project", PROJECT,
                "--statuses", "OPEN,CONFIRMED",
                "--format", "json",
                "--page-size", str(PAGE_SIZE),
                "--page", str(page),
            ],
            capture_output=True, text=True, check=True,
        )
        payload = json.loads(result.stdout)
        for raw in payload["issues"]:
            if raw["rule"] not in rules:
                continue
            issues.append(Issue(
                key=raw["key"],
                rule=raw["rule"],
                component=raw["component"],
                line=raw.get("line"),
                message=raw["message"],
            ))
        total_pages = -(-payload["paging"]["total"] // PAGE_SIZE)
        if page >= total_pages:
            break
        page += 1
    return issues


def resolve_false_positive(issue_keys: list[str], comment: str) -> None:
    """Mark issues as false positive in SonarCloud, batched at PAGE_SIZE."""
    for start in range(0, len(issue_keys), PAGE_SIZE):
        batch = issue_keys[start:start + PAGE_SIZE]
        subprocess.run(
            [
                "sonar", "api", "post", "/api/issues/bulk_change",
                "-d", json.dumps({
                    "issues": ",".join(batch),
                    "do_transition": "falsepositive",
                    "comment": comment,
                }),
            ],
            check=True,
        )
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pytest scripts/sonar_issues_test.py -v`
Expected: PASS (2 tests)

- [ ] **Step 5: Lint and commit**

```bash
ruff check scripts/sonar_issues.py scripts/sonar_issues_test.py
ruff format scripts/sonar_issues.py scripts/sonar_issues_test.py
git add scripts/sonar_issues.py scripts/sonar_issues_test.py
git commit -m "feat(scripts): add SonarCloud issue fetch/resolve helper for Phase 1 triage"
```

---

### Task 2: Resolve the `emit <Signal>(...)` false-positive class for `cpp:S1117`

**Files:**
- Create: `scripts/classify_s1117_signal_shadows.py`
- Test: `scripts/classify_s1117_signal_shadows_test.py`

**Interfaces:**
- Consumes: `Issue`, `fetch_open_issues`, `resolve_false_positive` from `scripts/sonar_issues.py` (Task 1).
- Produces: `find_signal_names(header_paths: list[str]) -> set[str]`, `classify(issues: list[Issue], signal_names: set[str]) -> tuple[list[Issue], list[Issue]]` (returns `(emit_artifact, remaining)`). Task 3 consumes the `remaining` list.

- [ ] **Step 1: Write the failing tests**

```python
# scripts/classify_s1117_signal_shadows_test.py
from __future__ import annotations

from scripts.classify_s1117_signal_shadows import classify, find_signal_names
from scripts.sonar_issues import Issue


def test_find_signal_names_parses_signals_blocks(tmp_path):
    header = tmp_path / "widget.h"
    header.write_text(
        "class Widget : public QWidget {\n"
        "    Q_OBJECT\n"
        "public:\n"
        "    void notASignal(int x);\n"
        "signals:\n"
        "    void LOG_I(QString message, bool timestamp, bool linefeed);\n"
        "    void progressChanged(int done, int total);\n"
        "public slots:\n"
        "    void onClicked();\n"
        "};\n"
    )
    assert find_signal_names([str(header)]) == {"LOG_I", "progressChanged"}


def test_classify_splits_emit_artifact_from_remaining():
    issues = [
        Issue(key="a", rule="cpp:S1117", component="P:x.cpp", line=1,
              message='Declaration shadows a local variable "LOG_I" in the outer scope.'),
        Issue(key="b", rule="cpp:S1117", component="P:x.cpp", line=2,
              message='Declaration shadows a local variable "item_local" in the outer scope.'),
    ]
    emit_artifact, remaining = classify(issues, signal_names={"LOG_I"})
    assert [i.key for i in emit_artifact] == ["a"]
    assert [i.key for i in remaining] == ["b"]
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `pytest scripts/classify_s1117_signal_shadows_test.py -v`
Expected: FAIL with `ModuleNotFoundError`

- [ ] **Step 3: Write the implementation**

```python
# scripts/classify_s1117_signal_shadows.py
"""Split cpp:S1117 findings into the `emit <Signal>(...)` scanner artifact
class and everything else.

See docs/superpowers/plans/2026-09-06-phase1-sonar-correctness-triage.md,
"Findings": SonarCloud's C-family parser misreads `emit SomeSignal(...)` as
a declaration of SomeSignal, so every call to a Qt signal declared under
`signals:` gets flagged as "shadowing" the previous call. This is not real
shadowing.
"""
from __future__ import annotations

import glob
import re

from scripts.sonar_issues import Issue

SIGNALS_BLOCK = re.compile(
    r"\bsignals:\s*(.*?)(?:\n\s*(?:public|protected|private|signals|slots)\b|\nclass\b|\Z)",
    re.S,
)
SIGNAL_DECL = re.compile(r"void\s+([A-Za-z_]\w*)\s*\(")
SHADOWED_NAME = re.compile(r'"([^"]+)"')


def find_signal_names(header_paths: list[str]) -> set[str]:
    names: set[str] = set()
    for path in header_paths:
        text = open(path, encoding="utf-8", errors="ignore").read()
        for block_match in SIGNALS_BLOCK.finditer(text):
            names.update(m.group(1) for m in SIGNAL_DECL.finditer(block_match.group(1)))
    return names


def classify(issues: list[Issue], signal_names: set[str]) -> tuple[list[Issue], list[Issue]]:
    emit_artifact, remaining = [], []
    for issue in issues:
        match = SHADOWED_NAME.search(issue.message)
        name = match.group(1) if match else None
        (emit_artifact if name in signal_names else remaining).append(issue)
    return emit_artifact, remaining


if __name__ == "__main__":
    from scripts.sonar_issues import fetch_open_issues, resolve_false_positive

    headers = glob.glob("src/**/*.h", recursive=True)
    signals = find_signal_names(headers)
    all_s1117 = fetch_open_issues(["cpp:S1117"])
    artifact, remaining = classify(all_s1117, signals)

    print(f"{len(artifact)} emit-signal artifact issues, {len(remaining)} remaining")
    for issue in remaining:
        print(f"REMAINING: {issue.file_path}:{issue.line} - {issue.message}")

    answer = input(f"Resolve {len(artifact)} issues as false positive? [y/N] ")
    if answer.lower() == "y":
        resolve_false_positive(
            [i.key for i in artifact],
            comment=(
                "False positive: SonarCloud's C-family parser misreads Qt's "
                "`emit <Signal>(...)` idiom as a declaration of <Signal>, so every "
                "call to this Qt signal is flagged as shadowing the previous call. "
                "Confirmed by cross-referencing against the `signals:` declaration in "
                "the class header; see docs/superpowers/plans/"
                "2026-09-06-phase1-sonar-correctness-triage.md."
            ),
        )
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pytest scripts/classify_s1117_signal_shadows_test.py -v`
Expected: PASS (2 tests)

- [ ] **Step 5: Run the script against the live backlog and resolve the artifacts**

Run: `python3 -m scripts.classify_s1117_signal_shadows`

Expect it to print approximately 466 artifact issues and 84 remaining (counts may drift slightly if the backlog changed since this plan was written — re-read the printed `REMAINING` list and carry the actual count into Task 3, don't assume 84). Answer `y` at the prompt to bulk-resolve the artifact issues.

Verify: `sonar list issues --project RcusStackwalker_FastECU --statuses OPEN,CONFIRMED --format json | python3 -c "import json,sys; d=json.load(sys.stdin); print(sum(1 for i in d['issues'] if i['rule']=='cpp:S1117'))"` prints a number close to the "remaining" count from the script (not 0 — the false-positive transition doesn't remove the issue from OPEN/CONFIRMED status filters used elsewhere; use `--statuses` without filtering, or check `issueStatus` — confirm by spot-checking 3 keys from the artifact list return `"issueStatus": "FALSE_POSITIVE"` via `sonar api get "/api/issues/search?issues=<key>&organization=rcusstackwalker"`).

- [ ] **Step 6: Lint and commit**

```bash
ruff check scripts/classify_s1117_signal_shadows.py scripts/classify_s1117_signal_shadows_test.py
ruff format scripts/classify_s1117_signal_shadows.py scripts/classify_s1117_signal_shadows_test.py
git add scripts/classify_s1117_signal_shadows.py scripts/classify_s1117_signal_shadows_test.py
git commit -m "fix(sonar): resolve emit-signal S1117 false positives (~466 issues)"
```

---

### Task 3: Compiler cross-check the remaining `cpp:S1117` candidates

**Files:**
- Create: `scripts/crosscheck_s1117_shadow.py`
- Test: `scripts/crosscheck_s1117_shadow_test.py`

**Interfaces:**
- Consumes: `Issue` from `scripts/sonar_issues.py`; the `remaining` list from Task 2.
- Produces: `load_compile_commands(path: str) -> dict[str, dict]`, `classify(issues, compile_commands) -> tuple[list[Issue], list[Issue], list[Issue]]` returning `(confirmed, disagree, no_compile_command)`. Tasks 4 and 5 consume `confirmed` and `no_compile_command` respectively; this task itself resolves `disagree`.

- [ ] **Step 1: Write the failing test**

```python
# scripts/crosscheck_s1117_shadow_test.py
from __future__ import annotations

from unittest.mock import patch

from scripts.crosscheck_s1117_shadow import classify
from scripts.sonar_issues import Issue


def test_classify_splits_confirmed_disagree_and_missing_compile_command():
    issues = [
        Issue(key="a", rule="cpp:S1117", component="P:has_cc.cpp", line=10, message="m"),
        Issue(key="b", rule="cpp:S1117", component="P:has_cc.cpp", line=20, message="m"),
        Issue(key="c", rule="cpp:S1117", component="P:no_cc.h", line=5, message="m"),
    ]
    compile_commands = {"has_cc.cpp": {"arguments": ["clang++"], "directory": "."}}

    with patch(
        "scripts.crosscheck_s1117_shadow.shadow_warning_lines",
        return_value={10},
    ):
        confirmed, disagree, no_cc = classify(issues, compile_commands)

    assert [i.key for i in confirmed] == ["a"]
    assert [i.key for i in disagree] == ["b"]
    assert [i.key for i in no_cc] == ["c"]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest scripts/crosscheck_s1117_shadow_test.py -v`
Expected: FAIL with `ModuleNotFoundError`

- [ ] **Step 3: Write the implementation**

```python
# scripts/crosscheck_s1117_shadow.py
"""Cross-check remaining cpp:S1117 candidates against a real compiler.

Runs each affected file through `clang++ -Wshadow-all -fsyntax-only` using
its actual compile_commands.json entry, and compares the reported line
numbers against SonarCloud's. See docs/superpowers/plans/
2026-09-06-phase1-sonar-correctness-triage.md, "Findings".
"""
from __future__ import annotations

import json
import re
import subprocess
from pathlib import Path

from scripts.sonar_issues import Issue


def load_compile_commands(path: str = "compile_commands.json") -> dict[str, dict]:
    entries = json.loads(Path(path).read_text())
    # Only keep real source entries; bazel-out/**/moc_*.cpp duplicates shadow
    # the same basename and would silently override the real file's entry.
    return {e["file"]: e for e in entries if e["file"].startswith("src/")}


def shadow_warning_lines(entry: dict, relative_path: str) -> set[int]:
    args = entry.get("arguments") or entry["command"].split()
    cmd = [args[0], *args[1:], "-Wshadow-all", "-fsyntax-only"]
    result = subprocess.run(
        cmd, cwd=entry.get("directory", "."), capture_output=True, text=True, timeout=120,
    )
    base = relative_path.rsplit("/", 1)[-1]
    pattern = re.compile(re.escape(base) + r":(\d+):\d+:.*shadow")
    lines = set()
    for line in result.stderr.splitlines():
        if "bazel-out" in line:
            continue
        match = pattern.search(line)
        if match:
            lines.add(int(match.group(1)))
    return lines


def classify(
    issues: list[Issue], compile_commands: dict[str, dict],
) -> tuple[list[Issue], list[Issue], list[Issue]]:
    confirmed, disagree, no_cc = [], [], []
    lines_by_file: dict[str, set[int]] = {}
    for issue in issues:
        entry = compile_commands.get(issue.file_path)
        if entry is None:
            no_cc.append(issue)
            continue
        if issue.file_path not in lines_by_file:
            lines_by_file[issue.file_path] = shadow_warning_lines(entry, issue.file_path)
        (confirmed if issue.line in lines_by_file[issue.file_path] else disagree).append(issue)
    return confirmed, disagree, no_cc


if __name__ == "__main__":
    from scripts.classify_s1117_signal_shadows import classify as classify_signals
    from scripts.classify_s1117_signal_shadows import find_signal_names
    import glob
    from scripts.sonar_issues import fetch_open_issues, resolve_false_positive

    all_s1117 = fetch_open_issues(["cpp:S1117"])
    _, remaining = classify_signals(all_s1117, find_signal_names(glob.glob("src/**/*.h", recursive=True)))

    compile_commands = load_compile_commands()
    confirmed, disagree, no_cc = classify(remaining, compile_commands)

    print(f"{len(confirmed)} confirmed real by clang++ -Wshadow-all:")
    for i in confirmed:
        print(f"  {i.file_path}:{i.line} - {i.message}")
    print(f"\n{len(disagree)} not reproduced by clang++ (candidate false positives):")
    for i in disagree:
        print(f"  {i.file_path}:{i.line} - {i.message}")
    print(f"\n{len(no_cc)} in headers with no compile_commands.json entry (manual review, Task 5):")
    for i in no_cc:
        print(f"  {i.file_path}:{i.line} - {i.message}")

    answer = input(f"\nResolve {len(disagree)} 'disagree' issues as false positive? [y/N] ")
    if answer.lower() == "y":
        resolve_false_positive(
            [i.key for i in disagree],
            comment=(
                "False positive: not reproduced by `clang++ -Wshadow-all -fsyntax-only` "
                "against this file's actual compile_commands.json entry. SonarCloud's "
                "S1117 (based-on-misra/cert) flags identifier reuse across sibling, "
                "non-nested scopes (e.g. a loop counter named `i` reused in an unrelated "
                "later function) more strictly than real C++ shadowing. See "
                "docs/superpowers/plans/2026-09-06-phase1-sonar-correctness-triage.md."
            ),
        )
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest scripts/crosscheck_s1117_shadow_test.py -v`
Expected: PASS

- [ ] **Step 5: Regenerate compile_commands.json if stale, then run the script**

```bash
bazel run //bazel/compile_commands:refresh_sonar
python3 -m scripts.crosscheck_s1117_shadow
```

Read the three printed lists. Before answering the confirmation prompt, spot-check 3 issues from the "disagree" list by opening the file at the given line and confirming by eye that it is not a real nested shadow (e.g. a `for (int i = ...)` in a function that has no other `i` in the same or an enclosing scope). Then answer `y`.

Save the "confirmed" list's file groupings for Task 4 and the "no compile_commands.json entry" list for Task 5 — copy the script's printed output into your working notes; it is the authoritative worklist since it reflects current HEAD, not the snapshot in this plan's Findings section (which could have drifted).

- [ ] **Step 6: Lint and commit**

```bash
ruff check scripts/crosscheck_s1117_shadow.py scripts/crosscheck_s1117_shadow_test.py
ruff format scripts/crosscheck_s1117_shadow.py scripts/crosscheck_s1117_shadow_test.py
git add scripts/crosscheck_s1117_shadow.py scripts/crosscheck_s1117_shadow_test.py
git commit -m "fix(sonar): resolve compiler-unconfirmed S1117 candidates as false positive"
```

---

### Task 4: Fix the compiler-confirmed real `cpp:S1117` shadows

**Files (from this plan's Findings; re-confirm the exact set against Task 3's live output before starting, since the backlog may have drifted):**
- Modify: `src/ui/desktop/ecu_operations.cpp` (lines 539, 661, 779, 894 — a local/parameter named `test_write` shadows a field also named `test_write`)
- Modify: `src/ui/desktop/vehicle_select.cpp` (lines 167, 192, 206, 255, 290, 353 — `item_local` shadows an outer `item_local`)
- Modify: `src/ui/desktop/settings.cpp` (lines 58, 200 — a parameter named `configValues` shadows a field also named `configValues`)
- Modify: `src/ui/desktop/mainwindow.cpp` (lines 1487, 1536 — `ecuCalDef` shadows a field)
- Modify: `src/ui/desktop/menu_actions.cpp` (line 986 — `ecuCalDef` shadows a field)
- Modify: `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_unisia_jecs_operation.cpp` (line 13 — `parent` constructor parameter)
- Modify: `src/ui/desktop/flash/ecu/flash_ecu_subaru_unisia_jecs.cpp` (line 11 — `cmd_type`)
- Test: whichever `*_test.cpp` already exercises each modified function; add one where none exists for a function whose behavior actually changes (see per-file steps below).

**Fix recipe (apply per instance):** A parameter/local shadowing a field is not itself a runtime bug as long as the code inside that scope consistently means "the parameter", never accidentally reads "the field" expecting the parameter's value (or vice versa) — read the enclosing function fully to confirm which the code *means* at each use before renaming. Two possible fixes, in order of preference: (a) if the parameter/local is only ever meant to initialize or temporarily override the field, keep the field write but rename the parameter (e.g. `test_write` parameter → `test_write_arg`) and update its uses within that scope; (b) if the code already always means the field and the local declaration is genuinely redundant, remove the local declaration and use the field directly. Do not blindly rename without reading the function — that's exactly the class of bug S1117 exists to catch, and this plan already spent the effort to confirm these 17 are real; don't undo that by rushing the fix.

- [ ] **Step 1: For `ecu_operations.cpp`, read the four flagged sites and the class's `test_write` field declaration**

Read `src/ui/desktop/ecu_operations.h` for the `test_write` field's declared type and purpose, then read lines 520-900 of `src/ui/desktop/ecu_operations.cpp` covering all four sites. Determine whether each site's local `test_write` is meant to be a temporary override (fix (a)) or is redundant (fix (b)).

- [ ] **Step 2: Write a test capturing current behavior for the affected function(s) if none exists**

Check whether `src/ui/desktop/ecu_operations_test.cpp` exists and covers the function(s) containing lines 539/661/779/894. If not, this class likely has no scripted coverage yet (consistent with `docs/tech-debt.md`'s "most flash orchestration ... remain lightly covered") — write a minimal test that exercises the function with `test_write=true` and `test_write=false` and asserts on the resulting `STATUS_*` return value or emitted log calls, so the rename can't silently change behavior. If a widget-free unit test isn't feasible for this class without deep refactor, note that in the commit message and rely on manual verification (compile + run `-Wshadow-all` clean) instead — don't block the rename on an out-of-scope refactor.

- [ ] **Step 3: Apply the fix at all four sites, then the same at the other six files' sites, following the same read-first-then-fix procedure**

Repeat Steps 1-2 for `vehicle_select.cpp`, `settings.cpp`, `mainwindow.cpp`, `menu_actions.cpp`, `flash_ecu_subaru_unisia_jecs_operation.cpp`, and `flash_ecu_subaru_unisia_jecs.cpp` before writing any fix, since a rename applied without reading the surrounding function risks silently changing which value a later line reads.

- [ ] **Step 4: Rebuild the fixed files with `-Wshadow-all` to confirm the warning is gone**

For each fixed file, regenerate its clang invocation the same way Task 3's script does (or just re-run `python3 -m scripts.crosscheck_s1117_shadow` and confirm the file no longer appears in the "confirmed" list).

- [ ] **Step 5: Run the full test suite**

Run: `bazel test --config=release //...`
Expected: PASS, no new failures.

- [ ] **Step 6: Commit**

```bash
git add src/ui/desktop/ecu_operations.cpp src/ui/desktop/vehicle_select.cpp \
        src/ui/desktop/settings.cpp src/ui/desktop/mainwindow.cpp \
        src/ui/desktop/menu_actions.cpp \
        src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_unisia_jecs_operation.cpp \
        src/ui/desktop/flash/ecu/flash_ecu_subaru_unisia_jecs.cpp
git commit -m "fix: resolve compiler-confirmed S1117 shadowed declarations"
```

---

### Task 5: Manually triage the header-only `cpp:S1117` candidates

**Files:**
- Modify (if needed): `src/backend/config/car_model_catalog.h` (lines 37, 55 — `type`), `src/backend/definition/definition_model.h` (lines 96, 119, 166, 192 — `type`), `src/backend/protocol/transport_legacy_compat.h` (lines 27, 33, 48, 54, 64, 70 — `result`), `src/platform/desktop/common/serial/serial_port_actions.h` (line 265 — `result`)

These have no direct `compile_commands.json` entry because they're headers, not translation units. Task 3's script couldn't cross-check them automatically.

- [ ] **Step 1: Find a `.cpp` that includes each header and get its compile command**

```bash
grep -rl 'include "src/backend/config/car_model_catalog.h"' src/ | head -1
```

Repeat for each of the four headers. Look up that `.cpp`'s entry in `compile_commands.json`.

- [ ] **Step 2: Run `clang++ -Wshadow-all -fsyntax-only` on each including `.cpp`, using the same command construction as `shadow_warning_lines` in `scripts/crosscheck_s1117_shadow.py`, and check whether the header's flagged lines appear**

- [ ] **Step 3: For any line clang confirms, apply the same read-then-fix recipe as Task 4. For any line clang does not confirm, resolve it as false positive using `scripts/sonar_issues.py`'s `resolve_false_positive`, with the same "not reproduced by clang++ -Wshadow-all" comment used in Task 3**

- [ ] **Step 4: Run the full test suite**

Run: `bazel test --config=release //...`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add -A  # only the specific header/source files touched — review before committing
git commit -m "fix: resolve header-only S1117 shadow candidates"
```

---

### Task 6: Triage and fix `cpp:S5025` (raw `new`/`delete`) in the top 4 files

**Files:**
- Modify: `src/ui/desktop/settings.h`, `src/ui/desktop/settings.cpp` (31 findings)
- Modify: `src/ui/desktop/mainwindow.cpp` (18 findings)
- Modify: `src/ui/desktop/hexedit/hexedit.cpp` (20 findings)
- Modify: `src/ui/desktop/logvalues.cpp` (10 findings)
- Test: `src/ui/desktop/settings_test.cpp` (new, if it doesn't exist)

**Decision rule (apply per `new` call site):**
1. If the allocated type derives from `QObject`/`QWidget` **and** the call either passes a parent/owner as a constructor argument (e.g. `new QListWidgetItem(text, listWidget)`) or the result is added to a layout/container that itself has a parent (e.g. `layout->addWidget(new QLabel(...))`) in the same function — this is Qt's ownership model, which SonarCloud's analyzer doesn't understand. Resolve as false positive.
2. If the allocated type does **not** derive from `QObject` (grep its class declaration for `: public QObject`/`: public QWidget`), or derives from it but is never given a parent/owner — this is a genuine candidate for RAII. Convert the raw pointer to `std::unique_ptr<T>` (member) or `std::make_unique<T>` (local), updating every use site (raw pointer syntax works unchanged through `unique_ptr::operator->`/`operator*`; only assignment and the type declaration need to change).

- [ ] **Step 1: Fix the flagship confirmed leak — `Settings::fileActions`**

Read `src/ui/desktop/settings.h:53-54` and `src/ui/desktop/settings.cpp:47-56`. `FileActions` does not derive from `QObject` (confirm: `grep -n "^class FileActions" src/backend/definitions/file_actions.h`), the member is assigned via `new` in `save_config_file()`, immediately used once, and never deleted or read anywhere else in the file (confirm: `grep -n "fileActions" src/ui/desktop/settings.cpp src/ui/desktop/settings.h` returns only these 3 lines). Since it's write-once/read-once within a single function, it doesn't need to be a class member at all: remove the `FileActions *fileActions{};` field from `settings.h`, and change `save_config_file()` in `settings.cpp` to:

```cpp
int Settings::save_config_file()
{
    qDebug() << "Save config file";
    auto fileActions = std::make_unique<FileActions>(m_configFileSystem, m_configResourceBundle,
                                                       m_configFileRepository, m_definitionFileWriter,
                                                       m_fileActionsEvents);
    fileActions->save_config_file(configValues);
    qDebug() << "Config file saved";

    return 0;
}
```

Add `#include <memory>` to `settings.cpp` if not already present.

- [ ] **Step 2: Write a test proving `save_config_file()` still calls through correctly**

Check whether `src/ui/desktop/settings_test.cpp` exists. If not, create it following the pattern in an existing `*_test.cpp` that constructs a `Settings`-like class with fake ports (see `src/backend/ports/testing/` for the fake `IFileSystem`/`IResourceBundle`/`IFileRepository` used elsewhere) and assert that `save_config_file()` returns `0` and that the fake file system received the expected write. If `Settings` requires a live `QApplication` to construct (check its base class), note that as a pre-existing constraint in the commit message and instead add a narrower test at the `FileActions::save_config_file` level if one doesn't already exist — don't block this leak fix on an unrelated widget-testability refactor.

- [ ] **Step 3: Classify and resolve the remaining 30 `S5025` findings in `settings.cpp`**

Run `sonar list issues --project RcusStackwalker_FastECU --statuses OPEN,CONFIRMED --format json` filtered to `cpp:S5025` and `settings.cpp` (or reuse `fetch_open_issues(["cpp:S5025"])` from Task 1). For each, read the line and apply the decision rule above. Expect most to be Qt-parented widgets (rule 1) — resolve those as false positive with comment "False positive: Qt parent-owned allocation; SonarCloud's C-family analyzer doesn't model Qt's parent-child ownership transfer." Fix any genuine ones the same way as Step 1.

- [ ] **Step 4: Repeat Step 3's classify-and-resolve-or-fix procedure for `mainwindow.cpp`, `hexedit.cpp`, and `logvalues.cpp`**

- [ ] **Step 5: Run the full test suite**

Run: `bazel test --config=release //...`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/ui/desktop/settings.h src/ui/desktop/settings.cpp src/ui/desktop/mainwindow.cpp \
        src/ui/desktop/hexedit/hexedit.cpp src/ui/desktop/logvalues.cpp
# include settings_test.cpp / its BUILD.bazel entry if created
git commit -m "fix(sonar): fix genuine S5025 leaks and resolve Qt-owned false positives"
```

---

### Task 7: Fix `cpp:S5276` (implicit narrowing conversion) in the top 2 files

**Files:**
- Modify: `src/platform/desktop/unix/j2534/J2534_unix.cpp` (32 findings)
- Modify: `src/ui/desktop/ecu_operations.cpp` (30 findings — some may already be touched by Task 4; re-check line numbers after Task 4 lands, since a rename can shift lines)
- Test: `src/platform/desktop/unix/j2534/J2534_unix_test.cpp` if it exists; otherwise document the coverage gap.

**Fix recipe (apply per instance):** Each finding is `implicit conversion loses integer precision: 'X' to 'Y'` at an assignment or function-argument site. For each:
1. Determine the actual range of values the source expression (`X`) can take at that call site (read backwards through the function/caller).
2. If the value is provably always within `Y`'s range (e.g. it comes from a `uint16_t` field earlier widened to `unsigned long` for arithmetic, then narrowed back), add an explicit `static_cast<Y>(...)` to document that the narrowing is intentional and safe — no behavior change, just makes intent explicit for both the reader and the linter.
3. If the value is **not** provably bounded (e.g. it's a length or count computed from user/ECU-supplied data that could exceed `Y`'s range), this is a real truncation risk: add a bounds check that returns an error (via the function's existing `Result<T>`/`bool` return convention) before the narrowing, and write a test exercising the boundary value.

- [ ] **Step 1: List and classify all 32 `J2534_unix.cpp` instances**

Run `fetch_open_issues(["cpp:S5276"])` from Task 1, filter to `J2534_unix.cpp`, and for each line, read the surrounding function (this is a J2534 PassThru bridge — check `J2534_unix.h` for the signatures of `PassThruConnect`, `PassThruIoctl`, etc., since these narrow vendor DLL parameters like channel IDs and config IDs) and classify per the fix recipe above.

- [ ] **Step 2: Fix the "provably safe" instances with explicit casts**

Apply `static_cast<TargetType>(...)` at each such site.

- [ ] **Step 3: For any "real truncation risk" instances, write a failing test first**

Write a test in `J2534_unix_test.cpp` (create it, following the fake-transport pattern used by an existing protocol test, if it doesn't exist) that calls the affected function with a value exceeding the target type's range and asserts it returns an error rather than silently truncating. Run it and confirm it currently fails (i.e. the current code truncates instead of erroring).

- [ ] **Step 4: Add the bounds check to make the test pass**

- [ ] **Step 5: Run tests to verify they pass**

Run: `bazel test --config=release //src/platform/desktop/unix/j2534:all` (adjust target name to match the package's actual `BUILD.bazel`)
Expected: PASS.

- [ ] **Step 6: Repeat Steps 1-5 for `ecu_operations.cpp`'s 30 instances**

- [ ] **Step 7: Run the full test suite**

Run: `bazel test --config=release //...`
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add src/platform/desktop/unix/j2534/J2534_unix.cpp src/ui/desktop/ecu_operations.cpp
# include any new/modified *_test.cpp and BUILD.bazel changes
git commit -m "fix(sonar): resolve S5276 narrowing conversions in J2534 bridge and ecu_operations"
```

---

### Task 8: Re-verify against a fresh scan and close out the tech-debt entry

**Files:**
- Modify: `docs/tech-debt.md` (Phase 1 section)

- [ ] **Step 1: Push all Phase 1 branches/PRs and let CI's SonarCloud scan run**

- [ ] **Step 2: Re-pull the current backlog**

```bash
sonar list issues --project RcusStackwalker_FastECU --statuses OPEN,CONFIRMED --format json --page-size 500 | \
  python3 -c "import json,sys,collections; d=json.load(sys.stdin); c=collections.Counter(i['rule'] for i in d['issues']); print(c['cpp:S1117'], c['cpp:S5276'], c['cpp:S5025'])"
```

Confirm `S1117` dropped to roughly the count of genuinely-remaining scattered instances (should be near 0 for the classes this plan targeted), `S5025` dropped by the number of genuine fixes made (Qt-owned ones resolved as false positive don't reduce the *false positive resolution* count differently from a code fix — check `issueStatus` breakdown, not just OPEN count, since bulk-resolved issues move to `FALSE_POSITIVE` status, not `FIXED`), and `S5276` dropped by exactly the number of instances fixed in Task 7 (the remaining ~142 in other files are out of this plan's scope).

- [ ] **Step 3: Update `docs/tech-debt.md`'s Phase 1 description**

Replace the Phase 1 paragraph's "Read every `S1117`/`S5276` instance in the top 10 offending files" framing with the actual outcome: the `emit`-signal scanner artifact discovery, the real/false-positive split for `S1117`, and the fact that the remaining `S5276`/`S5025` instances outside the files this plan covered stay a documented backlog rather than a scheduled phase (matching how Phase 4's scattered instances are already described).

- [ ] **Step 4: Commit**

```bash
git add docs/tech-debt.md
git commit -m "docs(tech-debt): close out Phase 1 SonarCloud triage with actual outcomes"
```

## Self-Review Notes

- **Spec coverage:** Every Phase 1 rule from `docs/tech-debt.md` (`S1117`, `S5276`, `S5025`) has at least one task. The plan diverges from the spec's original "read every instance in the top 10 files" framing because the investigation that produced this plan found most `S1117` instances aren't real — Task 8 updates the spec to reflect that.
- **Type consistency:** `Issue`, `fetch_open_issues`, `resolve_false_positive` (Task 1) are used with identical signatures in Tasks 2, 3, 5, 6, 7. `classify()` in Task 2 returns `(emit_artifact, remaining)`; `classify()` in Task 3 returns `(confirmed, disagree, no_cc)` — different arity, both documented in each task's Interfaces block so an executor reading only one task isn't confused by the name reuse across two modules.
- **Known drift risk:** exact line numbers and issue counts in this plan reflect a SonarCloud snapshot taken 2026-09-06. Tasks 3 and 6 explicitly instruct re-fetching live data rather than trusting this document's numbers, since the backlog will have moved by execution time.
