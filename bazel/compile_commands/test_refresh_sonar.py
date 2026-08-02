"""Tests for the Sonar compile-commands wrapper."""

from __future__ import annotations

import importlib.util
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import patch

SCRIPT = Path(__file__).with_name("refresh_sonar.py")


def load_module() -> types.ModuleType:
    class FakeRunfiles:
        @staticmethod
        def Create() -> FakeRunfiles:
            return FakeRunfiles()

        def Rlocation(self, _: str) -> None:
            return None

    python_module = types.ModuleType("python")
    runfiles_module = types.ModuleType("python.runfiles")
    runfiles_module.runfiles = FakeRunfiles
    sys.modules["python"] = python_module
    sys.modules["python.runfiles"] = runfiles_module

    spec = importlib.util.spec_from_file_location("refresh_sonar", SCRIPT)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RefreshSonarTest(unittest.TestCase):
    def test_rejects_user_supplied_refresh_arguments(self) -> None:
        module = load_module()
        with (
            patch.object(module.sys, "argv", ["refresh_sonar.py", "--bazelrc=/tmp/evil"]),
            patch.dict(module.os.environ, {"BUILD_WORKSPACE_DIRECTORY": "/tmp"}),
            self.assertRaisesRegex(SystemExit, "does not accept arguments"),
        ):
            module.main()


if __name__ == "__main__":
    unittest.main()
