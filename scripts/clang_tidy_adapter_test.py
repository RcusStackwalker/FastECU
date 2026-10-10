#!/usr/bin/env python3
"""Exercise custom-check activation through an upstream runner."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class ClangTidyAdapterTest(unittest.TestCase):
    def test_adds_tidy_option_and_preserves_upstream_exit_status(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            upstream = Path(directory) / "run-clang-tidy"
            upstream.write_text(
                "import sys\n"
                "def get_tidy_invocation(*args, **kwargs):\n"
                "    return ['clang-tidy', '-p=database', 'source.cpp']\n"
                "async def main():\n"
                "    print(get_tidy_invocation())\n"
                "    print(sys.argv[1:])\n"
                "    sys.exit(7)\n"
            )
            result = subprocess.run(
                [
                    sys.executable,
                    str(Path(__file__).with_name("clang_tidy_adapter.py")),
                    "--upstream",
                    str(upstream),
                    "--",
                    "-j",
                    "2",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(7, result.returncode, result.stderr)
            self.assertEqual("", result.stderr)
            self.assertIn("'--experimental-custom-checks'", result.stdout)
            self.assertIn("'-p=database', 'source.cpp'", result.stdout)
            self.assertIn("['-j', '2']", result.stdout)

    def test_incompatible_upstream_fails_with_actionable_message(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            upstream = Path(directory) / "run-clang-tidy.py"
            upstream.write_text("async def main():\n    pass\n")
            result = subprocess.run(
                [
                    sys.executable,
                    str(Path(__file__).with_name("clang_tidy_adapter.py")),
                    "--upstream",
                    str(upstream),
                    "--",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(1, result.returncode)
            self.assertIn("incompatible run-clang-tidy", result.stderr)


if __name__ == "__main__":
    unittest.main()
