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
