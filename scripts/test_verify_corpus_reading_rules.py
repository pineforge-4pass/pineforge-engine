"""Tests for the corpus grader's source and export readers."""

import unittest

from verify_corpus import detect_parity_profile


class ReadingRulesTests(unittest.TestCase):
    def test_profile_scan_ignores_string_literals_and_comparisons(self):
        cases = (
            ('strategy.exit("ordinary", limit=101)', "strict"),
            ("// trail_points=1\n/* trail_offset=2 */", "strict"),
            ('// a comment\nstrategy.exit("trail", trail_price = 101)', "production"),
            ('strategy("trail_points=1")', "strict"),
            ("if trail_points == 1\n    strategy.close('L')", "strict"),
            ("int TRAIL_POINTS = 1", "strict"),
            (r'strategy("an escaped \" quote: trail_offset=1")', "strict"),
            ("""strategy('trail_price=1')""", "strict"),
            ('strategy("""a " quote: trail_points=1 " inside""")', "strict"),
            ('strategy.exit("https://x", trail_points=1)', "production"),
            ('strategy.exit("/*", trail_offset=1) // */', "production"),
        )
        for source, expected in cases:
            with self.subTest(source=source):
                self.assertEqual(detect_parity_profile(source), expected)


if __name__ == "__main__":
    unittest.main()
