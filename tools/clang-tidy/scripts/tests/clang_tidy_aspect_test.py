#!/usr/bin/env python3

import json
import tempfile
import unittest
from io import StringIO
from pathlib import Path, PurePath
from subprocess import CompletedProcess
from unittest import mock

import clang_tidy_aspect as aspect
import yaml

_SANDBOX = "/cache/sandbox/darwin-sandbox/87/execroot/_main"
_FINDING = (
    f"{_SANDBOX}/src/a.cpp:3:9: error: statement should be inside braces "
    "[readability-braces-around-statements,-warnings-as-errors]\n"
    "    3 |     if (x)\n"
    "      |          ^\n"
)
_FIXES = (
    "---\n"
    f"MainSourceFile:  '{_SANDBOX}/src/a.cpp'\n"
    "Diagnostics:\n"
    "  - DiagnosticName:  readability-braces-around-statements\n"
    "    DiagnosticMessage:\n"
    f"      FilePath:        '{_SANDBOX}/src/a.cpp'\n"
    "      Replacements:\n"
    f"        - FilePath:        '{_SANDBOX}/src/a.cpp'\n"
    "          Offset:          40\n"
    "          Length:          0\n"
    "          ReplacementText: ' {'\n"
    "...\n"
)
_EMPTY_FIXES = '---\nMainSourceFile: ""\nDiagnostics: []\n...\n'


class LocalizeTest(unittest.TestCase):
    def test_sandbox_prefix_with_spaces_is_fully_replaced(self) -> None:
        text = f"{_SANDBOX.replace('/cache', '/cache with spaces')}/src/a.cpp:3:9: error: x"
        self.assertEqual("/ws/src/a.cpp:3:9: error: x", aspect.localize(text, Path("/ws")))

    def test_a_sandbox_path_becomes_a_workspace_path(self) -> None:
        self.assertEqual(
            "/ws/src/a.cpp:3:9: error: x",
            aspect.localize(f"{_SANDBOX}/src/a.cpp:3:9: error: x", Path("/ws")),
        )

    def test_an_unsandboxed_execroot_path_is_localized_too(self) -> None:
        self.assertEqual(
            "/ws/src/a.cpp",
            aspect.localize("/home/u/.cache/bazel/x/execroot/_main/src/a.cpp", Path("/ws")),
        )

    def test_every_occurrence_on_a_line_is_localized(self) -> None:
        line = f"FilePath: '{_SANDBOX}/a.cpp' dir '{_SANDBOX}'"
        self.assertNotIn("execroot", aspect.localize(line, Path("/ws")))

    def test_paths_outside_an_execroot_are_untouched(self) -> None:
        self.assertEqual(
            "/usr/include/c++/v1/vector:5:1: note: x",
            aspect.localize("/usr/include/c++/v1/vector:5:1: note: x", Path("/ws")),
        )


def bep(groups: dict[str, list[str]], directory: Path) -> Path:
    """A build event file whose output groups hold the given files, via nested file sets."""
    target = {"targetCompleted": {"label": "//src:a", "aspect": "x"}}
    events = [{"id": {"started": {}}, "children": [target, {"buildFinished": {}}]}]
    group_sets = []
    for index, (name, files) in enumerate(groups.items()):
        leaf, parent = f"leaf{index}", f"parent{index}"
        events.append(
            {
                "id": {"namedSet": {"id": leaf}},
                "namedSetOfFiles": {
                    "files": [{"name": f, "uri": (directory / f).as_uri()} for f in files]
                },
            }
        )
        events.append(
            {
                "id": {"namedSet": {"id": parent}},
                "namedSetOfFiles": {"fileSets": [{"id": leaf}]},
            }
        )
        group_sets.append({"name": name, "fileSets": [{"id": parent}]})
    events.append(
        {
            "id": target,
            "completed": {"success": True, "outputGroup": group_sets},
        }
    )
    events.append(
        {
            "id": {"buildFinished": {}},
            "finished": {"exitCode": {"name": "SUCCESS"}},
            "lastMessage": True,
        }
    )
    path = directory / "bep.json"
    path.write_text("".join(json.dumps(event) + "\n" for event in events))
    return path


