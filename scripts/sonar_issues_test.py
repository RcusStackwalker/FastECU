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
