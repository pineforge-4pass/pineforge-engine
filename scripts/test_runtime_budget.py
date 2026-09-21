#!/usr/bin/env python3
"""Mutation controls for the relative runtime budget."""
import unittest

from check_runtime_budget import LIMIT, Sample, best, enforce_ratio, gate_ratio, parse_sample


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


class WitnessSamples(unittest.TestCase):
    """A40 rev 7 (Q9): the gate reads process CPU time; wall clock is a diagnostic only."""

    def test_parse_reads_both_markers(self):
        sample = parse_sample("PF_RUNTIME_SECONDS=0.767000958\n"
                              "PF_RUNTIME_CPU_SECONDS=0.310634000\n"
                              "runtime replay callbacks=43008 bars=43008\n")
        self.assertEqual(sample, Sample(cpu=0.310634, wall=0.767000958))

    def test_parse_rejects_a_witness_without_the_cpu_marker(self):
        # A pre-rev-7 witness (wall clock only) must not be gated silently on wall.
        with self.assertRaisesRegex(RuntimeError, "no timing marker"):
            parse_sample("PF_RUNTIME_SECONDS=0.5\n")
        with self.assertRaisesRegex(RuntimeError, "no timing marker"):
            parse_sample("PF_RUNTIME_CPU_SECONDS=0.5\n")
        with self.assertRaisesRegex(RuntimeError, "no timing marker"):
            parse_sample("runtime replay callbacks=1 bars=1\n")

    def test_best_is_the_minimum_per_axis(self):
        runs = [Sample(cpu=0.31, wall=0.72), Sample(cpu=0.29, wall=0.86), Sample(cpu=0.30, wall=0.51)]
        self.assertEqual(best(runs), Sample(cpu=0.29, wall=0.51))
        with self.assertRaisesRegex(ValueError, "empty"):
            best([])

    def test_gate_enforces_cpu_time_not_wall_clock(self):
        # Measured on the same tree at load average 135: the wall ratio of the
        # two legs was 16.5x, the CPU ratio 10.1x. A loaded host must pass.
        baselines = [Sample(cpu=0.0290, wall=0.0311), Sample(cpu=0.0353, wall=0.0424)]
        candidates = [Sample(cpu=0.2920, wall=0.5118), Sample(cpu=0.3138, wall=0.7208)]
        self.assertGreater(best(candidates).wall / best(baselines).wall, LIMIT)
        self.assertAlmostEqual(gate_ratio(candidates, baselines), 0.2920 / 0.0290)

    def test_gate_still_fails_a_doubled_cpu_cost(self):
        # A deliberate 2x slowdown of the candidate's own work: CPU time doubles
        # and the verdict flips, whatever the wall clock says.
        baselines = [Sample(cpu=0.0290, wall=0.0311)]
        candidates = [Sample(cpu=2 * 0.2920, wall=0.5118)]
        with self.assertRaisesRegex(ValueError, "20.138x"):
            gate_ratio(candidates, baselines)

    def test_a_quiet_wall_clock_cannot_rescue_a_slow_cpu_sample(self):
        baselines = [Sample(cpu=0.0290, wall=0.0290)]
        candidates = [Sample(cpu=0.60, wall=0.0290)]
        with self.assertRaisesRegex(ValueError, "20.690x"):
            gate_ratio(candidates, baselines)


if __name__ == "__main__":
    unittest.main()