class OutputFilesTest(unittest.TestCase):
    def test_incomplete_event_stream_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            for missing in ("finish", "target", "last_message", "failed_finish"):
                with self.subTest(missing=missing):
                    path = bep({}, directory)
                    events = [json.loads(line) for line in path.read_text().splitlines()]
                    if missing == "finish":
                        events.pop()
                    elif missing == "target":
                        events[0]["children"].append(
                            {"targetCompleted": {"label": "//src:missing", "aspect": "x"}}
                        )
                    elif missing == "last_message":
                        del events[-1]["lastMessage"]
                    else:
                        events[-1]["finished"]["exitCode"]["code"] = 1
                    path.write_text("".join(json.dumps(event) + "\n" for event in events))
                    with self.assertRaisesRegex(aspect.AspectError, "incomplete|unsuccessful"):
                        aspect.output_files(path)

    def test_windows_file_uri(self) -> None:
        with mock.patch.object(aspect.os, "name", "nt"):
            self.assertEqual(
                "C:/cache/a file.out",
                aspect._file_path("file:///C:/cache/a%20file.out").as_posix(),
            )

    def test_encoded_space_in_output_uri(self) -> None:
        self.assertEqual(Path("/cache/a file.out"), aspect._file_path("file:///cache/a%20file.out"))

    def test_source_identity_uses_bep_name(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            source = "a_rules_lint/src/a.cpp.AspectRulesLintClangTidy.out"
            path = bep({"rules_lint_human": [source, source + ".exit_code"]}, directory)
            events = [json.loads(line) for line in path.read_text().splitlines()]
            for event in events:
                for index, file in enumerate(event.get("namedSetOfFiles", {}).get("files", [])):
                    file["uri"] = (directory / f"digest-{index}").as_uri()
            path.write_text("".join(json.dumps(event) + "\n" for event in events))
            files = aspect.output_files(path)
            self.assertEqual("src/a.cpp", files.sources[directory / "digest-0"])
            self.assertEqual(1, aspect.source_count(files))

    def test_missing_named_set_fails(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            path = bep({"rules_lint_human": ["a.out"]}, Path(name))
            events = [json.loads(line) for line in path.read_text().splitlines()]
            path.write_text("".join(json.dumps(event) + "\n" for event in events[:1] + events[2:]))
            with self.assertRaisesRegex(aspect.AspectError, "missing.*file set"):
                aspect.output_files(path)

    def test_missing_exit_code_fails(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            path = bep({"rules_lint_human": ["a.AspectRulesLintClangTidy.out"]}, Path(name))
            with self.assertRaisesRegex(aspect.AspectError, "exit code"):
                aspect.output_files(path)

    def test_scope_filters_sources_not_uri_paths(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            human = [directory / "digest-a", directory / "digest-b", directory / "digest-c"]
            codes = [directory / "code-a", directory / "code-b", directory / "code-c"]
            fixes = [directory / "fix-a", directory / "fix-b", directory / "fix-c"]
            sources = {
                output: source
                for index, source in enumerate(
                    ("src/win/a.cpp", "src/portable/b.cpp", "src/win/a.cpp")
                )
                for output in (human[index], codes[index], fixes[index])
            }
            selected = aspect.select_outputs(
                aspect.OutputFiles(human, codes, fixes, sources), [PurePath("src/win")]
            )
            self.assertEqual([human[0], human[2]], selected.human)
            self.assertEqual([codes[0], codes[2]], selected.exit_codes)
            self.assertEqual([fixes[0], fixes[2]], selected.fixes)

    def test_files_are_classified_by_suffix_through_nested_file_sets(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            path = bep(
                {
                    "rules_lint_human": [
                        "a.AspectRulesLintClangTidy.out",
                        "a.AspectRulesLintClangTidy.out.exit_code",
                    ],
                    "rules_lint_fixes": ["a.AspectRulesLintClangTidy.fixes.yaml"],
                },
                directory,
            )
            files = aspect.output_files(path)
        self.assertEqual(["a.AspectRulesLintClangTidy.out"], [p.name for p in files.human])
        self.assertEqual(
            ["a.AspectRulesLintClangTidy.out.exit_code"], [p.name for p in files.exit_codes]
        )
        self.assertEqual(["a.AspectRulesLintClangTidy.fixes.yaml"], [p.name for p in files.fixes])

    def test_other_output_groups_are_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            path = bep({"default": ["a.out"]}, Path(name))
            files = aspect.output_files(path)
        self.assertEqual(([], [], []), (files.human, files.exit_codes, files.fixes))

    def test_classification_follows_the_output_name_not_the_uri(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            path = bep(
                {
                    "rules_lint_human": [
                        "a.AspectRulesLintClangTidy.out",
                        "a.AspectRulesLintClangTidy.out.exit_code",
                    ]
                },
                directory,
            )
            events = [json.loads(line) for line in path.read_text().splitlines()]
            for event in events:
                for file in event.get("namedSetOfFiles", {}).get("files", []):
                    file["uri"] = (
                        directory
                        / ("code" if file["name"].endswith("exit_code") else "digest-0123abcd")
                    ).as_uri()
            path.write_text("".join(json.dumps(event) + "\n" for event in events))
            files = aspect.output_files(path)
        self.assertEqual(["digest-0123abcd"], [p.name for p in files.human])

    def test_an_output_left_in_the_remote_cache_is_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            path = bep({"rules_lint_human": ["a.AspectRulesLintClangTidy.out"]}, Path(name))
            events = [json.loads(line) for line in path.read_text().splitlines()]
            for event in events:
                for file in event.get("namedSetOfFiles", {}).get("files", []):
                    file["uri"] = "bytestream://remote.example/blobs/abc/12"
            path.write_text("".join(json.dumps(event) + "\n" for event in events))
            with self.assertRaisesRegex(aspect.AspectError, "not downloaded"):
                aspect.output_files(path)


class MainTest(unittest.TestCase):
    def test_profile_does_not_change_build_command(self) -> None:
        _, ordinary, _ = self.run_main({})
        _, profiled, _ = self.run_main({}, ["--profile"])
        self.assertEqual(
            [arg for arg in ordinary if not arg.startswith("--build_event_json_file=")],
            [arg for arg in profiled if not arg.startswith("--build_event_json_file=")],
        )

    def test_preflight_rejects_invalid_analysis_host(self) -> None:
        err = StringIO()
        with (
            mock.patch.dict("os.environ", {"FASTECU_LLVM_ROOT": "/missing-llvm"}),
            mock.patch("sys.stderr", err),
        ):
            code = aspect.main(["//src/..."])
        self.assertEqual(2, code)
        self.assertIn("LLVM", err.getvalue())

    def test_release_configuration_is_default(self) -> None:
        _, commands, _ = self.run_main({})
        self.assertIn("--config=release", commands)

    def test_release_configuration_not_repeated(self) -> None:
        _, commands, _ = self.run_main({}, ["--bazel-arg=--config=release"])
        self.assertEqual(1, commands.count("--config=release"))

    def test_windows_scope_filters_dependency_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            (root / "src/win").mkdir(parents=True)
            (root / ".clang-tidy-scope.toml").write_text('[os.windows]\nprefixes = ["src/win"]\n')
            groups = {"rules_lint_human": []}
            for source, diagnostic in (("src/win/a.cpp", ""), ("src/portable/b.cpp", _FINDING)):
                out = f"a_rules_lint/{source}.AspectRulesLintClangTidy.out"
                path = root / out
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(diagnostic)
                Path(str(path) + ".exit_code").write_text("1" if diagnostic else "0")
                groups["rules_lint_human"].extend((out, out + ".exit_code"))

            def build(command: list[str], **kwargs: object) -> CompletedProcess[str]:
                flag = next(c for c in command if c.startswith("--build_event_json_file="))
                made = bep(groups, root)
                Path(flag.split("=", 1)[1]).write_text(made.read_text())
                return CompletedProcess(command, 0, "", "")

            with (
                mock.patch.dict("os.environ", {"BUILD_WORKSPACE_DIRECTORY": name}),
                mock.patch.object(aspect.analysis_toolchain, "runfiles_analysis_host"),
                mock.patch("sys.stdout", StringIO()) as output,
            ):
                code = aspect.main(["--scope-os", "windows"], run=build)
            self.assertEqual(0, code)
            self.assertNotIn("statement should", output.getvalue())
            self.assertIn("1 files", output.getvalue())

    def run_main(
        self, groups: dict[str, list[str]], extra: list[str] | None = None
    ) -> tuple[int, list[str], str]:
        commands: list[str] = []
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)

        def fake_run(command: list[str], **kwargs: object) -> CompletedProcess[str]:
            commands.extend(command)
            flag = next(c for c in command if c.startswith("--build_event_json_file="))
            made = bep(groups, Path(directory.name))
            Path(flag.split("=", 1)[1]).write_text(made.read_text())
            return CompletedProcess(command, 0, "", "")

        err = StringIO()
        with (
            mock.patch("sys.stderr", err),
            mock.patch.dict("os.environ", {"BUILD_WORKSPACE_DIRECTORY": "/ws"}),
            mock.patch("clang_tidy_toolchain.runfiles_analysis_host"),
        ):
            code = aspect.main(["//src/...", *(extra or [])], run=fake_run)
        return code, commands, err.getvalue()

    def test_a_build_that_linted_nothing_is_an_error_not_a_pass(self) -> None:
        code, _, err = self.run_main({})
        self.assertEqual(2, code)
        self.assertIn("no lint output", err)

    def test_lint_outputs_are_forced_to_download_from_a_remote_cache(self) -> None:
        _, commands, _ = self.run_main({})
        self.assertTrue(any(c.startswith("--remote_download_regex=") for c in commands))


class ReportTest(unittest.TestCase):
    def test_malformed_exit_code_fails(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            out, code = directory / "a.out", directory / "a.out.exit_code"
            out.write_text("")
            for status in ("", "not a status", "0\n1"):
                with self.subTest(status=status):
                    code.write_text(status)
                    with self.assertRaisesRegex(aspect.AspectError, "exit code"):
                        self.report(aspect.OutputFiles([out], [code], []))

    def test_failed_build_with_findings_remains_failure(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            output_name = "a_rules_lint/src/a.cpp.AspectRulesLintClangTidy.out"
            out = directory / output_name
            out.parent.mkdir(parents=True)
            out.write_text(_FINDING)
            Path(str(out) + ".exit_code").write_text("1")

            def failed_build(command: list[str], **kwargs: object) -> CompletedProcess[str]:
                flag = next(c for c in command if c.startswith("--build_event_json_file="))
                made = bep(
                    {"rules_lint_human": [output_name, output_name + ".exit_code"]}, directory
                )
                Path(flag.split("=", 1)[1]).write_text(made.read_text())
                return CompletedProcess(command, 1, "", "")

            args = type(
                "Args", (), {"targets": ["//src/..."], "bazel_arg": [], "annotate": False}
            )()
            with self.assertRaisesRegex(aspect.AspectError, "bazel build failed"):
                aspect._build_and_report(args, Path("/ws"), failed_build)

    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)

    def output(self, name: str, text: str, exit_code: str = "1") -> aspect.OutputFiles:
        out = self.directory / name
        out.write_text(text)
        code = self.directory / (name + ".exit_code")
        code.write_text(exit_code)
        return aspect.OutputFiles(human=[out], exit_codes=[code], fixes=[])

    def report(self, files: aspect.OutputFiles, *, annotate: bool = False) -> tuple[int, str]:
        buffer = StringIO()
        findings = aspect.report(
            files, workspace=Path("/ws"), write=buffer.write, color=False, annotate=annotate
        )
        return findings, buffer.getvalue()

    def test_findings_are_shown_with_workspace_paths(self) -> None:
        findings, text = self.report(self.output("a.out", _FINDING))
        self.assertEqual(1, findings)
        self.assertIn("src/a.cpp:3:9", text)
        self.assertNotIn("execroot", text)
        self.assertIn("[readability-braces-around-statements]", text)

    def test_the_same_finding_from_two_translation_units_counts_once(self) -> None:
        first = self.output("a.out", _FINDING)
        second = self.output("b.out", _FINDING)
        both = aspect.OutputFiles(first.human + second.human, first.exit_codes, [])
        self.assertEqual(1, self.report(both)[0])

    def test_annotations_are_added_on_request(self) -> None:
        _, text = self.report(self.output("a.out", _FINDING), annotate=True)
        self.assertIn("::error file=src/a.cpp,line=3,col=9", text)

    def test_only_linted_sources_are_counted_not_targets_without_sources(self) -> None:
        linted = self.directory / "a_rules_lint" / "src" / "a.cpp.AspectRulesLintClangTidy.out"
        noop = self.directory / "header_only.AspectRulesLintClangTidy.out"
        files = aspect.OutputFiles(human=[linted, noop], exit_codes=[], fixes=[])
        self.assertEqual(1, aspect.source_count(files))

    def test_a_clean_file_has_no_findings(self) -> None:
        self.assertEqual((0, ""), self.report(self.output("a.out", "", exit_code="0")))

    def test_a_nonzero_exit_without_a_parsed_finding_is_still_a_failure(self) -> None:
        with self.assertRaisesRegex(aspect.AspectError, "abnormally"):
            self.report(self.output("a.out", "clang-tidy crashed\n", exit_code="139"))


class ApplyFixesTest(unittest.TestCase):
    def test_paths_with_spaces_preserve_replacement_text(self) -> None:
        workspace = self.directory / "checkout with spaces"
        sandbox = _SANDBOX.replace("/cache", "/cache with spaces")
        literal = sandbox + "/literal"
        text = _FIXES.replace(_SANDBOX, sandbox).replace(
            "ReplacementText: ' {'", f"ReplacementText: '{literal}'"
        )
        aspect.apply_fixes(self.fixes(text), workspace=workspace, tool="x", run=self.run_tool)
        replacement = yaml.safe_load(self.applied[0])["Diagnostics"][0]["DiagnosticMessage"][
            "Replacements"
        ][0]
        self.assertEqual(str(workspace / "src/a.cpp"), replacement["FilePath"])
        self.assertEqual(literal, replacement["ReplacementText"])

    def test_replacements_outside_workspace_are_rejected(self) -> None:
        text = _FIXES.replace(_SANDBOX + "/src/a.cpp", "/external/file.cpp")
        with self.assertRaisesRegex(aspect.AspectError, "outside"):
            aspect.apply_fixes(self.fixes(text), workspace=Path("/ws"), tool="x", run=self.run_tool)
        self.assertEqual([], self.calls)

    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)
        self.calls: list[tuple[list[str], dict[str, object]]] = []

    def run_tool(self, command: list[str], **kwargs: object) -> CompletedProcess[str]:
        self.calls.append((command, kwargs))
        directory = Path(command[-1])
        self.applied = sorted(p.read_text() for p in directory.glob("*.yaml"))
        return CompletedProcess(command, 0, "", "")

    def fixes(self, *texts: str) -> list[Path]:
        paths = []
        for index, text in enumerate(texts):
            path = self.directory / f"{index}.fixes.yaml"
            path.write_text(text)
            paths.append(path)
        return paths

    def test_reports_with_replacements_are_applied_with_workspace_paths(self) -> None:
        applied = aspect.apply_fixes(
            self.fixes(_FIXES, _EMPTY_FIXES),
            workspace=Path("/ws"),
            tool="clang-apply-replacements",
            run=self.run_tool,
        )
        self.assertEqual(1, applied)
        ((command, kwargs),) = self.calls
        self.assertEqual("clang-apply-replacements", command[0])
        self.assertEqual(Path("/ws"), kwargs["cwd"])
        self.assertEqual(1, len(self.applied))
        self.assertIn("/ws/src/a.cpp", self.applied[0])
        self.assertNotIn("execroot", self.applied[0])

    def test_nothing_runs_when_no_report_has_replacements(self) -> None:
        applied = aspect.apply_fixes(
            self.fixes(_EMPTY_FIXES),
            workspace=Path("/ws"),
            tool="clang-apply-replacements",
            run=self.run_tool,
        )
        self.assertEqual((0, []), (applied, self.calls))

    def test_a_failing_tool_is_an_error(self) -> None:
        def failing(command: list[str], **kwargs: object) -> CompletedProcess[str]:
            return CompletedProcess(command, 1, "", "boom")

        with self.assertRaises(aspect.AspectError):
            aspect.apply_fixes(self.fixes(_FIXES), workspace=Path("/ws"), tool="x", run=failing)


class CommandTest(unittest.TestCase):
    def test_selected_llvm_missing_replacements_does_not_fall_back(self) -> None:
        with tempfile.TemporaryDirectory() as name, self.assertRaises(aspect.AspectError):
            aspect.find_apply_replacements(
                {"FASTECU_LLVM_ROOT": name}, which=lambda _: "/other/clang-apply-replacements"
            )

    def test_the_build_command_requests_both_output_groups_and_the_event_file(self) -> None:
        command = aspect.build_command(["//src/..."], Path("/tmp/bep.json"), ["--config=release"])
        self.assertEqual(["bazel", "build"], command[:2])
        self.assertIn("--aspects=//tools/clang-tidy/aspect:linters.bzl%clang_tidy", command)
        self.assertIn("--output_groups=rules_lint_human,rules_lint_fixes", command)
        self.assertIn(f"--build_event_json_file={Path('/tmp/bep.json')}", command)
        self.assertIn("--nobuild_event_json_file_path_conversion", command)
        self.assertIn("--keep_going", command)
        self.assertEqual(["--config=release", "//src/..."], command[-2:])

    def test_apply_replacements_is_found_next_to_the_selected_llvm(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            bin_directory = Path(name) / "bin"
            bin_directory.mkdir()
            (bin_directory / "clang-apply-replacements").write_text("")
            found = aspect.find_apply_replacements(
                {"FASTECU_LLVM_ROOT": name}, which=lambda _: None
            )
        self.assertEqual(str(bin_directory / "clang-apply-replacements"), found)

    def test_a_missing_apply_replacements_is_an_error(self) -> None:
        with self.assertRaises(aspect.AspectError):
            aspect.find_apply_replacements({}, which=lambda _: None, homebrew=())


class ProfileOutputTest(unittest.TestCase):
    def test_profile_includes_cached_outputs_once(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            first, second = root / "cached-a", root / "cached-b"
            row = "0.0100 (10%) 0.0000 (0%) 0.0100 (10%) 0.0300 (30%) check-a\n"
            first.write_text(row)
            second.write_text(row)
            summary = root / "summary.md"
            files = aspect.OutputFiles(
                human=[first, second, first],
                sources={first: "src/a.cpp", second: "src/b.cpp"},
            )
            with mock.patch("sys.stdout", StringIO()) as output:
                aspect.emit_profile(files, {"GITHUB_STEP_SUMMARY": str(summary)})
            self.assertIn("| check-a | 0.060000 |", output.getvalue())
            self.assertEqual(1, output.getvalue().count("| Check |"))
            self.assertEqual(summary.read_text().strip(), output.getvalue().strip())

    def test_missing_profile_is_explicit(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            out = Path(name) / "empty.out"
            out.write_text("")
            with mock.patch("sys.stdout", StringIO()) as output:
                aspect.emit_profile(aspect.OutputFiles(human=[out], sources={out: "a.cpp"}), {})
            self.assertIn("Missing profiles: 1", output.getvalue())


class FixWorkflowTest(unittest.TestCase):
    def workflow(
        self,
        steps: int = 1,
        *,
        residual: bool = False,
        compile_fail: bool = False,
        analysis_fail: bool = False,
        analysis_status: int = 1,
        windows: bool = False,
        incomplete_events: bool = False,
    ) -> tuple[int, str, list[int], list[list[str]], str]:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        source = root / "src/a.cpp"
        source.parent.mkdir()
        source.write_text("stage=0")
        (root / "bin").mkdir()
        (root / "bin/clang-apply-replacements").touch()
        stages: list[int] = []
        builds: list[list[str]] = []
        out_name = "target_rules_lint/src/a.cpp.AspectRulesLintClangTidy.out"
        out = root / out_name
        out.parent.mkdir(parents=True)
        fixes_name = out_name.removesuffix("out") + "fixes.yaml"

        def run(command: list[str], **kwargs: object) -> CompletedProcess[str]:
            if Path(command[0]).name == "clang-apply-replacements":
                for path in Path(command[1]).glob("*.yaml"):
                    replacement = yaml.safe_load(path.read_text())["Diagnostics"][0][
                        "DiagnosticMessage"
                    ]["Replacements"][0]
                    destination = Path(replacement["FilePath"])
                    text = destination.read_text()
                    offset, length = replacement["Offset"], replacement["Length"]
                    destination.write_text(
                        text[:offset] + replacement["ReplacementText"] + text[offset + length :]
                    )
                return CompletedProcess(command, 0)
            flags = [arg for arg in command if arg.startswith("--build_event_json_file=")]
            if not flags:
                builds.append(command)
                return CompletedProcess(command, int(compile_fail))
            stage = int(source.read_text()[-1])
            stages.append(stage)
            finding = stage < steps or residual
            out.write_text(_FINDING if finding else "")
            Path(str(out) + ".exit_code").write_text(str(analysis_status) if finding else "0")
            diagnostics = []
            if stage < steps:
                diagnostics = [
                    {
                        "DiagnosticMessage": {
                            "Replacements": [
                                {
                                    "FilePath": str(source),
                                    "Offset": 6,
                                    "Length": 1,
                                    "ReplacementText": str(stage + 1),
                                }
                            ]
                        }
                    }
                ]
            (root / fixes_name).write_text(json.dumps({"Diagnostics": diagnostics}))
            groups = {
                "rules_lint_human": [out_name, out_name + ".exit_code"],
                "rules_lint_fixes": [fixes_name],
            }
            made = bep(groups, root)
            if incomplete_events:
                events = [json.loads(line) for line in made.read_text().splitlines()]
                events[0]["children"].append({"targetCompleted": {"label": "//src:missing"}})
                made.write_text("".join(json.dumps(event) + "\n" for event in events))
            Path(flags[0].split("=", 1)[1]).write_text(made.read_text())
            return CompletedProcess(command, int(analysis_fail))

        host = aspect.analysis_toolchain.AnalysisHost(root / "bin/clang-tidy", (), root)
        err = StringIO()
        with (
            mock.patch.dict("os.environ", {"BUILD_WORKSPACE_DIRECTORY": str(root)}),
            mock.patch.object(
                aspect.analysis_toolchain, "runfiles_analysis_host", return_value=host
            ),
            mock.patch("sys.stdout", StringIO()),
            mock.patch("sys.stderr", err),
        ):
            code = aspect.main(
                ["//src:library", "--fix", "--bazel-arg=--config=custom"],
                run=run,
                platform_name="win32" if windows else "linux",
            )
        return code, source.read_text(), stages, builds, err.getvalue()

    def test_last_fix_pass_is_rechecked(self) -> None:
        code, text, stages, builds, _ = self.workflow(3)
        self.assertEqual((0, "stage=3", [0, 1, 2, 3]), (code, text, stages))
        self.assertEqual(1, len(builds))

    def test_post_fix_build_uses_selected_targets_and_configuration(self) -> None:
        code, text, _, builds, _ = self.workflow()
        self.assertEqual((0, "stage=1"), (code, text))
        self.assertEqual(
            [["bazel", "build", "--config=release", "--config=custom", "//src:library"]], builds
        )

    def test_remaining_findings_still_compile_and_fail(self) -> None:
        code, text, _, builds, _ = self.workflow(residual=True)
        self.assertEqual((1, "stage=1", 1), (code, text, len(builds)))

    def test_post_fix_compile_failure_fails(self) -> None:
        code, text, _, _, error = self.workflow(compile_fail=True)
        self.assertEqual((2, "stage=1"), (code, text))
        self.assertIn("post-fix", error)

    def test_failed_analysis_never_applies_fixes(self) -> None:
        code, text, _, builds, _ = self.workflow(analysis_fail=True)
        self.assertEqual((2, "stage=0", []), (code, text, builds))

    def test_partial_clean_analysis_is_an_error(self) -> None:
        code, text, _, builds, error = self.workflow(steps=0, incomplete_events=True)
        self.assertEqual((2, "stage=0", []), (code, text, builds))
        self.assertIn("incomplete", error)

    def test_incomplete_analysis_never_applies_fixes(self) -> None:
        code, text, _, builds, error = self.workflow(incomplete_events=True)
        self.assertEqual((2, "stage=0", []), (code, text, builds))
        self.assertIn("incomplete", error)

    def test_crashed_analysis_never_applies_fixes(self) -> None:
        code, text, _, builds, _ = self.workflow(analysis_status=139)
        self.assertEqual((2, "stage=0", []), (code, text, builds))

    def test_windows_fix_fails_before_mutation(self) -> None:
        code, text, stages, builds, _ = self.workflow(windows=True)
        self.assertEqual((2, "stage=0", [], []), (code, text, stages, builds))


if __name__ == "__main__":
    unittest.main()
