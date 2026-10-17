#!/usr/bin/env python3

import argparse
import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

import mutation_test as mt

ROOT = Path("/work/FastECU")
SOURCE = "src/algorithms/a.cpp"
ABSOLUTE_SOURCE = f"/work/FastECU/{SOURCE}"
SHORT = "src/a.cpp"
TITLE = "Mutation testing"
DIFF_REF = "origin/master"


def _mutant(line: int, status: str, mutator: str = "cxx_sub_to_add", replacement: str = "+"):
    return {
        "id": f"{mutator}:{line}",
        "mutatorName": mutator,
        "replacement": replacement,
        "location": {"start": {"line": line, "column": 7}, "end": {"line": line, "column": 8}},
        "status": status,
    }


def _report(path: str, *mutants):
    return {"files": {path: {"mutants": list(mutants)}}}


class RelativeSourcePathTest(unittest.TestCase):
    def test_strips_the_workspace_root(self) -> None:
        self.assertEqual(
            SOURCE,
            mt.relative_source_path(ABSOLUTE_SOURCE, ROOT),
        )

    def test_strips_a_bazel_execroot(self) -> None:
        self.assertEqual(
            "src/backend/b.cpp",
            mt.relative_source_path("/cache/abc/execroot/_main/src/backend/b.cpp", ROOT),
        )

    def test_leaves_an_already_relative_path(self) -> None:
        self.assertEqual("src/backend/b.cpp", mt.relative_source_path("./src/backend/b.cpp", ROOT))


class MergeReportsTest(unittest.TestCase):
    def test_a_mutant_killed_by_any_binary_is_killed(self) -> None:
        path = ABSOLUTE_SOURCE
        merged = mt.merge_reports(
            [_report(path, _mutant(3, "Survived")), _report(path, _mutant(3, "Killed"))], ROOT
        )
        self.assertEqual(
            {mt.MutantKey(SOURCE, 3, 7, "cxx_sub_to_add", "+"): "Killed"},
            merged,
        )

    def test_a_timeout_outranks_a_survivor_and_no_coverage(self) -> None:
        path = ABSOLUTE_SOURCE
        merged = mt.merge_reports(
            [
                _report(path, _mutant(3, "NoCoverage")),
                _report(path, _mutant(3, "Timeout")),
                _report(path, _mutant(3, "Survived")),
            ],
            ROOT,
        )
        self.assertEqual(["Timeout"], list(merged.values()))

    def test_different_replacements_at_one_site_stay_separate(self) -> None:
        path = ABSOLUTE_SOURCE
        merged = mt.merge_reports(
            [
                _report(
                    path,
                    _mutant(3, "Killed", "cxx_lt_to_le", "<="),
                    _mutant(3, "Survived", "cxx_lt_to_ge", ">="),
                )
            ],
            ROOT,
        )
        self.assertEqual(2, len(merged))

    def test_rejects_an_unknown_status(self) -> None:
        reports = [_report("/work/FastECU/a.cpp", _mutant(1, "Exploded"))]
        with self.assertRaises(mt.MutationTestError):
            mt.merge_reports(reports, ROOT)


