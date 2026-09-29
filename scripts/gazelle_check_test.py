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


def list_all(root: Path) -> list[str]:
    """Stand-in for git: every *.bazel file below root, repo-relative."""
    return sorted(p.relative_to(root).as_posix() for p in root.rglob("*.bazel"))


class CheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name).resolve()
        (self.root / "pkg").mkdir()
        self.build = self.root / "pkg" / _BUILD
        self.build.write_text("# original\n")
        self.diffs: list[tuple[list[str], list[str]]] = []

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def run_check(self, run_gazelle) -> tuple[int, str]:
        out = StringIO()

        def show_diff(_root: Path, paths, added) -> None:
            self.diffs.append((list(paths), list(added)))

        code = gc.check(self.root, list_all, run_gazelle, show_diff, out)
        return code, out.getvalue()

    def test_unchanged_tree_is_clean(self) -> None:
        code, output = self.run_check(lambda _root: 0)
        self.assertEqual(code, 0)
        self.assertEqual(output, "")
        self.assertEqual(self.diffs, [])

    def test_modified_file_is_drift(self) -> None:
        def gazelle(_root: Path) -> int:
            self.build.write_text("# regenerated\n")
            return 0

        code, output = self.run_check(gazelle)
        self.assertEqual(code, 1)
        self.assertIn("pkg/BUILD.bazel", output)
        self.assertEqual(self.diffs, [(["pkg/BUILD.bazel"], [])])

    def test_drift_message_tells_the_contributor_to_commit(self) -> None:
        def gazelle(_root: Path) -> int:
            self.build.write_text("# regenerated\n")
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
        self.assertIn("new/BUILD.bazel", output)
        self.assertEqual(self.diffs, [(["new/BUILD.bazel"], ["new/BUILD.bazel"])])

    def test_deleted_file_is_drift(self) -> None:
        def gazelle(_root: Path) -> int:
            self.build.unlink()
            return 0

        code, output = self.run_check(gazelle)
        self.assertEqual(code, 1)
        self.assertIn("pkg/BUILD.bazel", output)

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
        snapshot = gc.snapshot(self.root, lambda _root: ["pkg/BUILD.bazel", "gone/BUILD.bazel"])
        self.assertEqual(list(snapshot), ["pkg/BUILD.bazel"])


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
            self.assertEqual(gc.main(), 2)
        self.assertIn("no repo", stderr.getvalue())


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
