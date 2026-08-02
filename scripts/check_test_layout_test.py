import importlib.util
import tempfile
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).with_name("check-test-layout.py")
SPEC = importlib.util.spec_from_file_location("check_test_layout", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)
check = MODULE.check


class TestLayoutPolicyTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / "tests").mkdir()
        self.allowlist = self.root / "tests/layout_allowlist.txt"

    def tearDown(self):
        self.temp.cleanup()

    def write(self, relative: str, text: str = "// probe\n"):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_accepts_documented_probe(self):
        self.write("tests/probe.cpp", "int main() { return 0; }\n")
        self.allowlist.write_text("tests/probe.cpp|process-boundary probe\n")
        self.assertEqual([], check(self.root, self.allowlist))

    def test_rejects_new_centralized_gtest(self):
        self.write("tests/unit.cpp", "#include <gtest/gtest.h>\nTEST(X, Y) {}\n")
        self.allowlist.write_text("tests/unit.cpp|incorrectly centralized\n")
        self.assertTrue(
            any("GoogleTest suites" in error for error in check(self.root, self.allowlist))
        )

    def test_rejects_boilerplate_main_anywhere(self):
        self.write(
            "src/widget_test.cpp", "int main(){ InitGoogleTest(); return RUN_ALL_TESTS(); }\n"
        )
        self.allowlist.write_text("")
        self.assertTrue(
            any("runner boilerplate" in error for error in check(self.root, self.allowlist))
        )

    def test_rejects_missing_rationale(self):
        self.write("tests/probe.cpp")
        self.allowlist.write_text("tests/probe.cpp|\n")
        self.assertTrue(
            any("missing a rationale" in error for error in check(self.root, self.allowlist))
        )

    def test_rejects_stale_allowlist_entry(self):
        self.allowlist.write_text("tests/gone.cpp|removed probe\n")
        self.assertTrue(
            any("stale allowlist" in error for error in check(self.root, self.allowlist))
        )


if __name__ == "__main__":
    unittest.main()
