#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path, PureWindowsPath
from unittest import mock

import clang_tidy_benchmark as benchmark


class ClangTidyBenchmark(unittest.TestCase):
    def test_fingerprint_detects_runner_changes_with_windows_paths(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "runner.py"
            source.write_text("# original\n")
            tree = benchmark.runner._WorkspaceTree(
                root,
                {
                    PureWindowsPath("scripts/clang_tidy_runner.py"): source,
                },
                frozenset(),
            )
            with mock.patch.object(benchmark.runner, "_workspace_tree", return_value=tree):
                original = benchmark.fingerprint(root)
                source.write_text("# changed\n")
                self.assertNotEqual(original, benchmark.fingerprint(root))

    def test_fingerprint_detects_nested_configuration_and_implementation_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / ".clang-tidy").write_text("Checks: '*'\n")
            (root / "src").mkdir()
            nested = root / "src/.clang-tidy"
            nested.write_text("InheritParentConfig: true\n")
            (root / "scripts").mkdir()
            source = root / "scripts/clang_tidy_runner.py"
            source.write_text("# original\n")
            original = benchmark.fingerprint(root)
            nested.write_text("InheritParentConfig: false\n")
            self.assertNotEqual(original, benchmark.fingerprint(root))
            changed_configuration = benchmark.fingerprint(root)
            source.write_text("# revised\n")
            self.assertNotEqual(changed_configuration, benchmark.fingerprint(root))

    def test_summary_uses_analysis_phases_and_keeps_per_check_costs(self) -> None:
        runs = [
            {"enabled": False, "phases": {"analysis": 10.0, "prebuild": 900.0}, "checks": {}},
            {
                "enabled": True,
                "phases": {"analysis": 10.0, "custom-analysis": 5.0},
                "checks": {"custom-fastecu-result-assertion": 1.0},
            },
            {"enabled": False, "phases": {"analysis": 12.0}, "checks": {}},
            {
                "enabled": True,
                "phases": {"analysis": 12.0, "custom-analysis": 6.0},
                "checks": {"custom-fastecu-result-assertion": 2.0},
            },
            {"enabled": False, "phases": {"analysis": 11.0}, "checks": {}},
            {
                "enabled": True,
                "phases": {"analysis": 11.0, "custom-analysis": 5.0},
                "checks": {"custom-fastecu-result-assertion": 1.5},
            },
        ]
        result = benchmark.summarize(runs)
        self.assertEqual(11.0, result["baseline_median_seconds"])
        self.assertEqual(16.0, result["enabled_median_seconds"])
        self.assertAlmostEqual(500.0 / 11.0, result["slowdown_percent"])
        self.assertEqual(
            1.5, result["custom_check_median_seconds"]["custom-fastecu-result-assertion"]
        )


if __name__ == "__main__":
    unittest.main()
