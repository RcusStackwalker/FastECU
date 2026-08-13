#!/usr/bin/env python3

import json
import os
import re
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from unittest import mock

import clang_tidy_runner as runner
import yaml

_WINDOWS_COMPDB_TOOL = "C:/tools/clang_tidy_compdb.exe"
_CONFIG_RELEASE = "--config=release"
_FASTECU_TARGET = "//:fastecu"
_UNIX_COMPDB_TOOL = "/tools/clang_tidy_compdb"
_MAIN_CPP = "main.cpp"
_UNIX_TOOLS = runner.Tools(
    clang_tidy="/llvm/bin/clang-tidy",
    run_clang_tidy="/llvm/bin/run-clang-tidy",
)


class ClangTidyRunnerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name) / "workspace"
        self.root.mkdir()
        (self.root / ".clang-tidy").write_text("Checks: '-*'\n")

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def write_database(self, files: list[Path]) -> None:
        entries = [
            {
                "directory": str(self.root),
                "file": str(path),
                "arguments": ["clang++", "-c", str(path)],
            }
            for path in files
        ]
        (self.root / "compile_commands.json").write_text(json.dumps(entries))

    def prebuild_fixture(
        self, *, bazel_build_returncode: int = 0
    ) -> tuple[list[list[str]], runner.CommandRunner]:
        """A compilable source plus a fake runner that records every command.

        `xcrun --show-sdk-path` always succeeds (needed on the macOS run_workflow
        path); `bazel build` fails with bazel_build_returncode when non-zero.
        """
        source = self.root / _MAIN_CPP
        source.write_text("int main() { return 0; }\n")
        self.write_database([source])
        commands: list[list[str]] = []

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            commands.append(command)
            if bazel_build_returncode and command[:2] == ["bazel", "build"]:
                return subprocess.CompletedProcess(command, bazel_build_returncode)
            if command == ["xcrun", "--show-sdk-path"]:
                return subprocess.CompletedProcess(command, 0, stdout="/SDK/MacOSX.sdk\n")
            return subprocess.CompletedProcess(command, 0)

        return commands, fake_run

    def write_fixes(
        self,
        directory: Path,
        name: str,
        diagnostics: list[dict[str, object]],
    ) -> Path:
        fixes = directory / name
        fixes.write_text(
            yaml.safe_dump(
                {
                    "MainSourceFile": str(self.root / f"{name}.cpp"),
                    "Diagnostics": diagnostics,
                },
                sort_keys=False,
            )
        )
        return fixes

    def diagnostic(
        self,
        name: str,
        replacements: list[dict[str, object]],
    ) -> dict[str, object]:
        return {
            "DiagnosticName": name,
            "DiagnosticMessage": {
                "Message": f"message for {name}",
                "FilePath": str(self.root / _MAIN_CPP),
                "FileOffset": 0,
                "Replacements": replacements,
            },
            "Level": "Warning",
            "BuildDirectory": str(self.root),
        }

    @staticmethod
    def replacement(
        file_path: Path,
        offset: int,
        length: int,
        text: str,
    ) -> dict[str, object]:
        return {
            "FilePath": str(file_path),
            "Offset": offset,
            "Length": length,
            "ReplacementText": text,
        }

    @staticmethod
    def read_replacements(directory: Path) -> list[dict[str, object]]:
        replacements: list[dict[str, object]] = []
        for fixes in sorted(directory.glob("*.yaml")):
            document = yaml.safe_load(fixes.read_text())
            for diagnostic in document["Diagnostics"]:
                replacements.extend(diagnostic["DiagnosticMessage"]["Replacements"])
        return replacements

    def test_workspace_root_requires_bazel_run(self) -> None:
        with self.assertRaisesRegex(runner.WorkflowError, "bazel run"):
            runner.workspace_root({})

    def test_database_keeps_only_workspace_translation_units(self) -> None:
        source = self.root / _MAIN_CPP
        source.write_text("int main() { return 0; }\n")
        generated = Path(self.temp_dir.name) / "bazel-out" / "moc_main.cpp"
        generated.parent.mkdir()
        generated.write_text("// generated\n")
        unsupported = self.root / "README.txt"
        self.write_database([source, generated, unsupported])

        entries = runner.load_project_entries(
            self.root,
            self.root / "compile_commands.json",
        )

        self.assertEqual([str(source)], [entry["file"] for entry in entries])

    def test_database_rejects_missing_workspace_translation_unit(self) -> None:
        missing = self.root / "missing.cpp"
        self.write_database([missing])

        with self.assertRaisesRegex(
            runner.WorkflowError,
            rf"missing workspace translation unit.*{re.escape(str(missing.resolve()))}",
        ):
            runner.load_project_entries(
                self.root,
                self.root / "compile_commands.json",
            )

    def test_database_rejects_malformed_json(self) -> None:
        (self.root / "compile_commands.json").write_text("{")
        with self.assertRaisesRegex(runner.WorkflowError, "malformed"):
            runner.load_project_entries(
                self.root,
                self.root / "compile_commands.json",
            )

    def test_database_rejects_invalid_utf8(self) -> None:
        (self.root / "compile_commands.json").write_bytes(b"\xff")
        with self.assertRaisesRegex(runner.WorkflowError, "malformed"):
            runner.load_project_entries(
                self.root,
                self.root / "compile_commands.json",
            )

    def test_database_rejects_no_project_sources(self) -> None:
        self.write_database([])
        with self.assertRaisesRegex(runner.WorkflowError, "no workspace"):
            runner.load_project_entries(
                self.root,
                self.root / "compile_commands.json",
            )

    def test_missing_tool_is_actionable(self) -> None:
        with (
            mock.patch.object(runner.shutil, "which", return_value=None),
            self.assertRaisesRegex(runner.WorkflowError, "Install LLVM"),
        ):
            runner.find_executable(
                ("clang-tidy",),
                platform_name="linux",
                environ={"PATH": ""},
            )

    def test_windows_report_builds_parallel_runner_command(self) -> None:
        source = self.root / "windows.cpp"
        source.write_text("int windows_source;\n")
        self.write_database([source])
        commands: list[list[str]] = []

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            commands.append(command)
            return subprocess.CompletedProcess(command, 0)

        tools = runner.Tools(
            clang_tidy="C:/LLVM/bin/clang-tidy.exe",
            run_clang_tidy="C:/LLVM/bin/run-clang-tidy.py",
        )
        with mock.patch.object(runner, "discover_tools", return_value=tools):
            runner.run_workflow(
                mode="report",
                workspace=self.root,
                compdb_tool=_WINDOWS_COMPDB_TOOL,
                platform_name="win32",
                environ={},
                command_runner=fake_run,
            )

        self.assertEqual(_WINDOWS_COMPDB_TOOL, commands[0][0])
        self.assertEqual(runner.sys.executable, commands[1][0])
        self.assertEqual("C:/LLVM/bin/run-clang-tidy.py", commands[1][1])
        self.assertIn("-clang-tidy-binary", commands[1])
        self.assertNotIn("-fix", commands[1])

    def test_windows_report_wraps_extensionless_run_clang_tidy(self) -> None:
        # LLVM 18+ ships run-clang-tidy without a .py suffix (a shebang'd
        # Python script). Windows can't exec that directly -> WinError 193
        # ("%1 is not a valid Win32 application") unless it's routed through
        # the Python interpreter, same as the .py case.
        source = self.root / "windows.cpp"
        source.write_text("int windows_source;\n")
        self.write_database([source])
        commands: list[list[str]] = []

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            commands.append(command)
            return subprocess.CompletedProcess(command, 0)

        tools = runner.Tools(
            clang_tidy="C:/LLVM/bin/clang-tidy.exe",
            run_clang_tidy="C:/LLVM/bin/run-clang-tidy",
        )
        with mock.patch.object(runner, "discover_tools", return_value=tools):
            runner.run_workflow(
                mode="report",
                workspace=self.root,
                compdb_tool=_WINDOWS_COMPDB_TOOL,
                platform_name="win32",
                environ={},
                command_runner=fake_run,
            )

        self.assertEqual(runner.sys.executable, commands[1][0])
        self.assertEqual("C:/LLVM/bin/run-clang-tidy", commands[1][1])
        self.assertIn("-clang-tidy-binary", commands[1])

    def test_windows_fix_is_rejected_before_refresh(self) -> None:
        command_runner = mock.Mock()
        with self.assertRaisesRegex(runner.WorkflowError, "macOS and Linux"):
            runner.run_workflow(
                mode="fix",
                workspace=self.root,
                compdb_tool="unused",
                platform_name="win32",
                environ={},
                command_runner=command_runner,
            )
        command_runner.assert_not_called()

    def test_macos_report_adds_sdk_sysroot_to_analysis(self) -> None:
        source = self.root / _MAIN_CPP
        source.write_text("int main() { return 0; }\n")
        self.write_database([source])
        commands: list[list[str]] = []

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            commands.append(command)
            return subprocess.CompletedProcess(command, 0, stdout="/SDK/MacOSX.sdk\n")

        tools = runner.Tools(
            clang_tidy="/llvm/bin/clang-tidy",
            run_clang_tidy="/llvm/bin/run-clang-tidy",
        )
        with mock.patch.object(runner, "discover_tools", return_value=tools):
            runner.run_workflow(
                mode="report",
                workspace=self.root,
                compdb_tool=_UNIX_COMPDB_TOOL,
                platform_name="darwin",
                environ={},
                command_runner=fake_run,
            )

        self.assertIn(["xcrun", "--show-sdk-path"], commands)
        analysis_command = next(
            command for command in commands if command[0] == tools.run_clang_tidy
        )
        self.assertIn("-extra-arg-before=-isysroot", analysis_command)
        self.assertIn("-extra-arg-before=/SDK/MacOSX.sdk", analysis_command)

    def test_macos_sdk_lookup_failure_is_actionable(self) -> None:
        source = self.root / _MAIN_CPP
        source.write_text("int main() { return 0; }\n")
        self.write_database([source])

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            if command == ["xcrun", "--show-sdk-path"]:
                return subprocess.CompletedProcess(command, 72, stdout="")
            return subprocess.CompletedProcess(command, 0)

        tools = runner.Tools(
            clang_tidy="/llvm/bin/clang-tidy",
            run_clang_tidy="/llvm/bin/run-clang-tidy",
        )
        with (
            mock.patch.object(runner, "discover_tools", return_value=tools),
            self.assertRaisesRegex(runner.WorkflowError, "xcrun.*72.*Command Line Tools"),
        ):
            runner.run_workflow(
                mode="report",
                workspace=self.root,
                compdb_tool=_UNIX_COMPDB_TOOL,
                platform_name="darwin",
                environ={},
                command_runner=fake_run,
            )

    def test_fix_uses_deferred_replacements(self) -> None:
        source = self.root / _MAIN_CPP
        source.write_text("int main() { return 0; }\n")
        self.write_database([source])
        commands: list[list[str]] = []
        applied_replacements: list[dict[str, object]] = []
        tools = runner.Tools(
            clang_tidy="/llvm/bin/clang-tidy",
            run_clang_tidy="/llvm/bin/run-clang-tidy",
            clang_apply_replacements="/llvm/bin/clang-apply-replacements",
        )

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            commands.append(command)
            if "-export-fixes" in command:
                fixes_directory = Path(command[command.index("-export-fixes") + 1])
                duplicate = self.replacement(self.root / "shared.h", 4, 0, "const ")
                self.write_fixes(
                    fixes_directory,
                    "first.yaml",
                    [self.diagnostic("first-check", [duplicate])],
                )
                self.write_fixes(
                    fixes_directory,
                    "second.yaml",
                    [self.diagnostic("second-check", [duplicate])],
                )
            elif command[0] == tools.clang_apply_replacements:
                applied_replacements.extend(self.read_replacements(Path(command[-1])))
            return subprocess.CompletedProcess(command, 0, stdout="/SDK/MacOSX.sdk\n")

        with mock.patch.object(runner, "discover_tools", return_value=tools):
            runner.run_workflow(
                mode="fix",
                workspace=self.root,
                compdb_tool=_UNIX_COMPDB_TOOL,
                platform_name="darwin",
                environ={},
                command_runner=fake_run,
            )

        analysis_command = next(
            command for command in commands if command[0] == tools.run_clang_tidy
        )
        apply_command = next(
            command for command in commands if command[0] == tools.clang_apply_replacements
        )
        apply_commands = [
            command for command in commands if command[0] == tools.clang_apply_replacements
        ]
        self.assertIn("-export-fixes", analysis_command)
        self.assertNotIn("-fix", analysis_command)
        self.assertNotIn("--fix-errors", analysis_command)
        self.assertEqual(1, len(apply_commands))
        self.assertNotIn("-ignore-insert-conflict", apply_command)
        self.assertEqual(2, len(apply_command))
        self.assertEqual(
            [self.replacement(self.root / "shared.h", 4, 0, "const ")],
            applied_replacements,
        )

    def test_normalization_deduplicates_identical_replacements_deterministically(
        self,
    ) -> None:
        fixes_directory = Path(self.temp_dir.name) / "fixes"
        fixes_directory.mkdir()
        header = self.root / "shared.h"
        duplicate = self.replacement(header, 14, 0, "const ")
        distinct = self.replacement(header, 30, 3, "value")
        first = self.write_fixes(
            fixes_directory,
            "a.yaml",
            [self.diagnostic("first-check", [duplicate, distinct])],
        )
        second = self.write_fixes(
            fixes_directory,
            "b.yaml",
            [self.diagnostic("second-check", [duplicate])],
        )

        runner.normalize_replacements(fixes_directory)
        normalized_once = (first.read_bytes(), second.read_bytes())
        runner.normalize_replacements(fixes_directory)

        self.assertEqual(normalized_once, (first.read_bytes(), second.read_bytes()))
        self.assertEqual([duplicate, distinct], self.read_replacements(fixes_directory))
        self.assertEqual(
            ["first-check", "second-check"],
            [
                diagnostic["DiagnosticName"]
                for fixes in sorted(fixes_directory.glob("*.yaml"))
                for diagnostic in yaml.safe_load(fixes.read_text())["Diagnostics"]
            ],
        )

    def test_normalization_omits_entire_conflicting_same_range_group(self) -> None:
        fixes_directory = Path(self.temp_dir.name) / "fixes"
        fixes_directory.mkdir()
        header = self.root / "shared.h"
        first_text = self.replacement(header, 8, 0, "first")
        second_text = self.replacement(header, 8, 0, "second")
        preserved = self.replacement(header, 25, 0, "safe")
        self.write_fixes(
            fixes_directory,
            "a.yaml",
            [self.diagnostic("first-check", [first_text, preserved])],
        )
        self.write_fixes(
            fixes_directory,
            "b.yaml",
            [self.diagnostic("second-check", [first_text, second_text])],
        )
        output = StringIO()

        with redirect_stdout(output):
            runner.normalize_replacements(fixes_directory)

        self.assertEqual([preserved], self.read_replacements(fixes_directory))
        advisory = output.getvalue()
        self.assertIn(str(header), advisory)
        self.assertIn("offset 8", advisory)
        self.assertIn("length 0", advisory)
        self.assertIn("3 conflicting replacements", advisory)

    def test_normalization_omits_partially_overlapping_replacements(self) -> None:
        fixes_directory = Path(self.temp_dir.name) / "fixes"
        fixes_directory.mkdir()
        header = self.root / "shared.h"
        first = self.replacement(header, 0, 5, "AAAAA")  # [0, 5)
        second = self.replacement(header, 3, 4, "BBBB")  # [3, 7) overlaps first
        preserved = self.replacement(header, 20, 2, "safe")  # [20, 22) disjoint
        self.write_fixes(
            fixes_directory,
            "a.yaml",
            [self.diagnostic("first-check", [first, preserved])],
        )
        self.write_fixes(
            fixes_directory,
            "b.yaml",
            [self.diagnostic("second-check", [second])],
        )
        output = StringIO()

        with redirect_stdout(output):
            runner.normalize_replacements(fixes_directory)

        self.assertEqual([preserved], self.read_replacements(fixes_directory))
        advisory = output.getvalue()
        self.assertIn(str(header), advisory)
        self.assertIn("2 overlapping replacements", advisory)

    def test_normalization_keeps_adjacent_replacements(self) -> None:
        fixes_directory = Path(self.temp_dir.name) / "fixes"
        fixes_directory.mkdir()
        header = self.root / "shared.h"
        left = self.replacement(header, 0, 5, "AAAAA")  # [0, 5)
        right = self.replacement(header, 5, 3, "BBB")  # [5, 8) touches but no overlap
        self.write_fixes(
            fixes_directory,
            "a.yaml",
            [self.diagnostic("adjacent-check", [left, right])],
        )
        output = StringIO()

        with redirect_stdout(output):
            runner.normalize_replacements(fixes_directory)

        self.assertEqual([left, right], self.read_replacements(fixes_directory))
        self.assertNotIn("overlapping", output.getvalue())

    @unittest.skipIf(os.name == "nt", "clang-tidy fix mode is unsupported on Windows")
    def test_normalization_deduplicates_symlink_aliases(self) -> None:
        fixes_directory = Path(self.temp_dir.name) / "fixes"
        fixes_directory.mkdir()
        header = self.root / "shared.h"
        header.write_text("int value;\n")
        alias = self.root / "shared-alias.h"
        alias.symlink_to(header)
        original = self.replacement(header, 0, 0, "const ")
        duplicate = self.replacement(alias, 0, 0, "const ")
        self.write_fixes(
            fixes_directory,
            "a.yaml",
            [self.diagnostic("first-check", [original])],
        )
        self.write_fixes(
            fixes_directory,
            "b.yaml",
            [self.diagnostic("second-check", [duplicate])],
        )

        runner.normalize_replacements(fixes_directory)

        self.assertEqual([original], self.read_replacements(fixes_directory))

    @unittest.skipIf(os.name == "nt", "clang-tidy fix mode is unsupported on Windows")
    def test_normalization_omits_conflicting_symlink_aliases(self) -> None:
        fixes_directory = Path(self.temp_dir.name) / "fixes"
        fixes_directory.mkdir()
        header = self.root / "shared.h"
        header.write_text("int value;\n")
        alias = self.root / "shared-alias.h"
        alias.symlink_to(header)
        original = self.replacement(header, 0, 0, "const ")
        conflict = self.replacement(alias, 0, 0, "volatile ")
        self.write_fixes(
            fixes_directory,
            "a.yaml",
            [self.diagnostic("first-check", [original])],
        )
        self.write_fixes(
            fixes_directory,
            "b.yaml",
            [self.diagnostic("second-check", [conflict])],
        )

        runner.normalize_replacements(fixes_directory)

        self.assertEqual([], self.read_replacements(fixes_directory))

    def test_normalization_rejects_malformed_yaml(self) -> None:
        fixes_directory = Path(self.temp_dir.name) / "fixes"
        fixes_directory.mkdir()
        malformed = fixes_directory / "broken.yaml"
        malformed.write_text("Diagnostics: [")

        with self.assertRaisesRegex(
            runner.WorkflowError,
            r"exported fixes.*broken.yaml.*malformed",
        ):
            runner.normalize_replacements(fixes_directory)

    def test_normalization_rejects_malformed_replacement(self) -> None:
        fixes_directory = Path(self.temp_dir.name) / "fixes"
        fixes_directory.mkdir()
        malformed = self.replacement(self.root / _MAIN_CPP, 4, 0, "value")
        del malformed["Offset"]
        self.write_fixes(
            fixes_directory,
            "broken.yaml",
            [self.diagnostic("broken-check", [malformed])],
        )

        with self.assertRaisesRegex(
            runner.WorkflowError,
            r"exported fixes.*broken.yaml.*replacement.*Offset",
        ):
            runner.normalize_replacements(fixes_directory)

    def test_prebuild_runs_before_refresh_when_requested(self) -> None:
        commands, fake_run = self.prebuild_fixture()
        with mock.patch.object(runner, "discover_tools", return_value=_UNIX_TOOLS):
            runner.run_workflow(
                mode="report",
                workspace=self.root,
                compdb_tool=_UNIX_COMPDB_TOOL,
                platform_name="darwin",
                environ={},
                command_runner=fake_run,
                build_args=[_CONFIG_RELEASE, _FASTECU_TARGET],
            )

        self.assertEqual(
            ["bazel", "build", "--keep_going", _CONFIG_RELEASE, _FASTECU_TARGET],
            commands[0],
        )
        self.assertEqual(_UNIX_COMPDB_TOOL, commands[1][0])

    def test_compdb_args_are_forwarded_to_refresh_tool(self) -> None:
        commands, fake_run = self.prebuild_fixture()
        with mock.patch.object(runner, "discover_tools", return_value=_UNIX_TOOLS):
            runner.run_workflow(
                mode="report",
                workspace=self.root,
                compdb_tool=_UNIX_COMPDB_TOOL,
                platform_name="darwin",
                environ={},
                command_runner=fake_run,
                build_args=[_CONFIG_RELEASE, _FASTECU_TARGET],
                compdb_args=[_CONFIG_RELEASE],
            )

        self.assertEqual(
            [_UNIX_COMPDB_TOOL, _CONFIG_RELEASE],
            commands[1],
        )

    def test_prebuild_is_skipped_without_build_args(self) -> None:
        commands, fake_run = self.prebuild_fixture()
        with mock.patch.object(runner, "discover_tools", return_value=_UNIX_TOOLS):
            runner.run_workflow(
                mode="report",
                workspace=self.root,
                compdb_tool=_UNIX_COMPDB_TOOL,
                platform_name="darwin",
                environ={},
                command_runner=fake_run,
            )

        self.assertEqual(_UNIX_COMPDB_TOOL, commands[0][0])
        self.assertNotIn("bazel", [command[0] for command in commands])

    def test_prebuild_failure_does_not_block_analysis(self) -> None:
        commands, fake_run = self.prebuild_fixture(bazel_build_returncode=3)
        with mock.patch.object(runner, "discover_tools", return_value=_UNIX_TOOLS):
            result = runner.run_workflow(
                mode="report",
                workspace=self.root,
                compdb_tool=_UNIX_COMPDB_TOOL,
                platform_name="darwin",
                environ={},
                command_runner=fake_run,
                build_args=[_FASTECU_TARGET],
            )

        self.assertEqual(0, result)
        self.assertEqual(_UNIX_COMPDB_TOOL, commands[1][0])

    def test_refresh_failure_stops_before_analysis(self) -> None:
        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            return subprocess.CompletedProcess(command, 9)

        with self.assertRaisesRegex(runner.WorkflowError, "refresher failed.*9"):
            runner.run_workflow(
                mode="report",
                workspace=self.root,
                compdb_tool=_UNIX_COMPDB_TOOL,
                platform_name="darwin",
                environ={},
                command_runner=fake_run,
            )

    def test_analysis_failure_is_propagated(self) -> None:
        source = self.root / _MAIN_CPP
        source.write_text("int main() { return 0; }\n")
        self.write_database([source])
        return_codes = iter((0, 7))

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            if command == ["xcrun", "--show-sdk-path"]:
                return subprocess.CompletedProcess(command, 0, stdout="/SDK/MacOSX.sdk\n")
            return subprocess.CompletedProcess(command, next(return_codes))

        tools = runner.Tools(
            clang_tidy="/llvm/bin/clang-tidy",
            run_clang_tidy="/llvm/bin/run-clang-tidy",
        )
        with (
            mock.patch.object(runner, "discover_tools", return_value=tools),
            self.assertRaisesRegex(runner.WorkflowError, "failed.*7"),
        ):
            runner.run_workflow(
                mode="report",
                workspace=self.root,
                compdb_tool=_UNIX_COMPDB_TOOL,
                platform_name="darwin",
                environ={},
                command_runner=fake_run,
            )

    def test_replacement_failure_is_propagated(self) -> None:
        source = self.root / _MAIN_CPP
        source.write_text("int main() { return 0; }\n")
        self.write_database([source])
        return_codes = iter((0, 0, 8))

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            if command == ["xcrun", "--show-sdk-path"]:
                return subprocess.CompletedProcess(command, 0, stdout="/SDK/MacOSX.sdk\n")
            return subprocess.CompletedProcess(command, next(return_codes))

        tools = runner.Tools(
            clang_tidy="/llvm/bin/clang-tidy",
            run_clang_tidy="/llvm/bin/run-clang-tidy",
            clang_apply_replacements="/llvm/bin/clang-apply-replacements",
        )
        with (
            mock.patch.object(runner, "discover_tools", return_value=tools),
            self.assertRaisesRegex(runner.WorkflowError, "replacement application failed.*8"),
        ):
            runner.run_workflow(
                mode="fix",
                workspace=self.root,
                compdb_tool=_UNIX_COMPDB_TOOL,
                platform_name="darwin",
                environ={},
                command_runner=fake_run,
            )

    def test_fix_applies_exported_replacements_before_propagating_analysis_failure(self) -> None:
        source = self.root / _MAIN_CPP
        source.write_text("int main() { return 0; }\n")
        self.write_database([source])
        commands: list[list[str]] = []
        tools = runner.Tools(
            clang_tidy="/llvm/bin/clang-tidy",
            run_clang_tidy="/llvm/bin/run-clang-tidy",
            clang_apply_replacements="/llvm/bin/clang-apply-replacements",
        )

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            commands.append(command)
            if command == ["xcrun", "--show-sdk-path"]:
                return subprocess.CompletedProcess(command, 0, stdout="/SDK/MacOSX.sdk\n")
            if "-export-fixes" in command:
                fixes_directory = Path(command[command.index("-export-fixes") + 1])
                replacement = self.replacement(source, 0, 0, "// fixed\n")
                self.write_fixes(
                    fixes_directory,
                    "fix.yaml",
                    [self.diagnostic("readability-fix", [replacement])],
                )
                return subprocess.CompletedProcess(command, 1)
            return subprocess.CompletedProcess(command, 0)

        with (
            mock.patch.object(runner, "discover_tools", return_value=tools),
            self.assertRaisesRegex(runner.WorkflowError, "failed.*1.*fixes were applied"),
        ):
            runner.run_workflow(
                mode="fix",
                workspace=self.root,
                compdb_tool=_UNIX_COMPDB_TOOL,
                platform_name="darwin",
                environ={},
                command_runner=fake_run,
                build_args=[_CONFIG_RELEASE, "//..."],
            )

        self.assertIn(
            [tools.clang_apply_replacements, mock.ANY],
            commands,
        )
        apply_index = next(
            index
            for index, command in enumerate(commands)
            if command[0] == tools.clang_apply_replacements
        )
        self.assertEqual(
            ["bazel", "build", _CONFIG_RELEASE, "//..."],
            commands[apply_index + 1],
        )

    def test_fix_propagates_post_replacement_build_failure(self) -> None:
        source = self.root / _MAIN_CPP
        source.write_text("int main() { return 0; }\n")
        self.write_database([source])
        build_count = 0
        tools = runner.Tools(
            clang_tidy="/llvm/bin/clang-tidy",
            run_clang_tidy="/llvm/bin/run-clang-tidy",
            clang_apply_replacements="/llvm/bin/clang-apply-replacements",
        )

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            nonlocal build_count
            if command == ["xcrun", "--show-sdk-path"]:
                return subprocess.CompletedProcess(command, 0, stdout="/SDK/MacOSX.sdk\n")
            if command[:2] == ["bazel", "build"]:
                build_count += 1
                return subprocess.CompletedProcess(command, 9 if build_count == 2 else 0)
            return subprocess.CompletedProcess(command, 0)

        with (
            mock.patch.object(runner, "discover_tools", return_value=tools),
            self.assertRaisesRegex(runner.WorkflowError, "post-fix Bazel build failed.*9"),
        ):
            runner.run_workflow(
                mode="fix",
                workspace=self.root,
                compdb_tool=_UNIX_COMPDB_TOOL,
                platform_name="darwin",
                environ={},
                command_runner=fake_run,
                build_args=[_CONFIG_RELEASE, "//..."],
            )

    def test_prebuild_success_output_is_suppressed(self) -> None:
        commands, fake_run = self.prebuild_fixture()

        def noisy_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            result = fake_run(command, **kwargs)
            if command[:2] == ["bazel", "build"]:
                return subprocess.CompletedProcess(
                    command, result.returncode, stdout="noisy build log\n"
                )
            return result

        with mock.patch.object(runner, "discover_tools", return_value=_UNIX_TOOLS):
            output = StringIO()
            with redirect_stdout(output):
                runner.run_workflow(
                    mode="report",
                    workspace=self.root,
                    compdb_tool=_UNIX_COMPDB_TOOL,
                    platform_name="darwin",
                    environ={},
                    command_runner=noisy_run,
                    build_args=[_CONFIG_RELEASE, _FASTECU_TARGET],
                )

        self.assertNotIn("noisy build log", output.getvalue())

    def test_prebuild_failure_output_is_surfaced(self) -> None:
        commands, fake_run = self.prebuild_fixture(bazel_build_returncode=3)

        def noisy_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            result = fake_run(command, **kwargs)
            if command[:2] == ["bazel", "build"]:
                return subprocess.CompletedProcess(
                    command, result.returncode, stdout="prebuild trouble\n"
                )
            return result

        with mock.patch.object(runner, "discover_tools", return_value=_UNIX_TOOLS):
            output = StringIO()
            with redirect_stdout(output):
                runner.run_workflow(
                    mode="report",
                    workspace=self.root,
                    compdb_tool=_UNIX_COMPDB_TOOL,
                    platform_name="darwin",
                    environ={},
                    command_runner=noisy_run,
                    build_args=[_FASTECU_TARGET],
                )

        self.assertIn("prebuild trouble", output.getvalue())

    def test_post_fix_build_failure_output_is_surfaced(self) -> None:
        source = self.root / _MAIN_CPP
        source.write_text("int main() { return 0; }\n")
        self.write_database([source])
        build_count = 0
        tools = runner.Tools(
            clang_tidy="/llvm/bin/clang-tidy",
            run_clang_tidy="/llvm/bin/run-clang-tidy",
            clang_apply_replacements="/llvm/bin/clang-apply-replacements",
        )

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            nonlocal build_count
            if command == ["xcrun", "--show-sdk-path"]:
                return subprocess.CompletedProcess(command, 0, stdout="/SDK/MacOSX.sdk\n")
            if command[:2] == ["bazel", "build"]:
                build_count += 1
                if build_count == 2:
                    return subprocess.CompletedProcess(command, 9, stdout="post-fix build broke\n")
                return subprocess.CompletedProcess(command, 0)
            return subprocess.CompletedProcess(command, 0)

        with mock.patch.object(runner, "discover_tools", return_value=tools):
            output = StringIO()
            with (
                redirect_stdout(output),
                self.assertRaisesRegex(runner.WorkflowError, "post-fix Bazel build failed.*9"),
            ):
                runner.run_workflow(
                    mode="fix",
                    workspace=self.root,
                    compdb_tool=_UNIX_COMPDB_TOOL,
                    platform_name="darwin",
                    environ={},
                    command_runner=fake_run,
                    build_args=[_CONFIG_RELEASE, "//..."],
                )

        self.assertIn("post-fix build broke", output.getvalue())


if __name__ == "__main__":
    unittest.main()
