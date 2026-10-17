#!/usr/bin/env python3

import json
import tempfile
import unittest
from collections.abc import Sequence
from io import StringIO
from pathlib import Path

import nloc_loop as nl

_FUNCTIONS = """\
================================================
  NLOC    CCN   token  PARAM  length  location
------------------------------------------------
       3      1      8      0       3 f@10-12@./src/a/a.cpp
"""


def lizard_report(files: dict[str, int], total: int | None = None) -> str:
    """A Lizard report as the `nloc` prek hook prints it."""
    rows = "".join(
        f"{n:>9}       1.0     1.0        8.0         1     ./{p}\n" for p, n in files.items()
    )
    total = sum(files.values()) if total is None else total
    return (
        "C/C++ NLOC report........................................................Passed\n"
        "- hook id: nloc\n"
        "- duration: 3.97s\n\n" + _FUNCTIONS + "1 file analyzed.\n"
        "==============================================================\n"
        "  NLOC    Avg.NLOC  AvgCCN  Avg.token  function_cnt    file\n"
        "--------------------------------------------------------------\n" + rows + "\n"
        "=========================================================================\n"
        "Total nloc   Avg.NLOC  AvgCCN  Avg.token   Fun Cnt  Warning cnt   Fun Rt   nloc Rt\n"
        "-------------------------------------------------------------------------\n"
        f"    {total}      14.4     2.2      116.3     8642            0      0.00    0.00\n"
    )


class ParseReportTest(unittest.TestCase):
    def test_reads_every_file_row(self) -> None:
        report = lizard_report({"src/a/a.cpp": 12, "src/a/a.h": 0, "apps/b.cpp": 5})
        self.assertEqual(
            nl.parse_report(report), {"src/a/a.cpp": 12, "src/a/a.h": 0, "apps/b.cpp": 5}
        )

    def test_rejects_rows_that_disagree_with_the_total(self) -> None:
        with self.assertRaisesRegex(nl.NlocLoopError, "sum to 12.*total is 13"):
            nl.parse_report(lizard_report({"src/a.cpp": 12}, total=13))

    def test_rejects_a_report_without_a_file_table(self) -> None:
        with self.assertRaisesRegex(nl.NlocLoopError, "file table"):
            nl.parse_report(_FUNCTIONS)

    def test_rejects_a_report_without_a_total(self) -> None:
        report = lizard_report({"src/a.cpp": 12}).split("=====\nTotal nloc")[0]
        with self.assertRaisesRegex(nl.NlocLoopError, "total"):
            nl.parse_report(report)


class ClassifyTest(unittest.TestCase):
    def test_test_code(self) -> None:
        for path in (
            "tests/serial_pty_e2e_test.cpp",
            "tests/tst_serial_port_crash.cpp",
            "src/backend/flash/ecu/executor_test.cpp",
            "src/backend/flash/ecu/executor_test.h",
            "src/backend/flash/testing/mock_transport.h",
            "src/algorithms/checksum/testdata/fixture.h",
            "apps/desktop/startup_test_platform_macos.cpp",
        ):
            with self.subTest(path=path):
                self.assertEqual(nl.classify(path), nl.TEST)

    def test_production_code(self) -> None:
        for path in ("src/backend/flash/ecu/executor.cpp", "apps/bench/bench_format.h"):
            with self.subTest(path=path):
                self.assertEqual(nl.classify(path), nl.PRODUCTION)

    def test_outside_the_metric(self) -> None:
        for path in ("bazel/qt/shim.cpp", "resources/shared/x_test.cpp", "srcx/a.cpp"):
            with self.subTest(path=path):
                self.assertIsNone(nl.classify(path))


class MeasureTest(unittest.TestCase):
    def test_totals_only_in_scope_files(self) -> None:
        files = {"src/a.cpp": 10, "src/a_test.cpp": 4, "tests/b.cpp": 3, "bazel/c.cpp": 100}
        self.assertEqual(nl.measure_files(files), nl.Measurement(production=10, test=7))
        self.assertEqual(nl.measure_files(files).total, 17)

    def test_runs_the_pinned_hook_at_the_root(self) -> None:
        calls: list[Path] = []

        def run_hook(root: Path) -> str:
            calls.append(root)
            return lizard_report({"src/a.cpp": 10})

        self.assertEqual(nl.measure(Path("/repo"), run_hook), nl.Measurement(10, 0))
        self.assertEqual(calls, [Path("/repo")])


class LedgerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.path = Path(self.temp_dir.name) / ".nloc-loop" / "ledger.json"

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_missing_ledger_is_empty(self) -> None:
        self.assertEqual(nl.Ledger.load(self.path).attempts, [])

    def test_records_survive_a_reload(self) -> None:
        ledger = nl.Ledger.load(self.path)
        ledger.record(nl.Attempt("c1", "rejected", "2026-10-17", reason="mutation"))
        ledger.save(self.path)
        self.assertEqual(
            nl.Ledger.load(self.path).attempts,
            [nl.Attempt("c1", "rejected", "2026-10-17", reason="mutation")],
        )

    def test_later_attempt_supersedes_the_outcome(self) -> None:
        ledger = nl.Ledger([])
        ledger.record(nl.Attempt("c1", "open", "2026-10-17", pr=700))
        ledger.record(nl.Attempt("c1", "merged", "2026-10-18", pr=700))
        self.assertEqual(ledger.outcome("c1"), "merged")
        self.assertIsNone(ledger.outcome("c2"))

    def test_counts_trailing_rejections(self) -> None:
        ledger = nl.Ledger([])
        for cluster, outcome in (("a", "rejected"), ("b", "merged"), ("c", "rejected")):
            ledger.record(nl.Attempt(cluster, outcome, "2026-10-17"))
        self.assertEqual(ledger.consecutive_rejections(), 1)
        ledger.record(nl.Attempt("d", "rejected", "2026-10-17"))
        self.assertEqual(ledger.consecutive_rejections(), 2)

    def test_rejects_an_unknown_outcome(self) -> None:
        with self.assertRaisesRegex(nl.NlocLoopError, "outcome"):
            nl.Attempt("c1", "abandoned", "2026-10-17")

    def test_rejects_a_corrupt_ledger(self) -> None:
        self.path.parent.mkdir(parents=True)
        self.path.write_text("{not json")
        with self.assertRaisesRegex(nl.NlocLoopError, "ledger"):
            nl.Ledger.load(self.path)


class TrendTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.path = Path(self.temp_dir.name) / "nloc-trend.csv"

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_creates_the_header_then_appends(self) -> None:
        nl.append_trend(self.path, nl.TrendRow("2026-10-17", 700, "test-only", 152613, 152400))
        nl.append_trend(self.path, nl.TrendRow("2026-10-18", 701, "production", 152400, 152300))
        self.assertEqual(
            self.path.read_text(),
            "date,pr,kind,nloc_before,nloc_after\n"
            "2026-10-17,700,test-only,152613,152400\n"
            "2026-10-18,701,production,152400,152300\n",
        )

    def test_rejects_a_row_that_does_not_lower_nloc(self) -> None:
        with self.assertRaisesRegex(nl.NlocLoopError, "lower"):
            nl.append_trend(self.path, nl.TrendRow("2026-10-17", 700, "test-only", 100, 100))
        self.assertFalse(self.path.exists())


_ROOT = Path("/repo")


def clone(a: str, a_lines: tuple[int, int], b: str, b_lines: tuple[int, int], text="x") -> dict:
    """One jscpd duplicate pair with absolute paths, as the `jscpd` hook reports it."""

    def side(path: str, lines: tuple[int, int]) -> dict:
        return {"name": str(_ROOT / path), "start": lines[0], "end": lines[1]}

    return {"firstFile": side(a, a_lines), "secondFile": side(b, b_lines), "fragment": text}


_A = "src/flash/a_test.cpp"
_B = "src/flash/b_test.cpp"
_C = "src/flash/c_test.cpp"
_PROD = "src/flash/a.cpp"


def find(duplicates, host=None, ledger=None) -> list:
    host = {_A, _B, _C, _PROD} if host is None else host
    return nl.find_candidates({"duplicates": duplicates}, _ROOT, host, ledger or nl.Ledger([]))


