#!/usr/bin/env python3

import json
import tempfile
import unittest
from collections.abc import Mapping, Sequence
from io import StringIO
from pathlib import Path
from urllib.error import URLError

import dedup_loop as dl

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
            dl.parse_report(report), {"src/a/a.cpp": 12, "src/a/a.h": 0, "apps/b.cpp": 5}
        )

    def test_rejects_rows_that_disagree_with_the_total(self) -> None:
        with self.assertRaisesRegex(dl.DedupLoopError, "sum to 12.*total is 13"):
            dl.parse_report(lizard_report({"src/a.cpp": 12}, total=13))

    def test_rejects_a_report_without_a_file_table(self) -> None:
        with self.assertRaisesRegex(dl.DedupLoopError, "file table"):
            dl.parse_report(_FUNCTIONS)

    def test_rejects_a_report_without_a_total(self) -> None:
        report = lizard_report({"src/a.cpp": 12}).split("=====\nTotal nloc")[0]
        with self.assertRaisesRegex(dl.DedupLoopError, "total"):
            dl.parse_report(report)


class ClassifyTest(unittest.TestCase):
    def test_test_code(self) -> None:
        for path in (
            "tests/serial_pty_e2e_test.cpp",
            "tests/serial_port_crash_test.cpp",
            "src/backend/flash/ecu/executor_test.cpp",
            "src/backend/flash/ecu/executor_test.h",
            "src/backend/flash/testing/mock_transport.h",
            "src/algorithms/checksum/testdata/fixture.h",
            "apps/desktop/startup_test_platform_macos.cpp",
        ):
            with self.subTest(path=path):
                self.assertEqual(dl.classify(path), dl.TEST)

    def test_production_code(self) -> None:
        for path in ("src/backend/flash/ecu/executor.cpp", "apps/bench/bench_format.h"):
            with self.subTest(path=path):
                self.assertEqual(dl.classify(path), dl.PRODUCTION)

    def test_outside_the_metric(self) -> None:
        for path in ("bazel/qt/shim.cpp", "resources/shared/x_test.cpp", "srcx/a.cpp"):
            with self.subTest(path=path):
                self.assertIsNone(dl.classify(path))


class MeasureTest(unittest.TestCase):
    def test_totals_only_in_scope_files(self) -> None:
        files = {"src/a.cpp": 10, "src/a_test.cpp": 4, "tests/b.cpp": 3, "bazel/c.cpp": 100}
        self.assertEqual(dl.measure_files(files), dl.Measurement(production=10, test=7))
        self.assertEqual(dl.measure_files(files).total, 17)

    def test_runs_the_pinned_hook_at_the_root(self) -> None:
        calls: list[Path] = []

        def run_hook(root: Path) -> str:
            calls.append(root)
            return lizard_report({"src/a.cpp": 10})

        self.assertEqual(dl.measure(Path("/repo"), run_hook), dl.Measurement(10, 0))
        self.assertEqual(calls, [Path("/repo")])


def _component_tree(paths: Sequence[str], page: int, size: int, total: int) -> dict:
    return {
        "paging": {"pageIndex": page, "pageSize": size, "total": total},
        "components": [{"path": p} for p in paths],
    }


def _duplications(*groups: Sequence[tuple[str, int, int]]) -> dict:
    """A `api/duplications/show` response; each group is (path, from, size) blocks."""
    refs: dict[str, str] = {}
    out = []
    for group in groups:
        blocks = []
        for path, start, size in group:
            ref = refs.setdefault(path, str(len(refs) + 1))
            blocks.append({"from": start, "size": size, "_ref": ref})
        out.append({"blocks": blocks})
    files = {ref: {"key": f"{dl.PROJECT}:{path}", "name": path} for path, ref in refs.items()}
    return {"duplications": out, "files": files}


class FakeSonar:
    """Serves canned responses keyed on the API path and the file key or page."""

    def __init__(self, files: Sequence[str], groups: dict[str, dict], page_size: int = 500):
        self.files = list(files)
        self.groups = groups
        self.page_size = page_size
        self.calls: list[tuple[str, dict]] = []

    def __call__(self, path: str, params: Mapping[str, str]) -> Mapping:
        self.calls.append((path, dict(params)))
        if path == "api/measures/component_tree":
            page = int(params.get("p", "1"))
            size = self.page_size
            chunk = self.files[(page - 1) * size : page * size]
            return _component_tree(chunk, page, size, len(self.files))
        if path == "api/duplications/show":
            return self.groups[params["key"].removeprefix(f"{dl.PROJECT}:")]
        raise AssertionError(path)


