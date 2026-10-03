#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path, PurePath

import clang_tidy_scope as scope


def _manifest() -> scope.ScopeManifest:
    return scope.ScopeManifest(
        os_prefixes={"windows": (PurePath("src/win"), PurePath("tests"))},
        linux_covered=(PurePath("src/unix"),),
    )


class LoadManifestTest(unittest.TestCase):
    def write(self, text: str) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / scope.MANIFEST_NAME
        path.write_text(text)
        return path

    def test_loads_prefixes(self) -> None:
        manifest = scope.load_manifest(
            self.write(
                '[os.windows]\nprefixes = ["src/win", "tests"]\n'
                '[linux_covered]\nprefixes = ["src/unix"]\n'
            )
        )
        self.assertEqual((PurePath("src/win"), PurePath("tests")), manifest.os_prefixes["windows"])
        self.assertEqual((PurePath("src/unix"),), manifest.linux_covered)

    def test_rejects_absolute_and_parent_relative_prefixes(self) -> None:
        for bad in ('"/etc"', '"../outside"', '"src/../x"', '"C:/x"'):
            with self.subTest(bad=bad), self.assertRaises(scope.ScopeError):
                scope.load_manifest(self.write(f"[os.windows]\nprefixes = [{bad}]\n"))

    def test_rejects_a_missing_file_and_bad_toml(self) -> None:
        with self.assertRaises(scope.ScopeError):
            scope.load_manifest(Path("/no/such/.clang-tidy-scope.toml"))
        with self.assertRaises(scope.ScopeError):
            scope.load_manifest(self.write("[os.windows\n"))

    def test_rejects_non_string_prefixes(self) -> None:
        with self.assertRaises(scope.ScopeError):
            scope.load_manifest(self.write("[os.windows]\nprefixes = [1]\n"))


class ScopeQueriesTest(unittest.TestCase):
    def test_in_scope_matches_a_directory_and_its_descendants_only(self) -> None:
        prefixes = [PurePath("src/win")]
        self.assertTrue(scope.in_scope(PurePath("src/win/a.cpp"), prefixes))
        self.assertTrue(scope.in_scope(PurePath("src/win/deep/b.cpp"), prefixes))
        self.assertFalse(scope.in_scope(PurePath("src/windows_like/a.cpp"), prefixes))
        self.assertFalse(scope.in_scope(PurePath("src/a.cpp"), prefixes))

    def test_build_targets_are_recursive_patterns(self) -> None:
        self.assertEqual(
            ["//src/win/...", "//tests/..."],
            scope.build_targets([PurePath("src/win"), PurePath("tests")]),
        )

    def test_guard_reports_packages_no_prefix_covers(self) -> None:
        packages = [
            PurePath("src/win/j2534"),
            PurePath("src/unix"),
            PurePath("tests/force_asserts"),
            PurePath("src/new_gated"),
        ]
        self.assertEqual(
            [PurePath("src/new_gated")], scope.uncovered_gated_packages(packages, _manifest())
        )

    def test_guard_accepts_when_everything_is_covered(self) -> None:
        self.assertEqual([], scope.uncovered_gated_packages([PurePath("src/win")], _manifest()))


if __name__ == "__main__":
    unittest.main()
