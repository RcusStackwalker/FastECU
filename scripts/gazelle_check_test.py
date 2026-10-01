#!/usr/bin/env python3

import shutil
import subprocess
import tempfile
import unittest
from io import StringIO
from pathlib import Path
from unittest import mock

import gazelle_check as gc

_BUILD = "BUILD.bazel"
_PKG_BUILD = f"pkg/{_BUILD}"
_NEW_BUILD = f"new/{_BUILD}"
_REGENERATED = "# regenerated\n"
_FORMATTED = "# formatted\n"


_SCOPE = ("src", "apps", "tests", "resources/shared")


def write_root_build(root: Path, body: str | None = None) -> None:
    if body is None:
        body = "GAZELLE_ARGS = [\n" + "".join(f'    "{path}",\n' for path in _SCOPE) + "]\n"
    (root / _BUILD).write_text(body)


def list_all(root: Path) -> list[str]:
    """Stand-in for git: every *.bazel file below root, repo-relative."""
    return sorted(p.relative_to(root).as_posix() for p in root.rglob("*.bazel"))


class CheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name).resolve()
        write_root_build(self.root)
        (self.root / "pkg").mkdir()
        self.build = self.root / "pkg" / _BUILD
        self.build.write_text("# original\n")
        self.diffs: list[tuple[list[str], list[str]]] = []

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def run_check(
        self, run_gazelle, run_hook=lambda _root, _hook, _files: 0, fix=False
    ) -> tuple[int, str]:
        out = StringIO()

        def show_diff(_root: Path, paths, added) -> None:
            self.diffs.append((list(paths), list(added)))

        code = gc.check(
            self.root,
            list_all,
            run_gazelle,
            show_diff,
            out,
            run_hook=run_hook,
            fix=fix,
            allowed_keeps=frozenset(),
        )
        return code, out.getvalue()

    def test_unchanged_tree_is_clean(self) -> None:
        code, output = self.run_check(lambda _root: 0)
        self.assertEqual(code, 0)
        self.assertEqual(output, "")
        self.assertEqual(self.diffs, [])

    def test_modified_file_is_drift(self) -> None:
        def gazelle(_root: Path) -> int:
            self.build.write_text(_REGENERATED)
            return 0

        code, output = self.run_check(gazelle)
        self.assertEqual(code, 1)
        self.assertIn(_PKG_BUILD, output)
        self.assertEqual(self.diffs, [([_PKG_BUILD], [])])

    def test_drift_message_tells_the_contributor_to_commit(self) -> None:
        def gazelle(_root: Path) -> int:
            self.build.write_text(_REGENERATED)
            return 0

        _, output = self.run_check(gazelle)
        self.assertIn("commit", output)

    def test_created_file_is_drift_and_reported_as_added(self) -> None:
        def gazelle(root: Path) -> int:
            (root / "new").mkdir()
            (root / "new" / _BUILD).write_text("# created\n")
            return 0

        code, output = self.run_check(gazelle)
        self.assertEqual(code, 1)
        self.assertIn(_NEW_BUILD, output)
        self.assertEqual(self.diffs, [([_NEW_BUILD], [_NEW_BUILD])])

    def test_deleted_file_is_drift(self) -> None:
        def gazelle(_root: Path) -> int:
            self.build.unlink()
            return 0

        code, output = self.run_check(gazelle)
        self.assertEqual(code, 1)
        self.assertIn(_PKG_BUILD, output)

    def test_preexisting_uncommitted_edit_is_not_drift(self) -> None:
        self.build.write_text("# contributor edit, not yet staged\n")
        code, _ = self.run_check(lambda _root: 0)
        self.assertEqual(code, 0)

    def test_gazelle_failure_is_not_reported_as_clean(self) -> None:
        code, output = self.run_check(lambda _root: 1)
        self.assertEqual(code, 2)
        self.assertIn("gazelle failed", output)
        self.assertEqual(self.diffs, [])

    def test_gazelle_failure_after_partial_write_still_exits_two(self) -> None:
        def gazelle(_root: Path) -> int:
            self.build.write_text("# half written\n")
            return 1

        code, _ = self.run_check(gazelle)
        self.assertEqual(code, 2)

    def test_snapshot_ignores_listed_but_missing_files(self) -> None:
        snapshot = gc.snapshot(self.root, lambda _root: [_PKG_BUILD, "gone/BUILD.bazel"])
        self.assertEqual(list(snapshot), [_PKG_BUILD])

    def pilot_build(self):
        path = self.root / "src/algorithms/example/BUILD.bazel"
        path.parent.mkdir(parents=True)
        path.write_text("# original\n")
        return path

    def test_formatter_correction_is_drift_and_confirmed(self):
        build = self.pilot_build()
        calls = []

        def hook(root, name, files):
            calls.append(name)
            if len(calls) == 1:
                build.write_text(_FORMATTED)
                return 1
            return 0

        code, _ = self.run_check(lambda root: 0, hook)
        self.assertEqual(code, 1)
        self.assertEqual(calls, ["buildifier", "buildifier", "buildifier-lint"])

    def test_fix_returns_zero_after_formatter_correction(self):
        build = self.pilot_build()

        def hook(root, name, files):
            if build.read_text() != _FORMATTED:
                build.write_text(_FORMATTED)
                return 1
            return 0

        self.assertEqual(self.run_check(lambda root: 0, hook, fix=True)[0], 0)
        self.assertEqual(build.read_text(), _FORMATTED)

    def test_formatter_failure_without_corrections_exits_two(self):
        self.pilot_build()
        code, output = self.run_check(lambda root: 0, lambda *args: 1)
        self.assertEqual(code, 2)
        self.assertIn("buildifier failed", output)

    def test_formatter_retry_failure_exits_two(self):
        build = self.pilot_build()

        def hook(*args):
            build.write_text("# change\n")
            return 1

        self.assertEqual(self.run_check(lambda root: 0, hook)[0], 2)

    def test_linter_failure_exits_two_even_with_fix(self):
        self.pilot_build()

        def hook(root, name, files):
            return 1 if name == "buildifier-lint" else 0

        self.assertEqual(self.run_check(lambda root: 0, hook, fix=True)[0], 2)

    def test_new_build_is_formatted_and_exclusions_are_respected(self):
        seen = []

        def gazelle(root):
            self.pilot_build()
            excluded = root / "bazel/definitions/nested/BUILD.bazel"
            excluded.parent.mkdir(parents=True)
            excluded.write_text("# excluded\n")
            return 0

        def hook(root, name, files):
            seen.append(list(files))
            return 0

        self.assertEqual(self.run_check(gazelle, hook)[0], 1)
        self.assertEqual(seen, [["src/algorithms/example/BUILD.bazel"]] * 2)

    def test_fix_returns_zero_after_corrections(self):
        def gazelle(root):
            self.build.write_text(_REGENERATED)
            return 0

        code, output = self.run_check(gazelle, fix=True)
        self.assertEqual(code, 0)
        self.assertIn("commit", output)

    def test_managed_batches_and_new_subpackages_are_formatted(self):
        managed = [
            "src/backend/ports/BUILD.bazel",
            "src/backend/ports/testing/new/BUILD.bazel",
            "src/ui/desktop/BUILD.bazel",
            "src/platform/desktop/common/brand_new/nested/BUILD.bazel",
            "apps/bench/BUILD.bazel",
            "tests/force_asserts/BUILD.bazel",
            "resources/shared/BUILD.bazel",
        ]
        unmanaged = [
            "bazel/qt/BUILD.bazel",
            "scripts/BUILD.bazel",
            "srcish/BUILD.bazel",
            "docs/BUILD.bazel",
        ]

        def gazelle(root):
            for relative in managed + unmanaged:
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(_REGENERATED)
            return 0

        def hook(root, name, files):
            for relative in files:
                (root / relative).write_text(_FORMATTED)
            return 0

        self.assertEqual(self.run_check(gazelle, hook, fix=True)[0], 0)
        for relative in managed:
            with self.subTest(relative=relative):
                self.assertEqual((self.root / relative).read_text(), _FORMATTED)
        for relative in unmanaged:
            with self.subTest(relative=relative):
                self.assertEqual((self.root / relative).read_text(), _REGENERATED)

    def test_new_cpp_test_package_outside_scope_is_rejected(self):
        (self.root / "bazel" / "extra").mkdir(parents=True)
        (self.root / "bazel" / "extra" / _BUILD).write_text('fastecu_gtest(name = "t")\n')
        with self.assertRaisesRegex(gc.GazelleCheckError, "outside Gazelle scope"):
            self.run_check(lambda _root: 0)

    def test_new_nested_cpp_test_package_inside_scope_is_accepted(self):
        nested = self.root / "src" / "backend" / "brand_new" / "testing"
        nested.mkdir(parents=True)
        (nested / _BUILD).write_text('fastecu_portable_gtest(name = "t")\n')
        self.assertEqual(self.run_check(lambda _root: 0)[0], 0)

    def test_managed_roots_are_read_from_gazelle_args(self):
        self.assertEqual(gc.load_managed_roots(self.root), tuple(map(Path, _SCOPE)))

    def test_real_root_build_declares_scope(self):
        roots = gc.load_managed_roots(Path(__file__).resolve().parents[1])
        self.assertEqual(roots, tuple(map(Path, _SCOPE)))

    def test_fix_preserves_gazelle_failure(self):
        self.assertEqual(self.run_check(lambda root: 1, fix=True)[0], 2)


