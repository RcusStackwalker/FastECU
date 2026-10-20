#!/usr/bin/env python3

import json
import tempfile
import unittest
from pathlib import Path, PureWindowsPath
from unittest import mock

import clang_tidy_benchmark as benchmark


class ClangTidyBenchmark(unittest.TestCase):
    def test_main_compares_native_checks_without_false_input_changes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / ".clang-tidy").write_text("Checks: '-*,fastecu-*'\n")
            identity = root / "tools/clang-tidy/plugin/llvm-identity.json"
            identity.parent.mkdir(parents=True)
            identity.write_text('{"version":"23.1.2"}')
            executable = root / "host"
            executable.touch()
            host = benchmark.runner.analysis_toolchain.AnalysisHost(executable, (), root / "llvm")
            tools = benchmark.runner.Tools(str(executable), "/llvm/run-clang-tidy")

            def workflow(**kwargs):
                (root / "compile_commands.json").write_text("[]")
                return {
                    "translation_units": 2,
                    "findings": 0,
                    "phases": {"analysis": 1.2 if kwargs["custom_checks"] else 1.0},
                    "checks": {"fastecu-result-assertion": 0.1} if kwargs["custom_checks"] else {},
                }

            output = root / "reports/run"
            with (
                mock.patch.object(
                    benchmark,
                    "__file__",
                    str(root / "tools/clang-tidy/scripts/clang_tidy_benchmark.py"),
                ),
                mock.patch.object(
                    benchmark.sys,
                    "argv",
                    ["benchmark", "--compdb-tool", "/tool", "--output", str(output)],
                ),
                mock.patch.object(benchmark.runner, "workspace_root", return_value=root),
                mock.patch.object(
                    benchmark.runner.analysis_toolchain, "runfiles_analysis_host", return_value=host
                ),
                mock.patch.object(benchmark.runner, "discover_tools", return_value=tools),
                mock.patch.object(benchmark.runner, "run_workflow", side_effect=workflow),
                mock.patch.object(benchmark.subprocess, "check_output", return_value="23.1.2"),
                mock.patch.object(benchmark.platform, "platform", return_value="test-host"),
            ):
                self.assertEqual(0, benchmark.main())
            document = json.loads((output / "results.json").read_text())
            self.assertEqual(6, len(document["runs"]))
            self.assertTrue(
                any(key.startswith("artifact:") for key in document["metadata"]["input_sha256"])
            )

    def test_fingerprint_detects_runner_changes_with_windows_paths(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "runner.py"
            source.write_text("# original\n")
            tree = benchmark.runner._WorkspaceTree(
                root,
                {
                    PureWindowsPath("tools/clang-tidy/scripts/clang_tidy_runner.py"): source,
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
            source = root / "tools/clang-tidy/scripts/clang_tidy_runner.py"
            source.parent.mkdir(parents=True)
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
                "phases": {"analysis": 15.0},
                "checks": {"fastecu-result-assertion": 1.0},
            },
            {"enabled": False, "phases": {"analysis": 12.0}, "checks": {}},
            {
                "enabled": True,
                "phases": {"analysis": 18.0},
                "checks": {"fastecu-result-assertion": 2.0},
            },
            {"enabled": False, "phases": {"analysis": 11.0}, "checks": {}},
            {
                "enabled": True,
                "phases": {"analysis": 16.0},
                "checks": {"fastecu-result-assertion": 1.5},
            },
        ]
        result = benchmark.summarize(runs)
        self.assertEqual(11.0, result["baseline_median_seconds"])
        self.assertEqual(16.0, result["enabled_median_seconds"])
        self.assertAlmostEqual(500.0 / 11.0, result["slowdown_percent"])
        self.assertEqual(1.5, result["custom_check_median_seconds"]["fastecu-result-assertion"])

    def test_scope_change_invalidates_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scope = root / ".clang-tidy-scope.toml"
            scope.write_text("version = 1\n")
            original = benchmark.fingerprint(root)
            scope.write_text("version = 1\n# revised platform selection\n")
            self.assertNotEqual(original, benchmark.fingerprint(root))

    def test_plugin_change_invalidates_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "tools/clang-tidy/plugin/ResultAssertionCheck.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("original")
            artifact = root / "checks.dylib"
            artifact.write_bytes(b"original binary")
            before = benchmark.fingerprint(root, artifacts=(artifact,))
            source.write_text("changed")
            after_source = benchmark.fingerprint(root, artifacts=(artifact,))
            self.assertNotEqual(before, after_source)
            artifact.write_bytes(b"changed binary")
            self.assertNotEqual(after_source, benchmark.fingerprint(root, artifacts=(artifact,)))

    def test_diagnostic_comparison_preserves_message_and_location(self) -> None:
        original = {
            "DiagnosticName": "custom-fastecu-result-assertion",
            "Level": "Warning",
            "DiagnosticMessage": {
                "FilePath": "/old/src/test.cpp",
                "FileOffset": 7,
                "Message": "use matcher",
            },
        }
        migrated = {
            "DiagnosticName": "fastecu-result-assertion",
            "Level": "Warning",
            "DiagnosticMessage": {
                "FilePath": "/new/src/test.cpp",
                "FileOffset": 7,
                "Message": "use matcher",
            },
        }
        before = benchmark.normalized_diagnostics([original], Path("/old"))
        self.assertEqual(before, benchmark.normalized_diagnostics([migrated], Path("/new")))
        migrated["DiagnosticMessage"]["FileOffset"] = 8
        self.assertNotEqual(before, benchmark.normalized_diagnostics([migrated], Path("/new")))
        migrated["DiagnosticMessage"]["FileOffset"] = 7
        migrated["DiagnosticMessage"]["Message"] = "different"
        self.assertNotEqual(before, benchmark.normalized_diagnostics([migrated], Path("/new")))


if __name__ == "__main__":
    unittest.main()
