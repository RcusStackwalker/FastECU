#!/usr/bin/env python3

import argparse
import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path, PureWindowsPath

import mutation_test as mt

ROOT = Path("/work/FastECU")
SOURCE = "src/algorithms/a.cpp"
ABSOLUTE_SOURCE = f"/work/FastECU/{SOURCE}"
SHORT = "src/a.cpp"
TITLE = "Mutation testing"
DIFF_REF = "origin/master"
LABEL_A = "//a:t"
LABEL_B = "//b:t"
LABEL_C = "//c:t"
PACKAGE_SCOPE = "//src/a/..."


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
    def test_strips_a_windows_workspace_root(self) -> None:
        self.assertEqual(
            SOURCE,
            mt.relative_source_path(
                r"C:\work\FastECU\src\algorithms\a.cpp", PureWindowsPath(r"C:\work\FastECU")
            ),
        )

    def test_strips_a_windows_bazel_execroot(self) -> None:
        self.assertEqual(
            SOURCE,
            mt.relative_source_path(r"D:\_bazel\execroot\_main\src\algorithms\a.cpp", ROOT),
        )

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
        self.assertEqual(PACKAGE_SCOPE, mt.target_pattern(PACKAGE_SCOPE))
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

    def test_new_cli_paths_use_the_same_checkout_and_temp_policy(self) -> None:
        outside = "/etc/fastecu-mutation-test-nonexistent"
        for argv in (
            ["--schedule", outside],
            ["--schedule", "bazel/mutation-schedule.json", "--refresh-schedule", outside],
            ["--merge-reports", ".", "--shards", "1", "--baseline-reports", outside],
        ):
            errors = io.StringIO()
            with self.subTest(argv=argv), contextlib.redirect_stderr(errors):
                self.assertEqual(2, mt.main(argv))
                self.assertIn("outside the checkout", errors.getvalue())


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
            mt.tests_expression(PACKAGE_SCOPE, None),
        )

    def test_tests_expression_narrows_to_dependents_of_the_change(self) -> None:
        self.assertEqual(
            '(kind(cc_test, //src/a/...) except attr(tags, "\\bfuzz\\b", //src/a/...))'
            " intersect rdeps(//src/a/..., set(src/a/x.cpp src/a/y.h))",
            mt.tests_expression(PACKAGE_SCOPE, ["src/a/x.cpp", "src/a/y.h"]),
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
                "--minimum-timeout",
                "1000",
                "--allow-surviving",
                "--no-test-output",
                "--no-mutant-output",
                str(binary),
                "--gtest_fail_fast",
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
            mt.env_query_command([LABEL_A, "//b:u"], flags),
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
        labels = ["//e:t", LABEL_B, "//d:t", LABEL_A, LABEL_C]
        self.assertEqual([LABEL_B, "//d:t"], mt.Shard(1, 2).select(labels))
        self.assertEqual([LABEL_A, LABEL_C, "//e:t"], mt.Shard(0, 2).select(labels))

    def test_a_shard_past_the_last_label_selects_nothing(self) -> None:
        self.assertEqual([], mt.Shard(3, 4).select([LABEL_A, LABEL_B]))


class ScheduleTest(unittest.TestCase):
    def test_splits_slow_binaries_and_balances_all_work_without_loss(self) -> None:
        schedule = {LABEL_A: {"seconds": 100, "test_shards": 2}, LABEL_B: {"seconds": 40}}
        work = mt.plan_work([LABEL_C, LABEL_B, LABEL_A], schedule)
        shards = [mt.balance_work(work, mt.Shard(i, 2)) for i in range(2)]
        self.assertCountEqual(work, shards[0] + shards[1])
        self.assertEqual([90, 51], [sum(item.seconds for item in shard) for shard in shards])
        self.assertEqual(
            [mt.WorkItem(LABEL_A, 0, 2, 50), mt.WorkItem(LABEL_A, 1, 2, 50)],
            [item for item in work if item.label == LABEL_A],
        )

    def test_selection_is_deterministic_and_new_labels_are_never_dropped(self) -> None:
        first = mt.plan_work([LABEL_B, LABEL_A], {})
        second = mt.plan_work([LABEL_A, LABEL_B], {})
        self.assertEqual(first, second)
        self.assertEqual([LABEL_A, LABEL_B], [item.label for item in first])
        self.assertEqual(first, mt.balance_work(first, mt.Shard(0, 1)))

    def test_rejects_invalid_estimates_and_shard_counts(self) -> None:
        for entry in (
            {"seconds": -1},
            {"seconds": float("nan")},
            {"seconds": 1, "test_shards": 0},
            {"seconds": 1, "test_shards": 1.5},
        ):
            with self.subTest(entry=entry), self.assertRaises(mt.MutationTestError):
                mt.plan_work([LABEL_A], {LABEL_A: entry})

    def test_work_items_use_distinct_report_directories_and_google_test_slices(self) -> None:
        first = mt.WorkItem(LABEL_A, 0, 2, 1)
        second = mt.WorkItem(LABEL_B, 0, 2, 1)
        self.assertNotEqual(first.report_path(ROOT), second.report_path(ROOT))
        self.assertEqual(ROOT / "a/t/gtest-0-of-2", first.report_path(ROOT))
        self.assertEqual({"GTEST_TOTAL_SHARDS": "2", "GTEST_SHARD_INDEX": "0"}, first.environment())
        self.assertEqual({}, mt.WorkItem(LABEL_A).environment())


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

    def test_does_not_parse_performance_metadata_as_a_mutation_report(self) -> None:
        self._shard(0, 1, ("x_test", _report(ABSOLUTE_SOURCE)))
        (self.root / mt.PERFORMANCE_REPORT).write_text('{"binaries": []}')
        self.assertEqual(1, len(mt.collect_shard_reports(self.root, 1)[0]))

    def test_rejects_duplicate_shard_manifests(self) -> None:
        self._shard(0, 2)
        duplicate = self.root / "duplicate"
        duplicate.mkdir()
        mt.write_shard_manifest(duplicate, mt.Shard(0, 2), [])
        with self.assertRaises(mt.MutationTestError):
            mt.collect_shard_reports(self.root, 2)

    def test_rejects_a_completed_run_with_missing_or_duplicate_work(self) -> None:
        for labels in ([LABEL_A], [LABEL_A, LABEL_B, LABEL_B]):
            with self.subTest(labels=labels):
                for index in range(2):
                    directory = self.root / f"mutation-report-shard-{index}"
                    directory.mkdir(exist_ok=True)
                    mt.write_shard_manifest(
                        directory,
                        mt.Shard(index, 2),
                        labels if index == 0 else [],
                        selection=[LABEL_A, LABEL_B],
                    )
                with self.assertRaises(mt.MutationTestError):
                    mt.collect_shard_reports(self.root, 2)


class PerformanceTest(unittest.TestCase):
    def test_summary_keeps_timeouts_visible_and_ranks_expensive_work(self) -> None:
        performance = {
            "build_seconds": 20,
            "query_seconds": 2,
            "binaries": [
                {"id": LABEL_A, "seconds": 3, "status": "complete"},
                {"id": LABEL_B, "seconds": 50, "status": "complete"},
            ],
        }
        summary = mt.render_performance([performance])
        self.assertIn("53.0s", summary)
        self.assertLess(summary.index(LABEL_B), summary.index(LABEL_A))

    def test_summary_reports_timeouts_separately_without_changing_score(self) -> None:
        merged = {mt.MutantKey(SHORT, 1, 2, "m", "r"): "Timeout"}
        self.assertIn("1 timed out", mt.render_summary(merged, TITLE))

    def test_comparison_exposes_missing_mutants_lost_kills_and_new_timeouts(self) -> None:
        keys = [mt.MutantKey(SHORT, line, 2, "m", "r") for line in range(4)]
        baseline = dict(zip(keys[:3], ["Killed", "Killed", "Survived"], strict=True))
        current = {keys[0]: "Timeout", keys[2]: "Survived", keys[3]: "Killed"}
        summary = mt.render_comparison(current, baseline)
        self.assertIn("1 missing", summary)
        self.assertIn("1 added", summary)
        self.assertIn("2 lost direct kills", summary)
        self.assertIn("1 new timeouts", summary)

    def test_comparison_exposes_survivors_previously_hidden_by_timeouts(self) -> None:
        key = mt.MutantKey(SHORT, 1, 2, "m", "r")
        summary = mt.render_comparison({key: "Survived"}, {key: "Timeout"})
        self.assertIn("Additional survivors: **1**", summary)

    def test_refresh_uses_measured_slice_costs_and_preserves_other_scopes(self) -> None:
        schedule = {"tests": {LABEL_B: {"seconds": 5}}}
        reports = [
            {
                "run_id": "42",
                "binaries": [
                    {
                        "label": LABEL_A,
                        "test_shard": 1,
                        "test_shards": 2,
                        "seconds": 30,
                        "status": "complete",
                    },
                    {
                        "label": LABEL_A,
                        "test_shard": 0,
                        "test_shards": 2,
                        "seconds": 10,
                        "status": "complete",
                    },
                ],
            }
        ]
        updated = mt.refresh_schedule(schedule, reports)
        self.assertEqual({"seconds": 5}, updated["tests"][LABEL_B])
        self.assertEqual(
            {"seconds": 40, "test_shards": 2, "shard_seconds": [10, 30]}, updated["tests"][LABEL_A]
        )
        work = mt.plan_work([LABEL_A], updated["tests"])
        self.assertEqual([10, 30], [item.seconds for item in work])

    def test_refresh_rejects_incomplete_slices(self) -> None:
        reports = [
            {
                "run_id": "42",
                "binaries": [
                    {
                        "label": LABEL_A,
                        "test_shard": 0,
                        "test_shards": 2,
                        "seconds": 10,
                        "status": "complete",
                    },
                ],
            }
        ]
        with self.assertRaises(mt.MutationTestError):
            mt.refresh_schedule({"tests": {}}, reports)


class OrchestrationTest(unittest.TestCase):
    def test_runs_every_slice_and_merges_reports_without_metadata_polluting_counts(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            (root / "scripts").mkdir()
            shutil.copyfile(mt.__file__, root / "scripts/mutation_test.py")
            (root / "mull.yml").write_text("includePaths: []\n")
            tools = root / "tools"
            tools.mkdir()
            bazel = tools / "bazel"
            bazel.write_text(
                """import json, pathlib, sys
if sys.argv[1] == "query":
    print("//a:t")
elif sys.argv[1] == "build":
    prefix = "--action_env=MULL_CONFIG="
    config = next(arg.removeprefix(prefix) for arg in sys.argv if arg.startswith(prefix))
    assert pathlib.Path(config).parent.resolve() == pathlib.Path(__file__).resolve().parent.parent
elif sys.argv[1] == "cquery":
    print(json.dumps({"label": "//a:t", "env": {"FIXTURE": "available"}}))
"""
            )
            runner = tools / "mull-runner-22"
            runner.write_text(
                """import json, os, pathlib, sys
assert os.environ["FIXTURE"] == "available"
assert os.environ["GTEST_TOTAL_SHARDS"] == "2"
directory = pathlib.Path(sys.argv[sys.argv.index("--report-dir") + 1])
name = sys.argv[sys.argv.index("--report-name") + 1]
index = int(os.environ["GTEST_SHARD_INDEX"])
mutant = {"mutatorName": "m", "replacement": "r", "status": "Killed",
          "location": {"start": {"line": index + 1, "column": 1}}}
(directory / (name + ".json")).write_text(json.dumps({"files": {
    "src/algorithms/a.cpp": {"mutants": [mutant]}}}))
"""
            )
            plugin = tools / "mull-ir-frontend-22"
            plugin.touch()
            # Explicit Python launchers keep the subprocess fixture portable:
            # Windows cannot execute an extensionless script via a shebang.
            driver = root / "scripts/drive_mutation.py"
            driver.write_text(
                """import importlib.util, pathlib, sys
source = pathlib.Path(__file__).with_name("mutation_test.py")
spec = importlib.util.spec_from_file_location("mutation_fixture", source)
mt = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = mt
spec.loader.exec_module(mt)
run = mt._run
def launch(command, root, **kwargs):
    tool = pathlib.Path(__file__).parent.parent / "tools" / pathlib.Path(command[0]).name
    if tool.is_file():
        command = [sys.executable, str(tool), *command[1:]]
    return run(command, root, **kwargs)
mt._run = launch
sys.exit(mt.main())
"""
            )
            schedule = root / "schedule.json"
            schedule.write_text(json.dumps({"tests": {LABEL_A: {"seconds": 10, "test_shards": 2}}}))
            environment = {
                **os.environ,
                "PYTHONSAFEPATH": "1",
                "PYTHONPATH": str(Path(mt.__file__).parent),
                "MULL_RUNNER": str(runner),
                "MULL_IR_FRONTEND": str(plugin),
            }
            script = [sys.executable, str(driver)]
            reports = root / "reports"
            for index in range(2):
                result = subprocess.run(
                    [
                        *script,
                        "--shard",
                        f"{index}/2",
                        "--report-dir",
                        str(reports / str(index)),
                        "--schedule",
                        str(schedule),
                    ],
                    env=environment,
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            reused = subprocess.run(
                [
                    *script,
                    "--shard",
                    "0/2",
                    "--report-dir",
                    str(reports / "0"),
                    "--schedule",
                    str(schedule),
                ],
                env=environment,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(2, reused.returncode, reused.stdout + reused.stderr)
            self.assertIn("report directory must be empty", reused.stderr)
            result = subprocess.run(
                [*script, "--merge-reports", str(reports), "--shards", "2"],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertIn("2 killed, 0 survived", result.stdout)
            self.assertIn("### Performance", result.stdout)
            self.assertEqual(2, len(list(reports.rglob(mt.PERFORMANCE_REPORT))))
            refresh = [*script, "--refresh-schedule", str(reports), "--schedule", str(schedule)]
            result = subprocess.run(refresh, text=True, capture_output=True, check=False)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            (reports / "1" / mt.SHARD_MANIFEST).unlink()
            result = subprocess.run(refresh, text=True, capture_output=True, check=False)
            self.assertEqual(2, result.returncode, result.stdout + result.stderr)
            self.assertIn("incomplete run", result.stderr)

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
        args = mt._parse_args(["--shard", "2/4", "--report-dir", "r", "--scope", PACKAGE_SCOPE])
        self.assertEqual(mt.Shard(2, 4), args.shard)


class LogGroupTest(unittest.TestCase):
    def test_titles_a_binary_with_its_position(self) -> None:
        self.assertEqual("::group::[3/48] //src/a:t", mt.group_title(3, 48, "//src/a:t"))


if __name__ == "__main__":
    unittest.main()