def _ranges(*rows: tuple[str, str, int, int]) -> list:
    return [dl.FunctionRange(path, name, start, end) for path, name, start, end in rows]


class SonarGroupsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.err = StringIO()

    def build(self, sonar: FakeSonar, ranges: list, lines: int | None = 1000) -> list:
        return dl.build_groups(sonar, ranges, lambda _path: lines, err=self.err)

    def test_reads_every_page_of_duplicated_files(self) -> None:
        sonar = FakeSonar(["src/a.cpp", "src/b.cpp", "src/c.cpp"], {}, page_size=2)
        self.assertEqual(dl.duplicated_files(sonar), ["src/a.cpp", "src/b.cpp", "src/c.cpp"])
        self.assertEqual(len(sonar.calls), 2)
        self.assertEqual(sonar.calls[0][1]["metricKeys"], "duplicated_lines")

    def test_stops_at_the_first_file_without_duplication(self) -> None:
        def fetch(_path: str, _params: Mapping[str, str]) -> Mapping:
            components = [
                {"path": "src/a.cpp", "measures": [{"value": "12"}]},
                {"path": "src/b.cpp", "measures": [{"value": "0"}]},
                {"path": "src/c.cpp", "measures": [{"value": "0"}]},
            ]
            return {
                "paging": {"pageIndex": 1, "pageSize": 3, "total": 900},
                "components": components,
            }

        self.assertEqual(dl.duplicated_files(fetch), ["src/a.cpp"])

    def test_group_reported_from_both_files_is_one_group(self) -> None:
        pair = [("src/a.cpp", 10, 20), ("src/b.cpp", 30, 20)]
        sonar = FakeSonar(
            ["src/a.cpp", "src/b.cpp"],
            {"src/a.cpp": _duplications(pair), "src/b.cpp": _duplications(pair[::-1])},
        )
        ranges = _ranges(("src/a.cpp", "f", 5, 40), ("src/b.cpp", "g", 25, 60))
        (group,) = self.build(sonar, ranges)
        self.assertEqual(group.members, (("src/a.cpp", "f"), ("src/b.cpp", "g")))
        self.assertEqual(group.lines, 40)
        self.assertEqual(
            group.blocks, (dl.Block("src/a.cpp", 10, 29), dl.Block("src/b.cpp", 30, 49))
        )

    def test_block_outside_functions_keys_to_file(self) -> None:
        block = dl.Block("src/a.cpp", 1, 5)
        self.assertEqual(dl.key_block(block, []), [("src/a.cpp", "<file>")])

    def test_block_spanning_two_functions_keys_both(self) -> None:
        ranges = _ranges(("src/a.cpp", "f", 1, 10), ("src/a.cpp", "g", 11, 20))
        ranges += _ranges(("src/b.cpp", "h", 1, 20))
        self.assertEqual(
            dl.key_block(dl.Block("src/a.cpp", 8, 14), ranges),
            [("src/a.cpp", "f"), ("src/a.cpp", "g")],
        )

    def test_stale_block_is_dropped_with_a_note(self) -> None:
        pair = [("src/a.cpp", 30, 11), ("src/b.cpp", 1, 11)]
        sonar = FakeSonar(["src/a.cpp"], {"src/a.cpp": _duplications(pair)})
        self.assertEqual(self.build(sonar, [], lines=10), [])
        self.assertIn("dropped stale block src/a.cpp:30-40", self.err.getvalue())

    def test_missing_file_is_stale(self) -> None:
        pair = [("src/a.cpp", 1, 5), ("src/gone.cpp", 1, 5)]
        sonar = FakeSonar(["src/a.cpp"], {"src/a.cpp": _duplications(pair)})
        self.assertEqual(self.build(sonar, [], lines=None), [])
        self.assertIn("dropped stale block src/gone.cpp:1-5", self.err.getvalue())

    def test_test_files_never_join_a_group(self) -> None:
        trio = [("src/a.cpp", 1, 5), ("src/b.cpp", 1, 5), ("src/a_test.cpp", 1, 5)]
        sonar = FakeSonar(["src/a.cpp"], {"src/a.cpp": _duplications(trio)})
        (group,) = self.build(sonar, [])
        self.assertEqual([b.path for b in group.blocks], ["src/a.cpp", "src/b.cpp"])

    def test_id_is_stable_across_line_shifts(self) -> None:
        members = (("src/a.cpp", "f"), ("src/b.cpp", "g"))
        one = dl.Group(members, (dl.Block("src/a.cpp", 1, 5),), 5)
        two = dl.Group(members, (dl.Block("src/a.cpp", 9, 13),), 5)
        self.assertEqual(one.id, two.id)
        self.assertRegex(one.id, r"^[0-9a-f]{12}$")

    def test_sonar_failure_is_actionable(self) -> None:
        def opener(*_args, **_kwargs):
            raise URLError("timed out")

        with self.assertRaisesRegex(dl.DedupLoopError, r"^SonarCloud api/x: .*timed out"):
            dl.sonar_get("api/x", {}, opener=opener)


