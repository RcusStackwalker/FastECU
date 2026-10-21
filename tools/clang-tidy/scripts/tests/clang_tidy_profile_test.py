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

    def test_tolerates_padding_inside_the_percentage(self) -> None:
        row = "  0.0100 ( 50.0%) 0.0000 (  0.0%) 0.0100 ( 50.0%) 0.0100 ( 50.0%) misc-unused\n"
        self.assertEqual({"misc-unused": 0.01}, profile.parse_check_profile(row))

    def test_a_long_digit_run_is_not_a_column(self) -> None:
        self.assertEqual({}, profile.parse_check_profile("1" * 100_000 + " (\n"))

    def test_ignores_text_without_a_table(self) -> None:
        self.assertEqual({}, profile.parse_check_profile("warning: something\n"))


class RenderCheckTableTest(unittest.TestCase):
    def test_one_table_includes_every_check(self) -> None:
        text = profile.render_check_table({f"check-{i}": float(i + 1) for i in range(20)}, 2)
        self.assertEqual(1, text.count("| Check |"))
        for i in range(20):
            self.assertIn(f"| check-{i} |", text)
        self.assertLess(text.index("| check-19 |"), text.index("| check-0 |"))

    def test_recorded_wall_time_label(self) -> None:
        text = profile.render_check_table({"a": 0.03}, 2)
        self.assertIn("recorded wall seconds summed across translation units", text)
        self.assertIn("cached", text)
        self.assertNotIn("| Phase |", text)

    def test_missing_profile_is_explicit(self) -> None:
        text = profile.render_check_table({}, 2, missing_profiles=1)
        self.assertIn("Missing profiles: 1", text)
        self.assertIn("No check profile data", text)


if __name__ == "__main__":
    unittest.main()
