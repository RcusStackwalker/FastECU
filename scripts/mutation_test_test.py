#!/usr/bin/env python3

import argparse
import unittest
from pathlib import Path

import mutation_test as mt

ROOT = Path("/work/FastECU")
SOURCE = "src/algorithms/a.cpp"
ABSOLUTE_SOURCE = f"/work/FastECU/{SOURCE}"
SHORT = "src/a.cpp"
TITLE = "Mutation testing"


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
        self.assertEqual("origin/master", mt.operand("origin/master"))

    def test_rejects_an_option_like_value(self) -> None:
        with self.assertRaises(argparse.ArgumentTypeError):
            mt.operand("--output=/etc/passwd")

    def test_rejects_an_empty_value(self) -> None:
        with self.assertRaises(argparse.ArgumentTypeError):
            mt.operand("")


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
        config = mt.diff_config("includePaths:\n  - 'x'\n", "origin/master", ROOT)
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
        self.assertEqual(
            [
                "mull-runner-22",
                "--reporters",
                "Elements",
                "--report-dir",
                "/tmp/mutation",
                "--report-name",
                "checksum_test",
                "--timeout",
                str(mt.ORIGINAL_RUN_TIMEOUT_MS),
                "--allow-surviving",
                "--no-test-output",
                "--no-mutant-output",
                str(binary),
            ],
            mt.runner_command("mull-runner-22", binary, Path("/tmp/mutation")),
        )


if __name__ == "__main__":
    unittest.main()
