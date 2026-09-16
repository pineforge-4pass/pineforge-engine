#!/usr/bin/env python3
"""Mutation controls for the relative runtime budget."""
import unittest

from check_runtime_budget import enforce_ratio


class RuntimeBudget(unittest.TestCase):
    def test_ratio_at_or_below_limit_passes(self):
        self.assertAlmostEqual(enforce_ratio(15.0, 1.0), 15.0)
        self.assertAlmostEqual(enforce_ratio(1.5, 1.0), 1.5)
        self.assertAlmostEqual(enforce_ratio(0.75, 1.0), 0.75)

    def test_absolute_twelve_second_escape_is_gone(self):
        with self.assertRaisesRegex(ValueError, "16.000x"):
            enforce_ratio(32.0, 2.0)
        with self.assertRaisesRegex(ValueError, "15.020x"):
            enforce_ratio(15.02, 1.0)

    def test_nonpositive_sample_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "positive"):
            enforce_ratio(0.0, 1.0)


if __name__ == "__main__":
    unittest.main()