class CandidatesTest(unittest.TestCase):
    def test_pairs_sharing_a_fragment_form_one_cluster(self) -> None:
        [cluster] = find([clone(_A, (1, 60), _B, (5, 64)), clone(_B, (5, 64), _C, (9, 68))])
        self.assertEqual(
            [(o.path, o.start, o.end) for o in cluster.occurrences],
            [(_A, 1, 60), (_B, 5, 64), (_C, 9, 68)],
        )
        self.assertEqual((cluster.kind, cluster.savings), ("test-only", 2 * 60))

    def test_overlapping_fragments_in_one_file_merge(self) -> None:
        [cluster] = find([clone(_A, (1, 60), _B, (1, 60)), clone(_A, (50, 120), _C, (1, 71))])
        self.assertEqual(len(cluster.occurrences), 3)
        # Conservative: the shortest occurrence bounds what each removal saves.
        self.assertEqual(cluster.savings, 2 * 60)

    def test_savings_below_the_minimum_are_dropped(self) -> None:
        self.assertEqual(find([clone(_A, (1, 49), _B, (1, 49))]), [])
        self.assertEqual(len(find([clone(_A, (1, 50), _B, (1, 50))])), 1)

    def test_files_not_built_on_the_host_are_dropped(self) -> None:
        self.assertEqual(find([clone(_A, (1, 60), _B, (1, 60))], host={_A}), [])
        [cluster] = find(
            [clone(_A, (1, 60), _B, (1, 60)), clone(_B, (1, 60), _C, (1, 60))], host={_A, _B}
        )
        self.assertEqual([o.path for o in cluster.occurrences], [_A, _B])

    def test_any_production_occurrence_makes_a_production_cluster(self) -> None:
        [cluster] = find([clone(_A, (1, 60), _PROD, (1, 60))])
        self.assertEqual(cluster.kind, "production")

    def test_test_only_clusters_come_first_and_by_savings(self) -> None:
        clusters = find(
            [
                clone(_PROD, (1, 500), _PROD, (600, 1099)),
                clone(_A, (1, 60), _B, (1, 60)),
                clone(_A, (100, 199), _C, (100, 199)),
            ]
        )
        self.assertEqual(
            [(c.kind, c.savings) for c in clusters],
            [("test-only", 100), ("test-only", 60), ("production", 500)],
        )

    def test_identity_ignores_line_numbers_and_whitespace(self) -> None:
        [before] = find([clone(_A, (1, 60), _B, (1, 60), text="int  a;\n  f();")])
        [after] = find([clone(_A, (31, 90), _B, (7, 66), text="int a; f();")])
        self.assertEqual(before.cluster_id, after.cluster_id)
        [other] = find([clone(_A, (1, 60), _C, (1, 60), text="int a; f();")])
        self.assertNotEqual(before.cluster_id, other.cluster_id)

    def test_attempted_clusters_are_dropped(self) -> None:
        [cluster] = find([clone(_A, (1, 60), _B, (1, 60))])
        ledger = nl.Ledger([nl.Attempt(cluster.cluster_id, "rejected", "2026-10-17")])
        self.assertEqual(find([clone(_A, (1, 60), _B, (1, 60))], ledger=ledger), [])

    def test_rejects_a_report_outside_the_root(self) -> None:
        duplicate = clone(_A, (1, 60), _B, (1, 60))
        duplicate["firstFile"]["name"] = "/elsewhere/a.cpp"
        with self.assertRaisesRegex(nl.NlocLoopError, "outside"):
            find([duplicate])


class HostFilesTest(unittest.TestCase):
    def test_reads_first_party_source_labels(self) -> None:
        output = (
            "//src/backend/config:catalog.h (null)\n"
            "//:BUILD.bazel (null)\n"
            "//tests:tst_serial_port_crash.cpp (null)\n"
            "@@rules_qt++fetch+qt//:lib/QtGui.h (null)\n"
        )
        self.assertEqual(
            nl.parse_host_files(output),
            {"src/backend/config/catalog.h", "BUILD.bazel", "tests/tst_serial_port_crash.cpp"},
        )


def coverage_report(root: Path, files: dict[str, dict[int, str]]) -> str:
    """`llvm-cov show` output after coverage-local.sh rewrote headers to absolute paths."""
    sections = []
    for path, lines in files.items():
        body = "".join(f"{n:>5}|{count:>7}|  code\n" for n, count in sorted(lines.items()))
        sections.append(f"{root / path}:\n{body}")
    return "\n".join(sections)


