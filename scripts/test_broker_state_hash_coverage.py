#!/usr/bin/env python3
"""Metadata-only mutation tests for nested broker-state hash coverage.

Every mutation is written under TemporaryDirectory; engine source is read only.
No compile, engine execution, strategy, corpus, feed or grader is involved.
"""
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path
import tempfile
import unittest

import check_broker_state_hash_coverage as checker
from gen_pending_order_mirror import members

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "include/pineforge/engine.hpp").read_text()
EXPANSION = (ROOT / "include/pineforge/reservation_expansion.hpp").read_text()
EXPANSION_SOURCE = (ROOT / "src/reservation_expansion.cpp").read_text()
FROZEN = (ROOT / "include/pineforge/compat/pine/frozen_market_instruction.hpp").read_text()
QUANTITY = (ROOT / "include/pineforge/quantity_intent.hpp").read_text()
EVENTS = (ROOT / "include/pineforge/broker_events.hpp").read_text()
ACTIVATION = (ROOT / "include/pineforge/leg_activation.hpp").read_text()
EXIT_POLICY = (ROOT / "include/pineforge/compat/pine/exit_activation.hpp").read_text()
BIRTH = (ROOT / "include/pineforge/order_birth.hpp").read_text()
INTRADAY = (ROOT / "include/pineforge/compat/pine/intraday_order_budget.hpp").read_text()
POLICY = (ROOT / "include/pineforge/compat/pine/intraday_cap.hpp").read_text()
OBLIGATION = (ROOT / "include/pineforge/position_close_obligation.hpp").read_text()
SOURCE = (ROOT / "src/engine_state_hash.cpp").read_text()
STREAM = (ROOT / "src/engine_stream.cpp").read_text()
WAIVERS = (ROOT / "scripts/broker_state_hash_waivers.txt").read_text()


