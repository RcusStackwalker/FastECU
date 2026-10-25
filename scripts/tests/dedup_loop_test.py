#!/usr/bin/env python3

import json
import tempfile
import unittest
from io import StringIO
from pathlib import Path

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


class MainTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def run_main(self, *argv: str) -> tuple[int, str]:
        out = StringIO()
        files = {"src/a.cpp": 10, "src/a_test.cpp": 5}
        code = dl.main(
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


if __name__ == "__main__":
    unittest.main()
