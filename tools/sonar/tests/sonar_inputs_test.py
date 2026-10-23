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
        {"id": {"workspace": {}}, "workspaceInfo": {"localExecRoot": str(source.parent)}},
        {
            "id": {"namedSet": {"id": "inner"}},
            "namedSetOfFiles": {"files": [{"name": "probe.sonar.json", "uri": fragment.as_uri()}]},
        },
        {"id": {"namedSet": {"id": "outer"}}, "namedSetOfFiles": {"fileSets": [{"id": "inner"}]}},
        {
            "id": {"namedSet": {"id": "inputs"}},
            "namedSetOfFiles": {
                "files": [
                    {"name": "probe.cpp", "uri": source.as_uri()},
                    {"name": "header.h", "uri": (source.parent / "header.h").as_uri()},
                    {
                        "name": "qobjectdefs.h",
                        "uri": (source.parent / "include/QtCore/qobjectdefs.h").as_uri(),
                    },
                ]
            },
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
        (self.root / "header.h").write_text("#pragma once\n")
        self.fragment = self.root / ("a" * 64)
        self.fragment.write_text(
            json.dumps(
                {
                    "version": 2,
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
        self.assertEqual(outputs.execroot, self.root)
        self.assertEqual(outputs.groups["sonar_commands"], [self.fragment])
        self.assertEqual(
            set(outputs.groups["sonar_inputs"]),
            {self.source, self.root / "header.h", self.root / "include/QtCore/qobjectdefs.h"},
        )

    def test_rejects_missing_execution_root_and_empty_input_group(self):
        original = build_events(self.events, self.fragment, self.source)
        mutations = [
            [event for event in original if "workspaceInfo" not in event],
            [
                event | {"namedSetOfFiles": {"files": []}}
                if event["id"] == {"namedSet": {"id": "inputs"}}
                else event
                for event in original
            ],
        ]
        for events in mutations:
            self.events.write_text("\n".join(json.dumps(event) for event in events))
            with self.assertRaises(inputs.InputsError):
                inputs.read_outputs(self.events)

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
        self.assertIn("--config=sonar_inputs", self.builds[0])
        self.assertIn("--output_groups=sonar_commands,sonar_inputs", self.builds[0])
        self.assertNotIn("--output_groups=+sonar_commands,sonar_inputs", self.builds[0])

    def test_database_is_rooted_in_the_checkout_and_links_the_execution_root(self):
        generated = self.root / "bazel-out/bin"
        generated.mkdir(parents=True)
        (generated / "include").symlink_to(self.root / "include")
        fragment = json.loads(self.fragment.read_text())
        fragment["commands"][0]["arguments"][1] = "-Ibazel-out/bin/include"
        self.fragment.write_text(json.dumps(fragment))
        with tempfile.TemporaryDirectory(prefix="sonar checkout ") as checkout:
            workspace = Path(checkout).resolve()
            (workspace / "probe.cpp").write_text("int probe;\n")
            result = inputs.prepare(workspace, workspace / "prepared", run=self.runner())
            entry = json.loads(result.read_text())[0]
            self.assertEqual(entry["directory"], str(workspace))
            self.assertEqual((workspace / "bazel-out").resolve(), self.root / "bazel-out")

    def test_checkout_links_replace_stale_links_and_reject_real_directories(self):
        (self.root / "bazel-out/bin").mkdir(parents=True)
        with tempfile.TemporaryDirectory(prefix="sonar checkout ") as checkout:
            workspace = Path(checkout).resolve()
            (workspace / "bazel-out").symlink_to(workspace / "absent")
            inputs._link_execroot(workspace, self.root)
            self.assertEqual((workspace / "bazel-out").resolve(), self.root / "bazel-out")
            (workspace / "bazel-out").unlink()
            (workspace / "bazel-out").mkdir()
            with self.assertRaisesRegex(inputs.InputsError, "not a symlink"):
                inputs._link_execroot(workspace, self.root)

    def test_missing_execroot_output_tree_or_checkout_source_prevents_publication(self):
        with tempfile.TemporaryDirectory(prefix="sonar checkout ") as checkout:
            workspace = Path(checkout).resolve()
            with self.assertRaisesRegex(inputs.InputsError, "bazel-out"):
                inputs.prepare(workspace, workspace / "prepared", run=self.runner())
            generated = self.root / "bazel-out/bin"
            generated.mkdir(parents=True)
            (generated / "include").symlink_to(self.root / "include")
            fragment = json.loads(self.fragment.read_text())
            fragment["commands"][0]["arguments"][1] = "-Ibazel-out/bin/include"
            self.fragment.write_text(json.dumps(fragment))
            with self.assertRaisesRegex(inputs.InputsError, "source"):
                inputs.prepare(workspace, workspace / "prepared", run=self.runner())

    def test_missing_non_qt_header_or_broken_symlink_prevents_publication(self):
        header = self.root / "header.h"
        header.unlink()
        for broken_symlink in (False, True):
            if broken_symlink:
                header.symlink_to(self.root / "absent.h")
            with self.assertRaisesRegex(inputs.InputsError, "missing"):
                inputs.prepare(self.root, self.root / "prepared", run=self.runner())
            self.assertFalse((self.root / "prepared/compile_commands.json").exists())

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

    def test_qt_marker_must_resolve_through_a_compiler_include(self):
        fragment = json.loads(self.fragment.read_text())
        fragment["commands"][0]["arguments"][1] = "-Iabsent"
        self.fragment.write_text(json.dumps(fragment))
        with self.assertRaisesRegex(inputs.InputsError, "QtCore"):
            inputs.prepare(self.root, self.root / "prepared", run=self.runner())
        self.assertFalse((self.root / "prepared/compile_commands.json").exists())

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
