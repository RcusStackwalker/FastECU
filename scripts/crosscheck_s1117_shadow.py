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

from sonar_issues import Issue


def load_compile_commands(path: str = "compile_commands.json") -> dict[str, dict]:
    entries = json.loads(Path(path).read_text())
    # Only keep src/-prefixed entries: bazel-out/**/moc_*.cpp and other
    # generated entries can never match a src/-relative Issue.file_path
    # lookup anyway, so filtering them out up front keeps the dict small
    # and avoids storing entries that classify() would never look up.
    return {e["file"]: e for e in entries if e["file"].startswith("src/")}


def shadow_warning_lines(entry: dict, relative_path: str) -> set[int]:
    """Run clang++ -Wshadow-all -fsyntax-only and collect shadow-warning lines.

    Raises RuntimeError if the compile itself fails (non-zero returncode)
    and no shadow-diagnostic lines were found in stderr -- an empty result
    in that case does not mean "no shadows," it means the file didn't
    compile (e.g. an unresolved header outside Bazel's exec-root symlink
    structure), and silently returning an empty set would let callers
    misclassify a real finding as a false positive.
    """
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
    if result.returncode != 0 and not lines:
        stderr_snippet = "\n".join(result.stderr.splitlines()[-40:])
        raise RuntimeError(
            f"clang++ -fsyntax-only failed to compile {relative_path} "
            f"(exit {result.returncode}); cannot determine whether it has "
            f"shadow warnings. stderr (last 40 lines):\n{stderr_snippet}"
        )
    return lines


def classify(
    issues: list[Issue],
    compile_commands: dict[str, dict],
) -> tuple[list[Issue], list[Issue], list[Issue]]:
    """Classify issues as confirmed / disagree / no-compile-command.

    Note: a compile failure inside shadow_warning_lines() raises
    RuntimeError, which is intentionally allowed to propagate here rather
    than being caught and misclassified -- a broken compile should stop
    the triage run, not silently produce a wrong bucket.
    """
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

    from classify_s1117_signal_shadows import classify as classify_signals
    from classify_s1117_signal_shadows import find_signal_names
    from sonar_issues import fetch_open_issues, resolve_false_positive

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
