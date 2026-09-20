#!/usr/bin/env python3
"""Mutation controls for native C++ ABI ownership; no compiler or engine runs."""
import hashlib
import json
import re
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from check_aggregate_cpp_versions import body
from check_native_cpp_versions import (
    DRIVER_FORWARD, FILES, NATIVE_FX_CURVE_HEADER, NATIVE_FX_CURVE_SOURCE,
    ROOT, authenticate_historical_host_manifests, check, check_fx_curve_introduced_at,
    check_texts, load,
)

DATA = load()


class NativeVersions(unittest.TestCase):
    def reject(self, path, before, after):
        self.assertIn(before, DATA[path])
        changed = dict(DATA)
        changed[path] = changed[path].replace(before, after, 1)
        with self.assertRaises(ValueError):
            check_texts(changed)

    def test_current_command_and_preview_have_one_authority(self):
        self.reject(
            FILES[8],
            'struct NativeCurrentExecution {\n    native_order::RequestHandle target;',
            'struct NativeCurrentExecution {\n    native_order::RequestHandle target; '
            'std::optional<execution::SelectedOpeningSet> selected_close;')
        self.reject(FILES[8], 'std::optional<execution::Status> settlement_readiness;', '')
        self.reject(FILES[6], 'CurrentExecution = 8', 'CurrentExecution = 7')
        self.reject(FILES[6], 'Calculation = 7', 'Calculation = 9')
        self.reject(FILES[6], 'native-driver/v5', 'native-driver/v3')

    def test_current_cause_and_selected_hash_coverage(self):
        for fold in ('f.u(value.openings.size());', 'hash_cohort_handle(f, value.cohort);',
                     'f.u(selected->incarnations.size());', 'f.d(point.price);',
                     'f.u(point.quote_origin_ordinal);', 'f.u(current_frame_->acceptance_cutoff);',
                     'f.u(notification.ordinal);'):
            with self.subTest(fold=fold):
                self.reject(FILES[10], fold, '')

    def test_undetected_timeframe_spec_is_explicit_and_hashed(self):
        for before, after in (
            ('bool timeframe_undetected = false;', ''),
            ('bool timeframe_undetected = false;', 'bool timeframe_undetected = true;'),
            ('TimeframeUndetected,', 'MissingTimeframeUndetected,'),
            ('InvalidUndetectedTimeframe,', 'MissingUndetectedTimeframe,'),
        ):
            with self.subTest(before=before, after=after):
                self.reject(FILES[4], before, after)
        self.reject(FILES[10], 'f.b(spec.timeframe_undetected);', '')
        self.reject(FILES[10], 'args.n >= 2', 'args.n > 2')

    def test_timeframe_subscription_publication_modes_are_owned(self):
        # The series row's shape, and the rule that `gaps` folds into the
        # subscription digest only where a series set it.
        for before, after in (
            ('bool lookahead = false;\n    bool gaps = false;',
             'bool lookahead = false;'),
            ('bool lookahead = false;\n    bool gaps = false;',
             'bool gaps = false;\n    bool lookahead = false;'),
        ):
            with self.subTest(before=before, after=after):
                self.reject(FILES[4], before, after)
        self.reject(FILES[5], 'if (subscription.gaps) u(2u);', 'u(2u);')

    def test_declared_series_are_registered_after_the_host_run_begin(self):
        # The hook is the host calling the kernel: never virtual, and legal
        # only inside on_native_run_begin.
        self.reject(FILES[8],
                    'bool declare_timeframe_subscriptions(',
                    'virtual bool declare_timeframe_subscriptions(')
        self.reject(FILES[8], 'bool declare_timeframe_subscriptions(',
                    'bool declare_series(')
        # The registration order, the guard the kernel's own wiring stands
        # down, and the gaps clear are all structural.
        for token in ('in_run_begin_ = true;', 'wiring_subscriptions_ = true;',
                      'if (subscription.gaps) subscription.latest.reset();'):
            with self.subTest(token=token):
                self.reject(FILES[10], token, '')
        self.reject(FILES[4], 'validate_native_timeframe_subscriptions',
                    'validate_series')

    def test_legacy_tolerant_slot_policy_is_explicit_and_hashed(self):
        for before, after in (
            ('NativeSlotLabelPolicy slot_label_policy = NativeSlotLabelPolicy::Canonical;', ''),
            ('NativeFeedTolerance legacy_tolerance = NativeFeedTolerance::None;', ''),
            ('SlotLabelPolicy, LegacyTolerance,', 'SlotLabelPolicy,'),
            ('UnknownSlotLabelPolicy,', 'MissingSlotLabelPolicy,'),
            ('UnknownLegacyTolerance,', 'MissingLegacyTolerance,'),
        ):
            with self.subTest(before=before, after=after):
                self.reject(FILES[4], before, after)
        self.reject(FILES[5], 'spec.slot_label_policy', 'spec.removed_slot_label_policy')
        self.reject(FILES[5], 'spec.legacy_tolerance', 'spec.removed_legacy_tolerance')
        self.reject(FILES[10], 'f.u(static_cast<uint64_t>(spec.slot_label_policy));', '')
        self.reject(FILES[10], 'f.u(static_cast<uint64_t>(spec.legacy_tolerance));', '')
        self.reject(FILES[7],
                    'spec.slot_label_policy == NativeSlotLabelPolicy::FeedTolerant',
                    'false')
        self.reject(FILES[7], 'NativeFeedTolerance::BatchStructuralBars',
                    'NativeFeedTolerance::RemovedBatchStructuralBars')
        # The deprecated spellings must keep existing for callers that still
        # use them; dropping either alias is rejected too.
        self.reject(FILES[4], 'using NativeLegacyTolerance = NativeFeedTolerance;', '')
        self.reject(FILES[4], 'LegacyTolerant = FeedTolerant,', '')

    def test_path_order_policy_is_explicit_validated_hashed_and_consumed(self):
        for before, after in (
            ('enum class NativePathOrder : std::uint32_t {',
             'enum class MissingNativePathOrder : std::uint32_t {'),
            ('NativePathOrder path_order = NativePathOrder::Auto;', ''),
            ('PathOrder,', 'MissingPathOrder,'),
            ('UnknownPathOrder,', 'MissingUnknownPathOrder,'),
        ):
            with self.subTest(before=before, after=after):
                self.reject(FILES[4], before, after)
        self.reject(FILES[5], 'spec.path_order', 'spec.removed_path_order')
        self.reject(FILES[10], 'f.u(static_cast<uint64_t>(spec.path_order));', '')
        self.reject(FILES[10], 'bool path_uses_high_first(',
                    'bool removed_path_uses_high_first(')
        self.reject(FILES[10], 'class NativePathOrderScope {',
                    'class RemovedNativePathOrderScope {')

    def test_anchored_leg_rounding_is_pinned_and_folded_when_set(self):
        # R5 L7b: the per-anchor rounding is a native_order_v6 member appended
        # last with a Raw default, its enumerators are pinned, the arm reads
        # the tick through ArmContext, and the fold is conditional.
        for before, after in (
            ('enum class NativeAnchorRounding : std::uint8_t {',
             'enum class MissingAnchorRounding : std::uint8_t {'),
            ('    NativeAnchorRounding rounding = NativeAnchorRounding::Raw;\n', ''),
            ('    HalfUp = 1,\n    Directional = 2,', '    Directional = 1,\n    HalfUp = 2,'),
            ('struct ArmContext {\n    std::optional<double> price_tick;',
             'struct ArmContext {\n    std::optional<double> tick;'),
        ):
            with self.subTest(before=before, after=after):
                self.reject(FILES[0], before, after)
        for before, after in (
            ('if (anchor->rounding != native_order::NativeAnchorRounding::Raw) {',
             'if (true) {'),
            ('f.u(static_cast<uint64_t>(anchor->rounding));', ''),
            ('if (const auto* spec = spec_ptr()) arm.price_tick = spec->price_tick;', ''),
        ):
            with self.subTest(before=before, after=after):
                self.reject(FILES[10], before, after)

    def test_anchored_level_hook_is_pinned_and_consulted(self):
        # R5 L7b: the arm hook is a v18 answering virtual over an exact
        # read-only view, the core carries the resolver as a value, and the
        # consumer consults it at the arm.
        for before, after in (
            ('struct NativeAnchoredLevelView {', 'struct MissingAnchoredLevelView {'),
            ('    double kernel_level = 0.0;\n};', '};'),
            ('virtual std::optional<double> resolve_anchored_level(',
             'virtual std::optional<double> resolve_anchored_level_renamed('),
        ):
            with self.subTest(before=before, after=after):
                self.reject(FILES[8], before, after)
        self.reject(FILES[0], '    AnchoredLevelResolver resolve_level;\n', '')
        for before, after in (
            ('return host->resolve_anchored_level(view);', 'return std::nullopt;'),
            ('next_timeline_ordinal_, arm);', 'next_timeline_ordinal_);'),
        ):
            with self.subTest(before=before, after=after):
                self.reject(FILES[10], before, after)

    def test_execution_grid_policy_is_explicit_hashed_and_consumed(self):
        self.reject(FILES[0], 'enum class ExecutionGridPolicy : std::uint8_t {',
                    'enum class MissingExecutionGridPolicy : std::uint8_t {')
        self.reject(FILES[0],
                    'ExecutionGridPolicy grid_policy = ExecutionGridPolicy::SnapToGrid;', '')
        self.reject(FILES[10], 'f.u(static_cast<uint64_t>(terms.grid_policy));', '')
        self.reject(FILES[10], 'bool execution_terms_grid_representable(',
                    'bool removed_execution_terms_grid_representable(')

    def test_placement_time_sizing_members_are_pinned_and_hashed(self):
        # R5 L3b: the sizing-price rule, the scope basis and the three frozen
        # placement-time measurements are native_order_v6 members, each folded
        # into the continuation digest only where it is actually set.
        for before, after in (
            ('enum class SizePrice : std::uint8_t '
             '{ Resolved = 0, Signal = 1, SignalOnTick = 2 };',
             'enum class MissingSizePrice : std::uint8_t '
             '{ Resolved = 0, Signal = 1, SignalOnTick = 2 };'),
            ('enum class ScopeBasis : std::uint8_t { AtMatch = 0, AtAcceptance = 1 };',
             'enum class MissingScopeBasis : std::uint8_t { AtMatch = 0, AtAcceptance = 1 };'),
            ('    SizePrice price = SizePrice::Resolved;', ''),
            ('    ScopeBasis basis = ScopeBasis::AtMatch;', ''),
            ('    std::optional<double> sizing_units;   // SizeTime::AtAcceptance', ''),
            ('    std::optional<double> sizing_scope;   // ScopeBasis::AtAcceptance', ''),
            ('    std::optional<double> sizing_price;   '
             '// SizePrice::Signal / SignalOnTick', ''),
            ('    bool sizing_admissible = true;', ''),
        ):
            with self.subTest(before=before, after=after):
                self.reject(FILES[0], before, after)
        for before, after in (
            ('if (live.sizing_units) { f.u(1); f.d(*live.sizing_units); }', ''),
            ('if (live.sizing_scope) { f.u(2); f.d(*live.sizing_scope); }', ''),
            ('if (live.sizing_price) { f.u(3); f.d(*live.sizing_price); }', ''),
            ('f.u(static_cast<uint64_t>(payload.price));', ''),
            ('f.u(static_cast<uint64_t>(fraction->basis));', ''),
        ):
            with self.subTest(before=before, after=after):
                self.reject(FILES[10], before, after)

    def test_abort_reporting_policy_and_input_hook_are_explicit_and_hashed(self):
        for before, after in (
            ('enum class NativeAbortReporting : std::uint32_t {',
             'enum class MissingAbortReporting : std::uint32_t {'),
            ('NativeAbortReporting abort_reporting = NativeAbortReporting::Error;', ''),
            ('AbortReporting,', 'MissingAbortReporting,'),
            ('UnknownAbortReporting,', 'MissingAbortReporting,'),
        ):
            with self.subTest(before=before, after=after):
                self.reject(FILES[4], before, after)
        self.reject(FILES[5], 'spec.abort_reporting', 'spec.removed_abort_reporting')
        self.reject(FILES[10], 'f.u(static_cast<uint64_t>(spec.abort_reporting));', '')
        self.reject(FILES[8], 'struct NativeInputContext {', 'struct MissingNativeInputContext {')
        self.reject(FILES[8],
                    'on_native_input(const Bar&, const NativeInputContext&)',
                    'on_native_input_missing(const Bar&, const NativeInputContext&)')
        self.reject(FILES[10], 'input_callback_context_', 'removed_input_context_')
        self.reject(FILES[10], 'input_callback_bar_', 'removed_input_bar_')

    def test_tick_hook_is_explicit_and_hashed(self):
        self.reject(FILES[8], 'struct NativeTickContext {', 'struct MissingNativeTickContext {')
        self.reject(FILES[8],
                    'on_native_tick(const Bar&, const NativeTickContext&)',
                    'on_native_tick_missing(const Bar&, const NativeTickContext&)')
        self.reject(FILES[10], 'tick_callback_context_', 'removed_tick_context_')
        self.reject(FILES[10], 'tick_callback_bar_', 'removed_tick_bar_')
        self.reject(FILES[10], 'invoke_tick_callback(engine, tick_bar, tick_context)',
                    'invoke_tick_callback_missing(engine, tick_bar, tick_context)')

    def test_distribution_sample_eligibility_is_explicit_and_hashed(self):
        for before, after in (
            ('enum class SampleEligibility : std::uint32_t {',
             'enum class MissingSampleEligibility : std::uint32_t {'),
            ('SampleEligibility sample_eligibility = SampleEligibility::ContinuousSegments;', ''),
            ('IntrabarSampleEligibility,', 'MissingIntrabarSampleEligibility,'),
            ('UnknownIntrabarSampleEligibility,', 'MissingIntrabarSampleEligibility,'),
        ):
            with self.subTest(before=before, after=after):
                self.reject(FILES[4], before, after)
        self.reject(FILES[5], 'u(static_cast<std::uint64_t>(lower->sample_eligibility));',
                    'u(static_cast<std::uint64_t>(lower->removed_sample_eligibility));')
        self.reject(FILES[6], 'NativeDriverStatistics driver_statistics{};',
                    'NativeDriverStatistics removed_driver_statistics{};')
        self.reject(FILES[10],
                    'IntrabarPath::SampleEligibility::DistributionSamples',
                    'IntrabarPath::SampleEligibility::ContinuousSegments')
        self.reject(FILES[10], 'driver_statistics_.sample_ticks_processed',
                    'driver_statistics_.removed_sample_ticks_processed')
        self.reject(FILES[10], 'f.b(staged_ingress_fx_);', '')
        self.reject(FILES[10], 'if (failed() && !recoverable_abort())',
                    'if (failed() && !removed_recoverable_abort())')

    def test_synthesized_intrabar_path_is_explicit_and_hashed(self):
        for before, after in (
            ('struct synthesized {', 'struct removed_synthesized {'),
            ('using value_type = std::variant<none, lower_tf, synthesized>;',
             'using value_type = std::variant<none, lower_tf>;'),
            ('synthesized_path() const noexcept', 'removed_synthesized_path() const noexcept'),
        ):
            with self.subTest(before=before, after=after):
                self.reject(FILES[4], before, after)
        self.reject(FILES[5], 'i(synthesized->samples);',
                    'i(synthesized->removed_samples);')
        self.reject(FILES[5], 'u(static_cast<std::uint64_t>(synthesized->distribution));',
                    'u(static_cast<std::uint64_t>(synthesized->removed_distribution));')
        self.reject(FILES[10],
                    'const auto* synthesized = spec ? spec->intrabar.synthesized_path() : nullptr;',
                    'const auto* synthesized = nullptr;')
        self.reject(FILES[10],
                    'const bool intrabar_points_drive_floor = kind == InputContribution::ConfirmedBar',
                    'const bool removed_intrabar_points_drive_floor = kind == InputContribution::ConfirmedBar')

    def test_current(self):
        check_texts(DATA)

    def test_stale_wrapper(self):
        for path, namespace, stale in (
            (FILES[0], "native_order_v6", "native_order_v1"),
            (FILES[1], "native_order_v6", "native_order_v1"),
            (FILES[11], "native_order_v1", "native_order_v6"),
            (FILES[2], "native_calendar_v2", "native_calendar_v1"),
            (FILES[3], "native_calendar_v2", "native_calendar_v3"),
            (FILES[4], "native_run_spec_v3", "native_run_spec_v1"),
            (FILES[5], "native_run_spec_v3", "native_run_spec_v1"),
            (FILES[6], "native_driver_v5", "native_driver_v2"),
            (FILES[7], "native_driver_v5", "native_driver_v3"),
            (FILES[8], "engine_script_run_v18", "engine_script_run_v12"),
            (FILES[9], "engine_script_run_v18", "engine_script_run_v12"),
            (FILES[10], "engine_script_run_v18", "engine_script_run_v12"),
        ):
            with self.subTest(path=path, namespace=namespace):
                self.reject(path, namespace, stale)

    def test_duplicate_wrapper(self):
        for path, namespace in (
            (FILES[0], "native_order_v6"),
            (FILES[2], "native_calendar_v2"),
            (FILES[4], "native_run_spec_v3"),
            (FILES[6], "native_driver_v5"),
            (FILES[8], "engine_script_run_v18"),
            (FILES[11], "native_order_v1"),
        ):
            with self.subTest(path=path):
                opening = "inline namespace " + namespace + " {"
                self.reject(path, opening, opening + " } inline namespace " + namespace + " {")

    def test_empty_namespace_is_not_ownership(self):
        for path, namespace in (
            (FILES[0], "native_order_v6"),
            (FILES[2], "native_calendar_v2"),
            (FILES[4], "native_run_spec_v3"),
            (FILES[6], "native_driver_v5"),
            (FILES[8], "engine_script_run_v18"),
            (FILES[11], "native_order_v1"),
        ):
            with self.subTest(path=path):
                self.reject(path, "inline namespace " + namespace + " {",
                            "inline namespace " + namespace + " {} namespace misplaced {")

    def test_comment_only_namespace_is_not_ownership(self):
        for path, namespace, decoy in (
            (FILES[0], "native_order_v6", "struct WorkingRequestCore"),
            (FILES[11], "native_order_v1", "struct RunIdentity"),
            (FILES[2], "native_calendar_v2", "parse_timeframe NativeInterval"),
            (FILES[8], "engine_script_run_v18", "class NativeStrategyHost"),
        ):
            with self.subTest(path=path):
                self.reject(
                    path,
                    "inline namespace " + namespace + " {",
                    "inline namespace " + namespace + " { /* " + decoy + " */ } namespace misplaced {")

    def test_type_moved_outside_owner(self):
        self.reject(FILES[0], "struct Request {",
                    "} struct Request {")
        self.reject(FILES[11], "struct RunIdentity {",
                    "} struct RunIdentity {")
        self.reject(FILES[2], "struct NativeInterval {",
                    "} struct NativeInterval {")
        self.reject(FILES[4], "struct NativeRunSpec {",
                    "} struct NativeRunSpec {")
        self.reject(FILES[6], "struct NativeCoordinate {",
                    "} struct NativeCoordinate {")
        self.reject(FILES[8], "class NativeStrategyHost : public BacktestEngine {",
                    "} class NativeStrategyHost : public BacktestEngine {")

    def test_command_event_alias_moved_outside_owner(self):
        self.reject(
            FILES[0],
            "using CommandEvent = std::variant<AcceptedEvent,",
            "} using CommandEvent = std::variant<AcceptedEvent,")

    def test_return_only_function_escape(self):
        calendar = FILES[2]
        self.reject(
            calendar,
            "std::optional<Timeframe> parse_timeframe(std::string_view text);",
            "")
        changed = dict(DATA)
        decl = "std::optional<Timeframe> parse_timeframe(std::string_view text);"
        text = changed[calendar].replace(decl, "", 1)
        text = text.replace(
            "}  // inline namespace native_calendar_v2",
            "}  // inline namespace native_calendar_v2\n" + decl,
            1)
        changed[calendar] = text
        with self.assertRaises(ValueError):
            check_texts(changed)

        changed = dict(DATA)
        decl = "std::optional<TimezoneIdentityDescriptor>\ntimezone_identity_descriptor(std::string_view timezone);"
        self.assertIn(decl, changed[calendar])
        text = changed[calendar].replace(decl, "", 1)
        text = text.replace(
            "}  // inline namespace native_calendar_v2",
            "}  // inline namespace native_calendar_v2\n" + decl,
            1)
        changed[calendar] = text
        with self.assertRaises(ValueError):
            check_texts(changed)

        changed = dict(DATA)
        decl = "std::optional<int64_t> period_key(const SessionCalendar& calendar, const Timeframe& tf, int64_t ms);"
        self.assertIn(decl, changed[calendar])
        text = changed[calendar].replace(decl, "", 1)
        text = text.replace(
            "}  // inline namespace native_calendar_v2",
            "}  // inline namespace native_calendar_v2\n" + decl,
            1)
        changed[calendar] = text
        with self.assertRaises(ValueError):
            check_texts(changed)

        spec = FILES[4]
        changed = dict(DATA)
        decl = "NativeRunSpecValidation validate_native_run_spec(const NativeRunSpec& spec) noexcept;"
        text = changed[spec].replace(decl, "", 1)
        text = text.replace(
            "}  // inline namespace native_run_spec_v3",
            "}  // inline namespace native_run_spec_v3\n" + decl,
            1)
        changed[spec] = text
        with self.assertRaises(ValueError):
            check_texts(changed)

    def test_out_of_line_function_escape(self):
        changed = dict(DATA)
        src = FILES[7]
        start = changed[src].index("bool native_bar_structurally_valid")
        end = changed[src].index("NativeInputPreflightResult preflight_native_inputs")
        func = changed[src][start:end]
        text = changed[src][:start] + changed[src][end:]
        text = text.replace(
            "}  // inline namespace native_driver_v5",
            "}  // inline namespace native_driver_v5\n" + func,
            1)
        changed[src] = text
        with self.assertRaises(ValueError):
            check_texts(changed)

        changed = dict(DATA)
        src = FILES[3]
        needle = "std::optional<Timeframe> parse_timeframe(std::string_view text) {"
        self.assertIn(needle, changed[src])
        changed[src] = changed[src].replace(
            needle, "/* escaped parse_timeframe */ int parse_timeframe_unused() {", 1)
        with self.assertRaises(ValueError):
            check_texts(changed)

        changed = dict(DATA)
        src = FILES[1]
        needle = "WorkingRequestCore::reset("
        self.assertIn(needle, changed[src])
        changed[src] = changed[src].replace(
            "}  // inline namespace native_order_v6",
            "}  // inline namespace native_order_v6\nvoid WorkingRequestCore::reset(RunIdentity) {}\n",
            1)
        with self.assertRaises(ValueError):
            check_texts(changed)

    def test_driver_native_run_spec_forward_must_stay_outside(self):
        self.reject(FILES[6], DRIVER_FORWARD, "")
        self.reject(
            FILES[6],
            DRIVER_FORWARD + "\ninline namespace native_driver_v5 {",
            "inline namespace native_driver_v5 {\n" + DRIVER_FORWARD)
        self.reject(
            FILES[6],
            DRIVER_FORWARD,
            "inline namespace native_run_spec_v3 { struct NativeRunSpec {}; }")

    def test_host_public_values_cannot_leave_v18(self):
        self.reject(FILES[8], "struct NativeStateView {", "} struct NativeStateView {")
        self.reject(FILES[8], "struct NativeFailure {", "} struct NativeFailure {")
        self.reject(FILES[8], "struct NativeFailureContext {", "} struct NativeFailureContext {")
        self.reject(FILES[9], "class NativeExecutionConsumer final : public IExecutionConsumer {",
                    "} class NativeExecutionConsumer final : public IExecutionConsumer {")
        self.reject(
            FILES[8],
            "native_order::SubmitResult submit(const native_order::Request& request);",
            "")
        self.reject(
            FILES[6],
            'kNativeConsumerSemanticVersion = "native-consumer/v7"',
            'kNativeConsumerSemanticVersion = "native-consumer/v3"')

    def test_terms_ownership_and_alias_shapes_are_exact(self):
        for path, before, after in (
            (FILES[0], "struct HostSized {", "struct MissingHostSized {"),
            (FILES[0], "enum class HostSizedKind", "enum class MissingHostSizedKind"),
            (FILES[0], "struct ReverseTo {", "struct MissingReverseTo {"),
            (FILES[0], "struct RemainingDeferred {}", "struct MissingRemainingDeferred {}"),
            (FILES[0], "struct RemainingProjectionDeferred {}", "struct MissingRemainingProjectionDeferred {}"),
            (FILES[0], "struct NoTarget {}", "struct MissingNoTarget {}"),
            (FILES[0], "struct CohortHandle {", "struct MissingCohortHandle {"),
            (FILES[0], "struct BindCohort {", "struct MissingBindCohort {"),
            (FILES[0], "struct CohortClose {", "struct MissingCohortClose {"),
            (FILES[0], "struct AllowanceDeferred {", "struct MissingAllowanceDeferred {"),
            (FILES[0], "enum class OpeningShape", "enum class MissingOpeningShape"),
            (FILES[0], "struct ExecutionTerms {", "struct MissingExecutionTerms {"),
            (FILES[0], "struct TermsResolvedInput {", "struct MissingTermsResolvedInput {"),
            (FILES[0], "struct TermsResolvedEvent {", "struct MissingTermsResolvedEvent {"),
            (FILES[0], "enum class NativeCandidatePriceKind", "enum class MissingNativeCandidatePriceKind"),
            (FILES[0], "prepare_terms(const RequestHandle& target,", "prepare_terms_missing(const RequestHandle& target,"),
            (FILES[0], "evaluated_allowance(const LiveRequest& live, uint64_t point)",
             "evaluated_allowance_missing(const LiveRequest& live, uint64_t point)"),
            (FILES[0], "effective_host_units(const PendingAdjustments& pending,",
             "effective_host_units_missing(const PendingAdjustments& pending,"),
            (FILES[8], "struct NativeExecutionTermsFacts {", "struct MissingNativeExecutionTermsFacts {"),
            (FILES[8], "struct NativePrecommitView {", "struct MissingNativePrecommitView {"),
            (FILES[8], "struct NativeTrailState {", "struct MissingNativeTrailState {"),
            (FILES[8], "enum class NativePrecommitVerdict", "enum class MissingNativePrecommitVerdict"),
            (FILES[8], "struct NativeFxCurveSetupResult {", "struct MissingNativeFxCurveSetupResult {"),
            (FILES[8], "struct NativeBeginArgs {", "struct MissingNativeBeginArgs {"),
            (FILES[8], "struct NativeInputContext {", "struct MissingNativeInputContext {"),
            (FILES[8], "const SymInfo* syminfo = nullptr;", "const SymInfo* missing_syminfo = nullptr;"),
            (FILES[8], "resolve_execution_terms(\n", "resolve_execution_terms_missing(\n"),
            (FILES[8], "validate_execution_precommit(\n", "validate_execution_precommit_missing(\n"),
            (FILES[8], "configure_native_fx_curve(const NativeFxCurve& curve)",
             "configure_native_fx_curve_missing(const NativeFxCurve& curve)"),
            (FILES[8], "prepare_native_begin(const NativeBeginArgs&)",
             "prepare_native_begin_missing(const NativeBeginArgs&)"),
            (FILES[8], "on_native_input(const Bar&, const NativeInputContext&)",
             "on_native_input_missing(const Bar&, const NativeInputContext&)"),
            (FILES[8], "on_native_bar_open(const Bar&, const NativeDecisionContext&)",
             "on_native_bar_open_missing(const Bar&, const NativeDecisionContext&)"),
        ):
            with self.subTest(before=before):
                self.reject(path, before, after)

        aliases = (
            ("using OrderIntent = std::variant<Flatten, Reduce, Transact, ReverseTo, HostSized, Sized>;",
             "using OrderIntent = std::variant<Flatten, Reduce, Transact, HostSized, ReverseTo, Sized>;"),
            ("using ReductionSize = std::variant<ExplicitUnits, OwnerOpenedUnits, ScopeFraction>;",
             "using ReductionSize = std::variant<ExplicitUnits, ScopeFraction, OwnerOpenedUnits>;"),
            ("using SizeBasis = std::variant<CashValue, EquityFraction>;",
             "using SizeBasis = std::variant<EquityFraction, CashValue>;"),
            ("using Remaining = std::variant<RemainingUnbound, RemainingFlattenAll, RemainingUnits,\n"
             "                               RemainingDeferred, NoTarget>;",
             "using Remaining = std::variant<RemainingUnbound, RemainingFlattenAll, RemainingDeferred,\n"
             "                               RemainingUnits, NoTarget>;"),
            ("using RemainingProjection =\n        std::variant<RemainingProjectionUnbound, RemainingProjectionFlattenAll,\n"
             "                     RemainingProjectionUnits, RemainingProjectionDeferred,\n"
             "                     RemainingProjectionNoTarget>;",
             "using RemainingProjection =\n        std::variant<RemainingProjectionUnbound, RemainingProjectionFlattenAll,\n"
             "                     RemainingProjectionDeferred, RemainingProjectionUnits,\n"
             "                     RemainingProjectionNoTarget>;"),
            ("using Allowance = std::variant<AllowanceUnset, AllowanceUnits, AllowanceAllScope,\n"
             "                               AllowanceDeferred>;",
             "using Allowance = std::variant<AllowanceUnset, AllowanceUnits, AllowanceDeferred,\n"
             "                               AllowanceAllScope>;"),
            ("using ExecutionPlan = std::variant<execution::Flatten, order_action::Reduce,\n"
             "                                   order_action::Transact, execution::ReverseTo>;",
             "using ExecutionPlan = std::variant<execution::Flatten, order_action::Reduce,\n"
             "                                   execution::ReverseTo, order_action::Transact>;"),
        )
        for before, after in aliases:
            with self.subTest(alias=before.split('=', 1)[0]):
                self.reject(FILES[0], before, after)

        result = ("using NativeCurrentExecutionResult = std::variant<NativeCurrentRefusal,\n"
                  "    native_order::ExecutionAppliedEvent, native_order::NoEffectEvent,\n"
                  "    native_order::MatchRejectedEvent, native_order::CancelledEvent>;")
        self.reject(FILES[8], result,
                    result.replace(", native_order::CancelledEvent", ""))
        self.reject(FILES[8], result,
                    result.replace("native_order::NoEffectEvent,\n    native_order::MatchRejectedEvent",
                                   "native_order::MatchRejectedEvent,\n    native_order::NoEffectEvent"))
        self.reject(FILES[8], "native_order::CancelledEvent", "/* native_order::CancelledEvent */")

        rejection = "std::optional<native_order::MatchRejectReason> terms_rejection;"
        cancellation = "std::optional<native_order::CancelReason> terms_cancellation;"
        self.reject(FILES[8], rejection, "")
        self.reject(FILES[8], cancellation, "/* " + cancellation + " */")
        changed = dict(DATA)
        changed[FILES[8]] = changed[FILES[8]].replace(rejection, "@REJECTION@", 1).replace(
            cancellation, rejection, 1).replace("@REJECTION@", cancellation, 1)
        with self.assertRaises(ValueError):
            check_texts(changed)

    def test_phase1c_native_abi_templates_are_active(self):
        from check_native_cpp_abi import (
            CURRENT_EXECUTION_V15_CALLER, NATIVE_FX_CURVE_CALLER, NATIVE_TICK_CALLER,
            NATIVE_TRAIL_STATE_CALLER, CURRENT_TERMS_SURFACE_READY, control_applicability,
        )
        self.assertTrue(CURRENT_TERMS_SURFACE_READY)
        self.assertIn('R4B_CURRENT_RESULT_ALTERNATIVES', CURRENT_EXECUTION_V15_CALLER)
        self.assertIn('configure_native_fx_curve', CURRENT_EXECUTION_V15_CALLER)
        self.assertIn('validate_native_fx_curve', NATIVE_FX_CURVE_CALLER)
        self.assertIn('on_native_tick', NATIVE_TICK_CALLER)
        self.assertIn('trail_state', NATIVE_TRAIL_STATE_CALLER)
        controls = {row['name']: row for row in control_applicability()}
        self.assertEqual(controls['v14_current_execution_shape_agnostic_compile']['status'], 'required')
        for name in ('v18_current_execution_surface_compile',
                     'v18_current_result_missing_cancelled_compile_reject',
                     'v18_native_fx_curve_surface_compile',
                     'v18_native_tick_surface_compile',
                     'v18_native_trail_state_surface_compile',
                     'v18_to_v16_frozen_current_execution_compile_reject',
                     'v18_to_v16_frozen_native_fx_curve_compile_reject',
                     'v18_to_v16_frozen_native_tick_compile_reject',
                     'v18_to_v16_frozen_native_trail_state_compile_reject'):
            self.assertEqual(controls[name]['status'], 'required')

    def test_order_namespace_is_derived_not_literal(self):
        from check_native_cpp_abi import current_order_namespace
        self.assertEqual(current_order_namespace(
            'inline namespace native_order_v6 { struct X {}; }'), 'native_order_v6')
        with self.assertRaises(RuntimeError):
            current_order_namespace(
                'inline namespace native_order_v6 { }\n'
                'inline namespace native_order_v3 { }')

    def test_missing_cancelled_mutation_is_exactly_one(self):
        from check_native_cpp_abi import remove_current_result_cancelled
        source = ('using NativeCurrentExecutionResult = std::variant<NativeCurrentRefusal, '
                  'native_order::ExecutionAppliedEvent, native_order::NoEffectEvent, '
                  'native_order::MatchRejectedEvent, native_order::CancelledEvent>;\n')
        changed = remove_current_result_cancelled(source)
        prefix, marker, suffix = source.partition(', native_order::CancelledEvent')
        self.assertTrue(marker)
        self.assertEqual(changed, prefix + suffix)
        self.assertNotIn('CancelledEvent', changed)
        with self.assertRaises(RuntimeError):
            remove_current_result_cancelled(source.replace('CancelledEvent', 'NoEffectEvent'))
        with self.assertRaises(RuntimeError):
            remove_current_result_cancelled(source.replace(
                'native_order::CancelledEvent>;',
                'native_order::CancelledEvent, native_order::CancelledEvent>;'))

    def test_compile_rejection_requires_named_diagnostic(self):
        from check_native_cpp_abi import (
            CURRENT_EXECUTION_V15_CALLER, expect_compile_rejection,
            remove_current_result_cancelled,
        )
        self.assertIn('R4B_CURRENT_RESULT_ALTERNATIVES', CURRENT_EXECUTION_V15_CALLER)
        compiler = shutil.which('c++') or shutil.which('clang++')
        if not compiler:
            self.fail('the named compile-rejection mirror requires a C++ compiler')
        with tempfile.TemporaryDirectory(prefix='pf-native-reject-mirror-') as temp:
            root = Path(temp)
            include = root / 'include'
            header = include / 'pineforge/native_host.hpp'
            header.parent.mkdir(parents=True)
            original = ('#include <variant>\nnamespace pineforge { struct NativeCurrentRefusal {}; '
                        'namespace native_order { struct ExecutionAppliedEvent {}; '
                        'struct NoEffectEvent {}; struct MatchRejectedEvent {}; struct CancelledEvent {}; }\n'
                        'using NativeCurrentExecutionResult = std::variant<NativeCurrentRefusal, '
                        'native_order::ExecutionAppliedEvent, native_order::NoEffectEvent, '
                        'native_order::MatchRejectedEvent, native_order::CancelledEvent>; }\n')
            header.write_text(original)
            mutated = remove_current_result_cancelled(original)
            header.write_text(mutated)
            source = ('#include <pineforge/native_host.hpp>\n#include <variant>\n'
                      'static_assert(std::variant_size_v<pineforge::NativeCurrentExecutionResult> == 5, '
                      '"R4B_CURRENT_RESULT_ALTERNATIVES");\n')
            receipt = expect_compile_rejection(
                'named-negative', source, include,
                compiler_flags=[compiler, '-std=c++17'], generated_include=str(root),
                scratch=root, original_header_sha256=hashlib.sha256(
                    original.encode()).hexdigest())
            self.assertEqual(receipt['outcome'], 'expected_compile_rejection')
            self.assertIn('R4B_CURRENT_RESULT_ALTERNATIVES', receipt['diagnostics'])
            self.assertEqual(receipt['source_sha256'], hashlib.sha256(source.encode()).hexdigest())
            self.assertEqual(receipt['original_header_sha256'], hashlib.sha256(
                original.encode()).hexdigest())
            self.assertEqual(receipt['header_sha256'], hashlib.sha256(
                mutated.encode()).hexdigest())
            header.write_text(original)
            with self.assertRaises(RuntimeError):
                expect_compile_rejection(
                    'unexpected-success', source, include,
                    compiler_flags=[compiler, '-std=c++17'], generated_include=str(root),
                    scratch=root, original_header_sha256='x')
            bad = source.replace('static_assert', 'static_assertion', 1)
            header.write_text(mutated)
            with self.assertRaises(RuntimeError):
                expect_compile_rejection(
                    'unnamed-negative', bad, include,
                    compiler_flags=[compiler, '-std=c++17'], generated_include=str(root),
                    scratch=root, original_header_sha256='x')

    def test_v14_tar_authentication_rejects_archive_and_manifest_tampering(self):
        from check_native_cpp_abi import FIXTURE, authenticate_v14_fixture
        fixture = FIXTURE / 'host-f736676'
        with tempfile.TemporaryDirectory(prefix='pf-native-v14-auth-') as temp:
            root = Path(temp)
            valid = root / 'valid'
            manifest = root / 'manifest-copy'
            shutil.copytree(fixture, manifest)
            authenticate_v14_fixture(manifest, valid)
            tampered = root / 'tampered'
            shutil.copytree(fixture, tampered)
            archive = bytearray((tampered / 'headers.tar').read_bytes())
            archive[-1] ^= 1
            (tampered / 'headers.tar').write_bytes(archive)
            with self.assertRaises(RuntimeError):
                authenticate_v14_fixture(tampered, root / 'bad-archive')
            mislabeled = root / 'mislabeled'
            shutil.copytree(fixture, mislabeled)
            data = json.loads((mislabeled / 'manifest.json').read_text())
            data['tree'] = '0' * 40
            (mislabeled / 'manifest.json').write_text(json.dumps(data))
            with self.assertRaises(RuntimeError):
                authenticate_v14_fixture(mislabeled, root / 'bad-manifest')

    def test_v15_frozen_tar_authentication_rejects_archive_and_manifest_tampering(self):
        from check_native_cpp_abi import FIXTURE, authenticate_v15_frozen_fixture
        fixture = FIXTURE / 'host-e7cdf05'
        with tempfile.TemporaryDirectory(prefix='pf-native-v15-auth-') as temp:
            root = Path(temp)
            valid = root / 'valid'
            manifest = root / 'manifest-copy'
            shutil.copytree(fixture, manifest)
            authenticate_v15_frozen_fixture(manifest, valid)
            tampered = root / 'tampered'
            shutil.copytree(fixture, tampered)
            archive = bytearray((tampered / 'headers.tar').read_bytes())
            archive[-1] ^= 1
            (tampered / 'headers.tar').write_bytes(archive)
            with self.assertRaises(RuntimeError):
                authenticate_v15_frozen_fixture(tampered, root / 'bad-archive')
            mislabeled = root / 'mislabeled'
            shutil.copytree(fixture, mislabeled)
            data = json.loads((mislabeled / 'manifest.json').read_text())
            data['tree'] = '0' * 40
            (mislabeled / 'manifest.json').write_text(json.dumps(data))
            with self.assertRaises(RuntimeError):
                authenticate_v15_frozen_fixture(mislabeled, root / 'bad-manifest')

    def test_v16_frozen_tar_authentication_rejects_archive_and_manifest_tampering(self):
        from check_native_cpp_abi import FIXTURE, authenticate_v16_frozen_fixture
        fixture = FIXTURE / 'host-ab9714b'
        with tempfile.TemporaryDirectory(prefix='pf-native-v16-auth-') as temp:
            root = Path(temp)
            valid = root / 'valid'
            manifest = root / 'manifest-copy'
            shutil.copytree(fixture, manifest)
            authenticate_v16_frozen_fixture(manifest, valid)
            tampered = root / 'tampered'
            shutil.copytree(fixture, tampered)
            archive = bytearray((tampered / 'headers.tar').read_bytes())
            archive[-1] ^= 1
            (tampered / 'headers.tar').write_bytes(archive)
            with self.assertRaises(RuntimeError):
                authenticate_v16_frozen_fixture(tampered, root / 'bad-archive')
            mislabeled = root / 'mislabeled'
            shutil.copytree(fixture, mislabeled)
            data = json.loads((mislabeled / 'manifest.json').read_text())
            data['tree'] = '0' * 40
            (mislabeled / 'manifest.json').write_text(json.dumps(data))
            with self.assertRaises(RuntimeError):
                authenticate_v16_frozen_fixture(mislabeled, root / 'bad-manifest')

    def test_current_execution_caller_is_rendered_per_provider(self):
        from check_native_cpp_abi import render_current_execution_caller
        v14 = render_current_execution_caller('engine_script_run_v14')
        v16 = render_current_execution_caller('engine_script_run_v18')
        self.assertIn('engine_script_run_v14', v14)
        self.assertNotIn('engine_script_run_v18', v14)
        self.assertIn('engine_script_run_v18', v16)
        self.assertNotIn('engine_script_run_v14', v16)
        with self.assertRaises(RuntimeError):
            render_current_execution_caller('engine_script_run_v13')
        with self.assertRaises(RuntimeError):
            render_current_execution_caller('engine_script_run_v15')


