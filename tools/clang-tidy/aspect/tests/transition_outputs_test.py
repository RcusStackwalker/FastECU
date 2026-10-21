#!/usr/bin/env python3
"""Read real lint reports produced through a transitioned C++ dependency graph."""

import os
import sys
import unittest
from pathlib import Path

from python.runfiles import runfiles

# A cached generated file can have an invalid directory junction in a Windows
# runfiles tree. The manifest names its actual output file on every platform.
_RUNFILES = runfiles.CreateManifestBased(str(Path(os.environ["TEST_SRCDIR"]) / "MANIFEST"))
REPORTS = [Path(_RUNFILES.Rlocation(f"_main/{argument}")) for argument in sys.argv[1:]]


class TransitionOutputsTest(unittest.TestCase):
    def test_transitioned_source_is_analyzed_and_reported(self) -> None:
        statuses = [path for path in REPORTS if path.name.endswith(".out.exit_code")]
        self.assertTrue(statuses)
        for path in statuses:
            self.assertTrue(
                path.is_file(),
                f"report is not a file: {path}; "
                f"directory={path.is_dir()}; symlink={path.is_symlink()}; "
                f"junction={path.is_junction()}; "
                f"entries={list(path.iterdir()) if path.is_dir() else []}",
            )
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
