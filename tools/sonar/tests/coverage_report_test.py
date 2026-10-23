"""Protect the lcov to Sonar generic coverage conversion."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

import coverage_report as report

LCOV_A = "SF:src/a.cpp\nDA:1,3\nDA:2,0\nBRDA:1,0,0,1\nBRDA:1,0,1,-\nend_of_record\n"


class CoverageReportTest(unittest.TestCase):
    def test_parses_lines_and_branches(self):
        self.assertEqual(
            report.parse_lcov(LCOV_A),
            {"src/a.cpp": report.FileCoverage({1: 3, 2: 0}, {1: (2, 1)})},
        )

    def test_merges_repeated_records(self):
        text = "SF:src/a.cpp\nDA:1,1\nend_of_record\nSF:src/a.cpp\nDA:1,2\nend_of_record\n"
        self.assertEqual(report.parse_lcov(text)["src/a.cpp"].lines, {1: 3})

    def test_a_branch_repeated_across_records_counts_once(self):
        text = (
            "SF:src/a.cpp\nDA:1,1\nBRDA:1,0,0,1\nBRDA:1,0,1,-\nend_of_record\n"
            "SF:src/a.cpp\nDA:1,1\nBRDA:1,0,0,0\nBRDA:1,0,1,2\nend_of_record\n"
        )
        self.assertEqual(report.parse_lcov(text)["src/a.cpp"].branches, {1: (2, 2)})

    def test_rejects_malformed_records(self):
        for record in ("DA:1", "DA:x,1", "BRDA:1,0,0", "BRDA:1,0,0,x"):
            with (
                self.subTest(record=record),
                self.assertRaisesRegex(report.LcovError, f"src/a.cpp: '{record}'"),
            ):
                report.parse_lcov(f"SF:src/a.cpp\n{record}\nend_of_record\n")

    def test_drops_ignored_absolute_missing(self):
        paths = [
            "src/a.cpp",
            "/usr/include/x.h",
            "external/qt/q.h",
            "src/moc_w.cpp",
            "src/tests/t.cpp",
            "src/gone.cpp",
        ]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for path in paths:
                if not path.startswith("/") and "gone" not in path:
                    (root / path).parent.mkdir(parents=True, exist_ok=True)
                    (root / path).write_text("")
            records = {path: report.FileCoverage({1: 1}, {}) for path in paths}
            self.assertEqual(list(report.select_files(records, root, report.IGNORE)), ["src/a.cpp"])

    def test_drops_paths_escaping_the_checkout(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "checkout"
            root.mkdir()
            (root / "src").mkdir()
            (root / "src/a.cpp").write_text("")
            (Path(tmp) / "outside").mkdir()
            (Path(tmp) / "outside/x.cpp").write_text("")
            records = {
                path: report.FileCoverage({1: 1}, {}) for path in ["src/a.cpp", "../outside/x.cpp"]
            }
            self.assertEqual(list(report.select_files(records, root, report.IGNORE)), ["src/a.cpp"])

    def test_generic_xml_is_exact(self):
        records = {"src/a.cpp": report.FileCoverage({1: 3, 2: 0}, {1: (2, 1)})}
        self.assertEqual(
            report.generic_xml(records),
            '<coverage version="1">\n  <file path="src/a.cpp">\n'
            '    <lineToCover lineNumber="1" covered="true" '
            'branchesToCover="2" coveredBranches="1"/>\n'
            '    <lineToCover lineNumber="2" covered="false"/>\n'
            "  </file>\n</coverage>\n",
        )

    def _run(self, min_files, extra=""):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        (root / "src").mkdir()
        (root / "src" / "a.cpp").write_text("")
        lcov = root / "c.dat"
        lcov.write_text(LCOV_A + "SF:src/gone.cpp\nDA:1,1\nend_of_record\n" + extra)
        out = root / "out"
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code = report.main(
                [
                    "--lcov", str(lcov), "--root", str(root),
                    "--output-dir", str(out), "--min-files", str(min_files),
                ]
            )  # fmt: skip
        return code, err.getvalue(), out

    def test_main_fails_below_floor(self):
        code, err, out = self._run(2)
        self.assertEqual(code, 1)
        self.assertIn("1 of", err)
        self.assertIn("src/gone.cpp", err)
        self.assertFalse((out / "sonar-coverage.xml").exists())

    def test_main_reports_a_malformed_record(self):
        code, err, out = self._run(1, extra="SF:src/a.cpp\nDA:oops\nend_of_record\n")
        self.assertEqual(code, 1)
        self.assertIn("malformed lcov record in src/a.cpp: 'DA:oops'", err)
        self.assertFalse((out / "sonar-coverage.xml").exists())

    def test_main_writes_both_outputs(self):
        code, _, out = self._run(1)
        self.assertEqual(code, 0)
        self.assertTrue((out / "sonar-coverage.xml").is_file())
        self.assertTrue(
            (out / "coverage-summary.txt").read_text().rstrip().endswith("TOTAL 1/2 50.0%")
        )


if __name__ == "__main__":
    unittest.main()