class LoadManagedRootsTest(unittest.TestCase):
    def load(self, body: str | None) -> tuple[Path, ...]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            if body is not None:
                write_root_build(root, body)
            return gc.load_managed_roots(root)

    def test_missing_root_build_fails_clearly(self):
        with self.assertRaisesRegex(gc.GazelleCheckError, "cannot read GAZELLE_ARGS"):
            self.load(None)

    def test_missing_assignment_fails_clearly(self):
        with self.assertRaisesRegex(gc.GazelleCheckError, "not found"):
            self.load("OTHER = []\n")

    def test_computed_value_fails_clearly(self):
        with self.assertRaisesRegex(gc.GazelleCheckError, "literal"):
            self.load('GAZELLE_ARGS = ["src"] + EXTRA\n')

    def test_empty_or_non_string_list_fails_clearly(self):
        for body in ("GAZELLE_ARGS = []\n", "GAZELLE_ARGS = [1]\n", 'GAZELLE_ARGS = "src"\n'):
            with self.subTest(body=body), self.assertRaisesRegex(gc.GazelleCheckError, "non-empty"):
                self.load(body)

    def test_syntax_error_fails_clearly(self):
        with self.assertRaisesRegex(gc.GazelleCheckError, "cannot read GAZELLE_ARGS"):
            self.load("GAZELLE_ARGS = [\n")

    def test_starlark_is_not_executed(self):
        roots = self.load('print("x")\nGAZELLE_ARGS = ["src"]\n')
        self.assertEqual(roots, (Path("src"),))