class RenderTest(unittest.TestCase):
    def setUp(self) -> None:
        self.merged = {
            mt.MutantKey(SHORT, 1, 2, "cxx_sub_to_add", "+"): "Killed",
            mt.MutantKey(SHORT, 4, 2, "cxx_sub_to_add", "+"): "Timeout",
            mt.MutantKey(SHORT, 9, 5, "cxx_lt_to_le", "<="): "Survived",
            mt.MutantKey("src/b.cpp", 2, 1, "cxx_remove_void_call", ""): "NoCoverage",
        }

    def test_summary_counts_timeouts_as_killed_and_excludes_uncovered_from_the_score(self) -> None:
        summary = mt.render_summary(self.merged, TITLE)
        self.assertIn("Mutation score **67%**: 2 killed, 1 survived, 1 not covered", summary)
        self.assertIn("| `src/a.cpp` | 2 | 1 | 0 | 67% |", summary)
        self.assertIn("| `src/b.cpp` | 0 | 0 | 1 | n/a |", summary)

    def test_summary_lists_files_with_most_survivors_first(self) -> None:
        summary = mt.render_summary(self.merged, TITLE)
        self.assertLess(summary.index(SHORT), summary.index("src/b.cpp"))

    def test_an_empty_run_says_so(self) -> None:
        self.assertIn("No mutants in scope.", mt.render_summary({}, TITLE))

    def test_annotates_only_survivors(self) -> None:
        self.assertEqual(
            [
                "::warning file=src/a.cpp,line=9,col=5,title=Surviving mutant::"
                "[cxx_lt_to_le] replaced with `<=`; no test failed"
            ],
            mt.render_annotations(self.merged),
        )

    def test_describes_a_deleting_mutant_as_a_removal(self) -> None:
        merged = {mt.MutantKey(SHORT, 3, 1, "cxx_minus_to_noop", ""): "Survived"}
        self.assertEqual(
            [
                "::warning file=src/a.cpp,line=3,col=1,title=Surviving mutant::"
                "[cxx_minus_to_noop] removed; no test failed"
            ],
            mt.render_annotations(merged),
        )

    def test_caps_annotations_and_points_at_the_summary(self) -> None:
        survivors = {mt.MutantKey(SHORT, n, 1, "m", "r"): "Survived" for n in range(1, 13)}
        annotations = mt.render_annotations(survivors, limit=10)
        self.assertEqual(11, len(annotations))
        self.assertEqual("::notice::2 more surviving mutants; see the job summary", annotations[-1])


class OperandTest(unittest.TestCase):
    def test_accepts_a_ref_or_pattern(self) -> None:
        self.assertEqual(DIFF_REF, mt.operand(DIFF_REF))

    def test_rejects_an_option_like_value(self) -> None:
        with self.assertRaises(argparse.ArgumentTypeError):
            mt.operand("--output=/etc/passwd")

    def test_rejects_an_empty_value(self) -> None:
        with self.assertRaises(argparse.ArgumentTypeError):
            mt.operand("")


class TargetPatternTest(unittest.TestCase):
    def test_accepts_the_portable_scope(self) -> None:
        self.assertEqual(mt.PORTABLE_SCOPE, mt.target_pattern(mt.PORTABLE_SCOPE))

    def test_accepts_a_package_or_label(self) -> None:
        self.assertEqual("//src/a/...", mt.target_pattern("//src/a/..."))
        self.assertEqual("//src/a:t_test", mt.target_pattern("//src/a:t_test"))

    def test_accepts_a_subtraction(self) -> None:
        self.assertEqual("//src/... - //src/ui/...", mt.target_pattern("//src/... - //src/ui/..."))

    def test_rejects_query_functions(self) -> None:
        for value in ("//src/...) union deps(//...", "kind(cc_binary, //...)", "--output=x", ""):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                mt.target_pattern(value)


class ReportDirectoryTest(unittest.TestCase):
    def test_accepts_a_directory_under_the_checkout(self) -> None:
        self.assertEqual(
            (ROOT / "reports").resolve(), mt.report_directory(Path("reports"), ROOT, {})
        )

    def test_accepts_a_directory_under_the_runner_temp(self) -> None:
        runner_temp = Path("/runner/_temp")
        self.assertEqual(
            (runner_temp / "mutation").resolve(),
            mt.report_directory(runner_temp / "mutation", ROOT, {"RUNNER_TEMP": str(runner_temp)}),
        )

    def test_rejects_a_directory_that_escapes_the_bases(self) -> None:
        for value in (Path("../elsewhere"), Path("/etc/fastecu")):
            with self.subTest(value=value), self.assertRaises(mt.MutationTestError):
                mt.report_directory(value, ROOT, {})


