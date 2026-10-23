"""Protect the scanner launcher's command line and workspace requirement."""

import contextlib
import io
import os
import unittest
from pathlib import Path
from unittest import mock

import sonar_scan


class SonarScanTest(unittest.TestCase):
    def test_command_disables_jre_provisioning_first(self):
        self.assertEqual(
            sonar_scan.scan_command(Path("/s/bin/sonar-scanner"), ["-X"]),
            ["/s/bin/sonar-scanner", "-Dsonar.scanner.skipJreProvisioning=true", "-X"],
        )

    def test_main_requires_workspace_directory(self):
        stderr = io.StringIO()
        with (
            mock.patch.dict(os.environ, clear=True),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual(sonar_scan.main([]), 2)
        self.assertIn("BUILD_WORKSPACE_DIRECTORY", stderr.getvalue())

    def test_main_requires_the_scanner_in_the_runfiles(self):
        stderr = io.StringIO()
        with (
            mock.patch.dict(os.environ, {"BUILD_WORKSPACE_DIRECTORY": "/w"}, clear=True),
            mock.patch.object(sonar_scan.runfiles, "Create") as create,
            contextlib.redirect_stderr(stderr),
        ):
            create.return_value.Rlocation.return_value = None
            self.assertEqual(sonar_scan.main([]), 2)
        self.assertIn(sonar_scan.SCANNER, stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