class EnvironmentTest(unittest.TestCase):
    def test_missing_bazel_is_an_actionable_error(self) -> None:
        with (
            mock.patch.object(gc.shutil, "which", return_value=None),
            self.assertRaisesRegex(gc.GazelleCheckError, "bazel"),
        ):
            gc.run_bazel_gazelle(Path("."))

    def test_main_reports_environment_errors_as_exit_two(self) -> None:
        with (
            mock.patch.object(gc, "repo_root", side_effect=gc.GazelleCheckError("no repo")),
            mock.patch.object(gc.sys, "stderr", new=StringIO()) as stderr,
        ):
            self.assertEqual(gc.main([]), 2)
        self.assertIn("no repo", stderr.getvalue())

    def test_missing_prek_is_actionable(self):
        with (
            mock.patch.object(gc.shutil, "which", return_value=None),
            self.assertRaisesRegex(gc.GazelleCheckError, "prek"),
        ):
            gc.run_prek_hook(Path("."), "buildifier", [_BUILD])

    def test_main_fix_applies_pipeline(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_root_build(root)

            def gazelle(root):
                (root / _BUILD).write_text("# generated\n")
                return 0

            with (
                mock.patch.object(gc, "repo_root", return_value=root),
                mock.patch.object(gc, "git_list_build_files", side_effect=list_all),
                mock.patch.object(gc, "run_bazel_gazelle", side_effect=gazelle),
                mock.patch.object(gc, "git_show_diff"),
                mock.patch.object(gc, "KEPT_CPP_PRODUCTION_RULES", frozenset()),
                mock.patch.object(gc.sys, "stderr", new=StringIO()),
            ):
                self.assertEqual(gc.main(["--fix"]), 0)
                self.assertEqual((root / _BUILD).read_text(), "# generated\n")

    def test_main_reports_process_errors_as_exit_two(self):
        with (
            mock.patch.object(gc, "repo_root", side_effect=OSError("tool cannot start")),
            mock.patch.object(gc.sys, "stderr", new=StringIO()) as stderr,
        ):
            self.assertEqual(gc.main([]), 2)
        self.assertIn("tool cannot start", stderr.getvalue())


@unittest.skipUnless(shutil.which("git"), "git is required")
class GitListBuildFilesTest(unittest.TestCase):
    def test_lists_tracked_and_untracked_but_not_ignored_files(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            (root / ".gitignore").write_text("ignored/\n")
            for name in ("tracked", "dir with space", "ignored"):
                (root / name).mkdir()
                (root / name / _BUILD).write_text("#\n")
            (root / "README.md").write_text("not a build file\n")
            subprocess.run(["git", "add", "tracked"], cwd=root, check=True)

            self.assertEqual(
                gc.git_list_build_files(root),
                ["dir with space/BUILD.bazel", "tracked/BUILD.bazel"],
            )


if __name__ == "__main__":
    unittest.main()