def gtest_xml(*cases: str) -> str:
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n<testsuites><testsuite name="s">'
        + "".join(cases)
        + "</testsuite></testsuites>\n"
    )


_RUN = '<testcase name="a" status="run" result="completed"/>'
_SKIPPED = '<testcase name="b" status="run" result="skipped"><skipped/></testcase>'
_DISABLED = '<testcase name="DISABLED_c" status="notrun" result="suppressed"/>'
_BAZEL = '<testcase name="//:py_test" status="run" duration="1" time="1"></testcase>'


class CoverageParseTest(unittest.TestCase):
    def test_covered_and_executable_lines(self) -> None:
        report = coverage_report(_ROOT, {_PROD: {1: "", 2: "3", 3: "0", 4: "10.2k", 5: "1.5M"}})
        self.assertEqual(
            nl.parse_coverage(report, _ROOT),
            {_PROD: nl.FileCoverage(covered=frozenset({2, 4, 5}), executable=4)},
        )

    def test_ignores_instantiation_subviews(self) -> None:
        report = (
            coverage_report(_ROOT, {_PROD: {1: "2"}})
            + "  ------------------\n  | f.cpp:_Z1fv:\n  |    7|      0|  code\n"
        )
        self.assertEqual(nl.parse_coverage(report, _ROOT)[_PROD].executable, 1)

    def test_rejects_sections_outside_the_root(self) -> None:
        with self.assertRaisesRegex(nl.NlocLoopError, "outside"):
            nl.parse_coverage("/elsewhere/a.cpp:\n    1|      1|  code\n", _ROOT)


class TestCaseCountTest(unittest.TestCase):
    def test_counts_executed_cases_only(self) -> None:
        xmls = [gtest_xml(_RUN, _SKIPPED, _DISABLED, _RUN), gtest_xml(_BAZEL)]
        self.assertEqual(nl.count_test_cases(xmls), 3)

    def test_rejects_malformed_xml(self) -> None:
        with self.assertRaisesRegex(nl.NlocLoopError, "test.xml"):
            nl.count_test_cases(["<testsuites>"])


def snapshot(test_cases: int, coverage: dict[str, tuple[set[int], int]]) -> nl.Snapshot:
    return nl.Snapshot(
        "c0ffee",
        test_cases,
        {p: nl.FileCoverage(frozenset(lines), n) for p, (lines, n) in coverage.items()},
    )


class GuardsTest(unittest.TestCase):
    def failures(self, base, head, kind, changed) -> list[str]:
        return [r.name for r in nl.check_guards(base, head, kind, changed) if not r.ok]

    def test_test_only_passes_when_coverage_and_cases_are_kept(self) -> None:
        base = snapshot(10, {_PROD: ({1, 2}, 4), _A: ({1}, 9)})
        head = snapshot(10, {_PROD: ({1, 2, 3}, 4), _A: (set(), 2)})
        self.assertEqual(self.failures(base, head, "test-only", [_A, "BUILD.bazel"]), [])

    def test_test_only_fails_on_a_lost_covered_line(self) -> None:
        base = snapshot(10, {_PROD: ({1, 2}, 4)})
        head = snapshot(10, {_PROD: ({1, 3}, 4)})
        results = nl.check_guards(base, head, "test-only", [_A])
        [lost] = [r for r in results if not r.ok]
        self.assertEqual(lost.name, "covered lines")
        self.assertIn(f"{_PROD}: 2", lost.detail)

    def test_test_only_fails_when_a_production_file_changed(self) -> None:
        base = snapshot(10, {_PROD: ({1}, 4)})
        self.assertEqual(
            self.failures(base, base, "test-only", [_A, _PROD]), ["production unchanged"]
        )

    def test_fewer_test_cases_fail_either_kind(self) -> None:
        base = snapshot(10, {_PROD: ({1}, 4)})
        head = snapshot(9, {_PROD: ({1}, 4)})
        for kind in nl.KINDS:
            with self.subTest(kind=kind):
                self.assertEqual(self.failures(base, head, kind, [_A]), ["test cases"])

    def test_production_compares_touched_files_by_percentage(self) -> None:
        other = "src/flash/b.cpp"
        base = snapshot(10, {_PROD: ({1, 2}, 4), other: ({1, 2}, 2)})
        # 2/4 -> 1/2 keeps 50%, and the untouched file may lose lines.
        head = snapshot(10, {_PROD: ({7}, 2), other: ({1}, 2)})
        self.assertEqual(self.failures(base, head, "production", [_PROD]), [])
        worse = snapshot(10, {_PROD: ({7}, 3), other: ({1, 2}, 2)})
        self.assertEqual(self.failures(base, worse, "production", [_PROD]), ["touched coverage"])

    def test_production_ignores_deleted_and_new_files(self) -> None:
        base = snapshot(10, {_PROD: ({1}, 4)})
        head = snapshot(10, {"src/flash/new.cpp": (set(), 4)})
        self.assertEqual(self.failures(base, head, "production", [_PROD, "src/flash/new.cpp"]), [])


