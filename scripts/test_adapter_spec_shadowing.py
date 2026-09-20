#!/usr/bin/env python3
"""Mutation self-test for check_adapter_spec_shadowing.py: the check must fail
on the shapes it exists to catch and pass on the repository's adapter."""
from __future__ import annotations

import re
import unittest

from check_adapter_spec_shadowing import ADAPTER, ShapeError, check

PROJECT_ANCHOR = "        margin.maintenance_short = margin_short_fraction;\n"


class SpecShadowing(unittest.TestCase):
    def setUp(self):
        self.text = ADAPTER.read_text()
        self.assertEqual(self.text.count(PROJECT_ANCHOR), 1, "project() anchor drifted")

    def test_repository_adapter_is_clean(self):
        self.assertEqual(check(self.text), [])

    def test_declaring_a_shadowed_sizing_knob_fails(self):
        for line in ("        margin.sizing = NativeLiquidationSizing::ShortfallMultiple;\n",
                     "        margin.shortfall_multiple = 4.0;\n",
                     "        margin.liquidation_min_units = 1.0;\n"):
            mutated = self.text.replace(PROJECT_ANCHOR, PROJECT_ANCHOR + line)
            findings = check(mutated)
            self.assertEqual(len(findings), 1, line)
            self.assertIn("resolve_margin_call_units() always answers", findings[0])

    def test_a_yielding_hook_makes_the_knob_live(self):
        # The same declaration is legitimate once the hook can return nullopt.
        declared = self.text.replace(PROJECT_ANCHOR, PROJECT_ANCHOR
                                     + "        margin.shortfall_multiple = 4.0;\n")
        hook = re.search(r"PineExecutionAdapter::resolve_margin_call_units\s*\([^{]*\{", declared)
        self.assertIsNotNone(hook)
        yielding = declared[:hook.end()] + "\n    if (view.mark < 0.0) return std::nullopt;\n" + declared[hook.end():]
        self.assertEqual(check(yielding), [])

    def test_a_comment_is_not_an_assignment(self):
        commented = self.text.replace(PROJECT_ANCHOR, PROJECT_ANCHOR
                                      + "        // margin.shortfall_multiple = 4.0;\n")
        self.assertEqual(check(commented), [])

    def test_gated_report_field_under_the_wrong_policy_fails(self):
        mutated = self.text.replace(PROJECT_ANCHOR, PROJECT_ANCHOR
                                    + "        spec.report_open_position_at_end = true;\n")
        findings = check(mutated)
        self.assertEqual(len(findings), 1)
        self.assertIn("report_open_position_at_end", findings[0])

    def test_missing_project_is_a_shape_error(self):
        with self.assertRaises(ShapeError):
            check(self.text.replace("PineExecutionAdapter::project(", "PineExecutionAdapter::projected("))


if __name__ == "__main__":
    unittest.main()
