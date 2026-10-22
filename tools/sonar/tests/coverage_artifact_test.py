"""Keep transferred coverage tied to its producing code and valid source paths."""

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import coverage_artifact as artifact

COMMIT = "a" * 40
TOOLCHAIN = {"clang": "clang 22", "llvm-cov": "LLVM 22", "llvm-profdata": "LLVM 22"}


class CoverageArtifactTest(unittest.TestCase):
    def test_tool_versions_follow_the_coverage_script_overrides(self):
        def run(command, **_kwargs):
            return subprocess.CompletedProcess(command, 0, command[0] + " version", "")

        with (
            mock.patch.dict(
                os.environ,
                {"LLVM_COV": "/chosen/llvm-cov", "LLVM_PROFDATA": "/chosen/llvm-profdata"},
            ),
            mock.patch.object(artifact.sys, "platform", "linux"),
            mock.patch.object(artifact.subprocess, "run", side_effect=run),
        ):
            versions = artifact.toolchain_identity()
        self.assertEqual(versions["llvm-cov"], "/chosen/llvm-cov version")
        self.assertEqual(versions["llvm-profdata"], "/chosen/llvm-profdata version")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name).resolve()
        self.producer = root / "producer"
        self.consumer = root / "consumer"
        self.coverage = root / "coverage"
        self.exported = root / "exported"
        self.imported = root / "imported"
        self.coverage.mkdir()
        self.make_report(100)

    def make_report(self, count):
        self.producer.mkdir(exist_ok=True)
        self.consumer.mkdir(exist_ok=True)
        headers = [str(self.producer / f"{number}.cpp") for number in range(count)]
        for root in (self.producer, self.consumer):
            for number in range(count):
                (root / f"{number}.cpp").write_text("int probe;\n")
        self.source_line = f'    1| 1|const char *root = "{self.producer}";\n'
        report = "".join(header + ":\n" + self.source_line for header in headers)
        report += "/external/sdk/header.h:\n    1| 1|external\n"
        headers.append("/external/sdk/header.h")
        (self.coverage / "llvm-cov.report").write_text(report)
        (self.coverage / "llvm-cov.headers").write_text("\n".join(headers) + "\n")

    def export(self):
        return artifact.export_report(
            self.coverage,
            self.exported,
            commit=COMMIT,
            run_id="123",
            attempt="1",
            producer_root=self.producer,
            toolchain=TOOLCHAIN,
        )

    def load(self, **overrides):
        kwargs = {
            "commit": COMMIT,
            "run_id": "123",
            "consumer_root": self.consumer,
            "toolchain": TOOLCHAIN,
        } | overrides
        return artifact.import_report(self.exported, self.imported, **kwargs)

    def test_relocates_only_section_headers_and_retains_external_paths(self):
        self.export()
        result = self.load()
        text = result.read_text()
        self.assertIn(str(self.consumer / "0.cpp") + ":\n", text)
        self.assertIn(self.source_line, text)
        self.assertIn("/external/sdk/header.h:\n", text)
        self.assertNotIn(str(self.producer / "0.cpp") + ":\n", text)
        self.assertEqual(
            {path.name for path in self.exported.iterdir()},
            {"manifest.json", "llvm-cov.report", "llvm-cov.headers"},
        )

    def test_99_resolvable_sections_fail_and_100_pass(self):
        self.make_report(99)
        self.export()
        with self.assertRaisesRegex(artifact.ArtifactError, "99.*100"):
            self.load()
        self.make_report(100)
        self.export()
        self.assertTrue(self.load().is_file())

    def test_rejects_wrong_commit_run_configuration_or_toolchain(self):
        self.export()
        for overrides in (
            {"commit": "b" * 40},
            {"run_id": "124"},
            {"configuration": "release"},
            {"toolchain": {"clang": "other"}},
        ):
            with self.subTest(overrides=overrides), self.assertRaises(artifact.ArtifactError):
                self.load(**overrides)

    def test_producer_attempt_is_provenance_and_does_not_prevent_scanner_reuse(self):
        manifest = self.export()
        self.assertEqual(json.loads(manifest.read_text())["producer_attempt"], "1")
        self.assertTrue(self.load().is_file())
        self.assertTrue(self.load().is_file())

    def test_corrupt_or_missing_report_fails(self):
        self.export()
        report = self.exported / "llvm-cov.report"
        report.write_text(report.read_text() + "changed")
        with self.assertRaisesRegex(artifact.ArtifactError, "digest"):
            self.load()
        report.unlink()
        with self.assertRaises(artifact.ArtifactError):
            self.load()

    def test_malformed_manifest_fails_cleanly(self):
        manifest = self.export()
        for value in ([], {"version": 99}, {"version": True}, {"version": 1, "files": []}):
            manifest.write_text(json.dumps(value))
            with self.subTest(value=value), self.assertRaises(artifact.ArtifactError):
                self.load()

    def test_header_manifest_must_describe_the_actual_report(self):
        (self.coverage / "llvm-cov.headers").write_text("unrelated.cpp\n")
        with self.assertRaises(artifact.ArtifactError):
            self.export()

    def test_import_does_not_overwrite_its_input_artifact(self):
        self.export()
        with self.assertRaises(artifact.ArtifactError):
            artifact.import_report(
                self.exported,
                self.exported,
                commit=COMMIT,
                run_id="123",
                consumer_root=self.consumer,
                toolchain=TOOLCHAIN,
            )
        self.assertTrue((self.exported / "llvm-cov.report").is_file())


if __name__ == "__main__":
    unittest.main()
