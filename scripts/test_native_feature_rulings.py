#!/usr/bin/env python3
"""Mutation self-test of check_native_feature_rulings.py (R5 audit lane P6).

The check reads the repository's own header, adapter and ADR; each case below
breaks one of them in memory and requires the finding that names the break,
so a check that silently stopped reading its inputs fails here.
"""
from __future__ import annotations

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_native_feature_rulings as rulings  # noqa: E402

ROOT = rulings.ROOT
HEADER = (ROOT / rulings.HEADER).read_text()
ADAPTER = (ROOT / rulings.ADAPTER).read_text()
ADR = (ROOT / rulings.ADR).read_text()
SOURCE_LAYER = rulings.read_source_layer(ROOT)


def read_file(relative: str) -> str | None:
    path = ROOT / relative
    return path.read_text(errors="replace") if path.is_file() else None


def findings(header: str = HEADER, adapter: str = ADAPTER, adr: str = ADR,
             source_layer: str = SOURCE_LAYER, reader=read_file) -> list[str]:
    return rulings.check(header, adapter, adr, source_layer, reader)


def drop_row(adr: str, first_cell_prefix: str) -> str:
    kept = [line for line in adr.splitlines() if not line.startswith(first_cell_prefix)]
    assert len(kept) == len(adr.splitlines()) - 1, first_cell_prefix
    return "\n".join(kept) + "\n"