class NativeFxCurveVersions(unittest.TestCase):
    def reject(self, path, before, after):
        self.assertIn(before, DATA[path])
        changed = dict(DATA)
        changed[path] = changed[path].replace(before, after, 1)
        with self.assertRaises(ValueError):
            check_texts(changed)

    def test_new_paths_append_without_reindexing_existing_pins(self):
        self.assertEqual(FILES[-2:], (NATIVE_FX_CURVE_HEADER, NATIVE_FX_CURVE_SOURCE))
        self.assertEqual(FILES[11], 'include/pineforge/native_order_identity.hpp')
        self.assertEqual(FILES.count(NATIVE_FX_CURVE_HEADER), 1)
        self.assertEqual(FILES.count(NATIVE_FX_CURVE_SOURCE), 1)

    def test_standard_includes_are_exact_and_format_independent(self):
        header = NATIVE_FX_CURVE_HEADER
        for before, after in (
            ('#include <cstddef>', ''),
            ('#include <cstdint>', '// #include <cstdint>'),
            ('#include <vector>', '#include <vector>\n#include <vector>'),
            ('#include <vector>', '#include <vector>\n#include <pineforge/engine.hpp>'),
            ('#include <vector>', '#include <vector>\n#include "pineforge/native_host.hpp"'),
            ('#include <vector>', '#include <vector>\n#include <limits>'),
            ('#include <vector>', '#include <vector>\n#include_next <pineforge/engine.hpp>'),
        ):
            with self.subTest(after=after):
                self.reject(header, before, after)
        changed = dict(DATA)
        changed[header] = changed[header].replace('#include ', '#  include')
        check_texts(changed)

    def test_wrappers_are_real_unique_current_owners(self):
        for path in (NATIVE_FX_CURVE_HEADER, NATIVE_FX_CURVE_SOURCE):
            for replacement in (
                'inline namespace native_fx_curve_v2 {',
                'inline namespace native_fx_curve_v1 {} namespace misplaced {',
                'inline namespace native_fx_curve_v1 { /* NativeFxCurve validate_native_fx_curve */ } namespace misplaced {',
                'inline namespace native_fx_curve_v1 {} inline namespace native_fx_curve_v1 {',
            ):
                with self.subTest(path=path, replacement=replacement):
                    self.reject(path, 'inline namespace native_fx_curve_v1 {', replacement)

    def test_three_public_types_are_required_once_in_order(self):
        header = NATIVE_FX_CURVE_HEADER
        for declaration in ('struct NativeFxCurve {',
                            'enum class NativeFxCurveError : std::uint8_t {',
                            'struct NativeFxCurveValidation {'):
            with self.subTest(declaration=declaration):
                self.reject(header, declaration, '} ' + declaration)
                self.reject(header, declaration, declaration.replace('NativeFxCurve', 'MissingFxCurve', 1))
        curve = re.search(r'struct NativeFxCurve \{.*?\};', DATA[header], re.S).group()
        validation = re.search(r'struct NativeFxCurveValidation \{.*?\};', DATA[header], re.S).group()
        self.reject(header, curve, curve + '\n' + curve)
        changed = dict(DATA)
        changed[header] = changed[header].replace(curve, '', 1).replace(validation, validation + '\n' + curve, 1)
        with self.assertRaises(ValueError):
            check_texts(changed)
        self.reject(header, curve, '/* ' + curve + ' */')

    def test_host_pairing_type_cannot_enter_value_header(self):
        self.reject(NATIVE_FX_CURVE_HEADER, 'struct NativeFxCurve {',
                    'struct NativeFxCurveSetupResult {};\nstruct NativeFxCurve {')
        self.reject(NATIVE_FX_CURVE_HEADER, '#pragma once',
                    '#pragma once\nstruct NativeFxCurveSetupResult {};')
        self.reject(NATIVE_FX_CURVE_HEADER, 'struct NativeFxCurve {',
                    'using NativeFxCurveSetupResult = int;\nstruct NativeFxCurve {')
        self.reject(NATIVE_FX_CURVE_HEADER, 'struct NativeFxCurve {',
                    'struct NativeFxCurveSetupResult;\nstruct NativeFxCurve {')

    def test_curve_array_members_keep_their_types_and_order(self):
        header = NATIVE_FX_CURVE_HEADER
        first = 'std::vector<std::int64_t> effective_from_ms;'
        second = 'std::vector<double> account_per_quote;'
        for before, after in ((first, ''), (second, '/* ' + second + ' */'),
                              (first, first + first), (first, first.replace('int64_t', 'int32_t'))):
            with self.subTest(after=after):
                self.reject(header, before, after)
        changed = dict(DATA)
        changed[header] = DATA[header].replace(first, '@FIRST@', 1).replace(second, first, 1).replace('@FIRST@', second, 1)
        with self.assertRaises(ValueError):
            check_texts(changed)

    def test_error_values_keep_width_names_numbers_and_order(self):
        header = NATIVE_FX_CURVE_HEADER
        self.reject(header, 'NativeFxCurveError : std::uint8_t', 'NativeFxCurveError : std::uint16_t')
        for entry in ('None = 0', 'LengthMismatch = 1', 'NotStrictlyIncreasing = 2',
                      'NotFinitePositive = 3', 'AllocationFailure = 4', 'WrongPhase = 5'):
            with self.subTest(entry=entry):
                self.reject(header, entry, '/* ' + entry + ' */')
                self.reject(header, entry, entry + ', ' + entry)
                self.reject(header, entry, entry[:-1] + '9')
        changed = dict(DATA)
        changed[header] = changed[header].replace('None = 0', '@NONE@', 1).replace(
            'LengthMismatch = 1', 'None = 0', 1).replace('@NONE@', 'LengthMismatch = 1', 1)
        with self.assertRaises(ValueError):
            check_texts(changed)

    def test_validation_fields_keep_defaults_types_and_order(self):
        header = NATIVE_FX_CURVE_HEADER
        error = 'NativeFxCurveError error = NativeFxCurveError::None;'
        index = 'std::size_t index = 0;'
        for before, after in ((error, ''), (index, index + index),
                              (error, error.replace('::None', '::WrongPhase')),
                              (index, 'std::uint64_t index = 0;'), (index, 'std::size_t index = 1;')):
            with self.subTest(after=after):
                self.reject(header, before, after)
        changed = dict(DATA)
        changed[header] = changed[header].replace(error, '@ERROR@', 1).replace(
            index, error, 1).replace('@ERROR@', index, 1)
        with self.assertRaises(ValueError):
            check_texts(changed)

    def test_value_functions_require_declarations_and_real_definitions(self):
        for return_type, name in (('NativeFxCurveValidation', 'validate_native_fx_curve'),
                                  ('std::uint64_t', 'native_fx_curve_digest')):
            signature = return_type + ' ' + name + '(const NativeFxCurve& curve) noexcept'
            declaration = signature + ';'
            definition = signature + ' {' + body(
                DATA[NATIVE_FX_CURVE_SOURCE], re.escape(signature) + r'\s*\{', name) + '}'
            with self.subTest(name=name):
                self.reject(NATIVE_FX_CURVE_HEADER, declaration, '')
                self.reject(NATIVE_FX_CURVE_HEADER, declaration, '/* ' + declaration + ' */')
                self.reject(NATIVE_FX_CURVE_HEADER, declaration, declaration + '\n' + declaration)
                self.reject(NATIVE_FX_CURVE_HEADER, declaration, declaration.replace(' noexcept', ''))
                self.reject(NATIVE_FX_CURVE_HEADER, declaration, declaration.replace('const NativeFxCurve&', 'NativeFxCurve&'))
                self.reject(NATIVE_FX_CURVE_SOURCE, definition, '')
                self.reject(NATIVE_FX_CURVE_SOURCE, definition, declaration)
                self.reject(NATIVE_FX_CURVE_SOURCE, signature + ' {', signature.replace(name, name + '_removed') + ' {')
                self.reject(NATIVE_FX_CURVE_SOURCE, signature + ' {', signature + ' { }\n' + signature + ' {')
                self.reject(NATIVE_FX_CURVE_SOURCE, signature + ' {', 'namespace misplaced {\n' + signature + ' {')
        header = NATIVE_FX_CURVE_HEADER
        declaration = 'NativeFxCurveValidation validate_native_fx_curve(const NativeFxCurve& curve) noexcept;'
        changed = dict(DATA)
        changed[header] = changed[header].replace(declaration, '', 1).replace(
            '} // inline namespace native_fx_curve_v1',
            '} // inline namespace native_fx_curve_v1\n' + declaration, 1)
        with self.assertRaises(ValueError):
            check_texts(changed)

    def test_introduced_at_authenticates_historical_host_closures(self):
        manifests = authenticate_historical_host_manifests()
        self.assertEqual(set(manifests), {'v13', 'v14'})
        check_fx_curve_introduced_at(manifests)
        for label in manifests:
            poisoned = {name: {**manifest, 'files': dict(manifest['files'])}
                        for name, manifest in manifests.items()}
            poisoned[label]['files'][NATIVE_FX_CURVE_HEADER] = {}
            with self.subTest(label=label):
                with self.assertRaises(ValueError):
                    check_fx_curve_introduced_at(poisoned)
                with mock.patch('check_native_cpp_versions.authenticate_historical_host_manifests',
                                return_value=poisoned) as authenticate:
                    with self.assertRaises(ValueError):
                        check()
                    authenticate.assert_called_once_with(ROOT)

    def test_introduced_at_cannot_skip_authentication_or_accept_wrong_identity(self):
        from prepare_settlement_cpp_abi_base import PROVIDERS
        with self.assertRaises(ValueError):
            authenticate_historical_host_manifests(providers={})
        providers = {label: dict(provider) for label, provider in PROVIDERS.items()}
        providers['v14']['commit'] = '0' * 40
        with self.assertRaises(RuntimeError):
            authenticate_historical_host_manifests(providers=providers)


