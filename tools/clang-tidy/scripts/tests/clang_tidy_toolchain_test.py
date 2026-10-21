"""Validate host selection before launching clang-tidy."""

import hashlib
import json
import re
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import clang_tidy_toolchain as toolchain


class AnalysisHostTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tidy host with spaces ")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.root = self.directory / "llvm"
        (self.root / "bin").mkdir(parents=True)
        (self.root / "include/clang-tidy").mkdir(parents=True)
        for name in ("clang-tidy", "llvm-config"):
            (self.root / "bin" / name).touch()
        (self.root / "include/clang-tidy/ClangTidyModule.h").touch()
        self.artifact = self.directory / "plugin.dylib"
        self.artifact.touch()
        self.identity = {
            "root": str(self.root.resolve()),
            "version": "23.1.2",
            "clang_tidy_sha256": hashlib.sha256(b"").hexdigest(),
        }
        self.save_identity()

    def save_identity(self):
        (self.artifact.parent / "llvm-identity.json").write_text(json.dumps(self.identity))

    def test_wrong_release_fails_before_launch(self):
        self.identity["version"] = "23.1.3"
        self.save_identity()
        with patch.object(toolchain.subprocess, "check_output") as launch:
            with self.assertRaisesRegex(toolchain.ToolchainError, "23.1.2"):
                toolchain.resolve_analysis_host(self.root, self.artifact)
            launch.assert_not_called()

    def test_mixed_installation_fails(self):
        self.identity["root"] = str(self.directory / "other-llvm")
        self.save_identity()
        with patch.object(toolchain.subprocess, "check_output") as launch:
            with self.assertRaisesRegex(toolchain.ToolchainError, "installation"):
                toolchain.resolve_analysis_host(self.root, self.artifact)
            launch.assert_not_called()

    def test_missing_artifact_fails(self):
        self.artifact.unlink()
        with self.assertRaisesRegex(toolchain.ToolchainError, "artifact"):
            toolchain.resolve_analysis_host(self.root, self.artifact)

    def test_incomplete_registration_fails(self):
        with (
            patch.object(
                toolchain.subprocess,
                "check_output",
                side_effect=["LLVM version 23.1.2", "fastecu-template-typename"],
            ),
            self.assertRaisesRegex(toolchain.ToolchainError, "register"),
        ):
            toolchain.resolve_analysis_host(self.root, self.artifact)

    def test_paths_with_spaces(self):
        def output(command, **kwargs):
            if command[-1] == "--version":
                return "LLVM version 23.1.2\n"
            return "\n".join(sorted(toolchain.REQUIRED_CHECKS))

        with patch.object(toolchain.subprocess, "check_output", side_effect=output):
            host = toolchain.resolve_analysis_host(self.root, self.artifact)
        self.assertEqual(host.executable, self.root.resolve() / "bin/clang-tidy")
        self.assertEqual(host.arguments, (f"--load={self.artifact.resolve()}",))

    def test_host_changed_since_build_fails(self):
        (self.root / "bin/clang-tidy").write_text("different executable")
        with patch.object(toolchain.subprocess, "check_output") as launch:
            with self.assertRaisesRegex(toolchain.ToolchainError, "changed"):
                toolchain.resolve_analysis_host(self.root, self.artifact)
            launch.assert_not_called()

    def test_ignored_plugin_load_fails(self):
        with (
            patch.object(
                toolchain.subprocess,
                "check_output",
                side_effect=["LLVM version 23.1.2", "bugprone-test"],
            ),
            self.assertRaisesRegex(toolchain.ToolchainError, "register"),
        ):
            toolchain.resolve_analysis_host(self.root, self.artifact)

    def test_pin_matches_ci(self):
        workflow = Path(__file__).parents[4] / ".github/workflows/pr.yml"
        version = re.search(r"CLANG_TIDY_LLVM_VERSION: '([^']+)'", workflow.read_text())
        self.assertIsNotNone(version)
        self.assertEqual(version.group(1), toolchain.REQUIRED_LLVM_VERSION)

    def test_linked_host_has_no_load_argument(self):
        executable = self.artifact.with_suffix(".exe")
        executable.touch()
        (self.root / "bin/clang-tidy.exe").touch()
        with patch.object(
            toolchain.subprocess,
            "check_output",
            side_effect=["LLVM version 23.1.2", "\n".join(sorted(toolchain.REQUIRED_CHECKS))],
        ):
            host = toolchain.resolve_analysis_host(self.root, executable)
        self.assertEqual(executable.resolve(), host.executable)
        self.assertEqual((), host.arguments)

    def test_identity_uses_path_equivalence(self):
        self.identity["root"] = str(self.root / ".." / self.root.name)
        self.save_identity()
        with patch.object(
            toolchain.subprocess,
            "check_output",
            side_effect=["LLVM version 23.1.2", "\n".join(sorted(toolchain.REQUIRED_CHECKS))],
        ):
            host = toolchain.resolve_analysis_host(self.root, self.artifact)
        self.assertEqual(self.root.resolve(), host.llvm_root)

    def test_invalid_identity_has_actionable_error(self):
        (self.artifact.parent / "llvm-identity.json").write_text("[]")
        with self.assertRaisesRegex(toolchain.ToolchainError, "identity"):
            toolchain.resolve_analysis_host(self.root, self.artifact)


if __name__ == "__main__":
    unittest.main()
