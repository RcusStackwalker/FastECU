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
