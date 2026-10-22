"""Protect the scanner from partial build events and stale prepared databases."""

import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import sonar_inputs as inputs


def build_events(path, fragment, source):
    events = [
        {"id": {"started": {}}, "children": [{"buildFinished": {}}]},
        {
            "id": {"namedSet": {"id": "inner"}},
            "namedSetOfFiles": {"files": [{"name": "probe.sonar.json", "uri": fragment.as_uri()}]},
        },
        {"id": {"namedSet": {"id": "outer"}}, "namedSetOfFiles": {"fileSets": [{"id": "inner"}]}},
        {
            "id": {"namedSet": {"id": "inputs"}},
            "namedSetOfFiles": {"files": [{"name": "probe.cpp", "uri": source.as_uri()}]},
        },
        {
            "id": {"targetCompleted": {"label": "//fixture:probe"}},
            "completed": {
                "outputGroup": [
                    {"name": "sonar_commands", "fileSets": [{"id": "outer"}]},
                    {"name": "sonar_inputs", "fileSets": [{"id": "inputs"}]},
                ]
            },
        },
        {"id": {"buildFinished": {}}, "finished": {"exitCode": {}}, "lastMessage": True},
    ]
    path.write_text("\n".join(json.dumps(event) for event in events))
    return events


class InputsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="sonar space ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "probe.cpp"
        self.source.write_text("int probe;\n")
        self.fragment = self.root / ("a" * 64)
        self.fragment.write_text(
            json.dumps(
                {
                    "version": 1,
                    "owner": "//fixture:probe",
                    "configuration": "bazel-out/test/bin",
                    "commands": [
                        {
                            "file": "probe.cpp",
                            "arguments": [
                                "clang++",
                                "-Iinclude",
                                "-c",
                                "probe.cpp",
                                "-o",
                                "probe.o",
                            ],
                            "inputs": ["probe.cpp", "include/QtCore/qobjectdefs.h"],
                            "outputs": ["probe.o"],
                        }
                    ],
                }
            )
        )
        qt_header = self.root / "include/QtCore/qobjectdefs.h"
        qt_header.parent.mkdir(parents=True)
        qt_header.write_text("#define emit\n")
        self.events = self.root / "events.json"
        build_events(self.events, self.fragment, self.source)

    def test_nested_sets_keep_cache_filenames_and_paths_with_spaces(self):
        outputs = inputs.read_outputs(self.events)
        self.assertEqual(outputs["sonar_commands"], [self.fragment])
        self.assertEqual(outputs["sonar_inputs"], [self.source])

    def test_rejects_incomplete_or_failed_event_streams(self):
        original = self.events.read_text()
        mutations = [
            original.replace('"id": "inner"', '"id": "absent"', 1),
            original.replace('"lastMessage": true', '"lastMessage": false'),
            original.replace('"exitCode": {}', '"exitCode": {"code": 1}'),
            original.replace('{"buildFinished": {}}]', '{"missingEvent": {}}]'),
        ]
        for changed in mutations:
            with self.subTest(changed=changed[-120:]):
                self.events.write_text(changed)
                with self.assertRaises(inputs.InputsError):
                    inputs.read_outputs(self.events)

    def test_remote_uris_require_materialized_outputs(self):
        self.events.write_text(
            self.events.read_text().replace(self.fragment.as_uri(), "bytestream://cache/blob")
        )
        with self.assertRaisesRegex(inputs.InputsError, "download"):
            inputs.read_outputs(self.events)

    def runner(self, fail=False):
        self.builds = []

        def run(command, **_kwargs):
            if command[:2] == ["bazel", "info"]:
                return subprocess.CompletedProcess(command, 0, str(self.root) + "\n", "")
            self.assertEqual(command[:2], ["bazel", "build"])
            self.builds.append(command)
            path = next(
                arg.split("=", 1)[1]
                for arg in command
                if arg.startswith("--build_event_json_file=")
            )
            build_events(Path(path), self.fragment, self.source)
            return subprocess.CompletedProcess(command, int(fail), "", "")

        return run

    def test_prepares_only_requested_outputs_and_publishes_validated_database(self):
        destination = self.root / "prepared"
        result = inputs.prepare(self.root, destination, run=self.runner())
        self.assertEqual(result, destination / "compile_commands.json")
        self.assertEqual(json.loads(result.read_text())[0]["file"], "probe.cpp")
        self.assertIn("--config=coverage", self.builds[0])
        for flag in (
            "--features=-compiler_param_file",
            "--features=-layering_check",
            "--host_features=-compiler_param_file",
            "--host_features=-layering_check",
        ):
            self.assertIn(flag, self.builds[0])
        self.assertIn("--output_groups=sonar_commands,sonar_inputs", self.builds[0])
        self.assertNotIn("--output_groups=+sonar_commands,sonar_inputs", self.builds[0])

    def test_failed_build_invalidates_previous_database(self):
        destination = self.root / "prepared"
        destination.mkdir()
        previous = destination / "compile_commands.json"
        previous.write_text("old result")
        with self.assertRaises(inputs.InputsError):
            inputs.prepare(self.root, destination, run=self.runner(fail=True))
        self.assertFalse(previous.exists())

    def test_apple_toolchain_is_resolved_once_per_preparation(self):
        fragment = json.loads(self.fragment.read_text())
        command = fragment["commands"][0]
        command["arguments"] += ["-isysroot", "__BAZEL_XCODE_SDKROOT__"]
        fragment["commands"].append(command | {"outputs": ["second.o"]})
        self.fragment.write_text(json.dumps(fragment))
        runner = self.runner()
        lookups = []

        def run(command, **kwargs):
            if command[0] in ("xcrun", "xcode-select"):
                lookups.append(command)
                return subprocess.CompletedProcess(command, 0, "/selected/toolchain\n", "")
            return runner(command, **kwargs)

        inputs.prepare(self.root, self.root / "prepared", run=run)
        self.assertEqual(len(lookups), 3)

    def test_missing_header_does_not_publish_a_database(self):
        (self.root / "include/QtCore/qobjectdefs.h").unlink()
        destination = self.root / "prepared"
        with self.assertRaises(ValueError):
            inputs.prepare(self.root, destination, run=self.runner())
        self.assertFalse((destination / "compile_commands.json").exists())

    def test_unsupported_host_returns_failure(self):
        with mock.patch.object(inputs.sys, "platform", "win32"):
            self.assertNotEqual(inputs.main([]), 0)

    def test_apple_placeholder_normalization_uses_selected_sdk(self):
        def run(command, **_kwargs):
            values = {
                ("xcode-select", "--print-path"): "/Xcode/Developer",
                ("xcrun", "--sdk", "macosx", "--show-sdk-path"): "/Xcode/SDKs/MacOSX.sdk",
                ("xcrun", "--find", "clang"): "/Xcode/bin/clang",
            }
            return subprocess.CompletedProcess(command, 0, values[tuple(command)] + "\n", "")

        argv = [
            "wrapped_clang",
            "-isysroot",
            "__BAZEL_XCODE_SDKROOT__",
            "-F__BAZEL_XCODE_DEVELOPER_DIR__/Frameworks",
        ]
        self.assertEqual(
            inputs.apple_arguments(argv, run=run),
            [
                "/Xcode/bin/clang",
                "-isysroot",
                "/Xcode/SDKs/MacOSX.sdk",
                "-F/Xcode/Developer/Frameworks",
            ],
        )


if __name__ == "__main__":
    unittest.main()
