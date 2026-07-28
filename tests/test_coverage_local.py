#!/usr/bin/env python3

import os
import stat
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


def write_executable(path: Path, contents: str) -> None:
    path.write_text(textwrap.dedent(contents).lstrip())
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


class CoverageLocalTest(unittest.TestCase):
    def test_standalone_test_binaries_use_offscreen_qt_platform(self) -> None:
        coverage_script = Path(__file__).resolve().parents[1] / "scripts" / "coverage-local.sh"

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            tools = root / "tools"
            tools.mkdir()
            output_base = root / "output-base"
            (output_base / "external").mkdir(parents=True)
            coverage_dir = root / "coverage"
            probe_result = root / "qt-platform.txt"
            probe = root / "instrumented-test"

            write_executable(
                probe,
                """
                #!/bin/sh
                printf '%s' "${QT_QPA_PLATFORM-}" > "$PROBE_RESULT_FILE"
                : > "$LLVM_PROFILE_FILE"
                """,
            )
            write_executable(
                tools / "bazel",
                """
                #!/bin/sh
                case "$1" in
                  info)
                    printf '%s\n' "$FAKE_OUTPUT_BASE"
                    ;;
                  build)
                    ;;
                  cquery)
                    printf '%s\n' "$PROBE_EXECUTABLE"
                    ;;
                  *)
                    exit 64
                    ;;
                esac
                """,
            )
            write_executable(
                tools / "llvm-profdata",
                """
                #!/bin/sh
                while [ "$#" -gt 0 ]; do
                  if [ "$1" = "-o" ]; then
                    shift
                    : > "$1"
                    exit 0
                  fi
                  shift
                done
                exit 64
                """,
            )
            write_executable(
                tools / "llvm-cov",
                """
                #!/bin/sh
                case "$1" in
                  report)
                    printf 'TOTAL 1 0 100.00%%\n'
                    ;;
                  show)
                    printf 'src/probe.cpp:\n    1|      1|int probe;\n'
                    ;;
                  *)
                    exit 64
                    ;;
                esac
                """,
            )

            env = os.environ.copy()
            env.update(
                {
                    "COVERAGE_DIR": str(coverage_dir),
                    "FAKE_OUTPUT_BASE": str(output_base),
                    "LLVM_COV": str(tools / "llvm-cov"),
                    "LLVM_PROFDATA": str(tools / "llvm-profdata"),
                    "PATH": f"{tools}{os.pathsep}{env['PATH']}",
                    "PROBE_EXECUTABLE": str(probe),
                    "PROBE_RESULT_FILE": str(probe_result),
                }
            )

            result = subprocess.run(
                [str(coverage_script)],
                check=False,
                cwd=coverage_script.parents[1],
                env=env,
                capture_output=True,
                text=True,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(probe_result.read_text(), "offscreen")


if __name__ == "__main__":
    unittest.main()
