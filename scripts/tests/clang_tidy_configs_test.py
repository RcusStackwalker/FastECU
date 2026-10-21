#!/usr/bin/env python3

import tempfile
import unittest
from io import StringIO
from pathlib import Path

import clang_tidy_configs as cfg

_CONFIG = ".clang-tidy"
_BUILD = "BUILD.bazel"
_INHERIT = "InheritParentConfig: true\nChecks: 'x'\n"
_STANDALONE = "Checks: 'x'\n"


class Repo(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)

    def write(self, relative: str, text: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def config(self, directory: str, text: str, build: str = "") -> None:
        prefix = f"{directory}/" if directory else ""
        self.write(prefix + _CONFIG, text)
        self.write(prefix + _BUILD, build)

    def listing(self, root: Path) -> list[str]:
        return sorted(
            path.relative_to(root).as_posix()
            for path in root.rglob("*")
            if path.is_file() and path.name == _CONFIG
        )

    def run_tool(self, *, check: bool) -> tuple[int, str]:
        err = StringIO()
        code = cfg.run(self.root, check=check, list_configs=self.listing, err=err)
        return code, err.getvalue()


class ChainsTest(Repo):
    def test_a_nested_config_that_inherits_lists_its_parents(self) -> None:
        self.config("", _STANDALONE)
        self.config("src/ui", _INHERIT)
        self.assertEqual(
            {"": [_CONFIG], "src/ui": ["src/ui/.clang-tidy", _CONFIG]},
            cfg.compute_chains(self.root, self.listing(self.root)),
        )

    def test_a_nested_config_that_does_not_inherit_stops_the_chain(self) -> None:
        self.config("", _STANDALONE)
        self.config("src/ui", _STANDALONE)
        self.assertEqual(
            ["src/ui/.clang-tidy"], cfg.compute_chains(self.root, self.listing(self.root))["src/ui"]
        )

    def test_inheritance_skips_directories_without_a_config(self) -> None:
        self.config("", _STANDALONE)
        self.config("a/b/c", _INHERIT)
        self.assertEqual(
            ["a/b/c/.clang-tidy", _CONFIG],
            cfg.compute_chains(self.root, self.listing(self.root))["a/b/c"],
        )

    def test_inheritance_continues_through_every_inheriting_level(self) -> None:
        self.config("", _STANDALONE)
        self.config("a", _INHERIT)
        self.config("a/b", _INHERIT)
        self.assertEqual(
            ["a/b/.clang-tidy", "a/.clang-tidy", _CONFIG],
            cfg.compute_chains(self.root, self.listing(self.root))["a/b"],
        )

    def test_inherit_false_and_comments_do_not_count(self) -> None:
        self.assertFalse(cfg.inherits_parent("# InheritParentConfig: true\nChecks: 'x'\n"))
        self.assertFalse(cfg.inherits_parent("InheritParentConfig: false\n"))
        self.assertTrue(cfg.inherits_parent("---\nInheritParentConfig: True\n"))

    def test_an_inheriting_config_with_no_parent_is_just_itself(self) -> None:
        self.config("src", _INHERIT)
        self.assertEqual(
            {"src": ["src/.clang-tidy"]}, cfg.compute_chains(self.root, self.listing(self.root))
        )


class RenderTest(Repo):
    def test_the_table_and_labels_are_sorted_and_stable(self) -> None:
        text = cfg.render_bzl({"src/ui": ["src/ui/.clang-tidy", _CONFIG], "": [_CONFIG]})
        self.assertIn(
            '    "": [".clang-tidy"],\n    "src/ui": ["src/ui/.clang-tidy", ".clang-tidy"],', text
        )
        self.assertIn(
            'CONFIG_LABELS = [\n    "//:.clang-tidy",\n    "//src/ui:.clang-tidy",\n]', text
        )
        self.assertEqual(
            text, cfg.render_bzl({"": [_CONFIG], "src/ui": ["src/ui/.clang-tidy", _CONFIG]})
        )


class ExportsTest(unittest.TestCase):
    def test_an_existing_literal_export_needs_no_edit(self) -> None:
        build = 'exports_files([\n    ".clang-tidy",\n    "other",\n])\n'
        self.assertIsNone(cfg.add_export(build))

    def test_a_missing_export_is_appended_once(self) -> None:
        edited = cfg.add_export('load("x", "y")\n')
        assert edited is not None
        self.assertTrue(edited.endswith('exports_files([".clang-tidy"])\n'))
        self.assertIsNone(cfg.add_export(edited))

    def test_other_files_exports_do_not_count(self) -> None:
        self.assertIsNotNone(cfg.add_export('exports_files(["x.clang-tidy.txt"])\n'))


class RunTest(Repo):
    def test_write_mode_creates_the_table_and_the_missing_export(self) -> None:
        self.config("", _STANDALONE, build='exports_files([".clang-tidy"])\n')
        self.config("src/ui", _INHERIT, build="# gazelle\n")
        code, _ = self.run_tool(check=False)
        self.assertEqual(1, code)
        self.assertIn("src/ui/.clang-tidy", (self.root / cfg.OUTPUT).read_text())
        self.assertIn('exports_files([".clang-tidy"])', (self.root / "src/ui" / _BUILD).read_text())
        self.assertEqual(0, self.run_tool(check=False)[0])

    def test_check_mode_reports_drift_without_touching_files(self) -> None:
        self.config("", _STANDALONE, build='exports_files([".clang-tidy"])\n')
        code, err = self.run_tool(check=True)
        self.assertEqual(1, code)
        self.assertIn("scripts/clang_tidy_configs.py", err)
        self.assertFalse((self.root / cfg.OUTPUT).exists())

    def test_check_mode_passes_when_current(self) -> None:
        self.config("", _STANDALONE, build='exports_files([".clang-tidy"])\n')
        self.run_tool(check=False)
        self.assertEqual((0, ""), self.run_tool(check=True))

    def test_a_config_outside_a_package_is_an_error(self) -> None:
        self.write(f"loose/{_CONFIG}", _STANDALONE)
        code, err = self.run_tool(check=False)
        self.assertEqual(2, code)
        self.assertIn("loose", err)

    def test_removing_a_config_removes_it_from_the_table(self) -> None:
        self.config("", _STANDALONE, build='exports_files([".clang-tidy"])\n')
        self.config("src/ui", _INHERIT, build='exports_files([".clang-tidy"])\n')
        self.run_tool(check=False)
        (self.root / "src/ui" / _CONFIG).unlink()
        self.assertEqual(1, self.run_tool(check=False)[0])
        self.assertNotIn("src/ui", (self.root / cfg.OUTPUT).read_text())


if __name__ == "__main__":
    unittest.main()
