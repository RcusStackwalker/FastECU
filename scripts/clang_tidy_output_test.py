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

    def test_enabled_checks_header_is_noise(self) -> None:
        self.assertTrue(out.is_noise("Enabled checks:"))


class PathTest(unittest.TestCase):
    def test_display_and_annotation_paths(self) -> None:
        self.assertEqual("src/a.cpp", out.display_path("/ws/src/a.cpp", _WORKSPACE))
        self.assertEqual("/usr/include/x.h", out.display_path("/usr/include/x.h", _WORKSPACE))
        self.assertIsNone(out.annotation_path("/usr/include/x.h", _WORKSPACE))
        self.assertIsNone(out.annotation_path("/ws/bazel-out/k8/bin/gen.h", _WORKSPACE))
        self.assertIsNone(out.annotation_path("/ws/external/qt/x.h", _WORKSPACE))
        self.assertEqual("src/a.cpp", out.annotation_path("/ws/src/a.cpp", _WORKSPACE))

    def test_relative_paths_are_workspace_relative(self) -> None:
        # Hedron's compile commands spell sources relative to the workspace,
        # and clang echoes that spelling in its diagnostics.
        self.assertEqual("src/a.cpp", out.annotation_path("src/a.cpp", _WORKSPACE))
        self.assertEqual("src/a.cpp", out.display_path("./src/a.cpp", _WORKSPACE))
        self.assertIsNone(out.annotation_path("../outside/a.cpp", _WORKSPACE))
        self.assertIsNone(out.annotation_path("bazel-out/k8/bin/gen.h", _WORKSPACE))

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


_NOTE = out.Finding(out.Diagnostic("/ws/src/a.h", 3, 1, "note", "declared here", None))
_FINDING = out.Finding(
    out.Diagnostic("/ws/src/a.cpp", 12, 5, "error", "use auto", "modernize-use-auto"),
    detail=("   12 |   int x = f();", "      |   ^"),
    notes=(_NOTE,),
)
_PROGRESS_A = "[1/2][0.3s] clang-tidy -p=/db /ws/src/a.cpp"
_PROGRESS_B = "[2/2][0.4s] clang-tidy -p=/db /ws/src/b.cpp"
_FINDING_LINE = "/ws/src/a.cpp:12:5: error: use auto [modernize-use-auto,-warnings-as-errors]"
_HEADER_LINE = "/ws/src/a.h:3:1: warning: shared [c]"


def _finding(severity: str, path: str = "/ws/src/a.cpp", message: str = "m") -> out.Finding:
    return out.Finding(out.Diagnostic(path, 1, 1, severity, message, "c"))


class FormatTest(unittest.TestCase):
    def test_console_format_plain(self) -> None:
        self.assertEqual(
            "error: src/a.cpp:12:5 [modernize-use-auto]\n"
            "  use auto\n"
            "       12 |   int x = f();\n"
            "          |   ^\n"
            "  note: src/a.h:3:1 declared here",
            out.format_finding(_FINDING, workspace=_WORKSPACE, color=False),
        )

    def test_console_format_indents_note_detail(self) -> None:
        note = out.Finding(_NOTE.diagnostic, detail=("    3 | int f();",))
        finding = out.Finding(_FINDING.diagnostic, notes=(note,))
        self.assertTrue(
            out.format_finding(finding, workspace=_WORKSPACE, color=False).endswith(
                "  note: src/a.h:3:1 declared here\n        3 | int f();"
            )
        )

    def test_console_format_colour_wraps_severity_and_check(self) -> None:
        text = out.format_finding(_FINDING, workspace=_WORKSPACE, color=True)
        self.assertTrue(
            text.startswith(
                "\x1b[1;31merror\x1b[0m: src/a.cpp:12:5 \x1b[2m[modernize-use-auto]\x1b[0m"
            ),
            text,
        )
        self.assertIn("\x1b[1;36mnote\x1b[0m: src/a.h:3:1", text)

    def test_annotation_error_with_folded_note(self) -> None:
        self.assertEqual(
            "::error file=src/a.cpp,line=12,col=5,title=modernize-use-auto::"
            "use auto%0Anote: src/a.h:3:1 declared here",
            out.format_annotation(_FINDING, workspace=_WORKSPACE),
        )

    def test_annotation_levels(self) -> None:
        for severity, level in [
            ("warning", "::warning "),
            ("remark", "::notice "),
            ("fatal error", "::error "),
        ]:
            annotation = out.format_annotation(_finding(severity), workspace=_WORKSPACE)
            assert annotation is not None
            self.assertTrue(annotation.startswith(level), annotation)

    def test_annotation_title_falls_back_to_severity(self) -> None:
        finding = out.Finding(out.Diagnostic("/ws/a.cpp", 1, 1, "warning", "m", None))
        self.assertIn(",title=warning::", out.format_annotation(finding, workspace=_WORKSPACE))

    def test_annotation_escapes_data_and_properties(self) -> None:
        finding = out.Finding(
            out.Diagnostic("/ws/src/a,b.cpp", 1, 1, "warning", "100% bad: x\ny", "c:d")
        )
        self.assertEqual(
            "::warning file=src/a%2Cb.cpp,line=1,col=1,title=c%3Ad::100%25 bad: x%0Ay",
            out.format_annotation(finding, workspace=_WORKSPACE),
        )

    def test_annotation_skipped_outside_workspace(self) -> None:
        self.assertIsNone(
            out.format_annotation(_finding("error", "/usr/include/x.h"), workspace=_WORKSPACE)
        )


