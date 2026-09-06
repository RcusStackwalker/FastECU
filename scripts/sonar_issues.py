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
                "sonar",
                "list",
                "issues",
                "--project",
                PROJECT,
                "--statuses",
                "OPEN,CONFIRMED",
                "--format",
                "json",
                "--page-size",
                str(PAGE_SIZE),
                "--page",
                str(page),
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        payload = json.loads(result.stdout)
        for raw in payload["issues"]:
            if raw["rule"] not in rules:
                continue
            issues.append(
                Issue(
                    key=raw["key"],
                    rule=raw["rule"],
                    component=raw["component"],
                    line=raw.get("line"),
                    message=raw["message"],
                )
            )
        total_pages = -(-payload["paging"]["total"] // PAGE_SIZE)
        if page >= total_pages:
            break
        page += 1
    return issues


def resolve_false_positive(issue_keys: list[str], comment: str) -> None:
    """Mark issues as false positive in SonarCloud, batched at PAGE_SIZE.

    Raises RuntimeError if the bulk_change response for any batch reports
    a nonzero failure count or a success count that doesn't match the
    batch size. SonarCloud's bulk_change endpoint can return HTTP success
    while actually resolving fewer issues than requested (permission gaps
    on a subset, already-transitioned issues, a bad key) -- for a mutation
    this consequential, under-resolution must fail loudly, not silently.
    """
    for start in range(0, len(issue_keys), PAGE_SIZE):
        batch = issue_keys[start : start + PAGE_SIZE]
        result = subprocess.run(
            [
                "sonar",
                "api",
                "post",
                "/api/issues/bulk_change",
                "-d",
                json.dumps(
                    {
                        "issues": ",".join(batch),
                        "do_transition": "falsepositive",
                        "comment": comment,
                    }
                ),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        response = json.loads(result.stdout)
        failures = response.get("failures", 0)
        success = response.get("success", 0)
        if failures > 0 or success != len(batch):
            raise RuntimeError(
                f"SonarCloud bulk_change under-resolved a batch of {len(batch)} issues: {response}"
            )