class SnapshotTest(unittest.TestCase):
    def test_round_trips(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "s.json"
            original = snapshot(10, {_PROD: ({3, 1}, 4)})
            original.save(path)
            self.assertEqual(nl.Snapshot.load(path), original)


class PrKindTest(unittest.TestCase):
    def test_kind_follows_the_changed_files(self) -> None:
        self.assertEqual(nl.pr_kind([_A, "BUILD.bazel", "docs/x.md"]), "test-only")
        self.assertEqual(nl.pr_kind([_A, _PROD]), "production")

    def test_portable_test_sources_must_exist_in_the_checkout(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for path in ("src/backend/x/a_test.cpp", "src/ui/b_test.cpp", "src/backend/x/a.cpp"):
                (root / path).parent.mkdir(parents=True, exist_ok=True)
                (root / path).write_text("")
            changed = [
                "src/backend/x/a_test.cpp",
                "src/backend/x/deleted_test.cpp",
                "src/backend/x/BUILD.bazel",
                "src/ui/b_test.cpp",
                "src/backend/x/a.cpp",
            ]
            self.assertEqual(nl.portable_test_sources(changed, root), ["src/backend/x/a_test.cpp"])


def mull_report(path: str, *mutants: tuple[int, str]) -> dict:
    """A Mutation Testing Elements report with one mutant per (line, status)."""
    return {
        "files": {
            path: {
                "mutants": [
                    {
                        "location": {"start": {"line": line, "column": 1}},
                        "mutatorName": "cxx_add_to_sub",
                        "replacement": "-",
                        "status": status,
                    }
                    for line, status in mutants
                ]
            }
        }
    }


class MutationGuardTest(unittest.TestCase):
    def merged(self, *mutants: tuple[int, str]) -> dict:
        return nl.mutation_test.merge_reports([mull_report(_PROD, *mutants)], _ROOT)

    def test_kills_must_survive_in_the_head(self) -> None:
        base = self.merged((1, "Killed"), (2, "Timeout"), (3, "Survived"), (4, "Killed"))
        head = self.merged((1, "Timeout"), (2, "Killed"), (3, "Survived"), (4, "Survived"))
        self.assertEqual([k.line for k in nl.lost_kills(base, head)], [4])

    def test_a_missing_mutant_is_a_lost_kill(self) -> None:
        self.assertEqual(
            [k.line for k in nl.lost_kills(self.merged((1, "Killed")), self.merged())], [1]
        )

    def test_survivors(self) -> None:
        merged = self.merged((1, "Killed"), (2, "Survived"), (3, "NoCoverage"))
        self.assertEqual([k.line for k in nl.survivors(merged)], [2])


class MainTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def run_main(self, *argv: str) -> tuple[int, str]:
        out = StringIO()
        files = {"src/a.cpp": 10, "src/a_test.cpp": 5}
        code = nl.main(
            ["--root", str(self.root), *argv],
            run_hook=lambda _root: lizard_report(files),
            out=out,
            today=lambda: "2026-10-17",
        )
        return code, out.getvalue()

    def test_measure_prints_the_split(self) -> None:
        self.assertEqual(self.run_main("measure"), (0, "total 15\nproduction 10\ntest 5\n"))

    def test_measure_json(self) -> None:
        code, text = self.run_main("measure", "--json")
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(text), {"total": 15, "production": 10, "test": 5})

    def test_ledger_record_and_list(self) -> None:
        self.assertEqual(
            self.run_main("ledger", "record", "c1", "rejected", "--reason", "coverage")[0], 0
        )
        code, text = self.run_main("ledger", "list")
        self.assertEqual(code, 0)
        self.assertEqual(text, "2026-10-17 c1 rejected coverage\nconsecutive rejections: 1\n")
        self.assertTrue((self.root / ".nloc-loop" / "ledger.json").exists())

    def test_trend_appends_with_the_current_measurement(self) -> None:
        code, _ = self.run_main("trend", "--pr", "700", "--kind", "test-only", "--before", "80")
        self.assertEqual(code, 0)
        self.assertEqual(
            (self.root / "docs/reference/nloc-trend.csv").read_text().splitlines()[1],
            "2026-10-17,700,test-only,80,15",
        )

    def test_candidates_lists_the_queue(self) -> None:
        out = StringIO()
        code = nl.main(
            ["--root", str(self.root), "candidates"],
            run_jscpd=lambda root: {
                "duplicates": [
                    {
                        "firstFile": {"name": str(root / _A), "start": 1, "end": 60},
                        "secondFile": {"name": str(root / _B), "start": 3, "end": 62},
                        "fragment": "x",
                    }
                ]
            },
            host_files=lambda _root: {_A, _B},
            out=out,
        )
        self.assertEqual(code, 0)
        [header, first, second] = out.getvalue().splitlines()
        self.assertRegex(header, r"^[0-9a-f]{12} test-only savings=60 occurrences=2$")
        self.assertEqual([first, second], [f"  {_A}:1-60", f"  {_B}:3-62"])

    def run_guards(self, *argv: str, coverage=None, git=None) -> tuple[int, str]:
        out = StringIO()
        report = coverage_report(self.root.resolve(), coverage or {_PROD: {1: "1", 2: "0"}})
        code = nl.main(
            ["--root", str(self.root), "guards", *argv],
            run_coverage=lambda _root: (report, [gtest_xml(_RUN, _RUN)]),
            run_git=git or (lambda _root, args: self.git(args)),
            out=out,
        )
        return code, out.getvalue()

    @staticmethod
    def git(args: Sequence[str]) -> str:
        responses = {
            ("status", "--porcelain", "--untracked-files=no"): "",
            ("rev-parse", "HEAD"): "head\n",
            ("merge-base", "origin/master", "HEAD"): "base\n",
            ("diff", "--name-only", "base", "head"): f"{_A}\n",
        }
        return responses[tuple(args)]

    def test_guards_snapshot_then_check(self) -> None:
        code, text = self.run_guards("snapshot")
        self.assertEqual((code, text), (0, "snapshot head: 2 test cases, 1 files\n"))
        snapshots = self.root / ".nloc-loop" / "snapshots"
        (snapshots / "head.json").rename(snapshots / "base.json")
        self.run_guards("snapshot")
        code, text = self.run_guards("check", "--kind", "test-only")
        self.assertEqual(code, 0, text)
        self.assertIn("PASS covered lines", text)

    def test_guards_check_fails_with_exit_one(self) -> None:
        self.run_guards("snapshot")
        snapshots = self.root / ".nloc-loop" / "snapshots"
        (snapshots / "head.json").rename(snapshots / "base.json")
        self.run_guards("snapshot", coverage={_PROD: {1: "0", 2: "0"}})
        code, text = self.run_guards("check", "--kind", "test-only")
        self.assertEqual(code, 1)
        self.assertIn("FAIL covered lines", text)

    def test_guards_check_names_a_missing_base_snapshot(self) -> None:
        self.run_guards("snapshot")
        code, text = self.run_guards("check", "--kind", "test-only")
        self.assertEqual(code, 2)
        self.assertIn("no snapshot for base", text)

    def test_guards_snapshot_needs_a_clean_tree(self) -> None:
        def dirty(_root: Path, args: Sequence[str]) -> str:
            return " M src/a.cpp\n" if args[0] == "status" else self.git(args)

        code, text = self.run_guards("snapshot", git=dirty)
        self.assertEqual(code, 2)
        self.assertIn("uncommitted", text)

    def write_shards(self, side: str, count: int, *mutants: tuple[int, str]) -> Path:
        """Report directories as `mutation_test.py --shard I/N --report-dir` leaves them."""
        directory = self.root / side
        for index in range(count):
            shard = directory / f"shard-{index}"
            shard.mkdir(parents=True)
            report = mull_report(str(self.root / _PROD), *mutants) if index == 0 else {}
            (shard / "item.json").write_text(json.dumps(report))
            manifest = {"index": index, "count": count, "labels": []}
            (shard / nl.mutation_test.SHARD_MANIFEST).write_text(json.dumps(manifest))
        return directory

    def run_mutation_guard(self, *argv: str) -> tuple[int, str]:
        out = StringIO()
        code = nl.main(["--root", str(self.root), "guards", "mutation", *argv], out=out)
        return code, out.getvalue()

    def test_mutation_guard_compares_sharded_runs(self) -> None:
        base = self.write_shards("base", 2, (1, "Killed"), (2, "Survived"))
        head = self.write_shards("head", 2, (1, "Killed"), (2, "Killed"))
        code, text = self.run_mutation_guard(
            "--base-reports", str(base), "--reports", str(head), "--shards", "2"
        )
        self.assertEqual(code, 0, text)
        self.assertIn("PASS killed mutants: 1 -> 2", text)

    def test_mutation_guard_names_lost_kills(self) -> None:
        base = self.write_shards("base", 1, (1, "Killed"), (7, "Killed"))
        head = self.write_shards("head", 1, (1, "Killed"), (7, "Survived"))
        code, text = self.run_mutation_guard(
            "--base-reports", str(base), "--reports", str(head), "--shards", "1"
        )
        self.assertEqual(code, 1)
        self.assertIn(f"FAIL killed mutants: {_PROD}:7:1 cxx_add_to_sub", text)

    def test_mutation_guard_rejects_an_incomplete_run(self) -> None:
        base = self.write_shards("base", 2, (1, "Killed"))
        head = self.write_shards("head", 2, (1, "Killed"))
        (head / "shard-1" / nl.mutation_test.SHARD_MANIFEST).unlink()
        code, text = self.run_mutation_guard(
            "--base-reports", str(base), "--reports", str(head), "--shards", "2"
        )
        self.assertEqual(code, 2)
        self.assertIn("missing shards [1]", text)

    def test_mutation_guard_without_a_base_fails_on_survivors(self) -> None:
        reports = self.root / "head"
        reports.mkdir()
        (reports / "item.json").write_text(
            json.dumps(mull_report(str(self.root / _PROD), (3, "Survived")))
        )
        code, text = self.run_mutation_guard("--reports", str(reports))
        self.assertEqual(code, 1)
        self.assertIn(f"FAIL surviving mutants: {_PROD}:3:1", text)

    def test_pr_kind(self) -> None:
        out = StringIO()
        code = nl.main(
            ["--root", str(self.root), "pr-kind", "--base", "base", "--head", "head"],
            run_git=lambda _root, args: self.git(args),
            out=out,
        )
        self.assertEqual((code, out.getvalue()), (0, "test-only\n"))

    def test_mutation_scope_lists_dependent_tests(self) -> None:
        portable = "src/backend/flash/a_test.cpp"
        (self.root / portable).parent.mkdir(parents=True)
        (self.root / portable).write_text("")
        queries = []
        out = StringIO()

        def query(root: Path, scope: str, changed: Sequence[str]) -> list[str]:
            queries.append((scope, list(changed)))
            return ["//src/flash:a_test", "//src/flash:b_test"]

        code = nl.main(
            ["--root", str(self.root), "mutation-scope", "--base", "base", "--head", "head"],
            run_git=lambda _root, _args: f"{portable}\n{_PROD}\n",
            query_tests=query,
            out=out,
        )
        self.assertEqual(code, 0)
        self.assertEqual(out.getvalue(), "//src/flash:a_test\n//src/flash:b_test\n")
        self.assertEqual(queries, [(nl.mutation_test.PORTABLE_SCOPE, [portable])])

    def test_errors_exit_two(self) -> None:
        code, text = self.run_main("trend", "--pr", "700", "--kind", "test-only", "--before", "15")
        self.assertEqual(code, 2)
        self.assertIn("lower", text)


if __name__ == "__main__":
    unittest.main()