class OutputStreamTest(unittest.TestCase):
    def stream(self, *, annotate: bool = False) -> tuple[list[str], out.OutputStream]:
        lines: list[str] = []
        stream = out.OutputStream(
            workspace=_WORKSPACE, write=lines.append, color=False, annotate=annotate
        )
        return lines, stream

    def test_stream_prints_progress_finding_and_drops_noise(self) -> None:
        lines, stream = self.stream()
        for line in [
            _PROGRESS_A,
            _FINDING_LINE,
            "   12 |   int x = f();",
            "      |   ^",
            "1 warning generated.",
            "",
        ]:
            stream.feed(line)
        self.assertEqual(out.StreamCounts(unique=1, raw=1), stream.finish())
        self.assertEqual("[1/2] src/a.cpp", lines[0])
        self.assertTrue(lines[1].startswith("error: src/a.cpp:12:5 [modernize-use-auto]"))
        self.assertIn("|   ^", lines[1])
        self.assertEqual(2, len(lines))

    def test_finding_is_written_when_the_next_line_arrives_not_at_finish(self) -> None:
        lines, stream = self.stream()
        stream.feed(_PROGRESS_A)
        stream.feed(_FINDING_LINE)
        stream.feed(_PROGRESS_B)
        self.assertEqual(3, len(lines))
        self.assertTrue(lines[1].startswith("error: src/a.cpp:12:5"))
        self.assertEqual("[2/2] src/b.cpp", lines[2])

    def test_header_finding_from_two_units_prints_once(self) -> None:
        lines, stream = self.stream(annotate=True)
        for line in [
            _PROGRESS_A,
            _HEADER_LINE,
            "/ws/src/a.cpp:1:1: note: included from here",
            _PROGRESS_B,
            _HEADER_LINE,
            "/ws/src/b.cpp:1:1: note: included from here",
        ]:
            stream.feed(line)
        self.assertEqual(out.StreamCounts(unique=1, raw=2), stream.finish())
        self.assertEqual(1, sum(line.startswith("warning: src/a.h:3:1") for line in lines))
        self.assertEqual(1, sum(line.startswith("::warning") for line in lines))

    def test_annotate_writes_annotation_after_console_block(self) -> None:
        lines, stream = self.stream(annotate=True)
        stream.feed(_FINDING_LINE)
        stream.finish()
        self.assertEqual(2, len(lines))
        self.assertTrue(lines[0].startswith("error: src/a.cpp:12:5"))
        self.assertTrue(lines[1].startswith("::error file=src/a.cpp,line=12,col=5"))

    def test_no_annotation_without_annotate(self) -> None:
        lines, stream = self.stream()
        stream.feed(_FINDING_LINE)
        stream.finish()
        self.assertFalse(any(line.startswith("::") for line in lines))

    def test_unrecognised_line_flushes_pending_finding_and_passes_through(self) -> None:
        lines, stream = self.stream()
        for line in [
            _FINDING_LINE,
            "Stack dump:",
            "0.  Program arguments: clang-tidy /ws/src/a.cpp",
        ]:
            stream.feed(line)
        stream.finish()
        self.assertEqual(3, len(lines))
        self.assertTrue(lines[0].startswith("error: src/a.cpp:12:5"))
        self.assertEqual(
            ["Stack dump:", "0.  Program arguments: clang-tidy /ws/src/a.cpp"], lines[1:]
        )

    def test_enabled_checks_listing_is_dropped(self) -> None:
        # run-clang-tidy's opening `clang-tidy -list-checks` call.
        lines, stream = self.stream()
        for line in [
            "Enabled checks:",
            "    bugprone-argument-comment",
            "    clang-analyzer-unix.cstring.NullArg",
            "",
            _PROGRESS_A,
        ]:
            stream.feed(line)
        self.assertEqual(["[1/2] src/a.cpp"], lines)

    def test_indented_word_outside_the_check_listing_passes_through(self) -> None:
        lines, stream = self.stream()
        for line in ["Enabled checks:", "    bugprone-x", _PROGRESS_A, "Traceback:", "    raise"]:
            stream.feed(line)
        self.assertEqual(["[1/2] src/a.cpp", "Traceback:", "    raise"], lines)

    def test_orphan_note_passes_through_verbatim(self) -> None:
        lines, stream = self.stream()
        stream.feed("/ws/src/a.h:3:1: note: declared here")
        self.assertEqual(out.StreamCounts(unique=0, raw=0), stream.finish())
        self.assertEqual(["/ws/src/a.h:3:1: note: declared here"], lines)


if __name__ == "__main__":
    unittest.main()
