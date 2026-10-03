#!/usr/bin/env python3

import unittest

import clang_tidy_profile as profile

_TABLE = """\
===-------------------------------------------------------------------------===
                          clang-tidy checks profiling
===-------------------------------------------------------------------------===
  Total Execution Time: 0.0500 seconds (0.0600 wall clock)

   ---User Time---   --System Time--   --User+System--   ---Wall Time---  --- Name ---
  0.0300 (60.0%) 0.0100 (50.0%) 0.0400 (57.1%) 0.0300 (50.0%) bugprone-infinite-loop
  0.0200 (40.0%) 0.0100 (50.0%) 0.0300 (42.9%) 0.0300 (50.0%) readability-braces-around-statements
  0.0500 (100.0%) 0.0200 (100.0%) 0.0700 (100.0%) 0.0600 (100.0%) Total
"""

_TABLE_WITH_INSTR = """\
   ---User Time---   --System Time--   --User+System--   ---Wall Time---  ---Instr---  --- Name ---
  0.0100 (50.0%) 0.0000 (0.0%) 0.0100 (50.0%) 0.0100 (50.0%) 20723 (50.0%) bugprone-infinite-loop
  0.0100 (50.0%) 0.0000 (0.0%) 0.0100 (50.0%) 0.0100 (50.0%) 20723 (50.0%) Total
"""


class ParseCheckProfileTest(unittest.TestCase):
    def test_reads_wall_time_per_check_and_skips_total(self) -> None:
        checks = profile.parse_check_profile(_TABLE)
        self.assertEqual(
            {
                "bugprone-infinite-loop": 0.03,
                "readability-braces-around-statements": 0.03,
            },
            checks,
        )

    def test_sums_the_same_check_across_several_tables(self) -> None:
        checks = profile.parse_check_profile(_TABLE + "other output\n" + _TABLE)
        self.assertAlmostEqual(0.06, checks["bugprone-infinite-loop"])

    def test_tolerates_an_instruction_count_column(self) -> None:
        checks = profile.parse_check_profile(_TABLE_WITH_INSTR)
        self.assertEqual({"bugprone-infinite-loop": 0.01}, checks)

    def test_ignores_text_without_a_table(self) -> None:
        self.assertEqual({}, profile.parse_check_profile("warning: something\n"))


class RenderMarkdownTest(unittest.TestCase):
    def test_lists_phases_and_checks_slowest_first(self) -> None:
        text = profile.render_markdown(
            {"a-fast": 1.0, "b-slow": 9.0},
            {"prebuild": 12.34, "analysis": 5.0},
            translation_units=42,
        )
        self.assertIn("Translation units: 42", text)
        self.assertIn("| prebuild | 12.3 |", text)
        self.assertLess(text.index("b-slow"), text.index("a-fast"))
        self.assertIn("| b-slow | 9.0 | 90.0% |", text)

    def test_limits_the_check_table_to_top(self) -> None:
        checks = {f"check-{index}": float(index + 1) for index in range(5)}
        text = profile.render_markdown(checks, {}, translation_units=1, top=2)
        self.assertIn("check-4", text)
        self.assertIn("check-3", text)
        self.assertNotIn("check-2", text)

    def test_says_so_when_there_is_no_profile_data(self) -> None:
        text = profile.render_markdown({}, {"analysis": 1.0}, translation_units=3)
        self.assertIn("No check profile data", text)
        self.assertIn("| analysis | 1.0 |", text)


if __name__ == "__main__":
    unittest.main()
