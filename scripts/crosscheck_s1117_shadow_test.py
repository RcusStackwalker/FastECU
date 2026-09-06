from __future__ import annotations

import unittest
from unittest.mock import MagicMock, patch

from crosscheck_s1117_shadow import classify, shadow_warning_lines
from sonar_issues import Issue

_raises = unittest.TestCase().assertRaises


def test_classify_splits_confirmed_disagree_and_missing_compile_command():
    issues = [
        Issue(key="a", rule="cpp:S1117", component="P:has_cc.cpp", line=10, message="m"),
        Issue(key="b", rule="cpp:S1117", component="P:has_cc.cpp", line=20, message="m"),
        Issue(key="c", rule="cpp:S1117", component="P:no_cc.h", line=5, message="m"),
    ]
    compile_commands = {"has_cc.cpp": {"arguments": ["clang++"], "directory": "."}}

    with patch(
        "crosscheck_s1117_shadow.shadow_warning_lines",
        return_value={10},
    ):
        confirmed, disagree, no_cc = classify(issues, compile_commands)

    assert [i.key for i in confirmed] == ["a"]
    assert [i.key for i in disagree] == ["b"]
    assert [i.key for i in no_cc] == ["c"]


def test_shadow_warning_lines_raises_when_compile_fails_with_no_shadow_lines():
    entry = {"arguments": ["clang++", "-std=c++23"], "directory": "."}
    fatal_stderr = (
        "src/ui/desktop/foo.cpp:1:10: fatal error: 'QtCore/QObject' file not found\n"
        "1 error generated.\n"
    )
    with patch("subprocess.run") as run:
        run.return_value = MagicMock(returncode=1, stderr=fatal_stderr, stdout="")
        with _raises(RuntimeError):
            shadow_warning_lines(entry, "src/ui/desktop/foo.cpp")


def test_shadow_warning_lines_returns_empty_set_on_clean_compile():
    entry = {"arguments": ["clang++", "-std=c++23"], "directory": "."}
    with patch("subprocess.run") as run:
        run.return_value = MagicMock(returncode=0, stderr="", stdout="")
        assert shadow_warning_lines(entry, "src/ui/desktop/foo.cpp") == set()
