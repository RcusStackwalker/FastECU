#!/usr/bin/env python3
"""Read real lint reports produced through a transitioned C++ dependency graph."""

import sys
import unittest
from pathlib import Path

REPORTS = [Path(argument) for argument in sys.argv[1:]]


class TransitionOutputsTest(unittest.TestCase):
    def test_transitioned_source_is_analyzed_and_reported(self) -> None:
        statuses = [path for path in REPORTS if path.name.endswith(".out.exit_code")]
        self.assertTrue(statuses)
        for path in statuses:
            self.assertEqual("0", path.read_text().strip(), path)
        human = [path for path in REPORTS if path.name == "probe.cpp.AspectRulesLintClangTidy.out"]
        self.assertEqual(1, len(human))
        self.assertIn("clang-tidy checks profiling", human[0].read_text())
        fixes = [
            path
            for path in REPORTS
            if path.name.endswith("probe.cpp.AspectRulesLintClangTidy.fixes.yaml")
        ]
        self.assertEqual(1, len(fixes))
        self.assertTrue(fixes[0].is_file())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
