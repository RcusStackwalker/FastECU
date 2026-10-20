"""Central exception policy must fail closed without penalizing partial scope."""

import tempfile
import unittest
from pathlib import Path

import clang_tidy_exceptions as policy


class ExceptionPolicyTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "exceptions.yaml"

    def valid(self, extra=""):
        self.path.write_text(
            "version: 1\nexceptions:\n"
            "  - check: fastecu-result-assertion\n"
            "    file: src/backend/ports/result_matchers_test.cpp\n"
            "    symbol: ResultMatchers.Status\n"
            "    rationale: This test exercises the any-error matcher itself.\n" + extra
        )

    def test_valid_identity_and_rationale(self):
        self.valid()
        rules = policy.load_exceptions(self.path)
        self.assertEqual(1, len(rules))
        self.assertEqual("ResultMatchers.Status", rules[0].symbol)
        self.assertIn("any-error", rules[0].rationale)

    def test_unknown_version_and_keys_are_rejected(self):
        for contents in (
            "version: 2\nexceptions: []\n",
            "version: true\nexceptions: []\n",
            "version: 1\nexceptions: []\nunknown: true\n",
            "version: 1\nexceptions: wrong\n",
        ):
            with self.subTest(contents=contents):
                self.path.write_text(contents)
                with self.assertRaises(policy.ExceptionConfigError):
                    policy.load_exceptions(self.path)

    def test_empty_rationale_is_rejected(self):
        self.valid()
        self.path.write_text(
            self.path.read_text().replace(
                "This test exercises the any-error matcher itself.", '"   "'
            )
        )
        with self.assertRaisesRegex(policy.ExceptionConfigError, "rationale"):
            policy.load_exceptions(self.path)

    def test_paths_are_portable_canonical_and_relative(self):
        for path in (
            "/absolute/test.cpp",
            "../test.cpp",
            "src/../test.cpp",
            "./test.cpp",
            "src//test.cpp",
            "C:/test.cpp",
        ):
            with self.subTest(path=path):
                self.valid()
                self.path.write_text(
                    self.path.read_text().replace(
                        "src/backend/ports/result_matchers_test.cpp", path
                    )
                )
                with self.assertRaisesRegex(policy.ExceptionConfigError, "path"):
                    policy.load_exceptions(self.path)

    def test_duplicate_entries_and_yaml_keys_are_rejected(self):
        self.valid()
        entry = self.path.read_text().split("exceptions:\n")[1]
        self.path.write_text(self.path.read_text() + entry)
        with self.assertRaisesRegex(policy.ExceptionConfigError, "duplicate"):
            policy.load_exceptions(self.path)
        self.path.write_text("version: 1\nversion: 1\nexceptions: []\n")
        with self.assertRaisesRegex(policy.ExceptionConfigError, "duplicate"):
            policy.load_exceptions(self.path)

    def test_unused_entries_fail_full_scope_only(self):
        self.valid()
        rules = policy.load_exceptions(self.path)
        policy.validate_usage(rules, set(), full_scope=False)
        with self.assertRaisesRegex(policy.ExceptionConfigError, "unused"):
            policy.validate_usage(rules, set(), full_scope=True)
        policy.validate_usage(rules, {rules[0].identity}, full_scope=True)

    def test_unknown_usage_fails(self):
        self.path.write_text("version: 1\nexceptions: []\n")
        with self.assertRaisesRegex(policy.ExceptionConfigError, "unknown"):
            policy.validate_usage(
                policy.load_exceptions(self.path),
                {("fastecu-result-assertion", "src/a.cpp", "Suite.Test")},
                full_scope=True,
            )


if __name__ == "__main__":
    unittest.main()
