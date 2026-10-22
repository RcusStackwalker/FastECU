"""Keep qualification sensitive to source/configuration and compiler drift."""

import json
import tempfile
import unittest
from pathlib import Path

import sonar_qualify as qualify


def record(configuration="bazel-out/linux-dbg/bin", source="src/probe.cpp", root="/old"):
    return {
        "owner": "//src:probe",
        "configuration": configuration,
        "file": source,
        "directory": root,
        "arguments": ["clang++", "-DFLAG=1", "-I" + root + "/include", "-c", source],
        "outputs": [configuration + "/probe.o"],
    }


class QualifyTest(unittest.TestCase):
    def test_main_repository_label_spellings_are_equivalent(self):
        candidate = record() | {"owner": "@@//src:probe"}
        self.assertEqual(
            qualify.compare_commands([record()], [candidate]),
            {"missing": [], "extra": [], "arguments": []},
        )

    def test_scanner_database_parity_ignores_only_debug_mapping(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline, candidate = root / "before", root / "after"
            baseline.mkdir()
            candidate.mkdir()
            for directory in (baseline, candidate):
                (directory / "records.json").write_text(json.dumps([record()]))
            before = {
                "directory": "/old",
                "file": "src/probe.cpp",
                "arguments": ["clang", "-c", "src/probe.cpp"],
            }
            after = before | {
                "arguments": ["/usr/bin/clang", "-fdebug-prefix-map=/old=.", "-c", "src/probe.cpp"]
            }
            (baseline / "compile_commands.json").write_text(json.dumps([before]))
            (candidate / "compile_commands.json").write_text(json.dumps([after]))
            args = ["--baseline", str(baseline), "--candidate", str(candidate)]
            self.assertEqual(qualify.main([*args, "--output", str(root / "matched")]), 0)
            after["arguments"].insert(1, "-DFLAG=2")
            (candidate / "compile_commands.json").write_text(json.dumps([after]))
            self.assertEqual(qualify.main([*args, "--output", str(root / "changed")]), 1)

    def test_root_relocation_alone_does_not_change_command_semantics(self):
        result = qualify.compare_commands([record()], [record(root="/new")])
        self.assertEqual(result, {"missing": [], "extra": [], "arguments": []})

    def test_generated_sources_and_configurations_cannot_be_dropped(self):
        baseline = [
            record(),
            record("bazel-out/other/bin"),
            record(source="bazel-out/linux-dbg/bin/generated.cpp"),
        ]
        result = qualify.compare_commands(baseline, [record()])
        self.assertEqual(len(result["missing"]), 2)

    def test_argument_changes_and_reordering_are_reported(self):
        for arguments in (
            ["clang++", "-DFLAG=2", "-I/old/include", "-c", "src/probe.cpp"],
            ["clang++", "-I/old/include", "-DFLAG=1", "-c", "src/probe.cpp"],
        ):
            candidate = record() | {"arguments": arguments}
            self.assertEqual(len(qualify.compare_commands([record()], [candidate])["arguments"]), 1)

    def test_query_recovers_owner_configuration_and_actual_arguments(self):
        query = {
            "targets": [{"id": 1, "label": "//src:probe"}],
            "pathFragments": [
                {"id": 1, "label": "bazel-out"},
                {"id": 2, "parentId": 1, "label": "linux-dbg"},
                {"id": 3, "parentId": 2, "label": "bin"},
                {"id": 4, "parentId": 3, "label": "probe.o"},
            ],
            "artifacts": [{"id": 9, "pathFragmentId": 4}],
            "actions": [
                {
                    "mnemonic": "CppCompile",
                    "targetId": 1,
                    "outputIds": [9],
                    "arguments": ["clang++", "-c", "src/probe.cpp"],
                }
            ],
        }
        records = qualify.records_from_aquery(query, Path("/execroot"))
        self.assertEqual(records[0]["owner"], "//src:probe")
        self.assertEqual(records[0]["configuration"], "bazel-out/linux-dbg/bin")
        self.assertEqual(records[0]["file"], "src/probe.cpp")


if __name__ == "__main__":
    unittest.main()
