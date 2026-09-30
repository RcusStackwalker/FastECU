#!/usr/bin/env python3
"""Regression tests for complete generated C++ test ownership."""

import tempfile
import unittest
from pathlib import Path

import gazelle_check as gc


class TestOwnershipTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, path, text):
        file = self.root / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(text)
        return path

    def test_rejects_uncovered_cpp_test_package(self):
        file = self.write("unmanaged/BUILD.bazel", 'cc_test(name = "probe", srcs = ["probe.cpp"])')
        with self.assertRaisesRegex(gc.GazelleCheckError, "outside Gazelle scope"):
            gc.validate_test_ownership(self.root, [file])

    def test_rejects_whole_rule_keep(self):
        file = self.write(
            "tests/BUILD.bazel", '# reason\n# keep\nfastecu_gtest(\n name = "probe",\n)'
        )
        with self.assertRaisesRegex(gc.GazelleCheckError, "whole-rule keep"):
            gc.validate_test_ownership(self.root, [file])

    def test_rejects_suffix_whole_rule_keep(self):
        file = self.write(
            "tests/BUILD.bazel", 'fastecu_gtest(name = "probe", srcs = ["p.cpp"]) # keep'
        )
        with self.assertRaisesRegex(gc.GazelleCheckError, "whole-rule keep"):
            gc.validate_test_ownership(self.root, [file])

    def test_rejects_keep_with_explanation(self):
        for text in [
            '# keep: intentional\nfastecu_gtest(name = "probe")',
            'fastecu_gtest(name = "probe") # keep: intentional',
        ]:
            file = self.write("tests/BUILD.bazel", text)
            with self.assertRaisesRegex(gc.GazelleCheckError, "whole-rule keep"):
                gc.validate_test_ownership(self.root, [file])

    def test_accepts_narrow_source_keep(self):
        file = self.write(
            "tests/BUILD.bazel", 'fastecu_gtest(\n name = "probe",\n srcs = ["main.cpp"], # keep\n)'
        )
        gc.validate_test_ownership(self.root, [file])

    def test_python_test_does_not_require_cpp_ownership(self):
        file = self.write("unmanaged/BUILD.bazel", 'py_test(name = "guard")')
        gc.validate_test_ownership(self.root, [file])


class ProductionKeepTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, text, path="pkg/BUILD.bazel"):
        file = self.root / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(text)
        return path

    def test_rejects_new_whole_rule_keep(self):
        file = self.write('# reason\n# keep\ncc_library(\n name = "lib",\n)')
        with self.assertRaisesRegex(gc.GazelleCheckError, "pkg:lib.*allowlist"):
            gc.validate_production_keeps(self.root, [file], frozenset())

    def test_rejects_suffix_keep_on_moc_library_and_binary(self):
        for kind in ("qt_cc_library", "cc_binary", "qt_cc_binary"):
            file = self.write(f'{kind}(name = "x", srcs = ["x.cpp"]) # keep: reason')
            with self.assertRaisesRegex(gc.GazelleCheckError, "pkg:x"):
                gc.validate_production_keeps(self.root, [file], frozenset())

    def test_accepts_allowlisted_keep(self):
        file = self.write('# keep\ncc_library(name = "lib")')
        gc.validate_production_keeps(self.root, [file], frozenset({"pkg:lib"}))

    def test_rejects_stale_allowlist_entry(self):
        file = self.write('cc_library(name = "lib")')
        with self.assertRaisesRegex(gc.GazelleCheckError, "stale"):
            gc.validate_production_keeps(self.root, [file], frozenset({"pkg:lib"}))

    def test_accepts_narrow_attribute_and_dependency_keeps(self):
        file = self.write(
            'cc_library(\n name = "lib",\n srcs = ["a.cpp"],  # keep\n'
            ' deps = [":link_only"],  # keep: registration\n)'
        )
        gc.validate_production_keeps(self.root, [file], frozenset())

    def test_keep_above_unrelated_rule_does_not_leak(self):
        file = self.write('# keep\nfastecu_gtest(name = "t")\ncc_library(name = "lib")')
        gc.validate_production_keeps(self.root, [file], frozenset())


if __name__ == "__main__":
    unittest.main()