class PhysicalLotCoverage(unittest.TestCase):
    def check(self, header=HEADER, source=SOURCE, waivers=WAIVERS, events=EVENTS,
              intraday=INTRADAY, policy=POLICY, obligation=OBLIGATION, stream=STREAM, quantity=QUANTITY, birth=BIRTH, activation=ACTIVATION, exit_policy=EXIT_POLICY, expansion=EXPANSION, expansion_source=EXPANSION_SOURCE, frozen=FROZEN):
        with tempfile.TemporaryDirectory(prefix="pf-lot-hash-check-") as temp:
            root = Path(temp)
            admission_files=["include/pineforge/market_admission.hpp", "src/market_admission.cpp", "scripts/market_admission_schema.json", "scripts/market_admission_mirror_fields.json", "scripts/pending_order_mirror_waivers.txt"]
            for name in admission_files:
                target=root/name;target.parent.mkdir(parents=True,exist_ok=True)
                target.write_text((ROOT/name).read_text())
            for name, content in [
                ("include/pineforge/engine.hpp", header),
                ("include/pineforge/compat/pine/frozen_market_instruction.hpp", frozen),
                ("include/pineforge/broker_events.hpp", events),
                ("include/pineforge/quantity_intent.hpp", quantity),
                ("include/pineforge/exit_leg_lifecycle.hpp", (ROOT / "include/pineforge/exit_leg_lifecycle.hpp").read_text()),
                ("include/pineforge/reservation_expansion.hpp", expansion),
                ("src/reservation_expansion.cpp", expansion_source),
                ("include/pineforge/order_birth.hpp", birth),
                ("include/pineforge/leg_activation.hpp", activation),
                ("include/pineforge/compat/pine/exit_activation.hpp", exit_policy),
                ("include/pineforge/compat/pine/intraday_order_budget.hpp", intraday),
                ("include/pineforge/compat/pine/intraday_cap.hpp", policy),
                ("include/pineforge/position_close_obligation.hpp", obligation),
                ("src/engine_state_hash.cpp", source),
                ("src/engine_stream.cpp", stream),
                ("scripts/broker_state_hash_waivers.txt", waivers),
            ]:
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content)
            output = StringIO()
            with redirect_stdout(output), redirect_stderr(output):
                try:
                    code = checker.main(root)
                except SystemExit as exc:
                    code = exc.code if isinstance(exc.code, int) else 2
                    output.write(str(exc.code))
            return code, output.getvalue()

    def test_standalone_reservation_abi_is_versioned_in_header_and_source(self):
        for old in ("reservation_expansion_v0", "reservation_expansion_v2"):
            self.assertEqual(self.check(expansion=EXPANSION.replace(
                "reservation_expansion_v1", old))[0], 1)
            self.assertEqual(self.check(expansion_source=EXPANSION_SOURCE.replace(
                "reservation_expansion_v1", old))[0], 1)
        for name in ("ReservationExpansionCapture", "ReservationExpansion", "ReservationGrowthSource"):
            self.assertEqual(self.check(expansion=EXPANSION.replace(
                name + " {", name + "Outside {"))[0], 1)

    def test_every_capture_and_source_fold_is_required(self):
        for fold in ["f.b(o.reservation_expansion.capture().has_value());",
                     "f.i(capture->position_cycle);", "f.i(static_cast<int64_t>(capture->side));",
                     "f.b(capture->first_later_admission.has_value());", "f.u(*admission);",
                     "f.b(o.reservation_growth_source.reservation_owner().has_value());", "f.u(*receiver);"]:
            with self.subTest(fold=fold):
                self.assertEqual(self.check(source=SOURCE.replace(fold, ""))[0], 1)
                self.assertEqual(self.check(source=SOURCE.replace(fold, "/* " + fold + " */"))[0], 1)
                self.assertEqual(self.check(source=SOURCE.replace(fold, fold.replace("f.", "wrong.")))[0], 1)
        for owner in ["reservation_expansion", "reservation_growth_source"]:
            self.assertEqual(self.check(waivers=WAIVERS + "\npending_order." + owner + " # forbidden\n")[0], 1)

    def test_capture_nested_storage_cannot_hide(self):
        for field in ["std::optional<ReservationExpansionCapture> capture_;", "int64_t position_cycle;",
                      "PositionSide side;", "std::optional<uint64_t> first_later_admission;",
                      "std::optional<uint64_t> reservation_owner_;"]:
            with self.subTest(field=field):
                self.assertEqual(self.check(expansion=EXPANSION.replace(field,field + " int hidden;"))[0],1)
                self.assertEqual(self.check(expansion=EXPANSION.replace(field,field.replace("int64_t", "int").replace("PositionSide", "int").replace("ReservationExpansionCapture", "int")))[0],1)

    def test_capture_folds_cannot_be_conditional_rebound_or_duplicated(self):
        start = SOURCE.index("        f.b(o.reservation_expansion.capture().has_value());")
        end = SOURCE.index("\n    }\n    market_admission_journal_", start)
        block = SOURCE[start:end]
        for mutated in ["if (false) {" + block + "}", block + block,
                        block.replace("capture = o.reservation_expansion.capture()", "capture = foreign.capture()"),
                        block.replace("receiver = o.reservation_growth_source.reservation_owner()", "receiver = foreign.reservation_owner()"),
                        block.replace("admission = capture->first_later_admission", "admission = foreign.first_later_admission"),
                        "if(false) " + block]:
            with self.subTest(mutated=mutated):
                self.assertEqual(self.check(source=SOURCE[:start] + mutated + SOURCE[end:])[0],1)
    def test_frozen_instruction_live_payloads_and_role_cannot_escape(self):
        for fold in ["f.i(static_cast<int64_t>(o.pine_frozen_market_instruction.kind()));",
                     "f.d(transaction->own_units);", "f.d(transaction->transaction_units);",
                     "f.s(close->target_id);"]:
            for replacement in ["", "// " + fold, "if (false) " + fold,
                                fold.replace("f.", "other.")]:
                with self.subTest(fold=fold, replacement=replacement):
                    self.assertIn(fold, SOURCE)
                    self.assertEqual(self.check(source=SOURCE.replace(fold, replacement))[0], 1)
            self.assertEqual(self.check(source=SOURCE.replace(fold, "") + "\n" + fold)[0], 1)
        self.assertEqual(self.check(waivers=WAIVERS +
            "\npending_order.pine_frozen_market_instruction # attempted omission\n")[0], 1)

    def test_frozen_instruction_new_fields_and_alternatives_fail_closed(self):
        for old, new in [
            ("double own_units;", "double own_units; double hidden;"),
            ("std::string target_id;", "std::string target_id; uint64_t hidden;"),
            ("Value value_;", "Value value_; bool hidden_;"),
            ("std::monostate, Transaction, TargetedClose", "std::monostate, TargetedClose, Transaction"),
            ("Ordinary, Transaction, TargetedClose", "Ordinary, Transaction, TargetedClose, Extra"),
        ]:
            with self.subTest(old=old):
                self.assertIn(old, FROZEN)
                self.assertEqual(self.check(frozen=FROZEN.replace(old, new))[0], 1)

    def test_exit_activation_has_complete_unconditional_coverage(self):
        for fold in ["f.b(o.leg_activation.bounds().has_value());", "f.i(bounds->position_cycle);",
                     "f.i(bounds->stop_first_bar);", "f.i(bounds->limit_first_bar);",
                     "f.b(o.pine_exit_activation.evidence().has_value());", "f.d(evidence->stop_level);",
                     "f.d(evidence->limit_level);", "f.i(evidence->direction);",
                     "f.u(continuation->observed_fill_sequence);", "f.b(evidence->limit_continuation.has_value());"]:
            with self.subTest(fold=fold): self.assertEqual(self.check(source=SOURCE.replace(fold,""))[0],1)
        self.assertEqual(self.check(activation=ACTIVATION.replace("int64_t limit_first_bar;", "int64_t limit_first_bar; int64_t hidden;"))[0],1)
        self.assertEqual(self.check(exit_policy=EXIT_POLICY.replace("double cursor_price;", "double cursor_price; double hidden;"))[0],1)

    def test_actual_source_and_named_parser(self):
        self.assertEqual(self.check()[0], 0)
        self.assertEqual(len(members(HEADER, "PyramidEntry")), 18)
        self.assertEqual(members(HEADER), members(HEADER, "PendingOrder"))

    def test_quantity_request_presence_and_every_value_are_hashed(self):
        for fold in ["f.b(o.quantity_request.intent().has_value());",
                     "f.i(static_cast<int64_t>(intent->kind()));",
                     "f.d(intent->units());", "f.d(intent->numerator());",
                     "f.d(intent->denominator());",
                     "f.b(o.quantity_request.reservation().has_value());",
                     "f.d(reservation->units);", "f.d(reservation->basis_units);"]:
            with self.subTest(fold=fold):
                self.assertIn(fold, SOURCE)
                self.assertEqual(self.check(source=SOURCE.replace(fold, ""))[0], 1)
        self.assertEqual(self.check(waivers=WAIVERS +
            "\npending_order.quantity_request # attempted omission\n")[0], 1)

    def test_quantity_model_additions_and_variant_drift_fail_closed(self):
        for old, new in [
            ("struct Units { double amount; };", "struct Units { double amount; double extra; };"),
            ("double basis_units;", "double basis_units; double extra;"),
            ("Units, Fraction, All", "Units, All, Fraction"),
            ("Value value_;", "Value value_; double extra_;"),
            ("std::optional<QuantityIntent> intent_;", "std::optional<QuantityIntent> intent_; double extra_;"),
        ]:
            with self.subTest(old=old):
                self.assertIn(old, QUANTITY)
                self.assertEqual(self.check(quantity=QUANTITY.replace(old, new))[0], 1)

    def test_layout_and_hash_versions_must_match_v12_contract(self):
        self.assertIn("engine_script_run_v12", HEADER)
        self.assertIn('f.s("pineforge-broker-state/v12");', SOURCE)
        self.assertIn("integer(12); integer(broker_state_hash());", STREAM)
        self.assertEqual(self.check(header=HEADER.replace("engine_script_run_v12", "engine_script_run_v11"))[0], 1)
        for replacement in ['f.s("pineforge-broker-state/v11");', '',
                            '// f.s("pineforge-broker-state/v12");']:
            self.assertEqual(self.check(source=SOURCE.replace(
                'f.s("pineforge-broker-state/v12");', replacement))[0], 1)
        for replacement in ["integer(11); integer(broker_state_hash());",
                            "integer(broker_state_hash());",
                            "if (false) { integer(12); integer(broker_state_hash()); }"]:
            self.assertEqual(self.check(stream=STREAM.replace(
                "integer(12); integer(broker_state_hash());", replacement))[0], 1)

    def test_version_folds_in_unrelated_helpers_do_not_cover_entry_points(self):
        broker_fold = 'f.s("pineforge-broker-state/v12");'
        altered = SOURCE.replace(broker_fold, '') + '\nvoid other() { ' + broker_fold + ' }\n'
        self.assertEqual(self.check(source=altered)[0], 1)
        stream_fold = "integer(12); integer(broker_state_hash());"
        altered = STREAM.replace(stream_fold, '') + '\nvoid other() { ' + stream_fold + ' }\n'
        self.assertEqual(self.check(stream=altered)[0], 1)

    def test_each_lot_field_requires_its_own_fold(self):
        # A correct container loop is insufficient if any child is omitted.
        start = SOURCE.index("for (const auto& e : pyramid_entries_) {")
        end = SOURCE.index("\n    }", start)
        loop = SOURCE[start:end]
        for _type, name in members(HEADER, "PyramidEntry"):
            with self.subTest(member=name):
                altered = loop.replace(f"e.{name}", f"e.missing_{name}")
                code, output = self.check(source=SOURCE[:start] + altered + SOURCE[end:])
                self.assertEqual(code, 1, output)
                self.assertIn(name, output)

    def test_new_field_refuses_until_explicit_decision(self):
        header = HEADER.replace("struct PyramidEntry {", "struct PyramidEntry {\n    double future_margin_basis = 0;")
        code, output = self.check(header=header)
        self.assertEqual(code, 1, output)
        self.assertIn("future_margin_basis", output)

    def test_comments_reads_wrong_types_and_external_folds_do_not_cover(self):
        fold = "f.d(e.entry_commission_account);"
        for replacement in [
            "// " + fold,
            "const double ignored = e.entry_commission_account;",
            "f.u(e.entry_commission_account);",
        ]:
            with self.subTest(replacement=replacement):
                self.assertEqual(self.check(source=SOURCE.replace(fold, replacement))[0], 1)
        source = SOURCE.replace(fold, "") + "\nvoid unrelated() { f.d(e.entry_commission_account); }\n"
        self.assertEqual(self.check(source=source)[0], 1)

    def test_missing_duplicate_or_unbalanced_owner_loop_refuses(self):
        opening = "for (const auto& e : pyramid_entries_) {"
        self.assertEqual(self.check(source=SOURCE.replace(opening, "if (false) {"))[0], 2)
        self.assertEqual(self.check(source=SOURCE + "\n" + opening + "}\n")[0], 2)
        output = StringIO()
        with redirect_stderr(output), self.assertRaises(SystemExit) as stopped:
            checker._collection_loop_body(opening, "pyramid_entries_", "e")
        self.assertEqual(stopped.exception.code, 2)

    def test_explicit_waiver_is_checked_and_cannot_hide_typos(self):
        source = SOURCE.replace("f.b(e.market_pyramid_add);", "")
        waiver = "\npyramid_entry.market_pyramid_add # test-only lifecycle decision\n"
        self.assertEqual(self.check(source=source, waivers=WAIVERS + waiver)[0], 0)
        self.assertEqual(self.check(waivers=WAIVERS + waiver)[0], 1)  # now redundant
        self.assertEqual(self.check(waivers=WAIVERS + "\npyramid_entry.typo # no such field\n")[0], 1)
        self.assertEqual(self.check(waivers=WAIVERS + "\npyramid_entry.market_pyramid_add #\n")[0], 1)

    def test_unclassified_nested_declaration_refuses(self):
        for declaration in ["double first, second;", "double first = 0, second = 0;",
                            "std::vector<double> state;", "double value() const;"]:
            with self.subTest(declaration=declaration):
                header = HEADER.replace("struct PyramidEntry {", "struct PyramidEntry {\n    " + declaration)
                self.assertNotEqual(self.check(header=header)[0], 0)

    def test_complete_birth_hash_block_cannot_be_conditional(self):
        start = SOURCE.index("        f.i(static_cast<int64_t>(o.birth.cause()));")
        end = SOURCE.index("        f.i(static_cast<int64_t>(o.pine_birth_reach));", start)
        end += len("        f.i(static_cast<int64_t>(o.pine_birth_reach));")
        block = SOURCE[start:end]
        self.assertEqual(self.check(source=SOURCE[:start] + "if (false) {\n" + block + "\n}" + SOURCE[end:])[0], 1)

    def test_birth_fields_and_nested_cursor_cannot_escape_hash_coverage(self):
        for fold in ["f.u(o.birth.first_fill());", "f.i(o.birth.cursor().index());",
                     "f.i(static_cast<int64_t>(o.pine_birth_reach));"]:
            self.assertNotEqual(self.check(source=SOURCE.replace(fold, ""))[0], 0)
        altered = BIRTH.replace("    int count_ = 0;", "    int count_ = 0;\n    int hidden_cursor_fact = 0;")
        self.assertNotEqual(self.check(birth=altered)[0], 0)

    def test_pending_order_coverage_still_refuses_omission(self):
        source = SOURCE.replace("o.legs.visit(f);", "")
        self.assertNotEqual(source, SOURCE)
        code, output = self.check(source=source)
        self.assertEqual(code, 1, output)
        self.assertIn("canonical lifecycle", output)

    def test_every_opening_owner_field_requires_its_own_fold(self):
        for _type, name in members(EVENTS, "OpeningOwner"):
            with self.subTest(member=name):
                source = SOURCE.replace(f"owner.{name}", f"owner.missing_{name}")
                code, output = self.check(source=source)
                self.assertEqual(code, 1, output)
                self.assertIn(name, output)
        events = EVENTS.replace("struct OpeningOwner {", "struct OpeningOwner {\n int64_t extra;")
        code, output = self.check(events=events)
        self.assertEqual(code, 1, output)
        self.assertIn("extra", output)

    def test_new_receipt_state_or_decision_alternative_requires_review(self):
        mutations = [
            ("OpeningOwner owner_;", "OpeningOwner owner_;\n double extra;"),
            ("std::optional<OpeningReceipt> pending_;", "std::optional<OpeningReceipt> pending_;\n bool extra_ = false;"),
            ("struct Check {", "struct Check { double extra;"),
            ("struct Exempt {}", "struct Exempt { double extra; }"),
            ("std::variant<Check, Exempt>", "std::variant<Check, Exempt, int>"),
            ("OpeningDecision { Check, Exempt }", "OpeningDecision { Check, Exempt, Extra }"),
            ("OpeningContinuation { None, RemainingAdversePath }", "OpeningContinuation { None, RemainingAdversePath, Extra }"),
        ]
        for before, after in mutations:
            with self.subTest(mutation=after):
                self.assertIn(before, EVENTS)
                self.assertNotEqual(self.check(events=EVENTS.replace(before, after))[0], 0)

    def test_opening_receipt_folds_are_unconditional_and_owned(self):
        fold = "f.d(receipt->raw_fill_base());"
        for replacement in ["// " + fold, "const auto ignored = receipt->raw_fill_base();",
                            "f.u(receipt->raw_fill_base());",
                            "if (receipt->decision() == broker::OpeningDecision::Check) " + fold]:
            with self.subTest(replacement=replacement):
                self.assertEqual(self.check(source=SOURCE.replace(fold, replacement))[0], 1)
        self.assertEqual(self.check(source=SOURCE.replace(fold, "") + "\n" + fold)[0], 1)
        self.assertEqual(self.check(source=SOURCE.replace("f.b(opening_obligations_.pending());", ""))[0], 1)
        self.assertEqual(self.check(source=SOURCE.replace("f.b(receipt->requires_adverse_pass());", ""))[0], 1)

    def test_every_intraday_child_requires_its_own_typed_owned_fold(self):
        for fold in [
            "f.i(day->key);",
            "f.i(transfer->day.key);", "f.u(transfer->close_fill);",
            "f.i(transfer->source_bar);", "f.u(transfer->inheritor);",
            "f.u(due->action_id);", "f.i(due->charged_day.key);",
            "f.i(due->charged_slots);", "f.i(due->trigger_bar);", "f.u(due->trigger_order);",
            "f.u(request->action_id);", "f.i(request->position_cycle);",
            "f.i(request->after_bar);", "f.s(request->comment);",
        ]:
            for replacement in ["", "// " + fold, fold.replace("f.", "other."),
                                fold.replace("f.i(", "f.u(") if "f.i(" in fold
                                else fold.replace("f.u(", "f.i(").replace("f.s(", "f.i("),
                                "if (false) " + fold]:
                with self.subTest(fold=fold, replacement=replacement):
                    self.assertIn(fold, SOURCE)
                    code, output = self.check(source=SOURCE.replace(fold, replacement))
                    self.assertEqual(code, 1, output)
            code, output = self.check(source=SOURCE.replace(fold, "") + "\n" + fold)
            self.assertEqual(code, 1, output)

    def test_policy_configuration_schema_attachment_and_all_presence_are_explicit(self):
        cap = "max_intraday_filled_orders_"
        for fold in [
            "f.u(compat::pine::IntradayCap::schema_version);",
            f"f.i(static_cast<int64_t>({cap}.attachment()));",
            f"f.i({cap}.configuration().limit);",
            f"f.b({cap}.configuration().skip_noop_market);",
            f"f.b({cap}.configuration().defer_pooc_close);",
            f"f.b({cap}.configuration().count_pooc_full_close);",
            f"f.b({cap}.budget().day().has_value());",
            f"f.i({cap}.budget().charged_slots());",
            f"f.b({cap}.budget().latched());",
            f"f.b({cap}.budget().transfer().has_value());",
            f"f.b({cap}.due_cause().has_value());",
            f"f.u({cap}.next_action());",
            "f.b(position_close_obligation_.pending());",
        ]:
            for replacement in ["", "if (false) " + fold, "// " + fold,
                                fold.replace("f.", "other."),
                                fold.replace("f.i(", "f.u(") if "f.i(" in fold
                                else fold.replace("f.u(", "f.i(").replace("f.b(", "f.i(")]:
                with self.subTest(fold=fold, replacement=replacement):
                    self.assertIn(fold, SOURCE)
                    code, output = self.check(source=SOURCE.replace(fold, replacement))
                    self.assertEqual(code, 1, output)
            self.assertEqual(self.check(source=SOURCE.replace(fold, "") + "\n" + fold)[0], 1)

    def test_new_intraday_private_storage_requires_classification(self):
        for input_name, content, owner in [
            ("intraday", INTRADAY, "IntradayOrderBudget"),
            ("policy", POLICY, "IntradayCap"),
            ("obligation", OBLIGATION, "PositionCloseObligation"),
        ]:
            with self.subTest(owner=owner):
                before = "class " + owner + " {"
                self.assertIn(before, content)
                altered = content.replace(before, before + "\n    int future_state;")
                code, output = self.check(**{input_name: altered})
                self.assertEqual(code, 1, output)
                self.assertIn(owner, output)

    def test_new_intraday_nested_fields_require_folds(self):
        for input_name, content, owners in [
            ("intraday", INTRADAY, ["OrderRiskDay", "CloseQuotaTransfer"]),
            ("policy", POLICY, ["CapConfiguration", "CloseCause"]),
            ("obligation", OBLIGATION, ["PositionCloseRequest"]),
        ]:
            for owner in owners:
                with self.subTest(owner=owner):
                    before = "struct " + owner + " {"
                    self.assertIn(before, content)
                    altered = content.replace(before, before + "\n    uint64_t future_owner;")
                    code, output = self.check(**{input_name: altered})
                    self.assertEqual(code, 1, output)

    def test_unclassified_intraday_declarations_refuse(self):
        for input_name, content, owner in [
            ("intraday", INTRADAY, "CloseQuotaTransfer"),
            ("policy", POLICY, "CapConfiguration"),
            ("policy", POLICY, "CloseCause"),
            ("obligation", OBLIGATION, "PositionCloseRequest"),
        ]:
            for declaration in ["int first, second;", "std::vector<double> state;",
                                "UnknownOwner owner;", "int value() const;"]:
                with self.subTest(owner=owner, declaration=declaration):
                    before = "struct " + owner + " {"
                    altered = content.replace(before, before + "\n" + declaration)
                    self.assertNotEqual(self.check(**{input_name: altered})[0], 0)
        altered = INTRADAY.replace("int64_t key;", "uint64_t key;")
        self.assertNotEqual(altered, INTRADAY)
        self.assertEqual(self.check(intraday=altered)[0], 1)

    def test_policy_attachment_encoding_schema_and_owner_types_are_classified(self):
        for before, after in [
            ("CapAttachment { LegacySource, None }", "CapAttachment { LegacySource, None, Other }"),
            ("CapAttachment { LegacySource, None }", "CapAttachment { None, LegacySource }"),
            ("static constexpr uint64_t schema_version", "static constexpr int schema_version"),
            ("uint64_t next_action_", "int64_t next_action_"),
            ("CapConfiguration configuration_;", "OtherConfiguration configuration_;"),
        ]:
            with self.subTest(mutation=after):
                self.assertIn(before, POLICY)
                self.assertEqual(self.check(policy=POLICY.replace(before, after))[0], 1)
        # Schema bumps are serialized by symbol; changing only the version
        # remains covered, unlike replacing the fold with a numeric literal.
        self.assertEqual(self.check(source=SOURCE.replace(
            "f.u(compat::pine::IntradayCap::schema_version);", "f.u(1);"))[0], 1)

    def test_intraday_optional_bodies_cannot_be_rebound_or_duplicated(self):
        cap = "max_intraday_filled_orders_"
        for opening in [
            f"if (const auto& day = {cap}.budget().day()) {{",
            f"if (const auto& transfer = {cap}.budget().transfer()) {{",
            f"if (const auto& due = {cap}.due_cause()) {{",
            "if (const auto& request = position_close_obligation_.peek()) {",
        ]:
            with self.subTest(opening=opening):
                self.assertIn(opening, SOURCE)
                self.assertEqual(self.check(source=SOURCE.replace(opening, "if (false) {"))[0], 1)
                self.assertEqual(self.check(source=SOURCE + "\n" + opening + "}")[0], 1)

    def test_whole_intraday_hash_block_cannot_be_conditional(self):
        start = SOURCE.index("    f.u(compat::pine::IntradayCap::schema_version);")
        end = SOURCE.index("\n    // --- Cached net-profit", start)
        block = SOURCE[start:end]
        altered = SOURCE[:start] + "if (false) {\n" + block + "\n}\n" + SOURCE[end:]
        code, output = self.check(source=altered)
        self.assertEqual(code, 1, output)
        self.assertIn("unconditional", output)

    def test_causal_policy_and_generic_obligation_cannot_be_waived(self):
        for owner in ["max_intraday_filled_orders_", "position_close_obligation_"]:
            with self.subTest(owner=owner):
                self.assertEqual(self.check(waivers=WAIVERS + f"\n{owner} # no broad waiver\n")[0], 1)


if __name__ == "__main__":
    unittest.main()