class FunctionRangeTest(unittest.TestCase):
    def test_parses_lizard_csv_and_skips_banner(self) -> None:
        output = (
            "C/C++ function ranges....................................Passed\n"
            "- hook id: nloc-functions\n"
            "- duration: 1.91s\n\n"
            '  8,3,37,1,9,"Foo::run@9-17@./src/a.cpp","./src/a.cpp","Foo::run",'
            '"Foo::run( int x)",9,17\n'
        )
        self.assertEqual(
            dl.parse_function_ranges(output), [dl.FunctionRange("src/a.cpp", "Foo::run", 9, 17)]
        )


def _attempt(group: str, outcome: str, reason: str = "", pr: int | None = None):
    return dl.Attempt(group, [["src/a.cpp", "f"]], outcome, "2026-10-25", reason, pr)


class LedgerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.path = Path(self.temp_dir.name) / "ledger.json"

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_missing_ledger_is_empty(self) -> None:
        self.assertEqual(dl.Ledger.load(self.path).attempts, [])

    def test_records_survive_a_reload(self) -> None:
        ledger = dl.Ledger.load(self.path)
        ledger.record(_attempt("g1", "unsuitable", "both reviewers: per-family timeouts"))
        ledger.save(self.path)
        self.assertEqual(dl.Ledger.load(self.path).attempts, ledger.attempts)

    def test_later_attempt_supersedes(self) -> None:
        ledger = dl.Ledger.load(self.path)
        ledger.record(_attempt("g1", "open", pr=5))
        ledger.record(_attempt("g1", "merged", pr=5))
        self.assertEqual(ledger.outcome("g1"), "merged")
        self.assertIsNone(ledger.outcome("g2"))

    def test_counts_trailing_failures(self) -> None:
        ledger = dl.Ledger.load(self.path)
        for group, outcome in (("g0", "failed"), ("g1", "merged"), ("g2", "failed")):
            ledger.record(_attempt(group, outcome))
        ledger.record(_attempt("g3", "open"))
        ledger.record(_attempt("g3", "failed"))
        self.assertEqual(ledger.consecutive_failures(), 2)

    def test_unsuitable_and_deferred_neither_count_nor_break_the_run(self) -> None:
        ledger = dl.Ledger.load(self.path)
        for group, outcome in (("g1", "failed"), ("g2", "unsuitable"), ("g3", "deferred")):
            ledger.record(_attempt(group, outcome))
        ledger.record(_attempt("g4", "failed"))
        self.assertEqual(ledger.consecutive_failures(), 2)

    def test_rejects_unknown_outcome(self) -> None:
        with self.assertRaisesRegex(dl.DedupLoopError, "outcome"):
            dl.Ledger.load(self.path).record(_attempt("g1", "rejected"))

    def test_rejects_a_corrupt_ledger(self) -> None:
        self.path.write_text("{not json")
        with self.assertRaisesRegex(dl.DedupLoopError, "ledger"):
            dl.Ledger.load(self.path)

    def test_save_is_byte_stable(self) -> None:
        ledger = dl.Ledger.load(self.path)
        ledger.record(_attempt("g1", "deferred", "no wire evidence"))
        ledger.save(self.path)
        first = self.path.read_bytes()
        dl.Ledger.load(self.path).save(self.path)
        self.assertEqual(self.path.read_bytes(), first)
        self.assertTrue(first.endswith(b"}\n"))


class TrendTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.path = Path(self.temp_dir.name) / "trend.csv"

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_creates_header_then_appends(self) -> None:
        dl.append_trend(self.path, dl.TrendRow("2026-10-25", 7, 100, 90, 1621, None))
        dl.append_trend(self.path, dl.TrendRow("2026-10-26", 8, 90, 70, 1500, None))
        self.assertEqual(
            self.path.read_text(),
            "date,pr,nloc_before,nloc_after,dup_before,dup_after\n"
            "2026-10-25,7,100,90,1621,\n2026-10-26,8,90,70,1500,\n",
        )

    def test_rejects_a_drop_under_ten(self) -> None:
        with self.assertRaisesRegex(dl.DedupLoopError, "at least 10"):
            dl.append_trend(self.path, dl.TrendRow("2026-10-25", 7, 100, 91, 1621, None))

    def test_bookkeeping_row_is_exempt(self) -> None:
        dl.append_trend(
            self.path, dl.TrendRow("2026-10-25", 7, 100, 100, 1621, None), bookkeeping=True
        )
        self.assertIn("2026-10-25,7,100,100,1621,", self.path.read_text())

    def test_fill_after_sets_only_the_last_empty_cell(self) -> None:
        self.path.write_text(
            "date,pr,nloc_before,nloc_after,dup_before,dup_after\n"
            "2026-10-17,699,152607,152412,,\n2026-10-25,7,100,90,1621,\n"
        )
        dl.fill_dup_after(self.path, 1580)
        self.assertEqual(
            self.path.read_text().splitlines()[1:],
            ["2026-10-17,699,152607,152412,,", "2026-10-25,7,100,90,1621,1580"],
        )
        with self.assertRaisesRegex(dl.DedupLoopError, "no trend row"):
            dl.fill_dup_after(self.path, 1500)

    def test_fill_after_ignores_rows_carried_over_from_the_nloc_loop(self) -> None:
        self.path.write_text(
            "date,pr,nloc_before,nloc_after,dup_before,dup_after\n2026-10-18,735,150748,150693,,\n"
        )
        with self.assertRaisesRegex(dl.DedupLoopError, "no trend row"):
            dl.fill_dup_after(self.path, 1500)


def _group(paths: Sequence[str], lines: int) -> object:
    blocks = tuple(dl.Block(p, 1, 5) for p in paths)
    return dl.Group(tuple((p, "f") for p in paths), blocks, lines)


class RankTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.ledger = dl.Ledger.load(Path(self.temp_dir.name) / "l.json")

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_backend_first_then_lines(self) -> None:
        ui = _group(["src/ui/a.cpp", "src/ui/b.cpp"], 400)
        small = _group(["src/backend/a.cpp", "src/backend/b.cpp"], 50)
        large = _group(["src/backend/c.cpp", "src/backend/d.cpp"], 300)
        self.assertEqual(dl.rank([ui, small, large], self.ledger, set()), [large, small, ui])

    def test_ledger_outcomes_are_skipped(self) -> None:
        groups = {
            o: _group([f"src/backend/{o}.cpp", f"src/backend/{o}2.cpp"], 10) for o in dl.OUTCOMES
        }
        for outcome, group in groups.items():
            self.ledger.record(_attempt(group.id, outcome))
        ranked = dl.rank(list(groups.values()), self.ledger, set())
        self.assertEqual(ranked, [groups["closed"], groups["failed"]])

    def test_groups_touching_busy_files_are_skipped(self) -> None:
        busy = _group(["src/backend/a.cpp", "src/backend/b.cpp"], 10)
        free = _group(["src/backend/c.cpp", "src/backend/d.cpp"], 10)
        self.assertEqual(dl.rank([busy, free], self.ledger, {"src/backend/b.cpp"}), [free])


def _mull_report(*mutants: tuple[str, int, str]) -> dict:
    """An Elements report with (path, line, status) mutants."""
    files: dict[str, dict] = {}
    for path, line, status in mutants:
        files.setdefault(path, {"mutants": []})["mutants"].append(
            {
                "location": {"start": {"line": line, "column": 5}},
                "mutatorName": "cxx_eq_to_ne",
                "status": status,
            }
        )
    return {"files": files}


class VerdictTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def checks(self, **overrides: object) -> dict[str, object]:
        arguments: dict = {
            "nloc_before": 100,
            "nloc_after": 90,
            "changed": ["src/backend/a.cpp"],
            "measures": {"new_duplicated_lines": 0, "new_uncovered_lines": 0},
            "survived": [],
        }
        arguments.update(overrides)
        return {c.name: c.ok for c in dl.verdict(**arguments)}

    def test_all_four_checks_pass(self) -> None:
        self.assertEqual(
            self.checks(),
            {
                "nloc drop": True,
                "new duplicated lines": True,
                "new uncovered lines": True,
                "surviving mutants": True,
                "loop tooling unchanged": True,
            },
        )

    def test_drop_of_ten_passes_nine_fails(self) -> None:
        self.assertTrue(self.checks(nloc_after=90)["nloc drop"])
        self.assertFalse(self.checks(nloc_after=91)["nloc drop"])

    def test_bookkeeping_pr_skips_the_nloc_rule(self) -> None:
        changed = [dl.LEDGER.as_posix(), dl.TREND.as_posix()]
        self.assertTrue(self.checks(nloc_after=100, changed=changed)["nloc drop"])
        self.assertFalse(self.checks(nloc_after=100, changed=[*changed, "src/a.cpp"])["nloc drop"])

    def test_any_new_duplicated_line_fails(self) -> None:
        measures = {"new_duplicated_lines": 1, "new_uncovered_lines": 0}
        self.assertFalse(self.checks(measures=measures)["new duplicated lines"])

    def test_any_new_uncovered_line_fails(self) -> None:
        measures = {"new_duplicated_lines": 0, "new_uncovered_lines": 3}
        self.assertFalse(self.checks(measures=measures)["new uncovered lines"])

    def test_changing_the_loop_tooling_fails(self) -> None:
        for path in (
            "scripts/dedup_loop.py",
            "scripts/mutation_test.py",
            ".pre-commit-config.yaml",
            "sonar-project.properties",
            "mull.yml",
            ".github/workflows/pr.yml",
        ):
            with self.subTest(path=path):
                self.assertFalse(
                    self.checks(changed=["src/backend/a.cpp", path])["loop tooling unchanged"]
                )
        self.assertTrue(self.checks()["loop tooling unchanged"])

    def test_absent_pr_metric_counts_as_zero(self) -> None:
        def fetch(path: str, params: Mapping[str, str]) -> Mapping:
            self.assertEqual((path, params["pullRequest"]), ("api/measures/component", "7"))
            measure = {"metric": "new_uncovered_lines", "periods": [{"index": 1, "value": "2"}]}
            return {"component": {"measures": [measure]}}

        self.assertEqual(
            dl.pr_measures(fetch, 7), {"new_duplicated_lines": 0, "new_uncovered_lines": 2}
        )

    def test_missing_report_dir_means_no_survivors(self) -> None:
        self.assertEqual(dl.survivors(None, self.root), [])
        self.assertEqual(dl.survivors(self.root / "absent", self.root), [])

    def test_survivor_fails_and_is_named(self) -> None:
        reports = self.root / "reports"
        reports.mkdir()
        report = _mull_report(("src/a.cpp", 12, "Survived"), ("src/a.cpp", 13, "Killed"))
        (reports / "a_test.json").write_text(json.dumps(report))
        survived = dl.survivors(reports, self.root)
        self.assertEqual(survived, ["src/a.cpp:12:5 cxx_eq_to_ne"])
        (check,) = [
            c
            for c in dl.verdict(
                nloc_before=100, nloc_after=90, changed=[], measures={}, survived=survived
            )
            if c.name == "surviving mutants"
        ]
        self.assertFalse(check.ok)
        self.assertIn("src/a.cpp:12:5", check.detail)


class MainTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    sonar: FakeSonar | None = None
    revision = "abc"
    analysed_sha = "head1"

    def fetch(self, path: str, params: Mapping[str, str]) -> Mapping:
        if path == "api/project_analyses/search":
            return {"analyses": [{"revision": self.revision}]}
        if path == "api/measures/component":
            return {"component": {"measures": []}}
        if path == "api/project_pull_requests/list":
            return {"pullRequests": [{"key": "7", "commit": {"sha": self.analysed_sha}}]}
        return (self.sonar or FakeSonar([], {}))(path, params)

    def run_main(self, *argv: str) -> tuple[int, str]:
        out = StringIO()
        files = {"src/a.cpp": 10, "src/a_test.cpp": 5}
        code = dl.main(
            ["--root", str(self.root), *argv],
            run_hook=lambda _root: lizard_report(files),
            out=out,
            today=lambda: "2026-10-17",
            fetch=self.fetch,
            run_functions=lambda _root: "",
            busy_files=lambda _root: set(),
            git=lambda _root, _args: "abc\n",
            err=StringIO(),
        )
        return code, out.getvalue()

    def test_measure_prints_the_split(self) -> None:
        self.assertEqual(self.run_main("measure"), (0, "total 15\nproduction 10\ntest 5\n"))

    def test_measure_json(self) -> None:
        code, text = self.run_main("measure", "--json")
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(text), {"total": 15, "production": 10, "test": 5})

    def test_candidates_prints_nothing_when_empty(self) -> None:
        self.assertEqual(self.run_main("candidates"), (0, ""))

    def test_candidates_lists_blocks_with_functions(self) -> None:
        pair = [("src/backend/a.cpp", 2, 3), ("src/backend/b.cpp", 2, 3)]
        self.sonar = FakeSonar(["src/backend/a.cpp"], {"src/backend/a.cpp": _duplications(pair)})
        for name in ("a", "b"):
            (self.root / "src/backend").mkdir(parents=True, exist_ok=True)
            (self.root / f"src/backend/{name}.cpp").write_text("x\n" * 10)
        code, text = self.run_main("candidates")
        self.assertEqual(code, 0)
        lines = text.splitlines()
        self.assertRegex(lines[0], r"^[0-9a-f]{12} lines=6$")
        self.assertEqual(
            lines[1:], ["  src/backend/a.cpp:2-4 <file>", "  src/backend/b.cpp:2-4 <file>"]
        )

    def test_ledger_record_and_list(self) -> None:
        members = json.dumps([["src/a.cpp", "f"]])
        argv = ("ledger", "record", "g1", "failed", "--members-json", members, "--pr", "7")
        self.assertEqual(self.run_main(*argv)[0], 0)
        code, text = self.run_main("ledger", "list")
        self.assertEqual(code, 0)
        self.assertEqual(text, "2026-10-17 g1 failed 7\nconsecutive failures: 1\n")
        saved = json.loads((self.root / dl.LEDGER).read_text())
        self.assertEqual(saved["attempts"][0]["members"], [["src/a.cpp", "f"]])

    def test_trend_add_measures_after(self) -> None:
        argv = ("trend", "add", "--pr", "7", "--before", "40", "--dup-before", "1621")
        self.assertEqual(self.run_main(*argv)[0], 0)
        self.assertIn("2026-10-17,7,40,15,1621,", (self.root / dl.TREND).read_text())

    def test_fresh_exits_zero(self) -> None:
        self.revision = "abc"
        self.assertEqual(self.run_main("fresh"), (0, "fresh abc\n"))

    def test_stale_prints_both_revisions_and_exits_one(self) -> None:
        self.revision = "old"
        self.assertEqual(self.run_main("fresh"), (1, "stale: analysis old, origin/master abc\n"))

    def verdict_argv(self) -> tuple[str, ...]:
        changed = self.root / "changed.txt"
        changed.write_text("src/backend/a.cpp\n")
        return (
            "verdict",
            "--pr",
            "7",
            "--nloc-before",
            "100",
            "--nloc-after",
            "95",
            "--changed-file",
            str(changed),
            "--head-sha",
            "head1",
        )

    def test_verdict_exits_one_on_failure(self) -> None:
        code, text = self.run_main(*self.verdict_argv())
        self.assertEqual(code, 1)
        self.assertIn("FAIL nloc drop: 100 -> 95", text)
        self.assertIn("PASS new duplicated lines: 0", text)

    def test_verdict_exits_two_when_sonar_is_down(self) -> None:
        def down(path: str, _params: Mapping[str, str]) -> Mapping:
            raise dl.DedupLoopError(f"SonarCloud {path}: timed out")

        self.fetch = down  # type: ignore[method-assign]
        code, text = self.run_main(*self.verdict_argv())
        self.assertEqual(code, 2)
        self.assertRegex(text, r"SonarCloud api/\S+: timed out")

    def test_verdict_exits_two_on_an_older_pr_analysis(self) -> None:
        self.analysed_sha = "head0"
        code, text = self.run_main(*self.verdict_argv())
        self.assertEqual(code, 2)
        self.assertIn("analysed head0, not the PR head head1", text)

    def test_malformed_sonar_response_exits_two(self) -> None:
        def empty(_path: str, _params: Mapping[str, str]) -> Mapping:
            return {"analyses": []}

        self.fetch = empty  # type: ignore[method-assign]
        code, text = self.run_main("fresh")
        self.assertEqual(code, 2)
        self.assertIn("SonarCloud api/project_analyses/search", text)

    def test_malformed_members_json_exits_two(self) -> None:
        code, text = self.run_main("ledger", "record", "g1", "failed", "--members-json", "[[")
        self.assertEqual(code, 2)
        self.assertIn("--members-json", text)


if __name__ == "__main__":
    unittest.main()