class NativeFeatureRulings(unittest.TestCase):
    def test_the_repository_is_clean(self):
        self.assertEqual(findings(), [])

    def test_inventory_is_the_ruled_one(self):
        fields = rulings.spec_fields(HEADER)
        declared = rulings.adapter_declared(ADAPTER, fields)
        undeclared = [field for field in fields if field not in declared]
        self.assertEqual(undeclared, [
            "price_grid", "grid_rounding", "max_abs_units", "max_open_lots",
            "initial_margin_fraction", "risk", "report_open_position_at_end",
            "open_bar_view", "subscriptions", "auxiliary_feed", "quantity_tolerance"])
        kinds = {field: row.kind for row in rulings.parse_rulings(ADR) for field in row.fields}
        self.assertEqual({field for field, kind in kinds.items() if kind == "native-only"},
                         {"price_grid", "grid_rounding", "risk"})
        self.assertEqual({field for field, kind in kinds.items() if kind == "adapter-hook"},
                         {"subscriptions"})

    def test_a_new_spec_field_without_a_ruling_fails(self):
        mutated = HEADER.replace("    std::optional<NativeRiskLimits> risk;",
                                 "    std::optional<NativeRiskLimits> risk;\n"
                                 "    std::optional<double> max_notional;", 1)
        self.assertNotEqual(mutated, HEADER)
        found = findings(header=mutated)
        self.assertEqual(len(found), 1)
        self.assertIn("max_notional: the adapter never declares it and no ruling row covers it", found[0])

    def test_an_emptied_table_reports_every_undeclared_field(self):
        begin = ADR.index(rulings.BEGIN_MARKER) + len(rulings.BEGIN_MARKER)
        end = ADR.index(rulings.END_MARKER)
        found = findings(adr=ADR[:begin] + "\n" + ADR[end:])
        self.assertEqual(len(found), 11)
        self.assertTrue(all("no ruling row covers it" in finding for finding in found))

    def test_missing_markers_are_a_shape_error(self):
        with self.assertRaises(rulings.ShapeError):
            findings(adr=ADR.replace(rulings.BEGIN_MARKER, ""))

    def test_a_dropped_ruling_fails(self):
        found = findings(adr=drop_row(ADR, "| `risk` |"))
        self.assertEqual(found, [f"risk: the adapter never declares it and no ruling row covers it "
                                 f"({rulings.ADR}, between the native-feature-rulings markers)"])

    def test_a_ruling_outliving_an_adapter_declaration_fails(self):
        mutated = ADAPTER.replace("    spec.price_tick = staged.syminfo.mintick;",
                                  "    spec.price_tick = staged.syminfo.mintick;\n"
                                  "    spec.price_grid = NativePriceGrid::QuantizeFills;", 1)
        self.assertNotEqual(mutated, ADAPTER)
        found = findings(adapter=mutated)
        self.assertEqual(len(found), 1)
        self.assertIn("price_grid: stale ruling", found[0])

    def test_a_member_assignment_counts_as_a_declaration(self):
        fields = rulings.spec_fields(HEADER)
        self.assertIn("intrabar", rulings.adapter_declared(ADAPTER, fields))  # spec.intrabar.value = ...
        mutated = ADAPTER.replace("    spec.price_tick = staged.syminfo.mintick;",
                                  "    spec.price_tick = staged.syminfo.mintick;\n"
                                  "    spec.subscriptions.push_back({});", 1)
        self.assertIn("subscriptions", rulings.adapter_declared(mutated, fields))
        # A comparison is not an assignment, and a comment is not code.
        mutated = ADAPTER.replace("    spec.price_tick = staged.syminfo.mintick;",
                                  "    spec.price_tick = staged.syminfo.mintick;\n"
                                  "    if (spec.price_grid == NativePriceGrid::None) {}\n"
                                  "    // spec.risk = limits;", 1)
        declared = rulings.adapter_declared(mutated, fields)
        self.assertNotIn("price_grid", declared)
        self.assertNotIn("risk", declared)

    def test_a_helper_that_fills_the_spec_by_reference_fails(self):
        helper = "\nvoid declare_risk(NativeRunSpec& out) { out.risk = NativeRiskLimits{}; }\n"
        found = findings(source_layer=SOURCE_LAYER + helper)
        self.assertEqual(len(found), 1)
        self.assertIn("risk: stale ruling -- the source layer assigns a member of that name "
                      "outside project()", found[0])
        # The adapter's own `risk_` state is a different identifier and stays clean.
        self.assertFalse(rulings.assigned_through_any_object("risk_.halted = true;", "risk"))
        self.assertFalse(rulings.assigned_through_any_object("if (a.risk == b) {}", "risk"))

    def test_a_second_spec_construction_site_fails(self):
        found = findings(source_layer=SOURCE_LAYER + "\nvoid other() { NativeRunSpec second; }\n")
        self.assertEqual(len(found), 1)
        self.assertIn("builds 2 NativeRunSpec locals", found[0])

    def test_a_missing_consumer_fails(self):
        mutated = ADR.replace("`examples/native/native_price_grid_c.c`",
                              "`examples/native/native_price_grid_gone.c`", 1)
        found = findings(adr=mutated)
        self.assertEqual(found, ["price_grid, grid_rounding: consumer "
                                 "examples/native/native_price_grid_gone.c does not exist"])

    def test_a_consumer_that_never_spells_the_field_fails(self):
        def reader(relative: str) -> str | None:
            text = read_file(relative)
            if relative == "examples/native/native_risk_limits_strategy.cpp" and text is not None:
                return "int main() { return 0; }\n"
            return text
        found = findings(reader=reader)
        self.assertEqual(found, ["risk: consumer examples/native/native_risk_limits_strategy.cpp "
                                 "never spells it"])

    def test_a_native_only_ruling_needs_an_example_and_a_test(self):
        row = next(line for line in ADR.splitlines() if line.startswith("| `risk` |"))
        cells = row.split("|")
        without_examples = "|".join(cells[:5] + [" `tests/test_native_risk_limits.cpp` "] + cells[6:])
        found = findings(adr=ADR.replace(row, without_examples, 1))
        self.assertEqual(found, ["risk: a native-only ruling needs an examples/native/ host that exercises it"])
        without_tests = "|".join(cells[:5] + [" `examples/native/native_risk_limits_strategy.cpp` "] + cells[6:])
        found = findings(adr=ADR.replace(row, without_tests, 1))
        self.assertEqual(found, ["risk: a native-only ruling needs a tests/ unit that pins it"])

    def test_a_ruling_for_a_name_that_is_no_field_fails(self):
        mutated = ADR.replace("| `open_bar_view` |", "| `open_bar_veiw` |", 1)
        found = findings(adr=mutated)
        self.assertTrue(any("open_bar_veiw: the ruling table names it, but NativeRunSpec has no such field"
                            in finding for finding in found))
        self.assertTrue(any(finding.startswith("open_bar_view: the adapter never declares it")
                            for finding in found))

    def test_an_adapter_hook_ruling_needs_the_hook_called(self):
        mutated = SOURCE_LAYER.replace("declare_timeframe_subscriptions(", "declare_nothing(")
        found = findings(source_layer=mutated)
        self.assertEqual(found, ["subscriptions: the source layer never calls the hook "
                                 "`declare_timeframe_subscriptions` the ruling names"])

    def test_a_row_without_one_kind_is_a_shape_error(self):
        with self.assertRaises(rulings.ShapeError):
            findings(adr=ADR.replace("| `open_bar_view` | **adapter-policy** |",
                                     "| `open_bar_view` | retained |", 1))


if __name__ == "__main__":
    unittest.main()
