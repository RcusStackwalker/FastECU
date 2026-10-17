#!/usr/bin/env python3

import json
import tempfile
import unittest
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

    def test_errors_exit_two(self) -> None:
        code, text = self.run_main("trend", "--pr", "700", "--kind", "test-only", "--before", "15")
        self.assertEqual(code, 2)
        self.assertIn("lower", text)


if __name__ == "__main__":
    unittest.main()