class SelectionTest(unittest.TestCase):
    def test_keeps_portable_production_sources_only(self) -> None:
        self.assertEqual(
            [SOURCE, "src/backend/b.h"],
            mt.changed_portable_sources(
                [
                    "src/backend/b.h",
                    SOURCE,
                    "src/algorithms/a_test.cpp",
                    "src/algorithms/a_fuzz_test.cpp",
                    "src/backend/ports/testing/fake.h",
                    "src/ui/desktop/widget.cpp",
                    "docs/README.md",
                    "src/algorithms/BUILD.bazel",
                ]
            ),
        )

    def test_diff_config_appends_incremental_keys(self) -> None:
        config = mt.diff_config("includePaths:\n  - 'x'\n", DIFF_REF, ROOT)
        self.assertEqual(
            "includePaths:\n  - 'x'\n"
            'gitDiffRef: "origin/master"\ngitProjectRoot: "/work/FastECU"\n',
            config,
        )

    def test_tests_expression_excludes_fuzz_harnesses(self) -> None:
        self.assertEqual(
            'kind(cc_test, //src/a/...) except attr(tags, "\\bfuzz\\b", //src/a/...)',
            mt.tests_expression("//src/a/...", None),
        )

    def test_tests_expression_narrows_to_dependents_of_the_change(self) -> None:
        self.assertEqual(
            '(kind(cc_test, //src/a/...) except attr(tags, "\\bfuzz\\b", //src/a/...))'
            " intersect rdeps(//src/a/..., set(src/a/x.cpp src/a/y.h))",
            mt.tests_expression("//src/a/...", ["src/a/x.cpp", "src/a/y.h"]),
        )

    def test_binary_path_follows_the_label(self) -> None:
        self.assertEqual(
            ROOT / "bazel-bin/src/algorithms/checksum/checksum_test",
            mt.binary_path(ROOT, "//src/algorithms/checksum:checksum_test"),
        )


class RunnerCommandTest(unittest.TestCase):
    def test_allows_slow_original_runs_and_surviving_mutants(self) -> None:
        binary = ROOT / "bazel-bin/src/algorithms/checksum/checksum_test"
        report_dir = ROOT / "mutation-reports"
        self.assertEqual(
            [
                "mull-runner-22",
                "--reporters",
                "Elements",
                "--report-dir",
                str(report_dir),
                "--report-name",
                "checksum_test",
                "--timeout",
                str(mt.ORIGINAL_RUN_TIMEOUT_MS),
                "--allow-surviving",
                "--no-test-output",
                "--no-mutant-output",
                str(binary),
            ],
            mt.runner_command("mull-runner-22", binary, report_dir),
        )


class TestEnvironmentTest(unittest.TestCase):
    def test_build_and_cquery_share_one_configuration(self) -> None:
        flags = mt.mutation_flags("/usr/lib/mull-ir-frontend-22", ROOT / "mull.yml")
        self.assertEqual(
            [
                "--config=mutation",
                "--copt=-fpass-plugin=/usr/lib/mull-ir-frontend-22",
                f"--action_env=MULL_CONFIG={ROOT / 'mull.yml'}",
            ],
            flags,
        )
        self.assertEqual(
            [
                "bazel",
                "cquery",
                *flags,
                "--output=starlark",
                f"--starlark:expr={mt.TEST_ENV_EXPR}",
                "--",
                "set(//a:t //b:u)",
            ],
            mt.env_query_command(["//a:t", "//b:u"], flags),
        )

    def test_parses_each_targets_env_by_its_label(self) -> None:
        output = (
            '{"env":{"KERNEL_FILES":"resources/a.bin resources/b.bin"},"label":"@@//src/a:t"}\n'
            '{"env":{},"label":"@@//src/b:u"}\n'
        )
        self.assertEqual(
            {"//src/a:t": {"KERNEL_FILES": "resources/a.bin resources/b.bin"}, "//src/b:u": {}},
            mt.parse_test_environments(output),
        )

    def test_rejects_output_that_is_not_one_json_object_per_line(self) -> None:
        with self.assertRaises(mt.MutationTestError):
            mt.parse_test_environments("Loading: 0 packages loaded\n")

    def test_runner_environment_adds_the_test_env_and_mull_config(self) -> None:
        environ = mt.runner_environment(
            {"PATH": "/usr/bin", "KERNEL_FILES": "stale", "MULL_CONFIG": "stale"},
            {"KERNEL_FILES": "resources/a.bin"},
            ROOT / "mull.yml",
        )
        self.assertEqual(
            {
                "PATH": "/usr/bin",
                "KERNEL_FILES": "resources/a.bin",
                "MULL_CONFIG": str(ROOT / "mull.yml"),
            },
            environ,
        )