class FixtureAuthentication(unittest.TestCase):
    def test_authentic_fixtures_unpack(self):
        from check_native_cpp_abi import FIXTURE, authenticate_fixture
        for name in ("order-1acaf33", "calendar-draft-a8e34c", "calendar-262a280",
                     "driver-08b5c88", "run-spec-262a280", "host-e7d023d",
                     "order-e7d023d"):
            with self.subTest(name=name):
                with tempfile.TemporaryDirectory(prefix="pf-native-abi-ok-") as temp:
                    authenticate_fixture(FIXTURE / name, Path(temp) / name)

    def test_rejects_tampered_hash(self):
        from check_native_cpp_abi import FIXTURE, authenticate_fixture
        fixture = FIXTURE / "calendar-draft-a8e34c"
        with tempfile.TemporaryDirectory(prefix="pf-native-abi-tamper-") as temp:
            dest = Path(temp)
            (dest / "headers.json.gz").write_bytes((fixture / "headers.json.gz").read_bytes())
            manifest = json.loads((fixture / "manifest.json").read_text())
            header = "pineforge/native_calendar.hpp"
            manifest["files"][header]["sha256"] = "0" * 64
            (dest / "manifest.json").write_text(json.dumps(manifest))
            with self.assertRaises(RuntimeError):
                authenticate_fixture(dest, dest / "out")

    def test_rejects_tampered_archive_digest(self):
        from check_native_cpp_abi import FIXTURE, authenticate_fixture
        fixture = FIXTURE / "order-1acaf33"
        with tempfile.TemporaryDirectory(prefix="pf-native-abi-archive-") as temp:
            dest = Path(temp)
            (dest / "headers.json.gz").write_bytes((fixture / "headers.json.gz").read_bytes())
            manifest = json.loads((fixture / "manifest.json").read_text())
            manifest["archive_sha256"] = "0" * 64
            (dest / "manifest.json").write_text(json.dumps(manifest))
            with self.assertRaises(RuntimeError):
                authenticate_fixture(dest, dest / "out")

    def test_rejects_path_set_mismatch_and_escape(self):
        from check_native_cpp_abi import FIXTURE, authenticate_fixture, frozen_contents, git_blob
        import gzip
        import hashlib
        import io
        fixture = FIXTURE / "driver-08b5c88"
        manifest = json.loads((fixture / "manifest.json").read_text())
        contents = frozen_contents(fixture, manifest)
        with tempfile.TemporaryDirectory(prefix="pf-native-abi-set-") as temp:
            dest = Path(temp)
            extra = dict(contents)
            extra["pineforge/not-in-manifest.hpp"] = "x"
            buf = io.BytesIO()
            with gzip.GzipFile(filename="", fileobj=buf, mode="wb", mtime=0) as z:
                z.write((json.dumps(extra, indent=2, sort_keys=True) + "\n").encode())
            archive = buf.getvalue()
            (dest / "headers.json.gz").write_bytes(archive)
            mutated = dict(manifest)
            mutated["archive_sha256"] = hashlib.sha256(archive).hexdigest()
            (dest / "manifest.json").write_text(json.dumps(mutated))
            with self.assertRaises(RuntimeError):
                authenticate_fixture(dest, dest / "out")

        raw = b"not a header"
        contents["../escape.hpp"] = raw.decode()
        buf = io.BytesIO()
        with gzip.GzipFile(filename="", fileobj=buf, mode="wb", mtime=0) as z:
            z.write((json.dumps(contents, indent=2, sort_keys=True) + "\n").encode())
        archive = buf.getvalue()
        with tempfile.TemporaryDirectory(prefix="pf-native-abi-path-") as temp:
            dest = Path(temp)
            (dest / "headers.json.gz").write_bytes(archive)
            mutated = dict(manifest)
            mutated["archive_sha256"] = hashlib.sha256(archive).hexdigest()
            mutated["files"] = dict(manifest["files"])
            mutated["files"]["../escape.hpp"] = {
                "sha256": hashlib.sha256(raw).hexdigest(),
                "git_blob": git_blob(raw),
                "bytes": len(raw),
            }
            (dest / "manifest.json").write_text(json.dumps(mutated))
            with self.assertRaises(RuntimeError):
                authenticate_fixture(dest, dest / "out")


class NativeLayoutAssembly(unittest.TestCase):
    def test_exact_symbol_and_target_constants(self):
        from check_native_cpp_abi import assembly_layout_values
        emitted = "other:\n.quad 999\nabi_layout_extra:\n.quad 888\nabi_layout:\n.quad 88\n.quad 0x130 # libstdc++ size\n.quad 7\n.zero 16\nnext_symbol:\n.quad 999\n"
        self.assertEqual(assembly_layout_values(emitted, 3), [88, 304, 7])
        self.assertEqual(assembly_layout_values("_abi_layout:\n.xword 80\n.8byte 240\n", 2), [80, 240])

    def test_partial_array_is_not_layout_evidence(self):
        from check_native_cpp_abi import assembly_layout_values
        for emitted in ("abi_layout:\n.quad 80\n.zero 8\nnext:\n.quad 7\n",
                        "abi_layout_extra:\n.quad 80\n.quad 240\n"):
            with self.assertRaises(RuntimeError):
                assembly_layout_values(emitted, 2)


if __name__ == "__main__":
    unittest.main()
