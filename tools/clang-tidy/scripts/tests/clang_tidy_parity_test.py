#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path

import clang_tidy_parity as parity

_BRACES = "error: src/a.cpp:4:19 [readability-braces-around-statements]"
_ENDL = "error: src/a.cpp:6:27 [performance-avoid-endl]"


class FindingsTest(unittest.TestCase):
    def test_header_lines_are_found_among_other_output(self) -> None:
        lines = [
            "[1/3] src/a.cpp",
            _BRACES,
            "  statement should be inside braces",
            "    4 | if (x)",
        ]
        self.assertEqual(
            {("error", "src/a.cpp:4:19", "readability-braces-around-statements")},
            parity.findings(lines),
        )

    def test_colored_headers_match_too(self) -> None:
        colored = "\x1b[1;31merror\x1b[0m: src/a.cpp:6:27 \x1b[2m[performance-avoid-endl]\x1b[0m"
        self.assertEqual(parity.findings([_ENDL]), parity.findings([colored]))

    def test_summary_and_note_lines_are_not_findings(self) -> None:
        self.assertEqual(
            set(), parity.findings(["clang-tidy: 3 findings", "note: src/a.h:1:1 declared here"])
        )

    def test_the_same_finding_twice_counts_once(self) -> None:
        self.assertEqual(1, len(parity.findings([_BRACES, _BRACES])))


class CompareTest(unittest.TestCase):
    def test_agreement_has_no_differences(self) -> None:
        found = parity.findings([_BRACES, _ENDL])
        self.assertEqual([], parity.compare(found, found))

    def test_each_side_reports_what_only_it_has(self) -> None:
        differences = parity.compare(parity.findings([_BRACES]), parity.findings([_ENDL]))
        self.assertEqual(2, len(differences))
        self.assertTrue(differences[0].startswith("only in expected:"))
        self.assertTrue(differences[1].startswith("only in actual:"))


class MainTest(unittest.TestCase):
    def run_main(self, expected: str, actual: str) -> int:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "expected.txt").write_text(expected)
            (directory / "actual.txt").write_text(actual)
            return parity.main([str(directory / "expected.txt"), str(directory / "actual.txt")])

    def test_exit_code_is_zero_when_the_runs_agree(self) -> None:
        self.assertEqual(0, self.run_main(_BRACES + "\n", _BRACES + "\n"))

    def test_exit_code_is_one_when_they_differ(self) -> None:
        self.assertEqual(1, self.run_main(_BRACES + "\n", ""))


if __name__ == "__main__":
    unittest.main()
