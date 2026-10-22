"""Protect the actual commands and generated files consumed by Sonar."""

import json
import tempfile
import unittest
from pathlib import Path

import sonar_commands as commands


def fragment(configuration="bazel-out/first/bin", source="src/probe.cpp"):
    return {
        "version": 1,
        "owner": "//src:probe",
        "configuration": configuration,
        "commands": [
            {
                "file": source,
                "arguments": [
                    "clang++",
                    "-DNAME=two words",
                    "-Iinclude",
                    "-c",
                    source,
                    "-o",
                    "probe.o",
                ],
                "inputs": [source, "include/generated.h"],
                "outputs": ["probe.o", "probe.d"],
            }
        ],
    }


class CommandsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def load(self, *values):
        paths = []
        for number, value in enumerate(values):
            path = self.root / f"fragment-{number}.json"
            path.write_text(json.dumps(value))
            paths.append(path)
        return commands.load_fragments(paths)

    def test_preserves_ordered_arguments_and_distinct_configurations(self):
        records = self.load(fragment(), fragment("bazel-out/second/bin"))
        database = commands.database_entries(records, self.root)
        self.assertEqual(len(database), 2)
        self.assertEqual(
            database[0]["arguments"],
            [
                "clang++",
                "-DNAME=two words",
                "-Iinclude",
                "-c",
                "src/probe.cpp",
                "-o",
                "probe.o",
            ],
        )
        self.assertEqual(database[0]["directory"], str(self.root))
        self.assertEqual(database[0]["file"], "src/probe.cpp")
        self.assertEqual(database[0]["output"], "probe.o")

    def test_generated_sources_are_retained(self):
        records = self.load(fragment(source="bazel-out/first/bin/moc_probe.cpp"))
        self.assertEqual(
            commands.database_entries(records, self.root)[0]["file"],
            "bazel-out/first/bin/moc_probe.cpp",
        )

    def test_duplicate_fragments_do_not_duplicate_the_same_action(self):
        records = self.load(fragment(), fragment())
        self.assertEqual(len(commands.database_entries(records, self.root)), 1)

    def test_rejects_unexpanded_response_files(self):
        value = fragment()
        value["commands"][0]["arguments"] = ["clang++", "@missing.params"]
        with self.assertRaisesRegex(commands.CommandError, "response"):
            self.load(value)

    def test_rejects_invalid_and_empty_commands(self):
        for mutation in ({"version": 2}, {"owner": ""}, {"commands": []}):
            value = fragment() | mutation
            with self.subTest(mutation=mutation), self.assertRaises(commands.CommandError):
                self.load(value)
        value = fragment()
        value["commands"][0]["arguments"] = []
        with self.assertRaises(commands.CommandError):
            self.load(value)

    def test_requires_generated_inputs_and_resolves_symlink_targets(self):
        records = self.load(fragment())
        (self.root / "src").mkdir()
        (self.root / "src/probe.cpp").write_text("int probe;\n")
        (self.root / "include").mkdir()
        target = self.root / "generated.h"
        (self.root / "include/generated.h").symlink_to(target)
        with self.assertRaisesRegex(commands.CommandError, "generated.h"):
            commands.validate_inputs(records, self.root)
        target.write_text("#pragma once\n")
        commands.validate_inputs(records, self.root)


if __name__ == "__main__":
    unittest.main()
