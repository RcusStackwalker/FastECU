#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path, PureWindowsPath

import clang_tidy_output as out

_WORKSPACE = Path("/ws")


class ParseDiagnosticTest(unittest.TestCase):
    def test_parses_finding_and_strips_warnings_as_errors(self) -> None:
        diagnostic = out.parse_diagnostic(
            "/ws/src/a.cpp:12:5: error: use auto [modernize-use-auto,-warnings-as-errors]"
        )
        self.assertEqual(
            out.Diagnostic("/ws/src/a.cpp", 12, 5, "error", "use auto", "modernize-use-auto"),
            diagnostic,
        )

    def test_parses_note_without_check(self) -> None:
        diagnostic = out.parse_diagnostic("/ws/src/a.h:3:1: note: declared here")
        assert diagnostic is not None
        self.assertEqual(("note", None), (diagnostic.severity, diagnostic.check))

    def test_parses_fatal_error(self) -> None:
        diagnostic = out.parse_diagnostic(
            "/ws/a.cpp:1:10: fatal error: 'x.h' file not found [clang-diagnostic-error]"
        )
        assert diagnostic is not None
        self.assertEqual("fatal error", diagnostic.severity)
        self.assertEqual("'x.h' file not found", diagnostic.message)

    def test_parses_windows_drive_letter_path(self) -> None:
        diagnostic = out.parse_diagnostic(r"C:\ws\src\a.cpp:12:5: warning: msg [bugprone-x]")
        assert diagnostic is not None
        self.assertEqual(
            (r"C:\ws\src\a.cpp", 12, 5), (diagnostic.path, diagnostic.line, diagnostic.column)
        )

    def test_line_without_column_is_not_a_diagnostic(self) -> None:
        self.assertIsNone(out.parse_diagnostic("main.cpp:1: warning: use Result matcher"))


class ParseProgressTest(unittest.TestCase):
    def test_parses_progress_echo(self) -> None:
        line = (
            "[ 3/12][0.4s] /llvm/bin/clang-tidy -checks=-* -export-fixes /tmp/f/x.yaml "
            "-p=/tmp/db /ws/src/a.cpp"
        )
        self.assertEqual((3, 12, "/ws/src/a.cpp"), out.parse_progress(line))

    def test_parses_progress_echo_with_quoted_windows_path(self) -> None:
        progress = out.parse_progress("[1/1][0.1s] clang-tidy.exe 'C:\\ws\\a.cpp'")
        assert progress is not None
        self.assertEqual("C:\\ws\\a.cpp", progress[2])

    def test_other_bracketed_lines_are_not_progress(self) -> None:
        self.assertIsNone(out.parse_progress("[1/2] something else"))


class ClassifyLineTest(unittest.TestCase):
    def test_detail_lines(self) -> None:
        self.assertTrue(out.is_detail("   12 |   foo();"))
        self.assertTrue(out.is_detail("      |   ^~~"))
        self.assertFalse(out.is_detail("Stack dump:"))

    def test_noise(self) -> None:
        for line in [
            "1 warning generated.",
            "3 warnings and 1 error generated.",
            "2 errors generated.",
            "Suppressed 40 warnings (40 in non-user code).",
            "Use -header-filter=.* to display errors from all non-system headers. "
            "Use -system-headers to display errors from system headers as well.",
            "1 warning treated as error",
            "2 warnings treated as errors",
            "Running clang-tidy in 8 threads for 3 files out of 9 in compilation database ...",
            "===-------------------------------------------------------------------------===",
            "                          clang-tidy checks profiling",
            "  Total Execution Time: 1.2000 seconds (1.2100 wall clock)",
            "   ---User Time---   --System Time--   --User+System--"
            "   ---Wall Time---  --- Name ---",
            "   0.1000 ( 10.0%)   0.0100 (  5.0%)   0.1100 ( 9.0%)   0.1200 ( 9.5%)  bugprone-foo",
            "",
            "   ",
        ]:
            self.assertTrue(out.is_noise(line), line)
        self.assertFalse(out.is_noise("/ws/a.cpp:1:1: error: x [y]"))
        self.assertFalse(out.is_noise("Stack dump:"))
        self.assertFalse(out.is_noise("12 is not a profile row"))


class PathTest(unittest.TestCase):
    def test_display_and_annotation_paths(self) -> None:
        self.assertEqual("src/a.cpp", out.display_path("/ws/src/a.cpp", _WORKSPACE))
        self.assertEqual("/usr/include/x.h", out.display_path("/usr/include/x.h", _WORKSPACE))
        self.assertIsNone(out.annotation_path("/usr/include/x.h", _WORKSPACE))
        self.assertIsNone(out.annotation_path("/ws/bazel-out/k8/bin/gen.h", _WORKSPACE))
        self.assertIsNone(out.annotation_path("/ws/external/qt/x.h", _WORKSPACE))
        self.assertEqual("src/a.cpp", out.annotation_path("/ws/src/a.cpp", _WORKSPACE))

    def test_windows_paths_display_with_forward_slashes(self) -> None:
        workspace = PureWindowsPath("C:/ws")
        self.assertEqual("src/a.cpp", out.display_path(r"C:\ws\src\a.cpp", workspace))
        self.assertEqual("src/a.cpp", out.annotation_path(r"c:\ws\src\a.cpp", workspace))

    def test_paths_through_a_symlinked_workspace(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            real = Path(directory).resolve() / "real"
            (real / "src").mkdir(parents=True)
            (real / "src" / "a.cpp").write_text("")
            link = Path(directory).resolve() / "link"
            link.symlink_to(real, target_is_directory=True)
            spelled = str(link / "src" / "a.cpp")
            self.assertEqual("src/a.cpp", out.display_path(spelled, real))
            self.assertEqual("src/a.cpp", out.annotation_path(spelled, real))


if __name__ == "__main__":
    unittest.main()
