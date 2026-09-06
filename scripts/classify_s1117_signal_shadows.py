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
        with open(path, encoding="utf-8", errors="ignore") as f:
            text = f.read()
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
