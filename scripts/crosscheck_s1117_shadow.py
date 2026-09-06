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
        cmd,
        cwd=entry.get("directory", "."),
        capture_output=True,
        text=True,
        timeout=120,
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
    issues: list[Issue],
    compile_commands: dict[str, dict],
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
    import glob

    from scripts.classify_s1117_signal_shadows import classify as classify_signals
    from scripts.classify_s1117_signal_shadows import find_signal_names
    from scripts.sonar_issues import fetch_open_issues, resolve_false_positive

    all_s1117 = fetch_open_issues(["cpp:S1117"])
    _, remaining = classify_signals(
        all_s1117, find_signal_names(glob.glob("src/**/*.h", recursive=True))
    )

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
