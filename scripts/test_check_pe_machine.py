"""Tests for the PE machine validation CLI."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().with_name("check-pe-machine.py")


class CheckPeMachineTest(unittest.TestCase):
    def test_rejects_a_path_outside_the_invocation_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            result = subprocess.run(
                [sys.executable, SCRIPT, "../outside.exe", "14c"],
                cwd=temp_dir,
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("must name a file beneath the current directory", result.stderr)


if __name__ == "__main__":
    unittest.main()