class ShardTest(unittest.TestCase):
    def test_parses_an_index_and_a_count(self) -> None:
        self.assertEqual(mt.Shard(1, 4), mt.parse_shard("1/4"))

    def test_rejects_an_index_outside_the_count(self) -> None:
        for value in ("4/4", "0/0", "-1/4", "1", "a/b"):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                mt.parse_shard(value)

    def test_takes_every_nth_label_of_the_sorted_list(self) -> None:
        labels = ["//e:t", "//b:t", "//d:t", "//a:t", "//c:t"]
        self.assertEqual(["//b:t", "//d:t"], mt.Shard(1, 2).select(labels))
        self.assertEqual(["//a:t", "//c:t", "//e:t"], mt.Shard(0, 2).select(labels))

    def test_a_shard_past_the_last_label_selects_nothing(self) -> None:
        self.assertEqual([], mt.Shard(3, 4).select(["//a:t", "//b:t"]))


class MergeShardsTest(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)

    def _shard(self, index: int, count: int, *reports: tuple[str, dict]) -> None:
        directory = self.root / f"mutation-report-shard-{index}"
        directory.mkdir()
        for name, report in reports:
            (directory / f"{name}.json").write_text(json.dumps(report))
            (directory / f"{name}.html").write_text("<html></html>")
        mt.write_shard_manifest(directory, mt.Shard(index, count), [f"//a:{n}" for n, _ in reports])

    def test_collects_every_shards_reports_but_not_their_manifests(self) -> None:
        first = _report(ABSOLUTE_SOURCE, _mutant(3, "Killed"))
        second = _report(ABSOLUTE_SOURCE, _mutant(4, "Survived"))
        self._shard(0, 2, ("x_test", first))
        self._shard(1, 2, ("y_test", second))

        reports, missing = mt.collect_shard_reports(self.root, 2)

        self.assertCountEqual([first, second], reports)
        self.assertEqual([], missing)

    def test_an_empty_shard_that_finished_is_not_missing(self) -> None:
        self._shard(0, 2, ("x_test", _report(ABSOLUTE_SOURCE)))
        self._shard(1, 2)

        self.assertEqual([], mt.collect_shard_reports(self.root, 2)[1])

    def test_a_shard_without_a_manifest_is_missing(self) -> None:
        # A shard writes its manifest last, so one that failed or was
        # cancelled mid-run leaves reports but no manifest.
        self._shard(0, 3, ("x_test", _report(ABSOLUTE_SOURCE)))
        partial = self.root / "mutation-report-shard-2"
        partial.mkdir()
        (partial / "z_test.json").write_text(json.dumps(_report(ABSOLUTE_SOURCE)))

        self.assertEqual([1, 2], mt.collect_shard_reports(self.root, 3)[1])

    def test_rejects_a_manifest_from_a_differently_sharded_run(self) -> None:
        self._shard(0, 4)
        with self.assertRaises(mt.MutationTestError):
            mt.collect_shard_reports(self.root, 2)

    def test_the_summary_names_the_missing_shards(self) -> None:
        self.assertEqual(
            "**Incomplete:** shards 1, 2 of 4 did not finish; their tests are missing below.\n",
            mt.render_missing_shards([1, 2], 4),
        )


class ParseArgsTest(unittest.TestCase):
    def _rejects(self, *argv: str) -> None:
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            mt._parse_args(list(argv))

    def test_a_shard_needs_a_report_dir(self) -> None:
        self._rejects("--shard", "0/4")

    def test_the_changed_lines_run_is_not_sharded(self) -> None:
        self._rejects("--shard", "0/4", "--report-dir", "r", "--diff-ref", DIFF_REF)

    def test_a_merge_needs_the_shard_count(self) -> None:
        self._rejects("--merge-reports", "r")

    def test_accepts_a_scoped_shard(self) -> None:
        args = mt._parse_args(["--shard", "2/4", "--report-dir", "r", "--scope", "//src/a/..."])
        self.assertEqual(mt.Shard(2, 4), args.shard)


class LogGroupTest(unittest.TestCase):
    def test_titles_a_binary_with_its_position(self) -> None:
        self.assertEqual("::group::[3/48] //src/a:t", mt.group_title(3, 48, "//src/a:t"))


if __name__ == "__main__":
    unittest.main()
