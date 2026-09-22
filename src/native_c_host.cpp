/*
 * native_c_host.cpp — runtime side of <pineforge/native_c_api.h> (R5 lane L13).
 *
 * One kernel translation unit. It holds:
 *   - CCallbackHost, a `final` NativeStrategyHost that forwards each existing
 *     virtual to a C callback table. No new virtual is introduced and no
 *     inline epoch moves: this is additive C transport over the kernel that
 *     lanes L2-L8 already built.
 *   - Field-by-field translation between the C PODs and the C++ values. A C
 *     request is never cast onto `native_order::Request`; every enum field is
 *     read by an exhaustive switch that refuses a value outside its
 *     enumeration.
 *   - The PF_API definitions themselves. They are pinned by the
 *     EXPECTED_NATIVE_C_API inventory in scripts/check_c_abi_runtime.py, the
 *     separate half of the same guard that pins src/c_abi.cpp — adding or
 *     removing one here without updating that list fails CI.
 *
 * Two boundary rules are load-bearing and are implemented here, not hoped for:
 *   - No C++ exception leaves a PF_API function. The kernel's own command
 *     surface throws when a command is illegal at the current point
 *     (native_execution_consumer.cpp `commands_allowed`); that throw is
 *     caught here and answered as PF_NATIVE_E_STATE, so a command issued from
 *     a C callback can never unwind through the callback and kill the run.
 *   - A C callback that returns non-zero raises CallbackFailure, a
 *     std::runtime_error the consumer's existing callback guard converts into
 *     NativeFailureCode::CallbackException. The C function itself never
 *     unwinds; the throw happens in the C++ virtual, above the C frame.
 */

#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/bar.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

/* Every switch over an enumeration in this file is exhaustive BY BUILD: a
 * kernel enumerator a translation below does not name is an error, not a
 * warning, under every GCC/Clang profile (no profile builds with -Werror).
 * The translations are what make an enumerator the kernel adds reach a C host
 * only once the C header names it. Popped at the end of the file. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
#endif

namespace {

using pineforge::Bar;
using pineforge::NativeStrategyHost;
namespace no = pineforge::native_order;

/* ── Enumerator pins ────────────────────────────────────────────────
 * The C values are the C++ alternatives' own integers. Translation still
 * goes through an exhaustive switch — these only keep the header honest. */

static_assert(PF_NATIVE_INTENT_FLATTEN == 0 && PF_NATIVE_INTENT_SIZED == 5,
              "pf_native_intent_e must span OrderIntent");
static_assert(std::variant_size_v<no::OrderIntent> == 6, "OrderIntent grew");
static_assert(std::variant_size_v<no::Trigger> == 5, "Trigger grew");
static_assert(std::variant_size_v<no::Owner> == 5, "Owner grew");
static_assert(std::variant_size_v<no::Capacity> == 2, "Capacity grew");
static_assert(std::variant_size_v<no::Group> == 2, "Group grew");
static_assert(std::variant_size_v<no::ReductionSize> == 3, "ReductionSize grew");
static_assert(std::variant_size_v<no::SizeBasis> == 2, "SizeBasis grew");
static_assert(std::variant_size_v<no::TriggerAnchor> == 2, "TriggerAnchor grew");
static_assert(std::variant_size_v<no::RemainingProjection> == 5, "RemainingProjection grew");
static_assert(std::variant_size_v<no::TriggerState> == 9, "TriggerState grew");
static_assert(std::variant_size_v<no::CommandEvent> == 19, "CommandEvent grew");
static_assert(PF_NATIVE_EVENT_MARGIN_CALL == 18 && PF_NATIVE_EVENT_RISK == 21,
              "the command-event tags must cover every CommandEvent alternative");
static_assert(static_cast<int>(no::RiskLimitKind::MaxFillsPerDay)
                  == PF_NATIVE_RISK_MAX_FILLS_PER_DAY, "RiskLimitKind drifted");
static_assert(static_cast<int>(pineforge::NativeRiskDay::CalendarDayInTimezone)
                  == PF_NATIVE_RISK_DAY_CALENDAR_TIMEZONE, "NativeRiskDay drifted");
static_assert(static_cast<int>(pineforge::NativeRiskAction::FlattenAndBlock)
                  == PF_NATIVE_RISK_FLATTEN_AND_BLOCK, "NativeRiskAction drifted");
/* The risk tail is append-only: the base layout must still end exactly where
 * PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE says, and the tail must be the twelve
 * fields below it and nothing else. */
static_assert(PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE
                  == PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE + 10u * sizeof(std::uint32_t)
                         + 2u * sizeof(double),
              "the pf_native_run_spec_ext_v1 risk tail moved");
/* So is the auxiliary-feed tail, last of the three: three pointers and two
 * words past the layout N8's intrabar / policy tail left. */
static_assert(sizeof(pf_native_run_spec_ext_v1)
                  == PF_NATIVE_RUN_SPEC_EXT_V1_POLICY_SIZE + 3u * sizeof(void*)
                         + 2u * sizeof(std::uint32_t),
              "the pf_native_run_spec_ext_v1 auxiliary tail moved");
static_assert(static_cast<int>(pineforge::NativeSeriesSource::Input)
                      == PF_NATIVE_SERIES_SOURCE_INPUT
                  && static_cast<int>(pineforge::NativeSeriesSource::AuxiliaryFeed)
                         == PF_NATIVE_SERIES_SOURCE_AUXILIARY_FEED,
              "NativeSeriesSource drifted");
/* A subscription row's two delivery rules are bools on the C++ side, so each
 * C enumeration names exactly those two values, false first: a zero-filled row
 * is the C++ default. A rule that grows a third value fails here before it can
 * reach a C host with no name. */
static_assert(std::is_same_v<decltype(pineforge::NativeTimeframeSubscription::lookahead), bool>
                  && std::is_same_v<decltype(pineforge::NativeTimeframeSubscription::gaps),
                                    bool>,
              "a subscription delivery rule is no longer a bool");
static_assert(PF_NATIVE_LOOKAHEAD_AT_COMPLETION == static_cast<int>(false)
                  && PF_NATIVE_LOOKAHEAD_AT_FIRST_INPUT == static_cast<int>(true),
              "pf_native_lookahead_e drifted from NativeTimeframeSubscription::lookahead");
static_assert(PF_NATIVE_GAPS_HOLD == static_cast<int>(false)
                  && PF_NATIVE_GAPS_CLEAR == static_cast<int>(true),
              "pf_native_gaps_e drifted from NativeTimeframeSubscription::gaps");
/* Typing the two words moved nothing: they are the row's uint32_t words, `gaps`
 * in the slot published as `reserved0`, at the offsets measured before the
 * enumerations existed (lookahead 4, gaps 28, size 32 on LP64). */
static_assert(std::is_same_v<decltype(pf_native_subscription_v1::lookahead), std::uint32_t>
                  && std::is_same_v<decltype(pf_native_subscription_v1::gaps), std::uint32_t>,
              "the subscription delivery words must stay uint32_t words");
static_assert(offsetof(pf_native_subscription_v1, lookahead) == sizeof(std::uint32_t)
                  && offsetof(pf_native_subscription_v1, tf) == 2u * sizeof(std::uint32_t)
                  && offsetof(pf_native_subscription_v1, authoritative_n)
                         == 2u * sizeof(std::uint32_t) + 2u * sizeof(void*)
                  && offsetof(pf_native_subscription_v1, gaps)
                         == offsetof(pf_native_subscription_v1, authoritative_n)
                                + sizeof(std::int32_t)
                  && sizeof(pf_native_subscription_v1)
                         == offsetof(pf_native_subscription_v1, gaps) + sizeof(std::uint32_t),
              "the pf_native_subscription_v1 layout moved");
/* The nine enum-valued words of the two run specifications: each C
 * enumeration names its kernel enumeration's values one for one, so every
 * enumerator is pinned, not only the last -- translate_word() reads a word
 * by its integer, and these pins are what make that integer name the same
 * rule on both sides. NativeReportPolicy's third value has no C name;
 * c_spelled() below refuses it. */
static_assert(static_cast<int>(pineforge::NativeFeeKind::Percent) == PF_NATIVE_FEE_PERCENT
                  && static_cast<int>(pineforge::NativeFeeKind::CashPerUnit)
                         == PF_NATIVE_FEE_CASH_PER_UNIT
                  && static_cast<int>(pineforge::NativeFeeKind::CashPerExecution)
                         == PF_NATIVE_FEE_CASH_PER_EXECUTION,
              "pf_native_fee_kind_e drifted from NativeFeeKind");
static_assert(static_cast<int>(pineforge::NativeCloseExecution::NextEligiblePoint)
                      == PF_NATIVE_CLOSE_EXECUTION_NEXT_ELIGIBLE_POINT
                  && static_cast<int>(pineforge::NativeCloseExecution::AfterCalculation)
                         == PF_NATIVE_CLOSE_EXECUTION_AFTER_CALCULATION,
              "pf_native_close_execution_e drifted from NativeCloseExecution");
static_assert(static_cast<int>(pineforge::NativeOpenDirections::None)
                      == PF_NATIVE_OPEN_DIRECTIONS_NONE
                  && static_cast<int>(pineforge::NativeOpenDirections::Long)
                         == PF_NATIVE_OPEN_DIRECTIONS_LONG
                  && static_cast<int>(pineforge::NativeOpenDirections::Short)
                         == PF_NATIVE_OPEN_DIRECTIONS_SHORT
                  && static_cast<int>(pineforge::NativeOpenDirections::Both)
                         == PF_NATIVE_OPEN_DIRECTIONS_BOTH,
              "pf_native_open_directions_e drifted from NativeOpenDirections");
static_assert(static_cast<int>(pineforge::NativeReportPolicy::HostRecorded)
                      == PF_NATIVE_REPORT_HOST_RECORDED
                  && static_cast<int>(pineforge::NativeReportPolicy::KernelRecorded)
                         == PF_NATIVE_REPORT_KERNEL_RECORDED,
              "pf_native_report_policy_e drifted from NativeReportPolicy");
static_assert(static_cast<int>(pineforge::NativePriceGrid::None) == PF_NATIVE_PRICE_GRID_NONE
                  && static_cast<int>(pineforge::NativePriceGrid::QuantizeFills)
                         == PF_NATIVE_PRICE_GRID_QUANTIZE_FILLS
                  && static_cast<int>(pineforge::NativePriceGrid::QuantizeFillsAndTriggers)
                         == PF_NATIVE_PRICE_GRID_QUANTIZE_FILLS_AND_TRIGGERS,
              "pf_native_price_grid_e drifted from NativePriceGrid");
static_assert(static_cast<int>(pineforge::NativeGridRounding::HalfUp)
                      == PF_NATIVE_GRID_ROUNDING_HALF_UP
                  && static_cast<int>(pineforge::NativeGridRounding::Directional)
                         == PF_NATIVE_GRID_ROUNDING_DIRECTIONAL,
              "pf_native_grid_rounding_e drifted from NativeGridRounding");
static_assert(static_cast<int>(pineforge::NativeCalculationTrigger::BarClose)
                      == PF_NATIVE_CALC_TRIGGER_BAR_CLOSE
                  && static_cast<int>(pineforge::NativeCalculationTrigger::BarCloseAndFills)
                         == PF_NATIVE_CALC_TRIGGER_BAR_CLOSE_AND_FILLS
                  && static_cast<int>(pineforge::NativeCalculationTrigger::EveryModeledPoint)
                         == PF_NATIVE_CALC_TRIGGER_EVERY_MODELED_POINT,
              "pf_native_calc_trigger_e drifted from NativeCalculationTrigger");
static_assert(static_cast<int>(pineforge::NativeOpenBarView::Complete)
                      == PF_NATIVE_OPEN_BAR_VIEW_COMPLETE
                  && static_cast<int>(pineforge::NativeOpenBarView::OpenOnly)
                         == PF_NATIVE_OPEN_BAR_VIEW_OPEN_ONLY,
              "pf_native_open_bar_view_e drifted from NativeOpenBarView");
static_assert(static_cast<int>(pineforge::NativeLiquidationSizing::RestoreMinimum)
                      == PF_NATIVE_LIQUIDATION_SIZING_RESTORE_MINIMUM
                  && static_cast<int>(pineforge::NativeLiquidationSizing::ShortfallMultiple)
                         == PF_NATIVE_LIQUIDATION_SIZING_SHORTFALL_MULTIPLE
                  && static_cast<int>(pineforge::NativeLiquidationSizing::Flatten)
                         == PF_NATIVE_LIQUIDATION_SIZING_FLATTEN,
              "pf_native_liquidation_sizing_e drifted from NativeLiquidationSizing");
/* Typing the nine words moved nothing: each is still the uint32_t word its
 * spec published, the six extension words at the offsets they always had
 * (the fourth to eleventh words of the struct, ahead of any pointer), and
 * the three base-spec words beside the words they always followed. */
static_assert(std::is_same_v<decltype(pf_native_run_spec_v1::fee_kind), std::uint32_t>
                  && std::is_same_v<decltype(pf_native_run_spec_v1::close_execution),
                                    std::uint32_t>
                  && std::is_same_v<decltype(pf_native_run_spec_v1::allowed_open_directions),
                                    std::uint32_t>
                  && std::is_same_v<decltype(pf_native_run_spec_ext_v1::report_policy),
                                    std::uint32_t>
                  && std::is_same_v<decltype(pf_native_run_spec_ext_v1::price_grid),
                                    std::uint32_t>
                  && std::is_same_v<decltype(pf_native_run_spec_ext_v1::grid_rounding),
                                    std::uint32_t>
                  && std::is_same_v<decltype(pf_native_run_spec_ext_v1::calculation),
                                    std::uint32_t>
                  && std::is_same_v<decltype(pf_native_run_spec_ext_v1::open_bar_view),
                                    std::uint32_t>
                  && std::is_same_v<decltype(pf_native_run_spec_ext_v1::margin_sizing),
                                    std::uint32_t>,
              "the run specifications' enum-valued words must stay uint32_t words");
static_assert(offsetof(pf_native_run_spec_ext_v1, report_policy) == 3u * sizeof(std::uint32_t)
                  && offsetof(pf_native_run_spec_ext_v1, price_grid)
                         == 5u * sizeof(std::uint32_t)
                  && offsetof(pf_native_run_spec_ext_v1, grid_rounding)
                         == 6u * sizeof(std::uint32_t)
                  && offsetof(pf_native_run_spec_ext_v1, calculation)
                         == 7u * sizeof(std::uint32_t)
                  && offsetof(pf_native_run_spec_ext_v1, open_bar_view)
                         == 9u * sizeof(std::uint32_t)
                  && offsetof(pf_native_run_spec_ext_v1, margin_sizing)
                         == 10u * sizeof(std::uint32_t),
              "a pf_native_run_spec_ext_v1 enum-valued word moved");
static_assert(offsetof(pf_native_run_spec_v1, fee_kind)
                      == offsetof(pf_native_run_spec_v1, slippage_ticks) + sizeof(std::uint32_t)
                  && offsetof(pf_native_run_spec_v1, close_execution)
                         == offsetof(pf_native_run_spec_v1, optional_mask)
                                + sizeof(std::uint32_t)
                  && offsetof(pf_native_run_spec_v1, allowed_open_directions)
                         == offsetof(pf_native_run_spec_v1, close_execution)
                                + sizeof(std::uint32_t),
              "a pf_native_run_spec_v1 enum-valued word moved");
static_assert(static_cast<int>(pineforge::NativeFailureCode::CallbackException)
                  == PF_NATIVE_FAILURE_CALLBACK,
              "PF_NATIVE_FAILURE_CALLBACK must mirror NativeFailureCode::CallbackException");
static_assert(static_cast<int>(no::Side::Short) == PF_NATIVE_SIDE_SHORT, "Side drifted");
static_assert(static_cast<int>(no::NativeAnchorRounding::Raw) == PF_NATIVE_ANCHOR_ROUNDING_RAW
                  && static_cast<int>(no::NativeAnchorRounding::HalfUp)
                         == PF_NATIVE_ANCHOR_ROUNDING_HALF_UP
                  && static_cast<int>(no::NativeAnchorRounding::Directional)
                         == PF_NATIVE_ANCHOR_ROUNDING_DIRECTIONAL,
              "NativeAnchorRounding drifted");
static_assert(static_cast<int>(no::NativeArmVisibility::Working)
                      == PF_NATIVE_ARM_VISIBILITY_WORKING
                  && static_cast<int>(no::NativeArmVisibility::PendingUntilArmed)
                         == PF_NATIVE_ARM_VISIBILITY_PENDING_UNTIL_ARMED,
              "NativeArmVisibility drifted");
/* Every request tail is append-only: each published layout must still end
 * exactly where its size constant says, and each tail must be the fields
 * below it and nothing else. */
static_assert(PF_NATIVE_REQUEST_V1_ANCHOR_SIZE
                  == PF_NATIVE_REQUEST_V1_BASE_SIZE + 2u * sizeof(std::uint32_t),
              "the pf_native_request_v1 anchored-leg tail moved");
static_assert(PF_NATIVE_REQUEST_V1_SIZING_SIZE
                  == PF_NATIVE_REQUEST_V1_ANCHOR_SIZE + 2u * sizeof(std::uint32_t),
              "the pf_native_request_v1 sizing-detail tail moved");
static_assert(offsetof(pf_native_request_v1, trail_has_best_seed)
                  == PF_NATIVE_REQUEST_V1_SIZING_SIZE + sizeof(double),
              "the pf_native_request_v1 trail-seed tail moved");
/* The seed layout ended in padding after its one-byte flag. reserved2 names
 * exactly those bytes, so the arm tail starts where that layout's sizeof
 * ended -- which is what lets a caller compiled before the arm tail send
 * PF_NATIVE_REQUEST_V1_SEED_SIZE -- and the tail is its two words. */
static_assert(offsetof(pf_native_request_v1, reserved2)
                      == offsetof(pf_native_request_v1, trail_has_best_seed) + 1u
                  && PF_NATIVE_REQUEST_V1_SEED_SIZE
                         == PF_NATIVE_REQUEST_V1_SIZING_SIZE + 2u * sizeof(double)
                  && PF_NATIVE_REQUEST_V1_SEED_SIZE % alignof(pf_native_request_v1) == 0u,
              "PF_NATIVE_REQUEST_V1_SEED_SIZE is not the seed layout's sizeof");
static_assert(sizeof(pf_native_request_v1)
                  == PF_NATIVE_REQUEST_V1_SEED_SIZE + 2u * sizeof(std::uint32_t),
              "the pf_native_request_v1 arm-relation tail moved");
static_assert(static_cast<int>(no::SizePrice::SignalOnTick)
                  == PF_NATIVE_SIZE_PRICE_SIGNAL_ON_TICK, "SizePrice drifted");
static_assert(static_cast<int>(no::ScopeBasis::AtAcceptance)
                  == PF_NATIVE_SCOPE_BASIS_AT_ACCEPTANCE, "ScopeBasis drifted");
static_assert(static_cast<int>(pineforge::NativeLiquidationCheck::PathAdverseExtremeMark)
                  == PF_NATIVE_LIQUIDATION_PATH_ADVERSE_EXTREME_MARK,
              "NativeLiquidationCheck drifted");
static_assert(static_cast<int>(pineforge::NativeMarginEquityBasis::MarkedEquityBeforeOpenCommission)
                  == PF_NATIVE_MARGIN_EQUITY_BEFORE_OPEN_COMMISSION,
              "NativeMarginEquityBasis drifted");
static_assert(static_cast<int>(pineforge::NativeLiquidationLevelBase::RealizedOnly)
                  == PF_NATIVE_MARGIN_LEVEL_REALIZED_ONLY, "NativeLiquidationLevelBase drifted");
static_assert(static_cast<int>(pineforge::NativeSlotLabelPolicy::FeedTolerant)
                  == PF_NATIVE_SLOT_LABEL_FEED_TOLERANT, "NativeSlotLabelPolicy drifted");
static_assert(static_cast<int>(pineforge::NativeFeedTolerance::WarmupNonNegativeOHLC)
                  == PF_NATIVE_FEED_TOLERANCE_WARMUP_NONNEGATIVE, "NativeFeedTolerance drifted");
static_assert(static_cast<int>(pineforge::NativePathOrder::LowFirst)
                  == PF_NATIVE_PATH_ORDER_LOW_FIRST, "NativePathOrder drifted");
static_assert(static_cast<int>(pineforge::OpenedLotFillPoint::OnPath)
                      == PF_NATIVE_OPENED_LOT_FILL_POINT_ON_PATH
                  && static_cast<int>(pineforge::OpenedLotFillPoint::AfterPath)
                         == PF_NATIVE_OPENED_LOT_FILL_POINT_AFTER_PATH,
              "OpenedLotFillPoint drifted");
static_assert(static_cast<int>(pineforge::NativeAbortReporting::Quiet)
                  == PF_NATIVE_ABORT_QUIET, "NativeAbortReporting drifted");
static_assert(static_cast<int>(pineforge::IntrabarPath::SampleEligibility::DistributionSamples)
                  == PF_NATIVE_SAMPLE_DISTRIBUTION_SAMPLES, "SampleEligibility drifted");
static_assert(static_cast<int>(pineforge::MagnifierDistribution::BACK_LOADED)
                  == PF_MAGNIFIER_BACK_LOADED, "MagnifierDistribution drifted");
/* The N8 tail is append-only in the same way: fourteen words and four
 * pointers past the layout L9 left, and it ends where the auxiliary tail
 * behind it begins. */
static_assert(PF_NATIVE_RUN_SPEC_EXT_V1_POLICY_SIZE
                  == PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE + 14u * sizeof(std::uint32_t)
                         + 4u * sizeof(const char*),
              "the pf_native_run_spec_ext_v1 intrabar/policy tail moved");
static_assert(static_cast<int>(no::SizeTime::AtAcceptance) == PF_NATIVE_SIZE_AT_ACCEPTANCE,
              "SizeTime drifted");
static_assert(static_cast<int>(no::ExecutionGridPolicy::ExplicitUnits)
                  == PF_NATIVE_GRID_EXPLICIT_UNITS, "ExecutionGridPolicy drifted");
static_assert(static_cast<int>(no::ScopeClaim::NetOfSiblings) == PF_NATIVE_SCOPE_NET_OF_SIBLINGS,
              "ScopeClaim drifted");
static_assert(static_cast<int>(no::GroupEffect::Reduce) == PF_NATIVE_GROUP_REDUCE,
              "GroupEffect drifted");
static_assert(static_cast<int>(pineforge::NativeCurrentPriceRule::NearestTick)
                  == PF_NATIVE_PRICE_NEAREST_TICK, "NativeCurrentPriceRule drifted");
static_assert(static_cast<int>(pineforge::NativeLifecycleKind::Failed)
                  == PF_NATIVE_LIFECYCLE_FAILED, "NativeLifecycleKind drifted");
static_assert(static_cast<int>(pineforge::NativeCurrentRefusal::ConfigurationMismatch)
                  == PF_NATIVE_REFUSAL_CONFIGURATION_MISMATCH, "NativeCurrentRefusal drifted");
static_assert(sizeof(pf_bar_t) == sizeof(Bar), "pf_bar_t / Bar size mismatch");
static_assert(static_cast<int>(pineforge::NativeCalculationReason::SubBar)
                  == PF_NATIVE_CALC_SUB_BAR, "NativeCalculationReason drifted");
static_assert(static_cast<int>(pineforge::NativeMarginCheckKind::Calculation)
                  == PF_NATIVE_MARGIN_CHECK_CALCULATION, "NativeMarginCheckKind drifted");
/* margin_view_pod() writes the kind through c_word() below, whose switch
 * fails the build for a kind the C header does not name -- which is how FxRoll
 * shipped in N6, as a number with no spelling. This pin and the ones beside
 * c_word() make each name its kernel value. */
static_assert(static_cast<int>(pineforge::NativeMarginCheckKind::FxRoll)
                  == PF_NATIVE_MARGIN_CHECK_FX_ROLL, "NativeMarginCheckKind drifted");
/* The hook tails are append-only: the base layout must still end exactly
 * where PF_NATIVE_CALLBACKS_V1_BASE_SIZE says, the six-hook tail must be the
 * seven function pointers below it, and the policy-hook tail the four after
 * those. */
static_assert(PF_NATIVE_CALLBACKS_V1_HOOKS_SIZE
                  == PF_NATIVE_CALLBACKS_V1_BASE_SIZE + 7u * sizeof(void (*)(void)),
              "the pf_native_callbacks_v1 hook tail moved");
static_assert(sizeof(pf_native_callbacks_v1)
                  == PF_NATIVE_CALLBACKS_V1_HOOKS_SIZE + 4u * sizeof(void (*)(void)),
              "the pf_native_callbacks_v1 policy-hook tail moved");
/* The working readout's tail is append-only too, and it is WRITTEN, so the
 * base length must be exactly what a base-layout caller's sizeof was: the
 * tail starts where `comment` ended, and that end carried no padding. The
 * tail itself is the two words below it and nothing else. */
static_assert(PF_NATIVE_WORKING_V1_BASE_SIZE
                      == offsetof(pf_native_working_v1, comment) + sizeof(const char*)
                  && PF_NATIVE_WORKING_V1_BASE_SIZE % alignof(pf_native_working_v1) == 0u,
              "PF_NATIVE_WORKING_V1_BASE_SIZE is not the base layout's sizeof");
static_assert(sizeof(pf_native_working_v1)
                  == PF_NATIVE_WORKING_V1_BASE_SIZE + 2u * sizeof(std::uint32_t),
              "the pf_native_working_v1 arm-presence tail moved");

/* ── The readout words, pinned enumerator by enumerator ─────────────
 * Every word the runtime WRITES for a C host is named in the C header, and
 * each C name is its kernel enumerator's own integer. c_word() below writes
 * them through an exhaustive switch; these pins are what make that integer
 * name the same thing on both sides. Every enumerator is pinned, not only
 * the last: an enumerator appended without a C name fails c_word()'s switch
 * (and scripts/check_native_c_api_surface.py), one inserted before an
 * existing name fails here. */
#define PF_PIN_WORD(kernel, c_name)                                                  \
    static_assert(static_cast<long long>(kernel) == static_cast<long long>(c_name), \
                  #c_name " drifted from " #kernel)

PF_PIN_WORD(pineforge::NativePriceProvenance::Confirmed, PF_NATIVE_PROVENANCE_CONFIRMED);
PF_PIN_WORD(pineforge::NativePriceProvenance::ObservedPrint, PF_NATIVE_PROVENANCE_OBSERVED_PRINT);
PF_PIN_WORD(pineforge::NativePriceProvenance::ModeledOHLCOpen,
            PF_NATIVE_PROVENANCE_MODELED_OHLC_OPEN);
PF_PIN_WORD(pineforge::NativePriceProvenance::ModeledOHLCClose,
            PF_NATIVE_PROVENANCE_MODELED_OHLC_CLOSE);
PF_PIN_WORD(pineforge::NativePriceProvenance::CarriedOpen, PF_NATIVE_PROVENANCE_CARRIED_OPEN);
PF_PIN_WORD(pineforge::NativePriceProvenance::AfterCalculationClose,
            PF_NATIVE_PROVENANCE_AFTER_CALCULATION_CLOSE);
PF_PIN_WORD(pineforge::NativePriceProvenance::PartialFinalized,
            PF_NATIVE_PROVENANCE_PARTIAL_FINALIZED);
PF_PIN_WORD(pineforge::NativePriceProvenance::Calculation, PF_NATIVE_PROVENANCE_CALCULATION);
PF_PIN_WORD(pineforge::NativePriceProvenance::CurrentExecution,
            PF_NATIVE_PROVENANCE_CURRENT_EXECUTION);
PF_PIN_WORD(pineforge::NativePathPhase::None, PF_NATIVE_PATH_PHASE_NONE);
PF_PIN_WORD(pineforge::NativePathPhase::Open, PF_NATIVE_PATH_PHASE_OPEN);
PF_PIN_WORD(pineforge::NativePathPhase::High, PF_NATIVE_PATH_PHASE_HIGH);
PF_PIN_WORD(pineforge::NativePathPhase::Low, PF_NATIVE_PATH_PHASE_LOW);
PF_PIN_WORD(pineforge::NativePathPhase::Close, PF_NATIVE_PATH_PHASE_CLOSE);
PF_PIN_WORD(pineforge::NativeCompletionKind::Confirmed, PF_NATIVE_COMPLETION_KIND_CONFIRMED);
PF_PIN_WORD(pineforge::NativeCompletionKind::LazyComplete, PF_NATIVE_COMPLETION_KIND_LAZY_COMPLETE);
PF_PIN_WORD(pineforge::NativeCompletionKind::SessionShortened,
            PF_NATIVE_COMPLETION_KIND_SESSION_SHORTENED);
PF_PIN_WORD(pineforge::NativeCompletionKind::PartialFinalized,
            PF_NATIVE_COMPLETION_KIND_PARTIAL_FINALIZED);
PF_PIN_WORD(pineforge::NativeCurrentQuoteKind::MarketDecision, PF_NATIVE_QUOTE_MARKET_DECISION);
PF_PIN_WORD(pineforge::NativeCurrentQuoteKind::ExecutionAnchor, PF_NATIVE_QUOTE_EXECUTION_ANCHOR);
PF_PIN_WORD(no::AppliedTerminalReason::WorkingUnitsSatisfied,
            PF_NATIVE_TERMINAL_WORKING_UNITS_SATISFIED);
PF_PIN_WORD(no::AppliedTerminalReason::Flattened, PF_NATIVE_TERMINAL_FLATTENED);
PF_PIN_WORD(no::AppliedTerminalReason::TargetExhausted, PF_NATIVE_TERMINAL_TARGET_EXHAUSTED);
PF_PIN_WORD(no::RequestRejectReason::InvalidQuantity, PF_NATIVE_REJECT_INVALID_QUANTITY);
PF_PIN_WORD(no::RequestRejectReason::OffGrid, PF_NATIVE_REJECT_OFF_GRID);
PF_PIN_WORD(no::RequestRejectReason::InvalidTrigger, PF_NATIVE_REJECT_INVALID_TRIGGER);
PF_PIN_WORD(no::RequestRejectReason::InvalidCapacity, PF_NATIVE_REJECT_INVALID_CAPACITY);
PF_PIN_WORD(no::RequestRejectReason::InvalidOwner, PF_NATIVE_REJECT_INVALID_OWNER);
PF_PIN_WORD(no::RequestRejectReason::InvalidQuantityBasis,
            PF_NATIVE_REJECT_INVALID_QUANTITY_BASIS);
PF_PIN_WORD(no::RequestRejectReason::InvalidGroup, PF_NATIVE_REJECT_INVALID_GROUP);
PF_PIN_WORD(no::RequestRejectReason::PlacementAdmission, PF_NATIVE_REJECT_PLACEMENT_ADMISSION);
PF_PIN_WORD(no::CancelReason::User, PF_NATIVE_CANCEL_USER);
PF_PIN_WORD(no::CancelReason::Group, PF_NATIVE_CANCEL_GROUP);
PF_PIN_WORD(no::CancelReason::OwnerGone, PF_NATIVE_CANCEL_OWNER_GONE);
PF_PIN_WORD(no::CancelReason::UnsupportedRelation, PF_NATIVE_CANCEL_UNSUPPORTED_RELATION);
PF_PIN_WORD(no::CancelReason::Superseded, PF_NATIVE_CANCEL_SUPERSEDED);
PF_PIN_WORD(no::MatchRejectReason::NonpositivePrice, PF_NATIVE_MATCH_REJECT_NONPOSITIVE_PRICE);
PF_PIN_WORD(no::MatchRejectReason::OpeningDirection, PF_NATIVE_MATCH_REJECT_OPENING_DIRECTION);
PF_PIN_WORD(no::MatchRejectReason::MaxAbsUnits, PF_NATIVE_MATCH_REJECT_MAX_ABS_UNITS);
PF_PIN_WORD(no::MatchRejectReason::MaxOpenLots, PF_NATIVE_MATCH_REJECT_MAX_OPEN_LOTS);
PF_PIN_WORD(no::MatchRejectReason::InitialMargin, PF_NATIVE_MATCH_REJECT_INITIAL_MARGIN);
PF_PIN_WORD(no::MatchRejectReason::TermsUnresolved, PF_NATIVE_MATCH_REJECT_TERMS_UNRESOLVED);
PF_PIN_WORD(no::MatchRejectReason::InvalidTerms, PF_NATIVE_MATCH_REJECT_INVALID_TERMS);
PF_PIN_WORD(no::MatchRejectReason::NoOppositeExposure,
            PF_NATIVE_MATCH_REJECT_NO_OPPOSITE_EXPOSURE);
PF_PIN_WORD(no::MatchRejectReason::HostPrecommit, PF_NATIVE_MATCH_REJECT_HOST_PRECOMMIT);
PF_PIN_WORD(no::MatchRejectReason::RiskLimit, PF_NATIVE_MATCH_REJECT_RISK_LIMIT);
PF_PIN_WORD(no::ActivationKind::Stop, PF_NATIVE_ACTIVATION_STOP);
PF_PIN_WORD(no::ActivationKind::StopLimit, PF_NATIVE_ACTIVATION_STOP_LIMIT);
PF_PIN_WORD(no::ActivationKind::TrailArm, PF_NATIVE_ACTIVATION_TRAIL_ARM);
PF_PIN_WORD(no::ActivationKind::TrailTrigger, PF_NATIVE_ACTIVATION_TRAIL_TRIGGER);
PF_PIN_WORD(no::RequestOrigin::Host, PF_NATIVE_ORIGIN_HOST);
PF_PIN_WORD(no::RequestOrigin::KernelLiquidation, PF_NATIVE_ORIGIN_KERNEL_LIQUIDATION);
PF_PIN_WORD(no::RequestOrigin::KernelRisk, PF_NATIVE_ORIGIN_KERNEL_RISK);
PF_PIN_WORD(pineforge::NativeFailureCode::None, PF_NATIVE_FAILURE_NONE);
PF_PIN_WORD(pineforge::NativeFailureCode::InvalidSpecification,
            PF_NATIVE_FAILURE_INVALID_SPECIFICATION);
PF_PIN_WORD(pineforge::NativeFailureCode::Contract, PF_NATIVE_FAILURE_CONTRACT);
PF_PIN_WORD(pineforge::NativeFailureCode::Preflight, PF_NATIVE_FAILURE_PREFLIGHT);
PF_PIN_WORD(pineforge::NativeFailureCode::UnsupportedSource, PF_NATIVE_FAILURE_UNSUPPORTED_SOURCE);
PF_PIN_WORD(pineforge::NativeFailureCode::CallbackException,
            PF_NATIVE_FAILURE_CALLBACK_EXCEPTION);
PF_PIN_WORD(pineforge::NativeFailureCode::SettlementFailure,
            PF_NATIVE_FAILURE_SETTLEMENT_FAILURE);
PF_PIN_WORD(pineforge::NativeFailureCode::Allocation, PF_NATIVE_FAILURE_ALLOCATION);
PF_PIN_WORD(pineforge::NativeFailureCode::CounterExhausted, PF_NATIVE_FAILURE_COUNTER_EXHAUSTED);
PF_PIN_WORD(pineforge::NativeFailureCode::Aborted, PF_NATIVE_FAILURE_ABORTED);
PF_PIN_WORD(pineforge::NativeFailureCode::ProjectionMismatch,
            PF_NATIVE_FAILURE_PROJECTION_MISMATCH);
PF_PIN_WORD(pineforge::NativeFailureCode::Calendar, PF_NATIVE_FAILURE_CALENDAR);
PF_PIN_WORD(pineforge::NativeFailureCode::Unexpected, PF_NATIVE_FAILURE_UNEXPECTED);
PF_PIN_WORD(pineforge::NativeFailureOperation::None, PF_NATIVE_OPERATION_NONE);
PF_PIN_WORD(pineforge::NativeFailureOperation::Configure, PF_NATIVE_OPERATION_CONFIGURE);
PF_PIN_WORD(pineforge::NativeFailureOperation::Begin, PF_NATIVE_OPERATION_BEGIN);
PF_PIN_WORD(pineforge::NativeFailureOperation::Command, PF_NATIVE_OPERATION_COMMAND);
PF_PIN_WORD(pineforge::NativeFailureOperation::Input, PF_NATIVE_OPERATION_INPUT);
PF_PIN_WORD(pineforge::NativeFailureOperation::Callback, PF_NATIVE_OPERATION_CALLBACK);
PF_PIN_WORD(pineforge::NativeFailureOperation::Settlement, PF_NATIVE_OPERATION_SETTLEMENT);
PF_PIN_WORD(pineforge::NativeFailureOperation::Mutation, PF_NATIVE_OPERATION_MUTATION);
PF_PIN_WORD(pineforge::NativeFailureOperation::Stream, PF_NATIVE_OPERATION_STREAM);
PF_PIN_WORD(no::OpeningShape::Transact, PF_NATIVE_OPENING_SHAPE_TRANSACT);
PF_PIN_WORD(no::OpeningShape::ReverseTo, PF_NATIVE_OPENING_SHAPE_REVERSE_TO);
PF_PIN_WORD(no::OpeningShape::CloseOpposite, PF_NATIVE_OPENING_SHAPE_CLOSE_OPPOSITE);
PF_PIN_WORD(no::ExecutionGridPolicy::SnapToGrid, PF_NATIVE_GRID_SNAP);
PF_PIN_WORD(no::NativeCandidatePriceKind::PointPrice, PF_NATIVE_CANDIDATE_PRICE_POINT_PRICE);
PF_PIN_WORD(no::NativeCandidatePriceKind::TriggerLevel, PF_NATIVE_CANDIDATE_PRICE_TRIGGER_LEVEL);
PF_PIN_WORD(no::NativeCandidatePriceKind::CurrentQuote, PF_NATIVE_CANDIDATE_PRICE_CURRENT_QUOTE);
PF_PIN_WORD(no::DriverEligibilityClass::ObservedPrint, PF_NATIVE_DRIVER_CLASS_OBSERVED_PRINT);
PF_PIN_WORD(no::DriverEligibilityClass::CarriedOpen, PF_NATIVE_DRIVER_CLASS_CARRIED_OPEN);
PF_PIN_WORD(no::DriverEligibilityClass::TickAfterCalculation,
            PF_NATIVE_DRIVER_CLASS_TICK_AFTER_CALCULATION);
PF_PIN_WORD(no::DriverEligibilityClass::ConfirmedOpen, PF_NATIVE_DRIVER_CLASS_CONFIRMED_OPEN);
PF_PIN_WORD(no::DriverEligibilityClass::ConfirmedExcursion,
            PF_NATIVE_DRIVER_CLASS_CONFIRMED_EXCURSION);
PF_PIN_WORD(no::DriverEligibilityClass::ConfirmedAfterCalculationClose,
            PF_NATIVE_DRIVER_CLASS_CONFIRMED_AFTER_CALCULATION_CLOSE);
PF_PIN_WORD(no::DriverEligibilityClass::CurrentExecution,
            PF_NATIVE_DRIVER_CLASS_CURRENT_EXECUTION);
PF_PIN_WORD(pineforge::NativePrecommitVerdict::Admit, PF_NATIVE_PRECOMMIT_ADMIT);
PF_PIN_WORD(pineforge::NativePrecommitVerdict::Refuse, PF_NATIVE_PRECOMMIT_REFUSE);
PF_PIN_WORD(pineforge::NativePrecommitVerdict::AdmitWithHostMargin,
            PF_NATIVE_PRECOMMIT_ADMIT_WITH_HOST_MARGIN);
PF_PIN_WORD(pineforge::NativeAnchoredTrigger::Limit, PF_NATIVE_ANCHORED_TRIGGER_LIMIT);
PF_PIN_WORD(pineforge::NativeAnchoredTrigger::Stop, PF_NATIVE_ANCHORED_TRIGGER_STOP);
PF_PIN_WORD(pineforge::NativeAnchoredTrigger::TrailArm, PF_NATIVE_ANCHORED_TRIGGER_TRAIL_ARM);
PF_PIN_WORD(pineforge::NativeCurrentPriceRule::AsPresented, PF_NATIVE_PRICE_AS_PRESENTED);
/* The plan the precommit view names is an alternative of a variant, in this
 * order; the visit in precommit_pod() names each one. */
static_assert(std::variant_size_v<no::ExecutionPlan> == 4, "ExecutionPlan grew");
static_assert(std::is_same_v<std::variant_alternative_t<0, no::ExecutionPlan>,
                             pineforge::execution::Flatten>
                  && std::is_same_v<std::variant_alternative_t<3, no::ExecutionPlan>,
                                    pineforge::execution::ReverseTo>,
              "pf_native_plan_e drifted from ExecutionPlan");
static_assert(std::variant_size_v<no::Remaining> == 5, "Remaining grew");
PF_PIN_WORD(no::NativeArmFirstMatch::AtArmPrint, PF_NATIVE_ARM_FIRST_MATCH_AT_ARM_PRINT);
PF_PIN_WORD(no::NativeArmFirstMatch::AfterArmPrint, PF_NATIVE_ARM_FIRST_MATCH_AFTER_ARM_PRINT);
PF_PIN_WORD(no::NativeArmScope::OwnerLot, PF_NATIVE_ARM_SCOPE_OWNER_LOT);
PF_PIN_WORD(no::NativeArmScope::Book, PF_NATIVE_ARM_SCOPE_BOOK);
PF_PIN_WORD(pineforge::NativeRunPhase::Batch, PF_NATIVE_PHASE_BATCH);
PF_PIN_WORD(pineforge::NativeRunPhase::Warmup, PF_NATIVE_PHASE_WARMUP);
PF_PIN_WORD(pineforge::NativeRunPhase::Realtime, PF_NATIVE_PHASE_REALTIME);
PF_PIN_WORD(pineforge::NativeCompletion::BatchComplete, PF_NATIVE_COMPLETION_BATCH_COMPLETE);
PF_PIN_WORD(pineforge::NativeCompletion::StreamEnded, PF_NATIVE_COMPLETION_STREAM_ENDED);
/* The readout words that were named before this lane, pinned whole now. */
PF_PIN_WORD(pineforge::NativeLifecycleKind::Unconfigured, PF_NATIVE_LIFECYCLE_UNCONFIGURED);
PF_PIN_WORD(pineforge::NativeLifecycleKind::Ready, PF_NATIVE_LIFECYCLE_READY);
PF_PIN_WORD(pineforge::NativeLifecycleKind::Running, PF_NATIVE_LIFECYCLE_RUNNING);
PF_PIN_WORD(pineforge::NativeLifecycleKind::Completed, PF_NATIVE_LIFECYCLE_COMPLETED);
PF_PIN_WORD(pineforge::NativeMarginCheckKind::BarOpen, PF_NATIVE_MARGIN_CHECK_BAR_OPEN);
PF_PIN_WORD(pineforge::NativeMarginCheckKind::AfterApplied, PF_NATIVE_MARGIN_CHECK_AFTER_APPLIED);
PF_PIN_WORD(pineforge::NativeCalculationReason::BarClose, PF_NATIVE_CALC_BAR_CLOSE);
PF_PIN_WORD(pineforge::NativeCalculationReason::OrderFill, PF_NATIVE_CALC_ORDER_FILL);
PF_PIN_WORD(pineforge::NativeCalculationReason::Tick, PF_NATIVE_CALC_TICK);
PF_PIN_WORD(pineforge::NativeCurrentRefusal::NoExecutionContext,
            PF_NATIVE_REFUSAL_NO_EXECUTION_CONTEXT);
PF_PIN_WORD(pineforge::NativeCurrentRefusal::Reentrant, PF_NATIVE_REFUSAL_REENTRANT);
PF_PIN_WORD(pineforge::NativeCurrentRefusal::InvalidHandle, PF_NATIVE_REFUSAL_INVALID_HANDLE);
PF_PIN_WORD(pineforge::NativeCurrentRefusal::NotWorking, PF_NATIVE_REFUSAL_NOT_WORKING);
PF_PIN_WORD(pineforge::NativeCurrentRefusal::NotAcceptedInCallback,
            PF_NATIVE_REFUSAL_NOT_ACCEPTED_IN_CALLBACK);
PF_PIN_WORD(pineforge::NativeCurrentRefusal::UnsupportedRequest,
            PF_NATIVE_REFUSAL_UNSUPPORTED_REQUEST);
PF_PIN_WORD(pineforge::NativeCurrentRefusal::UnreadyOwner, PF_NATIVE_REFUSAL_UNREADY_OWNER);
PF_PIN_WORD(pineforge::NativeCurrentRefusal::InvalidSelection,
            PF_NATIVE_REFUSAL_INVALID_SELECTION);
PF_PIN_WORD(no::Side::Long, PF_NATIVE_SIDE_LONG);
PF_PIN_WORD(no::GroupEffect::Cancel, PF_NATIVE_GROUP_CANCEL);
PF_PIN_WORD(no::RiskLimitKind::MaxDrawdown, PF_NATIVE_RISK_MAX_DRAWDOWN);
PF_PIN_WORD(no::RiskLimitKind::MaxIntradayLoss, PF_NATIVE_RISK_MAX_INTRADAY_LOSS);
PF_PIN_WORD(no::RiskLimitKind::MaxConsecutiveLossDays, PF_NATIVE_RISK_MAX_CONSECUTIVE_LOSS_DAYS);
/* pineforge.h's words: the close cause strategy_closed_trade_close_cause
 * answers, and the optional-field bits of the v1 base spec. */
PF_PIN_WORD(pineforge::execution::CloseCause::Unspecified, PF_CLOSE_CAUSE_UNSPECIFIED);
PF_PIN_WORD(pineforge::execution::CloseCause::Script, PF_CLOSE_CAUSE_SCRIPT);
PF_PIN_WORD(pineforge::execution::CloseCause::Bracket, PF_CLOSE_CAUSE_BRACKET);
PF_PIN_WORD(pineforge::execution::CloseCause::Liquidation, PF_CLOSE_CAUSE_LIQUIDATION);
PF_PIN_WORD(pineforge::execution::CloseCause::RiskLimit, PF_CLOSE_CAUSE_RISK_LIMIT);
PF_PIN_WORD(pineforge::execution::CloseCause::FillCap, PF_CLOSE_CAUSE_FILL_CAP);
PF_PIN_WORD(pineforge::execution::CloseCause::RangeEnd, PF_CLOSE_CAUSE_RANGE_END);
/* The typed refusal words of a begin-time declaration and of an append: the
 * run spec's own validation words, and the append's. */
PF_PIN_WORD(pineforge::NativeRunSpecError::None, PF_NATIVE_SPEC_ERROR_NONE);
PF_PIN_WORD(pineforge::NativeRunSpecError::EmptyRequiredString,
            PF_NATIVE_SPEC_ERROR_EMPTY_REQUIRED_STRING);
PF_PIN_WORD(pineforge::NativeRunSpecError::EmbeddedNul, PF_NATIVE_SPEC_ERROR_EMBEDDED_NUL);
PF_PIN_WORD(pineforge::NativeRunSpecError::InvalidUtf8, PF_NATIVE_SPEC_ERROR_INVALID_UTF8);
PF_PIN_WORD(pineforge::NativeRunSpecError::ZeroRunNumber, PF_NATIVE_SPEC_ERROR_ZERO_RUN_NUMBER);
PF_PIN_WORD(pineforge::NativeRunSpecError::InvalidTimeframe,
            PF_NATIVE_SPEC_ERROR_INVALID_TIMEFRAME);
PF_PIN_WORD(pineforge::NativeRunSpecError::IncompatibleTimeframes,
            PF_NATIVE_SPEC_ERROR_INCOMPATIBLE_TIMEFRAMES);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnresolvedTimezone,
            PF_NATIVE_SPEC_ERROR_UNRESOLVED_TIMEZONE);
PF_PIN_WORD(pineforge::NativeRunSpecError::InvalidSession, PF_NATIVE_SPEC_ERROR_INVALID_SESSION);
PF_PIN_WORD(pineforge::NativeRunSpecError::NotFinitePositive,
            PF_NATIVE_SPEC_ERROR_NOT_FINITE_POSITIVE);
PF_PIN_WORD(pineforge::NativeRunSpecError::SlippageOutOfRange,
            PF_NATIVE_SPEC_ERROR_SLIPPAGE_OUT_OF_RANGE);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownFeeKind, PF_NATIVE_SPEC_ERROR_UNKNOWN_FEE_KIND);
PF_PIN_WORD(pineforge::NativeRunSpecError::NotFiniteNonnegative,
            PF_NATIVE_SPEC_ERROR_NOT_FINITE_NONNEGATIVE);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownCloseExecution,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_CLOSE_EXECUTION);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownAbortReporting,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_ABORT_REPORTING);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownOpenDirections,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_OPEN_DIRECTIONS);
PF_PIN_WORD(pineforge::NativeRunSpecError::ZeroLotLimit, PF_NATIVE_SPEC_ERROR_ZERO_LOT_LIMIT);
PF_PIN_WORD(pineforge::NativeRunSpecError::AllocationFailure,
            PF_NATIVE_SPEC_ERROR_ALLOCATION_FAILURE);
PF_PIN_WORD(pineforge::NativeRunSpecError::CalendarFailure,
            PF_NATIVE_SPEC_ERROR_CALENDAR_FAILURE);
PF_PIN_WORD(pineforge::NativeRunSpecError::InvalidIntrabarPath,
            PF_NATIVE_SPEC_ERROR_INVALID_INTRABAR_PATH);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownIntrabarSampleEligibility,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_INTRABAR_SAMPLE_ELIGIBILITY);
PF_PIN_WORD(pineforge::NativeRunSpecError::InvalidUndetectedTimeframe,
            PF_NATIVE_SPEC_ERROR_INVALID_UNDETECTED_TIMEFRAME);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownSlotLabelPolicy,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_SLOT_LABEL_POLICY);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownLegacyTolerance,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_FEED_TOLERANCE);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownPathOrder,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_PATH_ORDER);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownReportPolicy,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_REPORT_POLICY);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownPriceGrid,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_PRICE_GRID);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownGridRounding,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_GRID_ROUNDING);
PF_PIN_WORD(pineforge::NativeRunSpecError::GridRequiresPriceTick,
            PF_NATIVE_SPEC_ERROR_GRID_REQUIRES_PRICE_TICK);
PF_PIN_WORD(pineforge::NativeRunSpecError::InvalidSubscriptionTimeframe,
            PF_NATIVE_SPEC_ERROR_INVALID_SUBSCRIPTION_TIMEFRAME);
PF_PIN_WORD(pineforge::NativeRunSpecError::SubscriptionFinerThanInput,
            PF_NATIVE_SPEC_ERROR_SUBSCRIPTION_FINER_THAN_INPUT);
PF_PIN_WORD(pineforge::NativeRunSpecError::DuplicateSubscriptionTimeframe,
            PF_NATIVE_SPEC_ERROR_DUPLICATE_SUBSCRIPTION_TIMEFRAME);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnorderedSubscriptionBars,
            PF_NATIVE_SPEC_ERROR_UNORDERED_SUBSCRIPTION_BARS);
PF_PIN_WORD(pineforge::NativeRunSpecError::SubscriptionWithoutTimeframe,
            PF_NATIVE_SPEC_ERROR_SUBSCRIPTION_WITHOUT_TIMEFRAME);
PF_PIN_WORD(pineforge::NativeRunSpecError::MarginModelConflict,
            PF_NATIVE_SPEC_ERROR_MARGIN_MODEL_CONFLICT);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownLiquidationSizing,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_LIQUIDATION_SIZING);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownLiquidationCheck,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_LIQUIDATION_CHECK);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownMarginEquityBasis,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_MARGIN_EQUITY_BASIS);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownLiquidationLevelBase,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_LIQUIDATION_LEVEL_BASE);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownCalculationTrigger,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_CALCULATION_TRIGGER);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownOpenBarView,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_OPEN_BAR_VIEW);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownRiskDay, PF_NATIVE_SPEC_ERROR_UNKNOWN_RISK_DAY);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownRiskAction,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_RISK_ACTION);
PF_PIN_WORD(pineforge::NativeRunSpecError::ZeroRiskLimit, PF_NATIVE_SPEC_ERROR_ZERO_RISK_LIMIT);
PF_PIN_WORD(pineforge::NativeRunSpecError::MarginSideUndeclared,
            PF_NATIVE_SPEC_ERROR_MARGIN_SIDE_UNDECLARED);
PF_PIN_WORD(pineforge::NativeRunSpecError::InvalidAuxiliaryFeedTimeframe,
            PF_NATIVE_SPEC_ERROR_INVALID_AUXILIARY_FEED_TIMEFRAME);
PF_PIN_WORD(pineforge::NativeRunSpecError::AuxiliaryFeedNotFinerThanInput,
            PF_NATIVE_SPEC_ERROR_AUXILIARY_FEED_NOT_FINER_THAN_INPUT);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnorderedAuxiliaryFeedBars,
            PF_NATIVE_SPEC_ERROR_UNORDERED_AUXILIARY_FEED_BARS);
PF_PIN_WORD(pineforge::NativeRunSpecError::InvalidAuxiliaryFeedBar,
            PF_NATIVE_SPEC_ERROR_INVALID_AUXILIARY_FEED_BAR);
PF_PIN_WORD(pineforge::NativeRunSpecError::AuxiliaryFeedWithoutTimeframe,
            PF_NATIVE_SPEC_ERROR_AUXILIARY_FEED_WITHOUT_TIMEFRAME);
PF_PIN_WORD(pineforge::NativeRunSpecError::UnknownSeriesSource,
            PF_NATIVE_SPEC_ERROR_UNKNOWN_SERIES_SOURCE);
PF_PIN_WORD(pineforge::NativeRunSpecError::SubscriptionWithoutAuxiliaryFeed,
            PF_NATIVE_SPEC_ERROR_SUBSCRIPTION_WITHOUT_AUXILIARY_FEED);
PF_PIN_WORD(pineforge::NativeRunSpecError::SubscriptionFinerThanAuxiliaryFeed,
            PF_NATIVE_SPEC_ERROR_SUBSCRIPTION_FINER_THAN_AUXILIARY_FEED);
PF_PIN_WORD(pineforge::NativeRunSpecError::WrongPhase, PF_NATIVE_SPEC_ERROR_WRONG_PHASE);
PF_PIN_WORD(pineforge::NativeRunSpecField::None, PF_NATIVE_SPEC_FIELD_NONE);
PF_PIN_WORD(pineforge::NativeRunSpecField::SessionKey, PF_NATIVE_SPEC_FIELD_SESSION_KEY);
PF_PIN_WORD(pineforge::NativeRunSpecField::RunNumber, PF_NATIVE_SPEC_FIELD_RUN_NUMBER);
PF_PIN_WORD(pineforge::NativeRunSpecField::InputTimeframe, PF_NATIVE_SPEC_FIELD_INPUT_TIMEFRAME);
PF_PIN_WORD(pineforge::NativeRunSpecField::ScriptTimeframe,
            PF_NATIVE_SPEC_FIELD_SCRIPT_TIMEFRAME);
PF_PIN_WORD(pineforge::NativeRunSpecField::Ticker, PF_NATIVE_SPEC_FIELD_TICKER);
PF_PIN_WORD(pineforge::NativeRunSpecField::TickerId, PF_NATIVE_SPEC_FIELD_TICKER_ID);
PF_PIN_WORD(pineforge::NativeRunSpecField::Type, PF_NATIVE_SPEC_FIELD_TYPE);
PF_PIN_WORD(pineforge::NativeRunSpecField::Currency, PF_NATIVE_SPEC_FIELD_CURRENCY);
PF_PIN_WORD(pineforge::NativeRunSpecField::BaseCurrency, PF_NATIVE_SPEC_FIELD_BASE_CURRENCY);
PF_PIN_WORD(pineforge::NativeRunSpecField::Description, PF_NATIVE_SPEC_FIELD_DESCRIPTION);
PF_PIN_WORD(pineforge::NativeRunSpecField::VolumeType, PF_NATIVE_SPEC_FIELD_VOLUME_TYPE);
PF_PIN_WORD(pineforge::NativeRunSpecField::Timezone, PF_NATIVE_SPEC_FIELD_TIMEZONE);
PF_PIN_WORD(pineforge::NativeRunSpecField::Session, PF_NATIVE_SPEC_FIELD_SESSION);
PF_PIN_WORD(pineforge::NativeRunSpecField::ChartTimezone, PF_NATIVE_SPEC_FIELD_CHART_TIMEZONE);
PF_PIN_WORD(pineforge::NativeRunSpecField::InitialCapital, PF_NATIVE_SPEC_FIELD_INITIAL_CAPITAL);
PF_PIN_WORD(pineforge::NativeRunSpecField::PointValue, PF_NATIVE_SPEC_FIELD_POINT_VALUE);
PF_PIN_WORD(pineforge::NativeRunSpecField::AccountFx, PF_NATIVE_SPEC_FIELD_ACCOUNT_FX);
PF_PIN_WORD(pineforge::NativeRunSpecField::PriceTick, PF_NATIVE_SPEC_FIELD_PRICE_TICK);
PF_PIN_WORD(pineforge::NativeRunSpecField::SlippageTicks, PF_NATIVE_SPEC_FIELD_SLIPPAGE_TICKS);
PF_PIN_WORD(pineforge::NativeRunSpecField::FeeKind, PF_NATIVE_SPEC_FIELD_FEE_KIND);
PF_PIN_WORD(pineforge::NativeRunSpecField::FeeValue, PF_NATIVE_SPEC_FIELD_FEE_VALUE);
PF_PIN_WORD(pineforge::NativeRunSpecField::QuantityGrid, PF_NATIVE_SPEC_FIELD_QUANTITY_GRID);
PF_PIN_WORD(pineforge::NativeRunSpecField::CloseExecution, PF_NATIVE_SPEC_FIELD_CLOSE_EXECUTION);
PF_PIN_WORD(pineforge::NativeRunSpecField::AbortReporting, PF_NATIVE_SPEC_FIELD_ABORT_REPORTING);
PF_PIN_WORD(pineforge::NativeRunSpecField::MaxAbsUnits, PF_NATIVE_SPEC_FIELD_MAX_ABS_UNITS);
PF_PIN_WORD(pineforge::NativeRunSpecField::MaxOpenLots, PF_NATIVE_SPEC_FIELD_MAX_OPEN_LOTS);
PF_PIN_WORD(pineforge::NativeRunSpecField::AllowedOpenDirections,
            PF_NATIVE_SPEC_FIELD_ALLOWED_OPEN_DIRECTIONS);
PF_PIN_WORD(pineforge::NativeRunSpecField::InitialMarginFraction,
            PF_NATIVE_SPEC_FIELD_INITIAL_MARGIN_FRACTION);
PF_PIN_WORD(pineforge::NativeRunSpecField::IntrabarTimeframe,
            PF_NATIVE_SPEC_FIELD_INTRABAR_TIMEFRAME);
PF_PIN_WORD(pineforge::NativeRunSpecField::IntrabarSamples,
            PF_NATIVE_SPEC_FIELD_INTRABAR_SAMPLES);
PF_PIN_WORD(pineforge::NativeRunSpecField::IntrabarDistribution,
            PF_NATIVE_SPEC_FIELD_INTRABAR_DISTRIBUTION);
PF_PIN_WORD(pineforge::NativeRunSpecField::IntrabarVolumeSamples,
            PF_NATIVE_SPEC_FIELD_INTRABAR_VOLUME_SAMPLES);
PF_PIN_WORD(pineforge::NativeRunSpecField::IntrabarSampleEligibility,
            PF_NATIVE_SPEC_FIELD_INTRABAR_SAMPLE_ELIGIBILITY);
PF_PIN_WORD(pineforge::NativeRunSpecField::TimeframeUndetected,
            PF_NATIVE_SPEC_FIELD_TIMEFRAME_UNDETECTED);
PF_PIN_WORD(pineforge::NativeRunSpecField::SlotLabelPolicy,
            PF_NATIVE_SPEC_FIELD_SLOT_LABEL_POLICY);
PF_PIN_WORD(pineforge::NativeRunSpecField::LegacyTolerance, PF_NATIVE_SPEC_FIELD_FEED_TOLERANCE);
PF_PIN_WORD(pineforge::NativeRunSpecField::PathOrder, PF_NATIVE_SPEC_FIELD_PATH_ORDER);
PF_PIN_WORD(pineforge::NativeRunSpecField::ReportPolicy, PF_NATIVE_SPEC_FIELD_REPORT_POLICY);
PF_PIN_WORD(pineforge::NativeRunSpecField::PriceGrid, PF_NATIVE_SPEC_FIELD_PRICE_GRID);
PF_PIN_WORD(pineforge::NativeRunSpecField::GridRounding, PF_NATIVE_SPEC_FIELD_GRID_ROUNDING);
PF_PIN_WORD(pineforge::NativeRunSpecField::SubscriptionTimeframe,
            PF_NATIVE_SPEC_FIELD_SUBSCRIPTION_TIMEFRAME);
PF_PIN_WORD(pineforge::NativeRunSpecField::SubscriptionBars,
            PF_NATIVE_SPEC_FIELD_SUBSCRIPTION_BARS);
PF_PIN_WORD(pineforge::NativeRunSpecField::MarginModel, PF_NATIVE_SPEC_FIELD_MARGIN_MODEL);
PF_PIN_WORD(pineforge::NativeRunSpecField::MarginInitial, PF_NATIVE_SPEC_FIELD_MARGIN_INITIAL);
PF_PIN_WORD(pineforge::NativeRunSpecField::MarginMaintenance,
            PF_NATIVE_SPEC_FIELD_MARGIN_MAINTENANCE);
PF_PIN_WORD(pineforge::NativeRunSpecField::MarginSizing, PF_NATIVE_SPEC_FIELD_MARGIN_SIZING);
PF_PIN_WORD(pineforge::NativeRunSpecField::MarginShortfallMultiple,
            PF_NATIVE_SPEC_FIELD_MARGIN_SHORTFALL_MULTIPLE);
PF_PIN_WORD(pineforge::NativeRunSpecField::MarginMinUnits, PF_NATIVE_SPEC_FIELD_MARGIN_MIN_UNITS);
PF_PIN_WORD(pineforge::NativeRunSpecField::MarginCheck, PF_NATIVE_SPEC_FIELD_MARGIN_CHECK);
PF_PIN_WORD(pineforge::NativeRunSpecField::MarginEquityBasis,
            PF_NATIVE_SPEC_FIELD_MARGIN_EQUITY_BASIS);
PF_PIN_WORD(pineforge::NativeRunSpecField::MarginLevelBase,
            PF_NATIVE_SPEC_FIELD_MARGIN_LEVEL_BASE);
PF_PIN_WORD(pineforge::NativeRunSpecField::Calculation, PF_NATIVE_SPEC_FIELD_CALCULATION);
PF_PIN_WORD(pineforge::NativeRunSpecField::OpenBarView, PF_NATIVE_SPEC_FIELD_OPEN_BAR_VIEW);
PF_PIN_WORD(pineforge::NativeRunSpecField::RiskLimits, PF_NATIVE_SPEC_FIELD_RISK_LIMITS);
PF_PIN_WORD(pineforge::NativeRunSpecField::RiskDrawdown, PF_NATIVE_SPEC_FIELD_RISK_DRAWDOWN);
PF_PIN_WORD(pineforge::NativeRunSpecField::RiskIntradayLoss,
            PF_NATIVE_SPEC_FIELD_RISK_INTRADAY_LOSS);
PF_PIN_WORD(pineforge::NativeRunSpecField::RiskLossDays, PF_NATIVE_SPEC_FIELD_RISK_LOSS_DAYS);
PF_PIN_WORD(pineforge::NativeRunSpecField::RiskFillsPerDay,
            PF_NATIVE_SPEC_FIELD_RISK_FILLS_PER_DAY);
PF_PIN_WORD(pineforge::NativeRunSpecField::RiskDayBasis, PF_NATIVE_SPEC_FIELD_RISK_DAY_BASIS);
PF_PIN_WORD(pineforge::NativeRunSpecField::RiskAction, PF_NATIVE_SPEC_FIELD_RISK_ACTION);
PF_PIN_WORD(pineforge::NativeRunSpecField::AuxiliaryFeedTimeframe,
            PF_NATIVE_SPEC_FIELD_AUXILIARY_FEED_TIMEFRAME);
PF_PIN_WORD(pineforge::NativeRunSpecField::AuxiliaryFeedBars,
            PF_NATIVE_SPEC_FIELD_AUXILIARY_FEED_BARS);
PF_PIN_WORD(pineforge::NativeRunSpecField::SubscriptionSource,
            PF_NATIVE_SPEC_FIELD_SUBSCRIPTION_SOURCE);
PF_PIN_WORD(pineforge::NativeAuxiliaryAppendError::None, PF_NATIVE_APPEND_ERROR_NONE);
PF_PIN_WORD(pineforge::NativeAuxiliaryAppendError::HostFailed,
            PF_NATIVE_APPEND_ERROR_HOST_FAILED);
PF_PIN_WORD(pineforge::NativeAuxiliaryAppendError::Reentrant, PF_NATIVE_APPEND_ERROR_REENTRANT);
PF_PIN_WORD(pineforge::NativeAuxiliaryAppendError::NotRealtime,
            PF_NATIVE_APPEND_ERROR_NOT_REALTIME);
PF_PIN_WORD(pineforge::NativeAuxiliaryAppendError::NoAuxiliaryFeed,
            PF_NATIVE_APPEND_ERROR_NO_AUXILIARY_FEED);
PF_PIN_WORD(pineforge::NativeAuxiliaryAppendError::InvalidBarArray,
            PF_NATIVE_APPEND_ERROR_INVALID_BAR_ARRAY);
PF_PIN_WORD(pineforge::NativeAuxiliaryAppendError::InvalidBar,
            PF_NATIVE_APPEND_ERROR_INVALID_BAR);
PF_PIN_WORD(pineforge::NativeAuxiliaryAppendError::UnorderedBars,
            PF_NATIVE_APPEND_ERROR_UNORDERED_BARS);
PF_PIN_WORD(pineforge::NativeAuxiliaryAppendError::InputPeriodAlreadyAccepted,
            PF_NATIVE_APPEND_ERROR_INPUT_PERIOD_ALREADY_ACCEPTED);
PF_PIN_WORD(pineforge::NativeAuxiliaryAppendError::AllocationFailure,
            PF_NATIVE_APPEND_ERROR_ALLOCATION_FAILURE);
static_assert((PF_NATIVE_SPEC_OPTIONAL_QUANTITY_GRID | PF_NATIVE_SPEC_OPTIONAL_MAX_ABS_UNITS
               | PF_NATIVE_SPEC_OPTIONAL_INITIAL_MARGIN_FRACTION
               | PF_NATIVE_SPEC_OPTIONAL_MAX_OPEN_LOTS)
                  == 0xfu,
              "the v1 base spec's optional-field bits moved");

/* The C++ -> C direction of the same words: one exhaustive switch per kernel
 * enumeration, with no default. The file-wide -Werror=switch above makes an
 * enumerator the C header does not name a build failure here. */
constexpr std::uint32_t c_word(pineforge::NativePriceProvenance value) noexcept {
    using V = pineforge::NativePriceProvenance;
    switch (value) {
    case V::Confirmed: return PF_NATIVE_PROVENANCE_CONFIRMED;
    case V::ObservedPrint: return PF_NATIVE_PROVENANCE_OBSERVED_PRINT;
    case V::ModeledOHLCOpen: return PF_NATIVE_PROVENANCE_MODELED_OHLC_OPEN;
    case V::ModeledOHLCClose: return PF_NATIVE_PROVENANCE_MODELED_OHLC_CLOSE;
    case V::CarriedOpen: return PF_NATIVE_PROVENANCE_CARRIED_OPEN;
    case V::AfterCalculationClose: return PF_NATIVE_PROVENANCE_AFTER_CALCULATION_CLOSE;
    case V::PartialFinalized: return PF_NATIVE_PROVENANCE_PARTIAL_FINALIZED;
    case V::Calculation: return PF_NATIVE_PROVENANCE_CALCULATION;
    case V::CurrentExecution: return PF_NATIVE_PROVENANCE_CURRENT_EXECUTION;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(pineforge::NativePathPhase value) noexcept {
    using V = pineforge::NativePathPhase;
    switch (value) {
    case V::None: return PF_NATIVE_PATH_PHASE_NONE;
    case V::Open: return PF_NATIVE_PATH_PHASE_OPEN;
    case V::High: return PF_NATIVE_PATH_PHASE_HIGH;
    case V::Low: return PF_NATIVE_PATH_PHASE_LOW;
    case V::Close: return PF_NATIVE_PATH_PHASE_CLOSE;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(pineforge::NativeCompletionKind value) noexcept {
    using V = pineforge::NativeCompletionKind;
    switch (value) {
    case V::Confirmed: return PF_NATIVE_COMPLETION_KIND_CONFIRMED;
    case V::LazyComplete: return PF_NATIVE_COMPLETION_KIND_LAZY_COMPLETE;
    case V::SessionShortened: return PF_NATIVE_COMPLETION_KIND_SESSION_SHORTENED;
    case V::PartialFinalized: return PF_NATIVE_COMPLETION_KIND_PARTIAL_FINALIZED;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(pineforge::NativeCurrentQuoteKind value) noexcept {
    using V = pineforge::NativeCurrentQuoteKind;
    switch (value) {
    case V::MarketDecision: return PF_NATIVE_QUOTE_MARKET_DECISION;
    case V::ExecutionAnchor: return PF_NATIVE_QUOTE_EXECUTION_ANCHOR;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(no::AppliedTerminalReason value) noexcept {
    using V = no::AppliedTerminalReason;
    switch (value) {
    case V::WorkingUnitsSatisfied: return PF_NATIVE_TERMINAL_WORKING_UNITS_SATISFIED;
    case V::Flattened: return PF_NATIVE_TERMINAL_FLATTENED;
    case V::TargetExhausted: return PF_NATIVE_TERMINAL_TARGET_EXHAUSTED;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(no::RequestRejectReason value) noexcept {
    using V = no::RequestRejectReason;
    switch (value) {
    case V::InvalidQuantity: return PF_NATIVE_REJECT_INVALID_QUANTITY;
    case V::OffGrid: return PF_NATIVE_REJECT_OFF_GRID;
    case V::InvalidTrigger: return PF_NATIVE_REJECT_INVALID_TRIGGER;
    case V::InvalidCapacity: return PF_NATIVE_REJECT_INVALID_CAPACITY;
    case V::InvalidOwner: return PF_NATIVE_REJECT_INVALID_OWNER;
    case V::InvalidQuantityBasis: return PF_NATIVE_REJECT_INVALID_QUANTITY_BASIS;
    case V::InvalidGroup: return PF_NATIVE_REJECT_INVALID_GROUP;
    case V::PlacementAdmission: return PF_NATIVE_REJECT_PLACEMENT_ADMISSION;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(no::CancelReason value) noexcept {
    using V = no::CancelReason;
    switch (value) {
    case V::User: return PF_NATIVE_CANCEL_USER;
    case V::Group: return PF_NATIVE_CANCEL_GROUP;
    case V::OwnerGone: return PF_NATIVE_CANCEL_OWNER_GONE;
    case V::UnsupportedRelation: return PF_NATIVE_CANCEL_UNSUPPORTED_RELATION;
    case V::Superseded: return PF_NATIVE_CANCEL_SUPERSEDED;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(no::MatchRejectReason value) noexcept {
    using V = no::MatchRejectReason;
    switch (value) {
    case V::NonpositivePrice: return PF_NATIVE_MATCH_REJECT_NONPOSITIVE_PRICE;
    case V::OpeningDirection: return PF_NATIVE_MATCH_REJECT_OPENING_DIRECTION;
    case V::MaxAbsUnits: return PF_NATIVE_MATCH_REJECT_MAX_ABS_UNITS;
    case V::MaxOpenLots: return PF_NATIVE_MATCH_REJECT_MAX_OPEN_LOTS;
    case V::InitialMargin: return PF_NATIVE_MATCH_REJECT_INITIAL_MARGIN;
    case V::TermsUnresolved: return PF_NATIVE_MATCH_REJECT_TERMS_UNRESOLVED;
    case V::InvalidTerms: return PF_NATIVE_MATCH_REJECT_INVALID_TERMS;
    case V::NoOppositeExposure: return PF_NATIVE_MATCH_REJECT_NO_OPPOSITE_EXPOSURE;
    case V::HostPrecommit: return PF_NATIVE_MATCH_REJECT_HOST_PRECOMMIT;
    case V::RiskLimit: return PF_NATIVE_MATCH_REJECT_RISK_LIMIT;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(no::ActivationKind value) noexcept {
    using V = no::ActivationKind;
    switch (value) {
    case V::Stop: return PF_NATIVE_ACTIVATION_STOP;
    case V::StopLimit: return PF_NATIVE_ACTIVATION_STOP_LIMIT;
    case V::TrailArm: return PF_NATIVE_ACTIVATION_TRAIL_ARM;
    case V::TrailTrigger: return PF_NATIVE_ACTIVATION_TRAIL_TRIGGER;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(no::RequestOrigin value) noexcept {
    using V = no::RequestOrigin;
    switch (value) {
    case V::Host: return PF_NATIVE_ORIGIN_HOST;
    case V::KernelLiquidation: return PF_NATIVE_ORIGIN_KERNEL_LIQUIDATION;
    case V::KernelRisk: return PF_NATIVE_ORIGIN_KERNEL_RISK;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(pineforge::NativeFailureCode value) noexcept {
    using V = pineforge::NativeFailureCode;
    switch (value) {
    case V::None: return PF_NATIVE_FAILURE_NONE;
    case V::InvalidSpecification: return PF_NATIVE_FAILURE_INVALID_SPECIFICATION;
    case V::Contract: return PF_NATIVE_FAILURE_CONTRACT;
    case V::Preflight: return PF_NATIVE_FAILURE_PREFLIGHT;
    case V::UnsupportedSource: return PF_NATIVE_FAILURE_UNSUPPORTED_SOURCE;
    case V::CallbackException: return PF_NATIVE_FAILURE_CALLBACK_EXCEPTION;
    case V::SettlementFailure: return PF_NATIVE_FAILURE_SETTLEMENT_FAILURE;
    case V::Allocation: return PF_NATIVE_FAILURE_ALLOCATION;
    case V::CounterExhausted: return PF_NATIVE_FAILURE_COUNTER_EXHAUSTED;
    case V::Aborted: return PF_NATIVE_FAILURE_ABORTED;
    case V::ProjectionMismatch: return PF_NATIVE_FAILURE_PROJECTION_MISMATCH;
    case V::Calendar: return PF_NATIVE_FAILURE_CALENDAR;
    case V::Unexpected: return PF_NATIVE_FAILURE_UNEXPECTED;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(pineforge::NativeFailureOperation value) noexcept {
    using V = pineforge::NativeFailureOperation;
    switch (value) {
    case V::None: return PF_NATIVE_OPERATION_NONE;
    case V::Configure: return PF_NATIVE_OPERATION_CONFIGURE;
    case V::Begin: return PF_NATIVE_OPERATION_BEGIN;
    case V::Command: return PF_NATIVE_OPERATION_COMMAND;
    case V::Input: return PF_NATIVE_OPERATION_INPUT;
    case V::Callback: return PF_NATIVE_OPERATION_CALLBACK;
    case V::Settlement: return PF_NATIVE_OPERATION_SETTLEMENT;
    case V::Mutation: return PF_NATIVE_OPERATION_MUTATION;
    case V::Stream: return PF_NATIVE_OPERATION_STREAM;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(pineforge::NativeRunPhase value) noexcept {
    using V = pineforge::NativeRunPhase;
    switch (value) {
    case V::Batch: return PF_NATIVE_PHASE_BATCH;
    case V::Warmup: return PF_NATIVE_PHASE_WARMUP;
    case V::Realtime: return PF_NATIVE_PHASE_REALTIME;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(pineforge::NativeCompletion value) noexcept {
    using V = pineforge::NativeCompletion;
    switch (value) {
    case V::BatchComplete: return PF_NATIVE_COMPLETION_BATCH_COMPLETE;
    case V::StreamEnded: return PF_NATIVE_COMPLETION_STREAM_ENDED;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(pineforge::NativeLifecycleKind value) noexcept {
    using V = pineforge::NativeLifecycleKind;
    switch (value) {
    case V::Unconfigured: return PF_NATIVE_LIFECYCLE_UNCONFIGURED;
    case V::Ready: return PF_NATIVE_LIFECYCLE_READY;
    case V::Running: return PF_NATIVE_LIFECYCLE_RUNNING;
    case V::Completed: return PF_NATIVE_LIFECYCLE_COMPLETED;
    case V::Failed: return PF_NATIVE_LIFECYCLE_FAILED;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(pineforge::NativeMarginCheckKind value) noexcept {
    using V = pineforge::NativeMarginCheckKind;
    switch (value) {
    case V::BarOpen: return PF_NATIVE_MARGIN_CHECK_BAR_OPEN;
    case V::AfterApplied: return PF_NATIVE_MARGIN_CHECK_AFTER_APPLIED;
    case V::Calculation: return PF_NATIVE_MARGIN_CHECK_CALCULATION;
    case V::FxRoll: return PF_NATIVE_MARGIN_CHECK_FX_ROLL;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(pineforge::NativeCalculationReason value) noexcept {
    using V = pineforge::NativeCalculationReason;
    switch (value) {
    case V::BarClose: return PF_NATIVE_CALC_BAR_CLOSE;
    case V::OrderFill: return PF_NATIVE_CALC_ORDER_FILL;
    case V::Tick: return PF_NATIVE_CALC_TICK;
    case V::SubBar: return PF_NATIVE_CALC_SUB_BAR;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(pineforge::NativeCurrentRefusal value) noexcept {
    using V = pineforge::NativeCurrentRefusal;
    switch (value) {
    case V::NoExecutionContext: return PF_NATIVE_REFUSAL_NO_EXECUTION_CONTEXT;
    case V::Reentrant: return PF_NATIVE_REFUSAL_REENTRANT;
    case V::InvalidHandle: return PF_NATIVE_REFUSAL_INVALID_HANDLE;
    case V::NotWorking: return PF_NATIVE_REFUSAL_NOT_WORKING;
    case V::NotAcceptedInCallback: return PF_NATIVE_REFUSAL_NOT_ACCEPTED_IN_CALLBACK;
    case V::UnsupportedRequest: return PF_NATIVE_REFUSAL_UNSUPPORTED_REQUEST;
    case V::UnreadyOwner: return PF_NATIVE_REFUSAL_UNREADY_OWNER;
    case V::InvalidSelection: return PF_NATIVE_REFUSAL_INVALID_SELECTION;
    case V::ConfigurationMismatch: return PF_NATIVE_REFUSAL_CONFIGURATION_MISMATCH;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(no::Side value) noexcept {
    switch (value) {
    case no::Side::Long: return PF_NATIVE_SIDE_LONG;
    case no::Side::Short: return PF_NATIVE_SIDE_SHORT;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(no::GroupEffect value) noexcept {
    switch (value) {
    case no::GroupEffect::Cancel: return PF_NATIVE_GROUP_CANCEL;
    case no::GroupEffect::Reduce: return PF_NATIVE_GROUP_REDUCE;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(no::RiskLimitKind value) noexcept {
    using V = no::RiskLimitKind;
    switch (value) {
    case V::MaxDrawdown: return PF_NATIVE_RISK_MAX_DRAWDOWN;
    case V::MaxIntradayLoss: return PF_NATIVE_RISK_MAX_INTRADAY_LOSS;
    case V::MaxConsecutiveLossDays: return PF_NATIVE_RISK_MAX_CONSECUTIVE_LOSS_DAYS;
    case V::MaxFillsPerDay: return PF_NATIVE_RISK_MAX_FILLS_PER_DAY;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(pineforge::NativeRunSpecError value) noexcept {
    using V = pineforge::NativeRunSpecError;
    switch (value) {
    case V::None: return PF_NATIVE_SPEC_ERROR_NONE;
    case V::EmptyRequiredString: return PF_NATIVE_SPEC_ERROR_EMPTY_REQUIRED_STRING;
    case V::EmbeddedNul: return PF_NATIVE_SPEC_ERROR_EMBEDDED_NUL;
    case V::InvalidUtf8: return PF_NATIVE_SPEC_ERROR_INVALID_UTF8;
    case V::ZeroRunNumber: return PF_NATIVE_SPEC_ERROR_ZERO_RUN_NUMBER;
    case V::InvalidTimeframe: return PF_NATIVE_SPEC_ERROR_INVALID_TIMEFRAME;
    case V::IncompatibleTimeframes: return PF_NATIVE_SPEC_ERROR_INCOMPATIBLE_TIMEFRAMES;
    case V::UnresolvedTimezone: return PF_NATIVE_SPEC_ERROR_UNRESOLVED_TIMEZONE;
    case V::InvalidSession: return PF_NATIVE_SPEC_ERROR_INVALID_SESSION;
    case V::NotFinitePositive: return PF_NATIVE_SPEC_ERROR_NOT_FINITE_POSITIVE;
    case V::SlippageOutOfRange: return PF_NATIVE_SPEC_ERROR_SLIPPAGE_OUT_OF_RANGE;
    case V::UnknownFeeKind: return PF_NATIVE_SPEC_ERROR_UNKNOWN_FEE_KIND;
    case V::NotFiniteNonnegative: return PF_NATIVE_SPEC_ERROR_NOT_FINITE_NONNEGATIVE;
    case V::UnknownCloseExecution: return PF_NATIVE_SPEC_ERROR_UNKNOWN_CLOSE_EXECUTION;
    case V::UnknownAbortReporting: return PF_NATIVE_SPEC_ERROR_UNKNOWN_ABORT_REPORTING;
    case V::UnknownOpenDirections: return PF_NATIVE_SPEC_ERROR_UNKNOWN_OPEN_DIRECTIONS;
    case V::ZeroLotLimit: return PF_NATIVE_SPEC_ERROR_ZERO_LOT_LIMIT;
    case V::AllocationFailure: return PF_NATIVE_SPEC_ERROR_ALLOCATION_FAILURE;
    case V::CalendarFailure: return PF_NATIVE_SPEC_ERROR_CALENDAR_FAILURE;
    case V::InvalidIntrabarPath: return PF_NATIVE_SPEC_ERROR_INVALID_INTRABAR_PATH;
    case V::UnknownIntrabarSampleEligibility:
        return PF_NATIVE_SPEC_ERROR_UNKNOWN_INTRABAR_SAMPLE_ELIGIBILITY;
    case V::InvalidUndetectedTimeframe: return PF_NATIVE_SPEC_ERROR_INVALID_UNDETECTED_TIMEFRAME;
    case V::UnknownSlotLabelPolicy: return PF_NATIVE_SPEC_ERROR_UNKNOWN_SLOT_LABEL_POLICY;
    case V::UnknownLegacyTolerance: return PF_NATIVE_SPEC_ERROR_UNKNOWN_FEED_TOLERANCE;
    case V::UnknownPathOrder: return PF_NATIVE_SPEC_ERROR_UNKNOWN_PATH_ORDER;
    case V::UnknownReportPolicy: return PF_NATIVE_SPEC_ERROR_UNKNOWN_REPORT_POLICY;
    case V::UnknownPriceGrid: return PF_NATIVE_SPEC_ERROR_UNKNOWN_PRICE_GRID;
    case V::UnknownGridRounding: return PF_NATIVE_SPEC_ERROR_UNKNOWN_GRID_ROUNDING;
    case V::GridRequiresPriceTick: return PF_NATIVE_SPEC_ERROR_GRID_REQUIRES_PRICE_TICK;
    case V::InvalidSubscriptionTimeframe:
        return PF_NATIVE_SPEC_ERROR_INVALID_SUBSCRIPTION_TIMEFRAME;
    case V::SubscriptionFinerThanInput: return PF_NATIVE_SPEC_ERROR_SUBSCRIPTION_FINER_THAN_INPUT;
    case V::DuplicateSubscriptionTimeframe:
        return PF_NATIVE_SPEC_ERROR_DUPLICATE_SUBSCRIPTION_TIMEFRAME;
    case V::UnorderedSubscriptionBars: return PF_NATIVE_SPEC_ERROR_UNORDERED_SUBSCRIPTION_BARS;
    case V::SubscriptionWithoutTimeframe:
        return PF_NATIVE_SPEC_ERROR_SUBSCRIPTION_WITHOUT_TIMEFRAME;
    case V::MarginModelConflict: return PF_NATIVE_SPEC_ERROR_MARGIN_MODEL_CONFLICT;
    case V::UnknownLiquidationSizing: return PF_NATIVE_SPEC_ERROR_UNKNOWN_LIQUIDATION_SIZING;
    case V::UnknownLiquidationCheck: return PF_NATIVE_SPEC_ERROR_UNKNOWN_LIQUIDATION_CHECK;
    case V::UnknownMarginEquityBasis: return PF_NATIVE_SPEC_ERROR_UNKNOWN_MARGIN_EQUITY_BASIS;
    case V::UnknownLiquidationLevelBase: return PF_NATIVE_SPEC_ERROR_UNKNOWN_LIQUIDATION_LEVEL_BASE;
    case V::UnknownCalculationTrigger: return PF_NATIVE_SPEC_ERROR_UNKNOWN_CALCULATION_TRIGGER;
    case V::UnknownOpenBarView: return PF_NATIVE_SPEC_ERROR_UNKNOWN_OPEN_BAR_VIEW;
    case V::UnknownRiskDay: return PF_NATIVE_SPEC_ERROR_UNKNOWN_RISK_DAY;
    case V::UnknownRiskAction: return PF_NATIVE_SPEC_ERROR_UNKNOWN_RISK_ACTION;
    case V::ZeroRiskLimit: return PF_NATIVE_SPEC_ERROR_ZERO_RISK_LIMIT;
    case V::MarginSideUndeclared: return PF_NATIVE_SPEC_ERROR_MARGIN_SIDE_UNDECLARED;
    case V::InvalidAuxiliaryFeedTimeframe:
        return PF_NATIVE_SPEC_ERROR_INVALID_AUXILIARY_FEED_TIMEFRAME;
    case V::AuxiliaryFeedNotFinerThanInput:
        return PF_NATIVE_SPEC_ERROR_AUXILIARY_FEED_NOT_FINER_THAN_INPUT;
    case V::UnorderedAuxiliaryFeedBars: return PF_NATIVE_SPEC_ERROR_UNORDERED_AUXILIARY_FEED_BARS;
    case V::InvalidAuxiliaryFeedBar: return PF_NATIVE_SPEC_ERROR_INVALID_AUXILIARY_FEED_BAR;
    case V::AuxiliaryFeedWithoutTimeframe:
        return PF_NATIVE_SPEC_ERROR_AUXILIARY_FEED_WITHOUT_TIMEFRAME;
    case V::UnknownSeriesSource: return PF_NATIVE_SPEC_ERROR_UNKNOWN_SERIES_SOURCE;
    case V::SubscriptionWithoutAuxiliaryFeed:
        return PF_NATIVE_SPEC_ERROR_SUBSCRIPTION_WITHOUT_AUXILIARY_FEED;
    case V::SubscriptionFinerThanAuxiliaryFeed:
        return PF_NATIVE_SPEC_ERROR_SUBSCRIPTION_FINER_THAN_AUXILIARY_FEED;
    case V::WrongPhase: return PF_NATIVE_SPEC_ERROR_WRONG_PHASE;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(pineforge::NativeRunSpecField value) noexcept {
    using V = pineforge::NativeRunSpecField;
    switch (value) {
    case V::None: return PF_NATIVE_SPEC_FIELD_NONE;
    case V::SessionKey: return PF_NATIVE_SPEC_FIELD_SESSION_KEY;
    case V::RunNumber: return PF_NATIVE_SPEC_FIELD_RUN_NUMBER;
    case V::InputTimeframe: return PF_NATIVE_SPEC_FIELD_INPUT_TIMEFRAME;
    case V::ScriptTimeframe: return PF_NATIVE_SPEC_FIELD_SCRIPT_TIMEFRAME;
    case V::Ticker: return PF_NATIVE_SPEC_FIELD_TICKER;
    case V::TickerId: return PF_NATIVE_SPEC_FIELD_TICKER_ID;
    case V::Type: return PF_NATIVE_SPEC_FIELD_TYPE;
    case V::Currency: return PF_NATIVE_SPEC_FIELD_CURRENCY;
    case V::BaseCurrency: return PF_NATIVE_SPEC_FIELD_BASE_CURRENCY;
    case V::Description: return PF_NATIVE_SPEC_FIELD_DESCRIPTION;
    case V::VolumeType: return PF_NATIVE_SPEC_FIELD_VOLUME_TYPE;
    case V::Timezone: return PF_NATIVE_SPEC_FIELD_TIMEZONE;
    case V::Session: return PF_NATIVE_SPEC_FIELD_SESSION;
    case V::ChartTimezone: return PF_NATIVE_SPEC_FIELD_CHART_TIMEZONE;
    case V::InitialCapital: return PF_NATIVE_SPEC_FIELD_INITIAL_CAPITAL;
    case V::PointValue: return PF_NATIVE_SPEC_FIELD_POINT_VALUE;
    case V::AccountFx: return PF_NATIVE_SPEC_FIELD_ACCOUNT_FX;
    case V::PriceTick: return PF_NATIVE_SPEC_FIELD_PRICE_TICK;
    case V::SlippageTicks: return PF_NATIVE_SPEC_FIELD_SLIPPAGE_TICKS;
    case V::FeeKind: return PF_NATIVE_SPEC_FIELD_FEE_KIND;
    case V::FeeValue: return PF_NATIVE_SPEC_FIELD_FEE_VALUE;
    case V::QuantityGrid: return PF_NATIVE_SPEC_FIELD_QUANTITY_GRID;
    case V::CloseExecution: return PF_NATIVE_SPEC_FIELD_CLOSE_EXECUTION;
    case V::AbortReporting: return PF_NATIVE_SPEC_FIELD_ABORT_REPORTING;
    case V::MaxAbsUnits: return PF_NATIVE_SPEC_FIELD_MAX_ABS_UNITS;
    case V::MaxOpenLots: return PF_NATIVE_SPEC_FIELD_MAX_OPEN_LOTS;
    case V::AllowedOpenDirections: return PF_NATIVE_SPEC_FIELD_ALLOWED_OPEN_DIRECTIONS;
    case V::InitialMarginFraction: return PF_NATIVE_SPEC_FIELD_INITIAL_MARGIN_FRACTION;
    case V::IntrabarTimeframe: return PF_NATIVE_SPEC_FIELD_INTRABAR_TIMEFRAME;
    case V::IntrabarSamples: return PF_NATIVE_SPEC_FIELD_INTRABAR_SAMPLES;
    case V::IntrabarDistribution: return PF_NATIVE_SPEC_FIELD_INTRABAR_DISTRIBUTION;
    case V::IntrabarVolumeSamples: return PF_NATIVE_SPEC_FIELD_INTRABAR_VOLUME_SAMPLES;
    case V::IntrabarSampleEligibility: return PF_NATIVE_SPEC_FIELD_INTRABAR_SAMPLE_ELIGIBILITY;
    case V::TimeframeUndetected: return PF_NATIVE_SPEC_FIELD_TIMEFRAME_UNDETECTED;
    case V::SlotLabelPolicy: return PF_NATIVE_SPEC_FIELD_SLOT_LABEL_POLICY;
    case V::LegacyTolerance: return PF_NATIVE_SPEC_FIELD_FEED_TOLERANCE;
    case V::PathOrder: return PF_NATIVE_SPEC_FIELD_PATH_ORDER;
    case V::ReportPolicy: return PF_NATIVE_SPEC_FIELD_REPORT_POLICY;
    case V::PriceGrid: return PF_NATIVE_SPEC_FIELD_PRICE_GRID;
    case V::GridRounding: return PF_NATIVE_SPEC_FIELD_GRID_ROUNDING;
    case V::SubscriptionTimeframe: return PF_NATIVE_SPEC_FIELD_SUBSCRIPTION_TIMEFRAME;
    case V::SubscriptionBars: return PF_NATIVE_SPEC_FIELD_SUBSCRIPTION_BARS;
    case V::MarginModel: return PF_NATIVE_SPEC_FIELD_MARGIN_MODEL;
    case V::MarginInitial: return PF_NATIVE_SPEC_FIELD_MARGIN_INITIAL;
    case V::MarginMaintenance: return PF_NATIVE_SPEC_FIELD_MARGIN_MAINTENANCE;
    case V::MarginSizing: return PF_NATIVE_SPEC_FIELD_MARGIN_SIZING;
    case V::MarginShortfallMultiple: return PF_NATIVE_SPEC_FIELD_MARGIN_SHORTFALL_MULTIPLE;
    case V::MarginMinUnits: return PF_NATIVE_SPEC_FIELD_MARGIN_MIN_UNITS;
    case V::MarginCheck: return PF_NATIVE_SPEC_FIELD_MARGIN_CHECK;
    case V::MarginEquityBasis: return PF_NATIVE_SPEC_FIELD_MARGIN_EQUITY_BASIS;
    case V::MarginLevelBase: return PF_NATIVE_SPEC_FIELD_MARGIN_LEVEL_BASE;
    case V::Calculation: return PF_NATIVE_SPEC_FIELD_CALCULATION;
    case V::OpenBarView: return PF_NATIVE_SPEC_FIELD_OPEN_BAR_VIEW;
    case V::RiskLimits: return PF_NATIVE_SPEC_FIELD_RISK_LIMITS;
    case V::RiskDrawdown: return PF_NATIVE_SPEC_FIELD_RISK_DRAWDOWN;
    case V::RiskIntradayLoss: return PF_NATIVE_SPEC_FIELD_RISK_INTRADAY_LOSS;
    case V::RiskLossDays: return PF_NATIVE_SPEC_FIELD_RISK_LOSS_DAYS;
    case V::RiskFillsPerDay: return PF_NATIVE_SPEC_FIELD_RISK_FILLS_PER_DAY;
    case V::RiskDayBasis: return PF_NATIVE_SPEC_FIELD_RISK_DAY_BASIS;
    case V::RiskAction: return PF_NATIVE_SPEC_FIELD_RISK_ACTION;
    case V::AuxiliaryFeedTimeframe: return PF_NATIVE_SPEC_FIELD_AUXILIARY_FEED_TIMEFRAME;
    case V::AuxiliaryFeedBars: return PF_NATIVE_SPEC_FIELD_AUXILIARY_FEED_BARS;
    case V::SubscriptionSource: return PF_NATIVE_SPEC_FIELD_SUBSCRIPTION_SOURCE;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(pineforge::NativeAuxiliaryAppendError value) noexcept {
    using V = pineforge::NativeAuxiliaryAppendError;
    switch (value) {
    case V::None: return PF_NATIVE_APPEND_ERROR_NONE;
    case V::HostFailed: return PF_NATIVE_APPEND_ERROR_HOST_FAILED;
    case V::Reentrant: return PF_NATIVE_APPEND_ERROR_REENTRANT;
    case V::NotRealtime: return PF_NATIVE_APPEND_ERROR_NOT_REALTIME;
    case V::NoAuxiliaryFeed: return PF_NATIVE_APPEND_ERROR_NO_AUXILIARY_FEED;
    case V::InvalidBarArray: return PF_NATIVE_APPEND_ERROR_INVALID_BAR_ARRAY;
    case V::InvalidBar: return PF_NATIVE_APPEND_ERROR_INVALID_BAR;
    case V::UnorderedBars: return PF_NATIVE_APPEND_ERROR_UNORDERED_BARS;
    case V::InputPeriodAlreadyAccepted: return PF_NATIVE_APPEND_ERROR_INPUT_PERIOD_ALREADY_ACCEPTED;
    case V::AllocationFailure: return PF_NATIVE_APPEND_ERROR_ALLOCATION_FAILURE;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(no::DriverEligibilityClass value) noexcept {
    using V = no::DriverEligibilityClass;
    switch (value) {
    case V::ObservedPrint: return PF_NATIVE_DRIVER_CLASS_OBSERVED_PRINT;
    case V::CarriedOpen: return PF_NATIVE_DRIVER_CLASS_CARRIED_OPEN;
    case V::TickAfterCalculation: return PF_NATIVE_DRIVER_CLASS_TICK_AFTER_CALCULATION;
    case V::ConfirmedOpen: return PF_NATIVE_DRIVER_CLASS_CONFIRMED_OPEN;
    case V::ConfirmedExcursion: return PF_NATIVE_DRIVER_CLASS_CONFIRMED_EXCURSION;
    case V::ConfirmedAfterCalculationClose:
        return PF_NATIVE_DRIVER_CLASS_CONFIRMED_AFTER_CALCULATION_CLOSE;
    case V::CurrentExecution: return PF_NATIVE_DRIVER_CLASS_CURRENT_EXECUTION;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(no::NativeCandidatePriceKind value) noexcept {
    using V = no::NativeCandidatePriceKind;
    switch (value) {
    case V::PointPrice: return PF_NATIVE_CANDIDATE_PRICE_POINT_PRICE;
    case V::TriggerLevel: return PF_NATIVE_CANDIDATE_PRICE_TRIGGER_LEVEL;
    case V::CurrentQuote: return PF_NATIVE_CANDIDATE_PRICE_CURRENT_QUOTE;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(pineforge::NativeAnchoredTrigger value) noexcept {
    using V = pineforge::NativeAnchoredTrigger;
    switch (value) {
    case V::Limit: return PF_NATIVE_ANCHORED_TRIGGER_LIMIT;
    case V::Stop: return PF_NATIVE_ANCHORED_TRIGGER_STOP;
    case V::TrailArm: return PF_NATIVE_ANCHORED_TRIGGER_TRAIL_ARM;
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t c_word(pineforge::NativeCurrentPriceRule value) noexcept {
    using V = pineforge::NativeCurrentPriceRule;
    switch (value) {
    case V::AsPresented: return PF_NATIVE_PRICE_AS_PRESENTED;
    case V::NearestTick: return PF_NATIVE_PRICE_NEAREST_TICK;
    }
    return static_cast<std::uint32_t>(value);
}

/* The narrow words of the decision, event and margin-view PODs. */
constexpr std::uint8_t c_byte(std::uint32_t word) noexcept {
    return static_cast<std::uint8_t>(word);
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/* A C callback said "stop". The consumer's existing callback guard turns any
 * std::exception into NativeFailureCode::CallbackException, so this is how a
 * non-zero C return reaches the documented failure without the C frame ever
 * unwinding. */
class CallbackFailure : public std::runtime_error {
public:
    explicit CallbackFailure(const char* hook)
        : std::runtime_error(std::string("native C callback refused in ") + hook) {}
};

const pf_bar_t* as_c_bar(const Bar& bar) {
    return reinterpret_cast<const pf_bar_t*>(&bar);
}

/* Marks the frame of one C `on_applied` call on its host, and restores the
 * outer mark on every exit, the CallbackFailure throw included — so a nested
 * applied execution cannot close the frame it runs inside. */
class AppliedFrame {
public:
    explicit AppliedFrame(bool& inside) : inside_(inside), outer_(inside) { inside_ = true; }
    ~AppliedFrame() { inside_ = outer_; }
    AppliedFrame(const AppliedFrame&) = delete;
    AppliedFrame& operator=(const AppliedFrame&) = delete;

private:
    bool& inside_;
    bool outer_;
};

/* Defined below, beside the other C++ value -> C POD translations; declared
 * here because the recalculation hook flattens an applied cause. */
pf_native_applied_v1 applied_pod(const no::ExecutionAppliedEvent& applied);
/* Defined below with the working-row readout; the policy hooks' views name a
 * request's intent and trigger the same way. */
std::uint32_t working_intent_tag(const no::OrderIntent& intent, double& value);
std::uint32_t working_trigger_tag(const no::Trigger& trigger, double& p1, double& p2);

/* ── The host ───────────────────────────────────────────────────── */

class CCallbackHost final : public NativeStrategyHost {
public:
    explicit CCallbackHost(const pf_native_callbacks_v1& table) : table_(table) {}

    const pf_native_callbacks_v1& table() const noexcept { return table_; }

    std::vector<pineforge::NativeWorkingRequest>& working_cache() noexcept {
        return working_cache_;
    }

    /* N18: the open-lot snapshot the copy-out reads from. Host-side only,
     * like working_cache_: never durable engine state, never hashed. */
    std::vector<pineforge::NativeOpenLot>& open_lot_cache() noexcept {
        return open_lot_cache_;
    }

    /* The C spelling of the protected kernel seam
     * BacktestEngine::declare_opened_lot_entry_bar_mask, legal where a host
     * learns that a fill opened a lot: inside the C `on_applied` alone. Any
     * other frame answers false and touches nothing. */
    bool declare_entry_bar_mask(std::uint64_t entry_incarnation, const Bar& entry_bar,
                                pineforge::OpenedLotFillPoint fill_point) {
        if (!in_applied_) return false;
        declare_opened_lot_entry_bar_mask(entry_incarnation, entry_bar, fill_point);
        return true;
    }

    pf_native_decision_v1 decision(const pineforge::NativeDecisionContext& ctx) const;

private:
    void on_native_run_begin() override {
        if (!table_.on_run_begin) return;
        if (table_.on_run_begin(table_.user) != 0) throw CallbackFailure("on_run_begin");
    }

    void on_native_input(const Bar& bar, const pineforge::NativeInputContext& ctx) override {
        if (!table_.on_input) return;
        if (table_.on_input(table_.user, as_c_bar(bar), ctx.input_index,
                            ctx.completes_script_interval ? 1 : 0) != 0) {
            throw CallbackFailure("on_input");
        }
    }

    void on_native_bar_open(const Bar& bar,
                            const pineforge::NativeDecisionContext& ctx) override {
        if (!table_.on_bar_open) return;
        const pf_native_decision_v1 at = decision(ctx);
        if (table_.on_bar_open(table_.user, as_c_bar(bar), &at) != 0) {
            throw CallbackFailure("on_bar_open");
        }
    }

    void on_native_bar(const Bar& bar, const pineforge::NativeDecisionContext& ctx) override {
        if (!table_.on_bar) return;
        const pf_native_decision_v1 at = decision(ctx);
        if (table_.on_bar(table_.user, as_c_bar(bar), &at) != 0) {
            throw CallbackFailure("on_bar");
        }
    }

    void on_native_tick(const Bar& bar, const pineforge::NativeTickContext& ctx) override {
        if (!table_.on_tick) return;
        const pf_native_decision_v1 at = decision(ctx.decision);
        if (table_.on_tick(table_.user, as_c_bar(bar), &at) != 0) {
            throw CallbackFailure("on_tick");
        }
    }

    void on_native_applied(const no::ExecutionAppliedEvent& applied,
                           const pineforge::NativeDecisionContext& ctx) override;

    void on_native_timeframe_bar(const Bar& bar,
                                 const pineforge::NativeTimeframeBarContext& ctx) override {
        if (!table_.on_timeframe_bar) return;
        if (table_.on_timeframe_bar(table_.user, as_c_bar(bar),
                                    static_cast<std::uint32_t>(ctx.subscription),
                                    c_word(ctx.completion),
                                    ctx.delivered_at_ms) != 0) {
            throw CallbackFailure("on_timeframe_bar");
        }
    }

    void on_native_margin_call(const no::MarginCallEvent& call) override;

    void on_native_recalculate(const Bar& bar, const pineforge::NativeDecisionContext& ctx,
                               pineforge::NativeCalculationReason reason,
                               const no::ExecutionAppliedEvent* cause) override {
        /* No hook installed is the established contract: the base forwards
         * every calculation to on_native_bar, which is table_.on_bar. */
        if (!table_.on_recalculate) {
            NativeStrategyHost::on_native_recalculate(bar, ctx, reason, cause);
            return;
        }
        const pf_native_decision_v1 at = decision(ctx);
        pf_native_applied_v1 cause_pod;
        const pf_native_applied_v1* cause_ptr = nullptr;
        if (cause) {
            cause_pod = applied_pod(*cause);
            cause_ptr = &cause_pod;
        }
        if (table_.on_recalculate(table_.user, as_c_bar(bar), &at,
                                  c_word(reason), cause_ptr) != 0) {
            throw CallbackFailure("on_recalculate");
        }
    }

    void on_native_sub_bar(const Bar& sub,
                           const pineforge::NativeDecisionContext& ctx) override {
        if (!table_.on_sub_bar) return;
        const pf_native_decision_v1 at = decision(ctx);
        if (table_.on_sub_bar(table_.user, as_c_bar(sub), &at) != 0) {
            throw CallbackFailure("on_sub_bar");
        }
    }

    /* The four answering hooks. They are consulted from kernel paths outside
     * the callback guard, so none of them can throw: the return value selects
     * whose answer is used and every value is in contract. */
    std::optional<pineforge::NativeMarginDecision> resolve_margin_requirement(
            const pineforge::NativeMarginRequirementView& view) const override {
        if (!table_.on_margin_requirement) return std::nullopt;
        pf_native_margin_view_v1 pod = margin_view_pod(view.kind, view.position, view.mark,
                                                       view.equity, view.required,
                                                       view.cursor, false);
        pf_native_margin_decision_v1 answer;
        std::memset(&answer, 0, sizeof(answer));
        answer.struct_size = static_cast<std::uint32_t>(sizeof(answer));
        answer.version = PF_NATIVE_API_VERSION;
        answer.required = view.required;
        answer.equity = view.equity;
        if (table_.on_margin_requirement(table_.user, &pod, &answer)
            == PF_NATIVE_ANSWER_DEFAULT) {
            return std::nullopt;
        }
        pineforge::NativeMarginDecision decision_out;
        decision_out.required = answer.required;
        decision_out.equity = answer.equity;
        decision_out.force_breach = answer.force_breach != 0u;
        return decision_out;
    }

    bool margin_check_allowed(const pineforge::NativeMarginCheckPoint& point) const override {
        if (!table_.on_margin_check) return true;
        pf_native_margin_view_v1 pod = margin_view_pod(point.kind, point.position, point.mark,
                                                       0.0, 0.0, point.cursor,
                                                       point.liquidation_resting);
        std::int32_t allowed = 1;
        if (table_.on_margin_check(table_.user, &pod, &allowed) == PF_NATIVE_ANSWER_DEFAULT) {
            return true;
        }
        return allowed != 0;
    }

    std::optional<double> resolve_margin_call_units(
            const pineforge::NativeMarginCallView& view) const override {
        if (!table_.on_margin_call_units) return std::nullopt;
        pf_native_margin_view_v1 pod = margin_view_pod(
            pineforge::NativeMarginCheckKind::BarOpen, view.position, view.mark, view.equity,
            view.required, view.cursor, false);
        /* A call is not a check point: neither field is a fact here. */
        pod.kind = 0;
        double units = 0.0;
        if (table_.on_margin_call_units(table_.user, &pod, &units)
            == PF_NATIVE_ANSWER_DEFAULT) {
            return std::nullopt;
        }
        return units;
    }

    no::ExecutionTerms resolve_execution_terms(
            const pineforge::NativeExecutionTermsFacts& facts) const override {
        no::ExecutionTerms terms{facts.default_resolved_price, std::nullopt,
                                 no::OpeningShape::Transact};
        /* The price half first, the units half after it: the C++ hook
         * answers both at once, and so does this pair. */
        if (table_.on_execution_terms && !answer_execution_terms(facts, terms)) return terms;
        if (!table_.on_close_units) return terms;
        if (!facts.definition
            || !std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            return terms;
        }
        pf_native_close_view_v1 view;
        std::memset(&view, 0, sizeof(view));
        view.struct_size = static_cast<std::uint32_t>(sizeof(view));
        view.version = PF_NATIVE_API_VERSION;
        view.incarnation = facts.target.incarnation;
        view.scope_exposure_units = facts.scope_exposure_units;
        view.default_resolved_price = facts.default_resolved_price;
        view.position_units = facts.position.signed_units;
        view.cursor_ordinal = facts.cursor.point.ordinal;
        view.cursor_effective_time_ms = facts.cursor.point.effective_time_ms;
        view.is_buy = facts.is_buy ? 1u : 0u;
        double units = 0.0;
        if (table_.on_close_units(table_.user, &view, &units) == PF_NATIVE_ANSWER_DEFAULT) {
            return terms;
        }
        terms.units = units;
        return terms;
    }

    /* on_execution_terms' answer into `terms`; false when the answer names
     * a word the kernel cannot read, in which case `terms` carries non-finite
     * units -- the kernel's own InvalidTerms for every candidate. */
    bool answer_execution_terms(const pineforge::NativeExecutionTermsFacts& facts,
                                no::ExecutionTerms& terms) const;

    pineforge::NativePrecommitVerdict validate_execution_precommit(
            const pineforge::NativePrecommitView& view) const override;

    std::optional<double> resolve_anchored_level(
            const pineforge::NativeAnchoredLevelView& view) const override;

    /* The kernel's own bytes, then -- when the host answers -- its digest
     * under a domain tag of this transport's own. A host that installs no
     * hook, or answers DEFAULT, folds exactly what it always folded. */
    void hash_host_extension(pineforge::BrokerStateHashSink& sink) const override {
        BacktestEngine::hash_host_extension(sink);
        if (!table_.on_hash_extension) return;
        std::uint64_t digest = 0;
        if (table_.on_hash_extension(table_.user, &digest) == PF_NATIVE_ANSWER_DEFAULT) return;
        sink.s("native-c-host:extension");
        sink.u(digest);
    }

    bool owns_lot_excursions() const noexcept override {
        return table_.on_lot_excursion != nullptr;
    }

    pineforge::ClosedLotExcursion closed_lot_excursion(
            const pineforge::ClosedLotExcursionFacts& facts) const override {
        if (!table_.on_lot_excursion) return {};
        pf_native_lot_excursion_v1 pod;
        std::memset(&pod, 0, sizeof(pod));
        pod.struct_size = static_cast<std::uint32_t>(sizeof(pod));
        pod.version = PF_NATIVE_API_VERSION;
        pod.entry_incarnation = facts.entry_incarnation;
        pod.entry_time_ms = facts.entry_time_ms;
        pod.entry_price = facts.entry_price;
        pod.lot_qty = facts.lot_qty;
        pod.closed_qty = facts.closed_qty;
        pod.fill_price = facts.fill_price;
        pod.carried_favorable = facts.carried_favorable;
        pod.carried_adverse = facts.carried_adverse;
        pod.entry_bar_index = facts.entry_bar_index;
        pod.exit_bar_index = facts.exit_bar_index;
        pod.is_long = facts.is_long ? 1u : 0u;
        pod.entry_bar_high_masked = facts.entry_bar_high_masked ? 1u : 0u;
        pod.entry_bar_low_masked = facts.entry_bar_low_masked ? 1u : 0u;
        double favorable = 0.0;
        double adverse = 0.0;
        if (table_.on_lot_excursion(table_.user, &pod, &favorable, &adverse)
            == PF_NATIVE_ANSWER_DEFAULT) {
            return {};
        }
        return pineforge::ClosedLotExcursion{favorable, adverse};
    }

    static pf_native_margin_view_v1 margin_view_pod(
            pineforge::NativeMarginCheckKind kind,
            const pineforge::NativePhysicalPosition& position, double mark, double equity,
            double required, const no::MatchCursor& cursor, bool liquidation_resting) {
        pf_native_margin_view_v1 out;
        std::memset(&out, 0, sizeof(out));
        out.struct_size = static_cast<std::uint32_t>(sizeof(out));
        out.version = PF_NATIVE_API_VERSION;
        out.kind = c_word(kind);
        out.liquidation_resting = liquidation_resting ? 1u : 0u;
        out.signed_units = position.signed_units;
        out.average_price = position.average_price;
        out.lot_count = static_cast<std::uint64_t>(position.lot_count);
        out.mark = mark;
        out.equity = equity;
        out.required = required;
        out.cursor_ordinal = cursor.point.ordinal;
        out.cursor_effective_time_ms = cursor.point.effective_time_ms;
        out.cursor_t = cursor.t;
        out.cursor_interval_index = cursor.point.interval_index;
        out.cursor_provenance = c_byte(c_word(cursor.point.provenance));
        out.cursor_path_phase = c_byte(c_word(cursor.point.path_phase));
        return out;
    }

    pf_native_callbacks_v1 table_{};
    std::vector<pineforge::NativeWorkingRequest> working_cache_;
    std::vector<pineforge::NativeOpenLot> open_lot_cache_;
    /* True while the C `on_applied` runs. Host-side, like the two caches:
     * never durable engine state, never hashed. */
    bool in_applied_ = false;
};

pf_native_decision_v1 CCallbackHost::decision(
        const pineforge::NativeDecisionContext& ctx) const {
    pf_native_decision_v1 out;
    std::memset(&out, 0, sizeof(out));
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.version = PF_NATIVE_API_VERSION;
    out.ordinal = ctx.coordinate.ordinal;
    out.interval_index = ctx.coordinate.interval_index;
    out.sub_index = ctx.sub_index;
    out.sub_count = ctx.sub_count;
    out.is_terminal_sub_bar = ctx.is_terminal_sub_bar ? 1 : 0;
    out.effective_time_ms = ctx.coordinate.effective_time_ms;
    out.script_bar_open_ms = ctx.script_bar_open_ms;
    out.sub_bar_open_ms = ctx.sub_bar_open_ms;
    out.decision_floor_ms = ctx.decision_floor_ms;
    out.provenance = c_byte(c_word(ctx.coordinate.provenance));
    out.path_phase = c_byte(c_word(ctx.coordinate.path_phase));
    out.completion = c_byte(c_word(ctx.coordinate.completion));
    out.price = kNaN;
    if (const auto point = current_execution_point()) {
        out.price = point->price;
        out.quote_kind = c_byte(c_word(point->quote_kind));
    }
    return out;
}

/* ── C++ value → C POD ──────────────────────────────────────────── */

void fill_cursor(pf_native_event_v1& out, const no::MatchCursor& cursor) {
    out.effective_time_ms = cursor.point.effective_time_ms;
    out.interval_index = cursor.point.interval_index;
    out.provenance = c_byte(c_word(cursor.point.provenance));
    out.path_phase = c_byte(c_word(cursor.point.path_phase));
}

pf_native_event_v1 blank_event(std::uint32_t kind, std::uint64_t ordinal) {
    pf_native_event_v1 out;
    std::memset(&out, 0, sizeof(out));
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.version = PF_NATIVE_API_VERSION;
    out.kind = kind;
    out.ordinal = ordinal;
    return out;
}

pf_native_event_v1 applied_event_pod(const no::ExecutionAppliedEvent& applied) {
    pf_native_event_v1 out = blank_event(PF_NATIVE_EVENT_APPLIED, applied.ordinal);
    out.incarnation = applied.handle().incarnation;
    out.raw_price = applied.raw_price;
    out.resolved_price = applied.resolved_price;
    out.price = applied.resolved_price;
    out.closed_units = applied.closed_units;
    out.opened_units = applied.opened_units;
    out.cycle_before = applied.cycle_before;
    out.cycle_after = applied.cycle_after;
    out.terminal = applied.terminal ? 1 : 0;
    if (applied.terminal_reason) out.reason = c_word(*applied.terminal_reason);
    fill_cursor(out, applied.cursor);
    return out;
}

pf_native_event_v1 margin_call_pod(const no::MarginCallEvent& call) {
    pf_native_event_v1 out = blank_event(PF_NATIVE_EVENT_MARGIN_CALL, call.ordinal);
    out.incarnation = call.handle().incarnation;
    out.reason = c_word(call.side);
    out.price = call.mark;
    out.resolved_price = call.mark;
    out.raw_price = call.liquidation_price;
    out.closed_units = call.units;
    out.opened_units = call.position_after;
    out.cycle_before = 0;
    out.cycle_after = 0;
    fill_cursor(out, call.cursor);
    return out;
}

pf_native_applied_v1 applied_pod(const no::ExecutionAppliedEvent& applied) {
    pf_native_applied_v1 out;
    std::memset(&out, 0, sizeof(out));
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.version = PF_NATIVE_API_VERSION;
    out.ordinal = applied.ordinal;
    out.incarnation = applied.handle().incarnation;
    out.opened_lot_incarnation = applied.opened_lot_incarnation;
    out.raw_price = applied.raw_price;
    out.resolved_price = applied.resolved_price;
    out.closed_units = applied.closed_units;
    out.opened_units = applied.opened_units;
    out.filled_working = applied.filled_working;
    out.ticket = applied.current_ticket;
    out.cycle_before = applied.cycle_before;
    out.cycle_after = applied.cycle_after;
    out.terminal = applied.terminal ? 1 : 0;
    if (applied.terminal_reason) {
        out.has_terminal_reason = 1;
        out.terminal_reason = c_byte(c_word(*applied.terminal_reason));
    }
    return out;
}

/* The whole MarginCallEvent: every margin fact of the liquidation fill. */
pf_native_margin_call_v1 margin_call_economics(const no::MarginCallEvent& call) {
    pf_native_margin_call_v1 out;
    std::memset(&out, 0, sizeof(out));
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.version = PF_NATIVE_API_VERSION;
    out.ordinal = call.ordinal;
    out.incarnation = call.handle().incarnation;
    out.applied_ordinal = call.applied.ordinal;
    out.side = c_word(call.side);
    out.mark = call.mark;
    out.equity = call.equity;
    out.required = call.required;
    out.liquidation_price = call.liquidation_price;
    out.units = call.units;
    out.position_before = call.position_before;
    out.position_after = call.position_after;
    out.cursor_ordinal = call.cursor.point.ordinal;
    out.cursor_effective_time_ms = call.cursor.point.effective_time_ms;
    out.cursor_t = call.cursor.t;
    out.cursor_interval_index = call.cursor.point.interval_index;
    out.cursor_provenance = c_byte(c_word(call.cursor.point.provenance));
    out.cursor_path_phase = c_byte(c_word(call.cursor.point.path_phase));
    return out;
}

void CCallbackHost::on_native_applied(const no::ExecutionAppliedEvent& applied,
                                      const pineforge::NativeDecisionContext& ctx) {
    if (!table_.on_applied) return;
    const pf_native_applied_v1 pod = applied_pod(applied);
    const pf_native_decision_v1 at = decision(ctx);
    const AppliedFrame frame(in_applied_);
    if (table_.on_applied(table_.user, &pod, &at) != 0) throw CallbackFailure("on_applied");
}

void CCallbackHost::on_native_margin_call(const no::MarginCallEvent& call) {
    if (!table_.on_margin_call) return;
    const pf_native_event_v1 pod = margin_call_pod(call);
    if (table_.on_margin_call(table_.user, &pod) != 0) throw CallbackFailure("on_margin_call");
}

/* One recorded event, flattened into the tagged POD. Every alternative is
 * named: a new CommandEvent alternative fails the static_assert above and
 * then this visit. */
bool translate_event(const pineforge::NativeMarketEvent& event, pf_native_event_v1& out) {
    switch (event.kind) {
    case pineforge::NativeEventKind::Driver: {
        if (!event.driver) return false;
        out = blank_event(PF_NATIVE_EVENT_DRIVER_POINT, event.ordinal);
        out.raw_price = event.driver->raw_price;
        out.price = event.driver->raw_price;
        out.effective_time_ms = event.driver->coordinate.effective_time_ms;
        out.interval_index = event.driver->coordinate.interval_index;
        out.provenance = c_byte(c_word(event.driver->coordinate.provenance));
        out.path_phase = c_byte(c_word(event.driver->coordinate.path_phase));
        return true;
    }
    case pineforge::NativeEventKind::Account: {
        if (!event.account) return false;
        out = blank_event(PF_NATIVE_EVENT_ACCOUNT, event.ordinal);
        out.price = event.account->marked_equity;
        out.raw_price = event.account->realized_balance;
        out.opened_units = event.account->signed_units;
        out.effective_time_ms = event.account->effective_time_ms;
        return true;
    }
    case pineforge::NativeEventKind::Command:
        break;
    }
    if (!event.command) return false;
    const std::uint64_t ordinal = event.ordinal;
    return std::visit([&](const auto& payload) -> bool {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, no::AcceptedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_ACCEPTED, ordinal);
            out.incarnation = payload.handle().incarnation;
        } else if constexpr (std::is_same_v<T, no::RejectedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_REJECTED, ordinal);
            out.reason = c_word(payload.reason);
        } else if constexpr (std::is_same_v<T, no::ReplacedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_REPLACED, ordinal);
            out.incarnation = payload.predecessor().incarnation;
            out.successor = payload.successor().incarnation;
        } else if constexpr (std::is_same_v<T, no::ReplaceRejectedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_REPLACE_REJECTED, ordinal);
            out.incarnation = payload.target().incarnation;
            out.reason = c_word(payload.reason);
        } else if constexpr (std::is_same_v<T, no::CancelledEvent>) {
            out = blank_event(PF_NATIVE_EVENT_CANCELLED, ordinal);
            out.incarnation = payload.handle().incarnation;
            out.reason = c_word(payload.reason);
        } else if constexpr (std::is_same_v<T, no::NotWorkingEvent>) {
            out = blank_event(PF_NATIVE_EVENT_NOT_WORKING, ordinal);
            out.incarnation = payload.target.incarnation;
        } else if constexpr (std::is_same_v<T, no::InvalidHandleEvent>) {
            out = blank_event(PF_NATIVE_EVENT_INVALID_HANDLE, ordinal);
            out.incarnation = payload.target.incarnation;
        } else if constexpr (std::is_same_v<T, no::NoEffectEvent>) {
            out = blank_event(PF_NATIVE_EVENT_NO_EFFECT, ordinal);
            out.incarnation = payload.handle().incarnation;
            fill_cursor(out, payload.cursor);
        } else if constexpr (std::is_same_v<T, no::MatchRejectedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_MATCH_REJECTED, ordinal);
            out.incarnation = payload.handle().incarnation;
            out.reason = c_word(payload.reason);
            fill_cursor(out, payload.cursor);
        } else if constexpr (std::is_same_v<T, no::ExecutionAppliedEvent>) {
            out = applied_event_pod(payload);
        } else if constexpr (std::is_same_v<T, no::CloseBoundEvent>) {
            out = blank_event(PF_NATIVE_EVENT_CLOSE_BOUND, ordinal);
            out.incarnation = payload.definition->handle.incarnation;
            out.reason = c_word(payload.side);
            out.cycle_after = payload.cycle;
            fill_cursor(out, payload.cursor);
        } else if constexpr (std::is_same_v<T, no::ActivatedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_ACTIVATED, ordinal);
            out.incarnation = payload.definition->handle.incarnation;
            out.reason = c_word(payload.kind);
            out.price = payload.reached_price;
            out.raw_price = payload.reached_price;
            fill_cursor(out, payload.cursor);
        } else if constexpr (std::is_same_v<T, no::ReservationReducedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_RESERVATION_REDUCED, ordinal);
            out.incarnation = payload.recipient.incarnation;
            out.reason = c_word(payload.effect);
            out.closed_units = payload.actual_deduction;
        } else if constexpr (std::is_same_v<T, no::DeferredGroupAdjustmentEvent>) {
            out = blank_event(PF_NATIVE_EVENT_DEFERRED_GROUP, ordinal);
            out.incarnation = payload.recipient.incarnation;
            out.reason = c_word(payload.effect);
            out.closed_units = payload.deferred_delta;
        } else if constexpr (std::is_same_v<T, no::QuantityBoundEvent>) {
            out = blank_event(PF_NATIVE_EVENT_QUANTITY_BOUND, ordinal);
            out.incarnation = payload.definition->handle.incarnation;
            out.opened_units = payload.source_units;
        } else if constexpr (std::is_same_v<T, no::ArmedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_ARMED, ordinal);
            out.incarnation = payload.definition->handle.incarnation;
        } else if constexpr (std::is_same_v<T, no::TermsResolvedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_TERMS_RESOLVED, ordinal);
            out.incarnation = payload.handle().incarnation;
            out.raw_price = payload.input.raw_price;
            out.resolved_price = payload.input.terms.resolved_price;
            out.price = payload.input.terms.resolved_price;
            fill_cursor(out, payload.cursor);
        } else if constexpr (std::is_same_v<T, no::MarginCallEvent>) {
            out = margin_call_pod(payload);
        } else if constexpr (std::is_same_v<T, no::NativeRiskEvent>) {
            /* An account fact, bound to no request: `incarnation` stays 0 and
             * the matching point travels in `successor` instead. `limit` is
             * already in the unit the breach was measured in — a percent
             * limit was resolved against its basis equity in the kernel. */
            out = blank_event(PF_NATIVE_EVENT_RISK, ordinal);
            out.reason = c_word(payload.kind);
            out.price = payload.observed;
            out.raw_price = payload.limit;
            out.cycle_before = payload.day_ordinal;
            out.successor = payload.cursor.point.ordinal;
            fill_cursor(out, payload.cursor);
        } else {
            static_assert(!sizeof(T), "untranslated native command event");
        }
        return true;
    }, *event.command);
}

/* ── C POD → C++ value ──────────────────────────────────────────── */

int translate_intent(const pf_native_request_v1& in, bool has_sizing_tail,
                     bool has_arm_tail, no::OrderIntent& out) {
    switch (in.intent) {
    case PF_NATIVE_INTENT_FLATTEN:
        out = no::Flatten{};
        return PF_NATIVE_OK;
    case PF_NATIVE_INTENT_TRANSACT:
        out = no::Transact{in.intent_value};
        return PF_NATIVE_OK;
    case PF_NATIVE_INTENT_REVERSE_TO:
        out = no::ReverseTo{in.intent_value};
        return PF_NATIVE_OK;
    case PF_NATIVE_INTENT_HOST_SIZED:
        /* HostSized is the adapter's sizing seam and a C host sizes with
         * SIZED -- with two exceptions the kernel leaves no other spelling
         * for, both a CLOSE whose units on_close_units answers. The cohort
         * close: native_order.cpp accepts BindCohort only for
         * HostSized{Close}, the roster's own live openings are the target
         * and the cohort authority decides the units. And a WAIT_FOR_APPLIED
         * child carrying the arm tail: the kernel admits a waiting
         * HostSized{Close} under NativeArmScope::Book alone and refuses the
         * owner-lot relation with its own typed InvalidOwner, so that choice
         * is left to it. Any other owner stays unsupported rather than being
         * accepted and then rejected at the candidate. */
        if (in.owner != PF_NATIVE_OWNER_BIND_COHORT
            && !(has_arm_tail && in.owner == PF_NATIVE_OWNER_WAIT_FOR_APPLIED)) {
            return PF_NATIVE_E_UNSUPPORTED;
        }
        out = no::HostSized{no::HostSizedKind::Close, std::nullopt};
        return PF_NATIVE_OK;
    case PF_NATIVE_INTENT_REDUCE: {
        no::Reduce reduce{no::ExplicitUnits{in.intent_value}};
        switch (in.reduce_size) {
        case PF_NATIVE_REDUCE_EXPLICIT_UNITS:
            reduce.size = no::ExplicitUnits{in.intent_value};
            break;
        case PF_NATIVE_REDUCE_OWNER_OPENED:
            reduce.size = no::OwnerOpenedUnits{};
            break;
        case PF_NATIVE_REDUCE_SCOPE_FRACTION: {
            no::ScopeFraction fraction;
            fraction.fraction = in.intent_value;
            switch (in.reduce_claim) {
            case PF_NATIVE_SCOPE_GROSS: fraction.claim = no::ScopeClaim::Gross; break;
            case PF_NATIVE_SCOPE_NET_OF_SIBLINGS:
                fraction.claim = no::ScopeClaim::NetOfSiblings;
                break;
            default: return PF_NATIVE_E_TAG;
            }
            if (has_sizing_tail) {
                switch (in.reduce_basis) {
                case PF_NATIVE_SCOPE_BASIS_AT_MATCH:
                    fraction.basis = no::ScopeBasis::AtMatch;
                    break;
                case PF_NATIVE_SCOPE_BASIS_AT_ACCEPTANCE:
                    fraction.basis = no::ScopeBasis::AtAcceptance;
                    break;
                default: return PF_NATIVE_E_TAG;
                }
            }
            reduce.size = fraction;
            break;
        }
        default: return PF_NATIVE_E_TAG;
        }
        out = reduce;
        return PF_NATIVE_OK;
    }
    case PF_NATIVE_INTENT_SIZED: {
        no::Sized sized;
        switch (in.side) {
        case PF_NATIVE_SIDE_LONG: sized.side = no::Side::Long; break;
        case PF_NATIVE_SIDE_SHORT: sized.side = no::Side::Short; break;
        default: return PF_NATIVE_E_TAG;
        }
        switch (in.size_basis) {
        case PF_NATIVE_SIZE_BASIS_CASH: sized.basis = no::CashValue{in.intent_value}; break;
        case PF_NATIVE_SIZE_BASIS_EQUITY_FRACTION:
            sized.basis = no::EquityFraction{in.intent_value};
            break;
        default: return PF_NATIVE_E_TAG;
        }
        switch (in.size_time) {
        case PF_NATIVE_SIZE_AT_MATCH: sized.time = no::SizeTime::AtMatch; break;
        case PF_NATIVE_SIZE_AT_ACCEPTANCE: sized.time = no::SizeTime::AtAcceptance; break;
        default: return PF_NATIVE_E_TAG;
        }
        switch (in.grid_policy) {
        case PF_NATIVE_GRID_SNAP:
            sized.grid_policy = no::ExecutionGridPolicy::SnapToGrid;
            break;
        case PF_NATIVE_GRID_EXPLICIT_UNITS:
            sized.grid_policy = no::ExecutionGridPolicy::ExplicitUnits;
            break;
        default: return PF_NATIVE_E_TAG;
        }
        if (has_sizing_tail) {
            switch (in.size_price) {
            case PF_NATIVE_SIZE_PRICE_RESOLVED: sized.price = no::SizePrice::Resolved; break;
            case PF_NATIVE_SIZE_PRICE_SIGNAL: sized.price = no::SizePrice::Signal; break;
            case PF_NATIVE_SIZE_PRICE_SIGNAL_ON_TICK:
                sized.price = no::SizePrice::SignalOnTick;
                break;
            default: return PF_NATIVE_E_TAG;
            }
        }
        if (in.reserve_percent_fee > 1u) return PF_NATIVE_E_TAG;
        sized.reserve_percent_fee = in.reserve_percent_fee != 0u;
        out = sized;
        return PF_NATIVE_OK;
    }
    default:
        return PF_NATIVE_E_TAG;
    }
}

int translate_trigger(const pf_native_request_v1& in, bool has_trail_seed_tail,
                      no::Trigger& out) {
    if (in.fill_through > 1u || in.trail_offset_in_ticks > 1u
        || in.trail_has_arm_price > 1u
        || (has_trail_seed_tail && in.trail_has_best_seed > 1u)) {
        return PF_NATIVE_E_TAG;
    }
    switch (in.trigger) {
    case PF_NATIVE_TRIGGER_MARKET:
        out = no::Market{};
        return PF_NATIVE_OK;
    case PF_NATIVE_TRIGGER_LIMIT:
        out = no::Limit{in.p1, in.fill_through != 0u};
        return PF_NATIVE_OK;
    case PF_NATIVE_TRIGGER_STOP:
        out = no::Stop{in.p1};
        return PF_NATIVE_OK;
    case PF_NATIVE_TRIGGER_STOP_LIMIT:
        out = no::StopLimit{in.p1, in.p2};
        return PF_NATIVE_OK;
    case PF_NATIVE_TRIGGER_TRAIL: {
        no::Trail trail;
        if (in.trail_offset_in_ticks != 0u) {
            trail.offset = 0.0;
            trail.ticks = no::TrailTicks{in.p1};
        } else {
            trail.offset = in.p1;
        }
        if (in.trail_has_arm_price != 0u) trail.arm_price = in.p2;
        /* The seed is the last tail: an earlier caller's trail has none, and
         * rides from its arm print exactly as it did before the field. */
        if (has_trail_seed_tail && in.trail_has_best_seed != 0u) {
            trail.best_seed = in.trail_best_seed;
        }
        out = trail;
        return PF_NATIVE_OK;
    }
    default:
        return PF_NATIVE_E_TAG;
    }
}

int translate_owner(const pf_native_request_v1& in, const no::RunIdentity& run,
                    no::Owner& out) {
    const auto handle_at = [&](std::uint32_t index) {
        return no::RequestHandle{run, in.owner_incarnations[index]};
    };
    switch (in.owner) {
    case PF_NATIVE_OWNER_INDEPENDENT:
        out = no::Independent{};
        return PF_NATIVE_OK;
    case PF_NATIVE_OWNER_WAIT_FOR_APPLIED:
        if (!in.owner_incarnations || in.owner_n != 1u) return PF_NATIVE_E_ARGUMENT;
        out = no::WaitForApplied{handle_at(0)};
        return PF_NATIVE_OK;
    case PF_NATIVE_OWNER_BIND_OPENING:
        if (!in.owner_incarnations || in.owner_n != 1u) return PF_NATIVE_E_ARGUMENT;
        out = no::BindOpening{handle_at(0), in.owner_cycle};
        return PF_NATIVE_OK;
    case PF_NATIVE_OWNER_BIND_OPENINGS: {
        if (!in.owner_incarnations || in.owner_n == 0u) return PF_NATIVE_E_ARGUMENT;
        no::BindOpenings openings;
        openings.openings.reserve(in.owner_n);
        for (std::uint32_t i = 0; i < in.owner_n; ++i) openings.openings.push_back(handle_at(i));
        openings.cycle = in.owner_cycle;
        out = openings;
        return PF_NATIVE_OK;
    }
    case PF_NATIVE_OWNER_BIND_COHORT:
        if (in.cohort == 0u) return PF_NATIVE_E_ARGUMENT;
        out = no::BindCohort{no::CohortHandle{in.cohort}};
        return PF_NATIVE_OK;
    default:
        return PF_NATIVE_E_TAG;
    }
}

int translate_request(const pf_native_request_v1& in, const no::RunIdentity& run,
                      no::Request& out) {
    /* Five published layouts: the base one the L13 lane first shipped, that
     * plus L7b's anchored-leg tail, that plus L3b's sizing detail, that plus
     * E14's trail seed, and the current one with the arm relation. An
     * earlier caller's later tails are never read; it gets their defaults. */
    const bool has_arm_tail = in.struct_size == sizeof(pf_native_request_v1);
    const bool has_trail_seed_tail =
        has_arm_tail || in.struct_size == PF_NATIVE_REQUEST_V1_SEED_SIZE;
    const bool has_sizing_tail =
        has_trail_seed_tail || in.struct_size == PF_NATIVE_REQUEST_V1_SIZING_SIZE;
    const bool has_anchor_tail =
        has_sizing_tail || in.struct_size == PF_NATIVE_REQUEST_V1_ANCHOR_SIZE;
    if ((!has_anchor_tail && in.struct_size != PF_NATIVE_REQUEST_V1_BASE_SIZE)
        || in.version != PF_NATIVE_API_VERSION) {
        return PF_NATIVE_E_STRUCT;
    }
    if (int rc = translate_intent(in, has_sizing_tail, has_arm_tail, out.intent);
        rc != PF_NATIVE_OK) {
        return rc;
    }
    if (int rc = translate_trigger(in, has_trail_seed_tail, out.trigger);
        rc != PF_NATIVE_OK) {
        return rc;
    }
    if (int rc = translate_owner(in, run, out.owner); rc != PF_NATIVE_OK) return rc;

    switch (in.capacity) {
    case PF_NATIVE_CAPACITY_IMMEDIATE: out.capacity = no::ImmediateRemaining{}; break;
    case PF_NATIVE_CAPACITY_POINT_BUDGET:
        out.capacity = no::PointBudget{in.capacity_units};
        break;
    default: return PF_NATIVE_E_TAG;
    }

    switch (in.group_kind) {
    case PF_NATIVE_GROUP_NONE:
        out.group = no::NoGroup{};
        break;
    case PF_NATIVE_GROUP_MEMBER: {
        no::Member member;
        member.group = in.group_id;
        member.cohort = in.group_cohort;
        switch (in.group_effect) {
        case PF_NATIVE_GROUP_CANCEL: member.effect = no::GroupEffect::Cancel; break;
        case PF_NATIVE_GROUP_REDUCE: member.effect = no::GroupEffect::Reduce; break;
        default: return PF_NATIVE_E_TAG;
        }
        out.group = member;
        break;
    }
    default: return PF_NATIVE_E_TAG;
    }

    if (in.anchor_offset_in_ticks > 1u) return PF_NATIVE_E_TAG;
    no::NativeAnchorRounding rounding = no::NativeAnchorRounding::Raw;
    if (has_anchor_tail) {
        /* Visibility belongs to the one owner relation that arms; on any
         * other owner a non-default value is a tag error, not a silent drop. */
        auto* wait = std::get_if<no::WaitForApplied>(&out.owner);
        switch (in.visibility) {
        case PF_NATIVE_ARM_VISIBILITY_WORKING: break;
        case PF_NATIVE_ARM_VISIBILITY_PENDING_UNTIL_ARMED:
            if (!wait) return PF_NATIVE_E_TAG;
            wait->visibility = no::NativeArmVisibility::PendingUntilArmed;
            break;
        default: return PF_NATIVE_E_TAG;
        }
        switch (in.anchor_rounding) {
        case PF_NATIVE_ANCHOR_ROUNDING_RAW: break;
        case PF_NATIVE_ANCHOR_ROUNDING_HALF_UP:
            rounding = no::NativeAnchorRounding::HalfUp;
            break;
        case PF_NATIVE_ANCHOR_ROUNDING_DIRECTIONAL:
            rounding = no::NativeAnchorRounding::Directional;
            break;
        default: return PF_NATIVE_E_TAG;
        }
        if (has_arm_tail) {
            /* The arm relation belongs to the same owner as visibility: a
             * non-default word on any other owner is a tag error, not a
             * silent drop, and the named padding before it must be zero. */
            for (const auto byte : in.reserved2) {
                if (byte != 0u) return PF_NATIVE_E_TAG;
            }
            switch (in.arm_first_match) {
            case PF_NATIVE_ARM_FIRST_MATCH_AT_ARM_PRINT: break;
            case PF_NATIVE_ARM_FIRST_MATCH_AFTER_ARM_PRINT:
                if (!wait) return PF_NATIVE_E_TAG;
                wait->first_match = no::NativeArmFirstMatch::AfterArmPrint;
                break;
            default: return PF_NATIVE_E_TAG;
            }
            switch (in.arm_scope) {
            case PF_NATIVE_ARM_SCOPE_OWNER_LOT: break;
            case PF_NATIVE_ARM_SCOPE_BOOK:
                if (!wait) return PF_NATIVE_E_TAG;
                wait->scope = no::NativeArmScope::Book;
                break;
            default: return PF_NATIVE_E_TAG;
            }
        }
    }
    switch (in.anchor) {
    case PF_NATIVE_ANCHOR_ABSOLUTE:
        if (rounding != no::NativeAnchorRounding::Raw) return PF_NATIVE_E_TAG;
        out.anchor = no::Absolute{};
        break;
    case PF_NATIVE_ANCHOR_FROM_OWNER_FILL: {
        no::FromOwnerFill anchor;
        anchor.offset = in.anchor_offset;
        anchor.ticks = in.anchor_offset_in_ticks != 0u;
        anchor.rounding = rounding;
        out.anchor = anchor;
        break;
    }
    default: return PF_NATIVE_E_TAG;
    }

    out.label = in.label ? std::string(in.label) : std::string();
    out.comment = in.comment ? std::string(in.comment) : std::string();
    return PF_NATIVE_OK;
}

/* ── Handle plumbing ────────────────────────────────────────────── */

CCallbackHost* host_of(pf_strategy_t s) {
    if (!s) return nullptr;
    auto* engine = static_cast<pineforge::BacktestEngine*>(s);
    return dynamic_cast<CCallbackHost*>(engine);
}

/* The staged/running specification's identity, which every RequestHandle a C
 * caller names belongs to. Absent before the first configure. */
const no::RunIdentity* run_identity(const CCallbackHost& host) {
    const auto state = host.native_state();
    return state.spec ? &state.spec->identity : nullptr;
}

std::uint32_t working_intent_tag(const no::OrderIntent& intent, double& value) {
    value = 0.0;
    return std::visit([&](const auto& alternative) -> std::uint32_t {
        using T = std::decay_t<decltype(alternative)>;
        if constexpr (std::is_same_v<T, no::Flatten>) {
            return PF_NATIVE_INTENT_FLATTEN;
        } else if constexpr (std::is_same_v<T, no::Reduce>) {
            if (const auto* units = std::get_if<no::ExplicitUnits>(&alternative.size)) {
                value = units->units;
            } else if (const auto* fraction = std::get_if<no::ScopeFraction>(&alternative.size)) {
                value = fraction->fraction;
            }
            return PF_NATIVE_INTENT_REDUCE;
        } else if constexpr (std::is_same_v<T, no::Transact>) {
            value = alternative.signed_units;
            return PF_NATIVE_INTENT_TRANSACT;
        } else if constexpr (std::is_same_v<T, no::ReverseTo>) {
            value = alternative.signed_units;
            return PF_NATIVE_INTENT_REVERSE_TO;
        } else if constexpr (std::is_same_v<T, no::HostSized>) {
            return PF_NATIVE_INTENT_HOST_SIZED;
        } else {
            if (const auto* cash = std::get_if<no::CashValue>(&alternative.basis)) {
                value = cash->cash;
            } else {
                value = std::get<no::EquityFraction>(alternative.basis).fraction;
            }
            return PF_NATIVE_INTENT_SIZED;
        }
    }, intent);
}

std::uint32_t working_trigger_tag(const no::Trigger& trigger, double& p1, double& p2) {
    p1 = 0.0;
    p2 = 0.0;
    return std::visit([&](const auto& alternative) -> std::uint32_t {
        using T = std::decay_t<decltype(alternative)>;
        if constexpr (std::is_same_v<T, no::Market>) {
            return PF_NATIVE_TRIGGER_MARKET;
        } else if constexpr (std::is_same_v<T, no::Limit>) {
            p1 = alternative.price;
            return PF_NATIVE_TRIGGER_LIMIT;
        } else if constexpr (std::is_same_v<T, no::Stop>) {
            p1 = alternative.price;
            return PF_NATIVE_TRIGGER_STOP;
        } else if constexpr (std::is_same_v<T, no::StopLimit>) {
            p1 = alternative.stop;
            p2 = alternative.limit;
            return PF_NATIVE_TRIGGER_STOP_LIMIT;
        } else {
            p1 = alternative.offset;
            if (alternative.arm_price) p2 = *alternative.arm_price;
            return PF_NATIVE_TRIGGER_TRAIL;
        }
    }, trigger);
}

/* ── The policy hooks' views and answers ────────────────────────── */

void fill_view_cursor(const no::MatchCursor& cursor, std::uint64_t& ordinal,
                      std::int64_t& effective_time_ms, double& t, std::int32_t& interval_index,
                      std::uint8_t& provenance, std::uint8_t& path_phase) {
    ordinal = cursor.point.ordinal;
    effective_time_ms = cursor.point.effective_time_ms;
    t = cursor.t;
    interval_index = cursor.point.interval_index;
    provenance = c_byte(c_word(cursor.point.provenance));
    path_phase = c_byte(c_word(cursor.point.path_phase));
}

pf_native_terms_view_v1 terms_view(const pineforge::NativeExecutionTermsFacts& facts) {
    pf_native_terms_view_v1 out;
    std::memset(&out, 0, sizeof(out));
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.version = PF_NATIVE_API_VERSION;
    out.incarnation = facts.target.incarnation;
    if (facts.definition) {
        double value = 0.0;
        double p1 = 0.0;
        double p2 = 0.0;
        out.intent = working_intent_tag(facts.definition->request.intent, value);
        out.trigger = working_trigger_tag(facts.definition->request.trigger, p1, p2);
    }
    out.trigger_state = static_cast<std::uint32_t>(facts.trigger_state.index());
    out.remaining_kind = static_cast<std::uint32_t>(facts.remaining.index());
    if (const auto* units = std::get_if<no::RemainingUnits>(&facts.remaining)) {
        out.remaining_units = units->q;
    }
    out.driver_class = c_word(facts.driver_class);
    out.price_kind = c_word(facts.price_kind);
    out.quote_kind = c_word(facts.quote_kind);
    out.price_rule = c_word(facts.price_rule);
    out.is_buy = facts.is_buy ? 1u : 0u;
    out.shared_cursor_collision = facts.shared_cursor_collision ? 1u : 0u;
    out.raw_price = facts.raw_price;
    if (facts.trigger_level) {
        out.has_trigger_level = 1u;
        out.trigger_level = *facts.trigger_level;
    }
    out.default_resolved_price = facts.default_resolved_price;
    out.scope_exposure_units = facts.scope_exposure_units;
    out.position_units = facts.position.signed_units;
    out.position_average_price = facts.position.average_price;
    out.position_lot_count = static_cast<std::uint64_t>(facts.position.lot_count);
    out.opposite_book_units = facts.opposite_book_units;
    out.pending_group_deduction = facts.pending_group_deduction;
    out.fx_effective_time_ms = facts.fx_effective_time_ms;
    out.active_fx = facts.active_fx;
    fill_view_cursor(facts.cursor, out.cursor_ordinal, out.cursor_effective_time_ms,
                     out.cursor_t, out.cursor_interval_index, out.cursor_provenance,
                     out.cursor_path_phase);
    return out;
}

bool CCallbackHost::answer_execution_terms(const pineforge::NativeExecutionTermsFacts& facts,
                                           no::ExecutionTerms& terms) const {
    const pf_native_terms_view_v1 view = terms_view(facts);
    pf_native_terms_v1 answer;
    std::memset(&answer, 0, sizeof(answer));
    answer.struct_size = static_cast<std::uint32_t>(sizeof(answer));
    answer.version = PF_NATIVE_API_VERSION;
    answer.resolved_price = terms.resolved_price;
    answer.shape = PF_NATIVE_OPENING_SHAPE_TRANSACT;
    answer.grid_policy = PF_NATIVE_GRID_SNAP;
    if (table_.on_execution_terms(table_.user, &view, &answer) == PF_NATIVE_ANSWER_DEFAULT) {
        return true;
    }
    terms.resolved_price = answer.resolved_price;
    bool readable = true;
    switch (answer.shape) {
    case PF_NATIVE_OPENING_SHAPE_TRANSACT: terms.shape = no::OpeningShape::Transact; break;
    case PF_NATIVE_OPENING_SHAPE_REVERSE_TO: terms.shape = no::OpeningShape::ReverseTo; break;
    case PF_NATIVE_OPENING_SHAPE_CLOSE_OPPOSITE:
        terms.shape = no::OpeningShape::CloseOpposite;
        break;
    default: readable = false; break;
    }
    switch (answer.grid_policy) {
    case PF_NATIVE_GRID_SNAP: terms.grid_policy = no::ExecutionGridPolicy::SnapToGrid; break;
    case PF_NATIVE_GRID_EXPLICIT_UNITS:
        terms.grid_policy = no::ExecutionGridPolicy::ExplicitUnits;
        break;
    default: readable = false; break;
    }
    if (!readable) {
        /* Never cast a word onto an enumeration: terms carrying non-finite
         * units are the kernel's own InvalidTerms, whatever the candidate. */
        terms.units = kNaN;
        return false;
    }
    return true;
}

pineforge::NativePrecommitVerdict CCallbackHost::validate_execution_precommit(
        const pineforge::NativePrecommitView& view) const {
    if (!table_.on_precommit) return NativeStrategyHost::validate_execution_precommit(view);
    pf_native_precommit_view_v1 pod;
    std::memset(&pod, 0, sizeof(pod));
    pod.struct_size = static_cast<std::uint32_t>(sizeof(pod));
    pod.version = PF_NATIVE_API_VERSION;
    pod.incarnation = view.target.incarnation;
    if (view.definition) {
        double value = 0.0;
        pod.intent = working_intent_tag(view.definition->request.intent, value);
    }
    std::visit([&pod](const auto& plan) {
        using T = std::decay_t<decltype(plan)>;
        if constexpr (std::is_same_v<T, pineforge::execution::Flatten>) {
            pod.plan = PF_NATIVE_PLAN_FLATTEN;
        } else if constexpr (std::is_same_v<T, pineforge::order_action::Reduce>) {
            pod.plan = PF_NATIVE_PLAN_REDUCE;
            pod.plan_units = plan.units;
        } else if constexpr (std::is_same_v<T, pineforge::order_action::Transact>) {
            pod.plan = PF_NATIVE_PLAN_TRANSACT;
            pod.plan_units = plan.signed_units;
        } else if constexpr (std::is_same_v<T, pineforge::execution::ReverseTo>) {
            pod.plan = PF_NATIVE_PLAN_REVERSE_TO;
            pod.plan_units = plan.signed_units;
        } else {
            static_assert(!sizeof(T), "untranslated execution plan");
        }
    }, view.plan);
    pod.raw_price = view.raw_price;
    pod.resolved_price = view.resolved_price;
    pod.inspected_closed_units = view.inspected_closed_units;
    pod.inspected_opened_units = view.inspected_opened_units;
    pod.inspected_current_ticket = view.inspected_current_ticket;
    pod.current = view.current ? 1u : 0u;
    pod.would_open = view.account.would_open ? 1u : 0u;
    pod.incoming_short = view.account.incoming_short ? 1u : 0u;
    pod.resulting_abs_units = view.account.resulting_abs_units;
    pod.resulting_lot_count = static_cast<std::uint64_t>(view.account.resulting_lot_count);
    pod.resulting_abs_notional = view.account.resulting_abs_notional;
    pod.realized_balance = view.account.realized_balance;
    pod.remaining_entry_cost = view.account.remaining_entry_cost;
    pod.marked_equity = view.account.marked_equity;
    pod.cycle_after = view.account.cycle_after;
    pod.signed_units_after = view.account.signed_units_after;
    pod.closed_row_pnl = view.closed_row_pnl.empty() ? nullptr : view.closed_row_pnl.data();
    pod.closed_row_count = static_cast<std::uint64_t>(view.closed_row_pnl.size());
    fill_view_cursor(view.cursor, pod.cursor_ordinal, pod.cursor_effective_time_ms,
                     pod.cursor_t, pod.cursor_interval_index, pod.cursor_provenance,
                     pod.cursor_path_phase);
    std::uint32_t verdict = PF_NATIVE_PRECOMMIT_ADMIT;
    if (table_.on_precommit(table_.user, &pod, &verdict) == PF_NATIVE_ANSWER_DEFAULT) {
        return pineforge::NativePrecommitVerdict::Admit;
    }
    switch (verdict) {
    case PF_NATIVE_PRECOMMIT_ADMIT: return pineforge::NativePrecommitVerdict::Admit;
    case PF_NATIVE_PRECOMMIT_REFUSE: return pineforge::NativePrecommitVerdict::Refuse;
    case PF_NATIVE_PRECOMMIT_ADMIT_WITH_HOST_MARGIN:
        return pineforge::NativePrecommitVerdict::AdmitWithHostMargin;
    default:
        /* An answer the kernel cannot act on admits nothing. */
        return pineforge::NativePrecommitVerdict::Refuse;
    }
}

std::optional<double> CCallbackHost::resolve_anchored_level(
        const pineforge::NativeAnchoredLevelView& view) const {
    if (!table_.on_anchored_level) return std::nullopt;
    pf_native_anchored_level_view_v1 pod;
    std::memset(&pod, 0, sizeof(pod));
    pod.struct_size = static_cast<std::uint32_t>(sizeof(pod));
    pod.version = PF_NATIVE_API_VERSION;
    pod.owner = view.owner.incarnation;
    pod.owner_applied_ordinal = view.owner_applied_ordinal;
    pod.owner_lot_incarnation = view.owner_lot_incarnation;
    pod.owner_fill_price = view.owner_fill_price;
    pod.leg = view.leg.incarnation;
    pod.leg_side = c_word(view.leg_side);
    pod.trigger = c_word(view.trigger);
    pod.offset = view.offset;
    pod.price_tick = view.price_tick;
    pod.kernel_level = view.kernel_level;
    fill_view_cursor(view.owner_cursor, pod.owner_cursor_ordinal,
                     pod.owner_cursor_effective_time_ms, pod.owner_cursor_t,
                     pod.owner_cursor_interval_index, pod.owner_cursor_provenance,
                     pod.owner_cursor_path_phase);
    double level = view.kernel_level;
    if (table_.on_anchored_level(table_.user, &pod, &level) == PF_NATIVE_ANSWER_DEFAULT) {
        return std::nullopt;
    }
    return level;
}

/* N18: one NativeOpenLot row into its C POD. The strings borrow the cached
 * snapshot exactly as fill_working's label/comment do. */
void fill_open_lot(const pineforge::NativeOpenLot& lot, pf_native_open_lot_v1& out) {
    std::memset(&out, 0, sizeof(out));
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.version = PF_NATIVE_API_VERSION;
    out.ordinal = static_cast<std::uint64_t>(lot.ordinal);
    out.entry_incarnation = lot.entry_incarnation;
    out.cycle = lot.cycle;
    out.side = c_word(lot.side);
    out.entry_bar_index = lot.entry_bar_index;
    out.entry_time_ms = lot.entry_time_ms;
    out.entry_price = lot.entry_price;
    out.signed_units = lot.signed_units;
    out.entry_commission = lot.entry_commission;
    out.mark = lot.mark;
    out.unrealized_pnl = lot.unrealized_pnl;
    out.favorable_excursion = lot.favorable_excursion;
    out.adverse_excursion = lot.adverse_excursion;
    out.entry_label = lot.entry_label.c_str();
    out.entry_comment = lot.entry_comment.c_str();
}

void fill_working(const pineforge::NativeWorkingRequest& live, pf_native_working_v1& out) {
    std::memset(&out, 0, sizeof(out));
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.version = PF_NATIVE_API_VERSION;
    const auto& definition = *live.definition;
    out.incarnation = definition.handle.incarnation;
    out.intent = working_intent_tag(definition.request.intent, out.intent_value);
    out.trigger = working_trigger_tag(definition.request.trigger, out.p1, out.p2);
    if (const auto* trail = std::get_if<no::Trail>(&definition.request.trigger)) {
        out.trail_has_arm_price = trail->arm_price ? 1u : 0u;
    }
    out.owner = static_cast<std::uint32_t>(definition.request.owner.index());
    out.capacity = static_cast<std::uint32_t>(definition.request.capacity.index());
    if (const auto* budget = std::get_if<no::PointBudget>(&definition.request.capacity)) {
        out.capacity_units = budget->units;
    }
    out.group_kind = static_cast<std::uint32_t>(definition.request.group.index());
    if (const auto* member = std::get_if<no::Member>(&definition.request.group)) {
        out.group_id = member->group;
        out.group_cohort = member->cohort;
        out.group_effect = c_word(member->effect);
    }
    out.remaining_kind = static_cast<std::uint32_t>(live.remaining.index());
    if (const auto* units = std::get_if<no::RemainingProjectionUnits>(&live.remaining)) {
        out.remaining_units = units->q;
    }
    out.trigger_state = static_cast<std::uint32_t>(live.trigger_state.index());
    out.origin = c_word(definition.origin);
    out.acceptance_ordinal = definition.birth.acceptance_ordinal;
    out.decision_time_lower_bound = definition.birth.decision_time_lower_bound;
    out.label = definition.request.label.c_str();
    out.comment = definition.request.comment.c_str();
}

/* A setup call's typed answer, written to the C out-parameters that asked for
 * it: the error word and the field it was found at. */
void write_validation(const pineforge::NativeRunSpecValidation& validation,
                      std::uint32_t* error, std::uint32_t* field) {
    if (error) *error = c_word(validation.error);
    if (field) *field = c_word(validation.field);
}

/* Every PF_API body below runs inside one of these: a C++ exception must
 * never cross the boundary, and the kernel throws on an illegal command. */
template <typename Fn>
int guarded(Fn&& fn) noexcept {
    try {
        return fn();
    } catch (const std::bad_alloc&) {
        return PF_NATIVE_E_EXCEPTION;
    } catch (const std::exception&) {
        return PF_NATIVE_E_STATE;
    } catch (...) {
        return PF_NATIVE_E_EXCEPTION;
    }
}

/* ── The run specifications' enum-valued words ─────────────────────
 * Each of the nine words is its kernel enumerator's own integer, pinned name
 * by name above, and each kernel enumeration has a fixed uint32_t underlying
 * type, so a word converts to it for EVERY value -- none is cast onto a value
 * its type cannot hold. c_spelled() is then an exhaustive switch over that
 * enumeration with no default: an enumerator the kernel adds is a -Wswitch
 * diagnostic in its overload until the C surface spells it or refuses it
 * here, and a word that names no enumerator falls out of the switch. Either
 * way translate_word() answers false, which every call site refuses with
 * PF_NATIVE_E_TAG, at the point of the spec where it always did. */
constexpr bool c_spelled(pineforge::NativeFeeKind kind) noexcept {
    switch (kind) {
    case pineforge::NativeFeeKind::Percent:
    case pineforge::NativeFeeKind::CashPerUnit:
    case pineforge::NativeFeeKind::CashPerExecution:
        return true;
    }
    return false;
}

constexpr bool c_spelled(pineforge::NativeCloseExecution rule) noexcept {
    switch (rule) {
    case pineforge::NativeCloseExecution::NextEligiblePoint:
    case pineforge::NativeCloseExecution::AfterCalculation:
        return true;
    }
    return false;
}

constexpr bool c_spelled(pineforge::NativeOpenDirections directions) noexcept {
    switch (directions) {
    case pineforge::NativeOpenDirections::None:
    case pineforge::NativeOpenDirections::Long:
    case pineforge::NativeOpenDirections::Short:
    case pineforge::NativeOpenDirections::Both:
        return true;
    }
    return false;
}

constexpr bool c_spelled(pineforge::NativeReportPolicy policy) noexcept {
    switch (policy) {
    case pineforge::NativeReportPolicy::HostRecorded:
    case pineforge::NativeReportPolicy::KernelRecorded:
        return true;
    case pineforge::NativeReportPolicy::KernelRecordedAtHostMarks:
        /* No C name: its host names each report point from inside its own
         * callbacks, and the C callback table has no call that marks one. */
        return false;
    }
    return false;
}

constexpr bool c_spelled(pineforge::NativePriceGrid grid) noexcept {
    switch (grid) {
    case pineforge::NativePriceGrid::None:
    case pineforge::NativePriceGrid::QuantizeFills:
    case pineforge::NativePriceGrid::QuantizeFillsAndTriggers:
        return true;
    }
    return false;
}

constexpr bool c_spelled(pineforge::NativeGridRounding rounding) noexcept {
    switch (rounding) {
    case pineforge::NativeGridRounding::HalfUp:
    case pineforge::NativeGridRounding::Directional:
        return true;
    }
    return false;
}

constexpr bool c_spelled(pineforge::NativeCalculationTrigger trigger) noexcept {
    switch (trigger) {
    case pineforge::NativeCalculationTrigger::BarClose:
    case pineforge::NativeCalculationTrigger::BarCloseAndFills:
    case pineforge::NativeCalculationTrigger::EveryModeledPoint:
        return true;
    }
    return false;
}

constexpr bool c_spelled(pineforge::NativeOpenBarView view) noexcept {
    switch (view) {
    case pineforge::NativeOpenBarView::Complete:
    case pineforge::NativeOpenBarView::OpenOnly:
        return true;
    }
    return false;
}

constexpr bool c_spelled(pineforge::NativeLiquidationSizing sizing) noexcept {
    switch (sizing) {
    case pineforge::NativeLiquidationSizing::RestoreMinimum:
    case pineforge::NativeLiquidationSizing::ShortfallMultiple:
    case pineforge::NativeLiquidationSizing::Flatten:
        return true;
    }
    return false;
}

template <typename Enum>
bool translate_word(std::uint32_t word, Enum& out) noexcept {
    static_assert(std::is_same_v<std::underlying_type_t<Enum>, std::uint32_t>,
                  "a C word converts only to a kernel enumeration fixed on uint32_t");
    const auto value = static_cast<Enum>(word);
    if (!c_spelled(value)) return false;
    out = value;
    return true;
}

/* The translation accepts exactly the C enumerations: every value the header
 * names is a word c_spelled() admits, and the one kernel value with no C name
 * is not. With the pins above, that makes each C enumeration the whole set
 * its word accepts. */
template <typename Enum, typename... Words>
constexpr bool all_spelled(Words... words) noexcept {
    return (c_spelled(static_cast<Enum>(words)) && ...);
}
static_assert(all_spelled<pineforge::NativeFeeKind>(PF_NATIVE_FEE_PERCENT,
                                                    PF_NATIVE_FEE_CASH_PER_UNIT,
                                                    PF_NATIVE_FEE_CASH_PER_EXECUTION)
                  && all_spelled<pineforge::NativeCloseExecution>(
                      PF_NATIVE_CLOSE_EXECUTION_NEXT_ELIGIBLE_POINT,
                      PF_NATIVE_CLOSE_EXECUTION_AFTER_CALCULATION)
                  && all_spelled<pineforge::NativeOpenDirections>(
                      PF_NATIVE_OPEN_DIRECTIONS_NONE, PF_NATIVE_OPEN_DIRECTIONS_LONG,
                      PF_NATIVE_OPEN_DIRECTIONS_SHORT, PF_NATIVE_OPEN_DIRECTIONS_BOTH)
                  && all_spelled<pineforge::NativeReportPolicy>(
                      PF_NATIVE_REPORT_HOST_RECORDED, PF_NATIVE_REPORT_KERNEL_RECORDED)
                  && all_spelled<pineforge::NativePriceGrid>(
                      PF_NATIVE_PRICE_GRID_NONE, PF_NATIVE_PRICE_GRID_QUANTIZE_FILLS,
                      PF_NATIVE_PRICE_GRID_QUANTIZE_FILLS_AND_TRIGGERS)
                  && all_spelled<pineforge::NativeGridRounding>(
                      PF_NATIVE_GRID_ROUNDING_HALF_UP, PF_NATIVE_GRID_ROUNDING_DIRECTIONAL)
                  && all_spelled<pineforge::NativeCalculationTrigger>(
                      PF_NATIVE_CALC_TRIGGER_BAR_CLOSE,
                      PF_NATIVE_CALC_TRIGGER_BAR_CLOSE_AND_FILLS,
                      PF_NATIVE_CALC_TRIGGER_EVERY_MODELED_POINT)
                  && all_spelled<pineforge::NativeOpenBarView>(
                      PF_NATIVE_OPEN_BAR_VIEW_COMPLETE, PF_NATIVE_OPEN_BAR_VIEW_OPEN_ONLY)
                  && all_spelled<pineforge::NativeLiquidationSizing>(
                      PF_NATIVE_LIQUIDATION_SIZING_RESTORE_MINIMUM,
                      PF_NATIVE_LIQUIDATION_SIZING_SHORTFALL_MULTIPLE,
                      PF_NATIVE_LIQUIDATION_SIZING_FLATTEN),
              "a value the C header names is refused by its translation");
static_assert(!c_spelled(pineforge::NativeReportPolicy::KernelRecordedAtHostMarks),
              "KernelRecordedAtHostMarks has no C name, so its word must stay refused");

/* The v1 base specification, translated exactly as
 * strategy_configure_native_v1 translates it (src/c_abi.cpp). It is
 * duplicated rather than shared because that symbol both translates and
 * configures in one step, and this lane may not change its behaviour. */
int translate_base_spec(const pf_native_run_spec_v1& in, pineforge::NativeRunSpec& out) {
    if (!in.session_key || !in.input_tf || !in.script_tf || !in.ticker || !in.tickerid
        || !in.type || !in.currency || !in.basecurrency || !in.description
        || !in.volumetype || !in.timezone || !in.session || !in.chart_timezone) {
        return PF_NATIVE_E_ARGUMENT;
    }
    constexpr std::uint32_t kOptional =
        PF_NATIVE_SPEC_OPTIONAL_QUANTITY_GRID | PF_NATIVE_SPEC_OPTIONAL_MAX_ABS_UNITS
        | PF_NATIVE_SPEC_OPTIONAL_INITIAL_MARGIN_FRACTION | PF_NATIVE_SPEC_OPTIONAL_MAX_OPEN_LOTS;
    if (in.optional_mask & ~kOptional) return PF_NATIVE_E_TAG;
    out.identity.session_key = in.session_key;
    out.identity.run_number = in.run_number;
    out.input_tf = in.input_tf;
    out.script_tf = in.script_tf;
    out.ticker = in.ticker;
    out.tickerid = in.tickerid;
    out.type = in.type;
    out.currency = in.currency;
    out.basecurrency = in.basecurrency;
    out.description = in.description;
    out.volumetype = in.volumetype;
    out.timezone = in.timezone;
    out.session = in.session;
    out.chart_timezone = in.chart_timezone;
    out.initial_capital = in.initial_capital;
    out.point_value = in.point_value;
    out.account_fx = in.account_fx;
    out.price_tick = in.price_tick;
    out.slippage_ticks = in.slippage_ticks;
    if (!translate_word(in.fee_kind, out.fee_kind)) return PF_NATIVE_E_TAG;
    out.fee_value = in.fee_value;
    if (!translate_word(in.close_execution, out.close_execution)) return PF_NATIVE_E_TAG;
    if (!translate_word(in.allowed_open_directions, out.allowed_open_directions)) {
        return PF_NATIVE_E_TAG;
    }
    if (in.optional_mask & PF_NATIVE_SPEC_OPTIONAL_QUANTITY_GRID) {
        out.quantity_grid = in.quantity_grid;
    }
    if (in.optional_mask & PF_NATIVE_SPEC_OPTIONAL_MAX_ABS_UNITS) {
        out.max_abs_units = in.max_abs_units;
    }
    if (in.optional_mask & PF_NATIVE_SPEC_OPTIONAL_INITIAL_MARGIN_FRACTION) {
        out.initial_margin_fraction = in.initial_margin_fraction;
    }
    if (in.optional_mask & PF_NATIVE_SPEC_OPTIONAL_MAX_OPEN_LOTS) {
        out.max_open_lots = in.max_open_lots;
    }
    return PF_NATIVE_OK;
}

/* `has_risk_tail` is false for a caller compiled against the base layout of
 * pf_native_run_spec_ext_v1 (PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE): its struct
 * stops at `reserved0`, so the risk fields must not be read at all.
 * `has_policy_tail` is false for that caller and for one compiled against the
 * risk layout, whose struct stops at `risk_action`. `has_auxiliary_tail` is
 * false for all three earlier layouts (the last of them stops at
 * `margin_liquidation_comment`): the auxiliary fields must not be read at
 * all either. */
/* The retained intrabar execution path. NONE keeps the whole default
 * surface; the two sampled alternatives share their sampler knobs and differ
 * in whether a finer feed is retained, which is also what decides whether
 * on_sub_bar is ever delivered. */
int translate_intrabar(const pf_native_run_spec_ext_v1& ext, pineforge::IntrabarPath& out) {
    if (ext.intrabar_volume_weighted > 1u) return PF_NATIVE_E_TAG;
    pineforge::MagnifierDistribution distribution;
    switch (ext.intrabar_distribution) {
    case PF_MAGNIFIER_UNIFORM: distribution = pineforge::MagnifierDistribution::UNIFORM; break;
    case PF_MAGNIFIER_COSINE: distribution = pineforge::MagnifierDistribution::COSINE; break;
    case PF_MAGNIFIER_TRIANGLE: distribution = pineforge::MagnifierDistribution::TRIANGLE; break;
    case PF_MAGNIFIER_ENDPOINTS:
        distribution = pineforge::MagnifierDistribution::ENDPOINTS;
        break;
    case PF_MAGNIFIER_FRONT_LOADED:
        distribution = pineforge::MagnifierDistribution::FRONT_LOADED;
        break;
    case PF_MAGNIFIER_BACK_LOADED:
        distribution = pineforge::MagnifierDistribution::BACK_LOADED;
        break;
    default: return PF_NATIVE_E_TAG;
    }
    switch (ext.intrabar_kind) {
    case PF_NATIVE_INTRABAR_NONE:
        out = pineforge::IntrabarPath{};
        return PF_NATIVE_OK;
    case PF_NATIVE_INTRABAR_SYNTHESIZED: {
        pineforge::IntrabarPath::synthesized path;
        path.samples = ext.intrabar_samples;
        path.distribution = distribution;
        path.volume_weighted = ext.intrabar_volume_weighted != 0u;
        path.volume_weighted_min_samples = ext.intrabar_volume_weighted_min_samples;
        path.volume_weighted_max_samples = ext.intrabar_volume_weighted_max_samples;
        out.value = std::move(path);
        return PF_NATIVE_OK;
    }
    case PF_NATIVE_INTRABAR_LOWER_TF: {
        if (!ext.intrabar_tf) return PF_NATIVE_E_ARGUMENT;
        if (ext.intrabar_n < 0 || (ext.intrabar_n > 0 && !ext.intrabar_bars)) {
            return PF_NATIVE_E_ARGUMENT;
        }
        pineforge::IntrabarPath::lower_tf path;
        path.tf = ext.intrabar_tf;
        const auto* bars = reinterpret_cast<const Bar*>(ext.intrabar_bars);
        path.bars.assign(bars, bars + ext.intrabar_n);
        path.samples = ext.intrabar_samples;
        path.distribution = distribution;
        path.volume_weighted = ext.intrabar_volume_weighted != 0u;
        path.volume_weighted_min_samples = ext.intrabar_volume_weighted_min_samples;
        path.volume_weighted_max_samples = ext.intrabar_volume_weighted_max_samples;
        switch (ext.intrabar_sample_eligibility) {
        case PF_NATIVE_SAMPLE_CONTINUOUS_SEGMENTS:
            path.sample_eligibility =
                pineforge::IntrabarPath::SampleEligibility::ContinuousSegments;
            break;
        case PF_NATIVE_SAMPLE_DISTRIBUTION_SAMPLES:
            path.sample_eligibility =
                pineforge::IntrabarPath::SampleEligibility::DistributionSamples;
            break;
        default: return PF_NATIVE_E_TAG;
        }
        out.value = std::move(path);
        return PF_NATIVE_OK;
    }
    default:
        return PF_NATIVE_E_TAG;
    }
}

/* One subscription list, shared by the configure-time block and the
 * begin-time declaration so both read a row the same way. */
int translate_subscriptions(const pf_native_subscription_v1* rows, std::uint32_t n,
                            const std::uint32_t* sources,
                            std::vector<pineforge::NativeTimeframeSubscription>& out) {
    if (n > 0 && !rows) return PF_NATIVE_E_ARGUMENT;
    std::vector<pineforge::NativeTimeframeSubscription> subscriptions;
    subscriptions.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto& row = rows[i];
        if (row.struct_size != sizeof(pf_native_subscription_v1)) return PF_NATIVE_E_STRUCT;
        if (!row.tf) return PF_NATIVE_E_ARGUMENT;
        pineforge::NativeTimeframeSubscription subscription;
        switch (row.lookahead) {
        case PF_NATIVE_LOOKAHEAD_AT_COMPLETION: subscription.lookahead = false; break;
        case PF_NATIVE_LOOKAHEAD_AT_FIRST_INPUT: subscription.lookahead = true; break;
        default: return PF_NATIVE_E_TAG;
        }
        switch (row.gaps) {
        case PF_NATIVE_GAPS_HOLD: subscription.gaps = false; break;
        case PF_NATIVE_GAPS_CLEAR: subscription.gaps = true; break;
        default: return PF_NATIVE_E_TAG;
        }
        if (row.authoritative_n < 0
            || (row.authoritative_n > 0 && !row.authoritative_bars)) {
            return PF_NATIVE_E_ARGUMENT;
        }
        subscription.tf = row.tf;
        /* The source column rides in the auxiliary tail, so a caller that
         * does not carry that tail declares input-built series only. */
        if (sources) {
            switch (sources[i]) {
            case PF_NATIVE_SERIES_SOURCE_INPUT:
                subscription.source = pineforge::NativeSeriesSource::Input;
                break;
            case PF_NATIVE_SERIES_SOURCE_AUXILIARY_FEED:
                subscription.source = pineforge::NativeSeriesSource::AuxiliaryFeed;
                break;
            default: return PF_NATIVE_E_TAG;
            }
        }
        const auto* bars = reinterpret_cast<const Bar*>(row.authoritative_bars);
        subscription.authoritative_bars.assign(bars, bars + row.authoritative_n);
        subscriptions.push_back(std::move(subscription));
    }
    out = std::move(subscriptions);
    return PF_NATIVE_OK;
}

int apply_spec_ext(pineforge::NativeRunSpec& spec, const pf_native_run_spec_ext_v1& ext,
                   bool has_risk_tail, bool has_policy_tail, bool has_auxiliary_tail) {
    if (ext.present_mask & ~0x3ffu) return PF_NATIVE_E_TAG;
    if ((ext.present_mask & PF_NATIVE_SPEC_EXT_RISK) && !has_risk_tail) {
        return PF_NATIVE_E_STRUCT;
    }
    if ((ext.present_mask & PF_NATIVE_SPEC_EXT_AUXILIARY_FEED) && !has_auxiliary_tail) {
        return PF_NATIVE_E_STRUCT;
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_AUXILIARY_FEED) {
        if (!ext.auxiliary_tf || ext.auxiliary_n < 0
            || (ext.auxiliary_n > 0 && !ext.auxiliary_bars)) {
            return PF_NATIVE_E_ARGUMENT;
        }
        if (ext.reserved1 != 0u) return PF_NATIVE_E_TAG;
        pineforge::NativeAuxiliaryFeed feed;
        feed.tf = ext.auxiliary_tf;
        const auto* bars = reinterpret_cast<const Bar*>(ext.auxiliary_bars);
        feed.bars.assign(bars, bars + ext.auxiliary_n);
        spec.auxiliary_feed = std::move(feed);
    }

    if (ext.present_mask & PF_NATIVE_SPEC_EXT_REPORT) {
        if (!translate_word(ext.report_policy, spec.report_policy)) return PF_NATIVE_E_TAG;
        if (ext.report_open_position_at_end > 1u) return PF_NATIVE_E_TAG;
        spec.report_open_position_at_end = ext.report_open_position_at_end != 0u;
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_PRICE_GRID) {
        if (!translate_word(ext.price_grid, spec.price_grid)) return PF_NATIVE_E_TAG;
        if (!translate_word(ext.grid_rounding, spec.grid_rounding)) return PF_NATIVE_E_TAG;
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_CALCULATION) {
        if (!translate_word(ext.calculation, spec.calculation)) return PF_NATIVE_E_TAG;
        spec.max_recalculations_per_point = ext.max_recalculations_per_point;
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_OPEN_BAR_VIEW) {
        if (!translate_word(ext.open_bar_view, spec.open_bar_view)) return PF_NATIVE_E_TAG;
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_MARGIN) {
        pineforge::NativeMarginModel margin;
        margin.initial_long = ext.margin_initial_long;
        margin.initial_short = ext.margin_initial_short;
        if (ext.margin_has_maintenance_long > 1u || ext.margin_has_maintenance_short > 1u
            || ext.margin_has_min_units > 1u) {
            return PF_NATIVE_E_TAG;
        }
        if (ext.margin_has_maintenance_long) margin.maintenance_long = ext.margin_maintenance_long;
        if (ext.margin_has_maintenance_short) {
            margin.maintenance_short = ext.margin_maintenance_short;
        }
        if (!translate_word(ext.margin_sizing, margin.sizing)) return PF_NATIVE_E_TAG;
        margin.shortfall_multiple = ext.margin_shortfall_multiple;
        if (ext.margin_has_min_units) margin.liquidation_min_units = ext.margin_min_units;
        switch (ext.margin_check) {
        case PF_NATIVE_LIQUIDATION_PATH_ADVERSE_EXTREME:
            margin.check = pineforge::NativeLiquidationCheck::PathAdverseExtreme;
            break;
        case PF_NATIVE_LIQUIDATION_CALCULATION_ONLY:
            margin.check = pineforge::NativeLiquidationCheck::CalculationOnly;
            break;
        case PF_NATIVE_LIQUIDATION_PATH_ADVERSE_EXTREME_MARK:
            margin.check = pineforge::NativeLiquidationCheck::PathAdverseExtremeMark;
            break;
        default: return PF_NATIVE_E_TAG;
        }
        if (has_policy_tail) {
            switch (ext.margin_equity_basis) {
            case PF_NATIVE_MARGIN_EQUITY_MARKED:
                margin.basis = pineforge::NativeMarginEquityBasis::MarkedEquity;
                break;
            case PF_NATIVE_MARGIN_EQUITY_BEFORE_OPEN_COMMISSION:
                margin.basis =
                    pineforge::NativeMarginEquityBasis::MarkedEquityBeforeOpenCommission;
                break;
            default: return PF_NATIVE_E_TAG;
            }
            switch (ext.margin_level_base) {
            case PF_NATIVE_MARGIN_LEVEL_MARKED_EQUITY:
                margin.level_base = pineforge::NativeLiquidationLevelBase::MarkedEquity;
                break;
            case PF_NATIVE_MARGIN_LEVEL_REALIZED_ONLY:
                margin.level_base = pineforge::NativeLiquidationLevelBase::RealizedOnly;
                break;
            default: return PF_NATIVE_E_TAG;
            }
            if (ext.margin_liquidation_label) margin.liquidation_label = ext.margin_liquidation_label;
            if (ext.margin_liquidation_comment) {
                margin.liquidation_comment = ext.margin_liquidation_comment;
            }
        }
        spec.margin = margin;
    }
    if ((ext.present_mask & (PF_NATIVE_SPEC_EXT_INTRABAR | PF_NATIVE_SPEC_EXT_FEED_POLICY))
        && !has_policy_tail) {
        return PF_NATIVE_E_STRUCT;
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_RISK) {
        pineforge::NativeRiskLimits risk;
        if (ext.risk_has_max_drawdown > 1u || ext.risk_max_drawdown_percent > 1u
            || ext.risk_has_max_intraday_loss > 1u || ext.risk_max_intraday_loss_percent > 1u
            || ext.risk_has_max_consecutive_loss_days > 1u
            || ext.risk_has_max_fills_per_day > 1u) {
            return PF_NATIVE_E_TAG;
        }
        if (ext.risk_has_max_drawdown) {
            pineforge::NativeLossLimit limit;
            limit.value = ext.risk_max_drawdown;
            limit.percent = ext.risk_max_drawdown_percent != 0u;
            risk.max_drawdown = limit;
        }
        if (ext.risk_has_max_intraday_loss) {
            pineforge::NativeLossLimit limit;
            limit.value = ext.risk_max_intraday_loss;
            limit.percent = ext.risk_max_intraday_loss_percent != 0u;
            risk.max_intraday_loss = limit;
        }
        if (ext.risk_has_max_consecutive_loss_days) {
            risk.max_consecutive_loss_days = ext.risk_max_consecutive_loss_days;
        }
        if (ext.risk_has_max_fills_per_day) {
            risk.max_fills_per_day = ext.risk_max_fills_per_day;
        }
        switch (ext.risk_day_basis) {
        case 0: risk.day_basis = pineforge::NativeRiskDay::SessionDay; break;
        case 1: risk.day_basis = pineforge::NativeRiskDay::CalendarDayInTimezone; break;
        default: return PF_NATIVE_E_TAG;
        }
        switch (ext.risk_action) {
        case 0: risk.action = pineforge::NativeRiskAction::BlockOpenings; break;
        case 1: risk.action = pineforge::NativeRiskAction::FlattenAndBlock; break;
        default: return PF_NATIVE_E_TAG;
        }
        spec.risk = risk;
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_SUBSCRIPTIONS) {
        if (int rc = translate_subscriptions(
                ext.subscriptions, ext.subscriptions_n,
                has_auxiliary_tail ? ext.subscription_sources : nullptr, spec.subscriptions);
            rc != PF_NATIVE_OK) {
            return rc;
        }
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_INTRABAR) {
        if (int rc = translate_intrabar(ext, spec.intrabar); rc != PF_NATIVE_OK) return rc;
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_FEED_POLICY) {
        switch (ext.slot_label_policy) {
        case PF_NATIVE_SLOT_LABEL_CANONICAL:
            spec.slot_label_policy = pineforge::NativeSlotLabelPolicy::Canonical;
            break;
        case PF_NATIVE_SLOT_LABEL_FEED_TOLERANT:
            spec.slot_label_policy = pineforge::NativeSlotLabelPolicy::FeedTolerant;
            break;
        default: return PF_NATIVE_E_TAG;
        }
        /* A bit mask, not an enumerator: any bit outside the published set
         * is a value this version cannot represent. */
        constexpr std::uint32_t kTolerance = PF_NATIVE_FEED_TOLERANCE_BATCH_STRUCTURAL
                                           | PF_NATIVE_FEED_TOLERANCE_WARMUP_NONNEGATIVE;
        if (ext.feed_tolerance & ~kTolerance) return PF_NATIVE_E_TAG;
        spec.legacy_tolerance = static_cast<pineforge::NativeFeedTolerance>(ext.feed_tolerance);
        switch (ext.path_order) {
        case PF_NATIVE_PATH_ORDER_AUTO: spec.path_order = pineforge::NativePathOrder::Auto; break;
        case PF_NATIVE_PATH_ORDER_HIGH_FIRST:
            spec.path_order = pineforge::NativePathOrder::HighFirst;
            break;
        case PF_NATIVE_PATH_ORDER_LOW_FIRST:
            spec.path_order = pineforge::NativePathOrder::LowFirst;
            break;
        default: return PF_NATIVE_E_TAG;
        }
        switch (ext.abort_reporting) {
        case PF_NATIVE_ABORT_ERROR:
            spec.abort_reporting = pineforge::NativeAbortReporting::Error;
            break;
        case PF_NATIVE_ABORT_QUIET:
            spec.abort_reporting = pineforge::NativeAbortReporting::Quiet;
            break;
        default: return PF_NATIVE_E_TAG;
        }
    }
    return PF_NATIVE_OK;
}

}  // namespace

extern "C" {

PF_API int strategy_native_api_version(void) { return PF_NATIVE_API_VERSION; }

PF_API pf_strategy_t strategy_native_host_create_v1(const pf_native_callbacks_v1* callbacks) {
    try {
        if (!callbacks) return nullptr;
        /* Three published layouts, and only three: the base one the lane
         * first shipped, that plus the six-hook tail, and the current one
         * with the policy-hook tail. An earlier caller's tails are never
         * read; they are zero-filled here, which is exactly "no hook
         * installed". */
        const std::uint32_t caller_size = callbacks->struct_size;
        if (caller_size != sizeof(pf_native_callbacks_v1)
            && caller_size != PF_NATIVE_CALLBACKS_V1_HOOKS_SIZE
            && caller_size != PF_NATIVE_CALLBACKS_V1_BASE_SIZE) {
            return nullptr;
        }
        if (callbacks->version != PF_NATIVE_API_VERSION) return nullptr;
        pf_native_callbacks_v1 table;
        std::memset(&table, 0, sizeof(table));
        std::memcpy(&table, callbacks, callbacks->struct_size);
        /* The retained copy is always the current layout. */
        table.struct_size = static_cast<std::uint32_t>(sizeof(table));
        auto host = std::make_unique<CCallbackHost>(table);
        /* Convert through the base the rest of the C ABI casts back to, so
         * `static_cast<BacktestEngine*>(handle)` in c_abi.cpp is exact. */
        auto* engine = static_cast<pineforge::BacktestEngine*>(host.release());
        return static_cast<pf_strategy_t>(engine);
    } catch (...) {
        return nullptr;
    }
}

PF_API void strategy_native_host_free(pf_strategy_t s) {
    try {
        if (!s) return;
        auto* host = host_of(s);
        if (!host) return;  /* not ours: freeing it would be a type error */
        delete host;
    } catch (...) {
    }
}

PF_API int strategy_native_run_v1(pf_strategy_t s, const pf_bar_t* bars, int n,
                                  pf_report_t* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (n < 0 || (n > 0 && !bars)) return PF_NATIVE_E_ARGUMENT;
        host->run(reinterpret_cast<const Bar*>(bars), n);
        if (out) host->fill_report(reinterpret_cast<pineforge::ReportC*>(out));
        return host->native_state().kind == pineforge::NativeLifecycleKind::Completed
            ? PF_NATIVE_OK
            : PF_NATIVE_E_RUN_FAILED;
    });
}

PF_API void strategy_native_report_free_v1(pf_report_t* report) {
    try {
        if (!report) return;
        pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(report));
    } catch (...) {
    }
}

PF_API int strategy_native_submit_v1(pf_strategy_t s, const pf_native_request_v1* request,
                                     uint64_t* incarnation, uint32_t* reject) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!request) return PF_NATIVE_E_ARGUMENT;
        const auto* run = run_identity(*host);
        if (!run) return PF_NATIVE_E_STATE;
        no::Request translated;
        if (int rc = translate_request(*request, *run, translated); rc != PF_NATIVE_OK) return rc;
        const auto result = host->submit(translated);
        if (result.status == no::SubmitStatus::Accepted) {
            if (incarnation && result.handle) *incarnation = result.handle->incarnation;
            return PF_NATIVE_OK;
        }
        if (reject && result.reason) *reject = c_word(*result.reason);
        return PF_NATIVE_E_REJECTED;
    });
}

PF_API int strategy_native_replace_ext_v1(pf_strategy_t s, uint64_t incarnation,
                                          const pf_native_request_v1* request,
                                          uint64_t* successor, uint32_t* reject) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!request) return PF_NATIVE_E_ARGUMENT;
        const auto* run = run_identity(*host);
        if (!run) return PF_NATIVE_E_STATE;
        no::Request translated;
        if (int rc = translate_request(*request, *run, translated); rc != PF_NATIVE_OK) return rc;
        const auto result = host->replace(no::RequestHandle{*run, incarnation}, translated);
        switch (result.status) {
        case no::ReplaceStatus::Replaced:
            if (successor && result.successor) *successor = result.successor->incarnation;
            return PF_NATIVE_OK;
        case no::ReplaceStatus::ReplaceRejected:
            /* The one verdict that carries a reason, written exactly as
             * strategy_native_submit_v1 writes a refused submit's. */
            if (reject && result.reason) *reject = c_word(*result.reason);
            return PF_NATIVE_E_REJECTED;
        case no::ReplaceStatus::NotWorking:
            return PF_NATIVE_E_NOT_WORKING;
        case no::ReplaceStatus::InvalidHandle:
            return PF_NATIVE_E_INVALID_TARGET;
        }
        return PF_NATIVE_E_EXCEPTION;
    });
}

/* The established spelling: this call with no reason to write. */
PF_API int strategy_native_replace_v1(pf_strategy_t s, uint64_t incarnation,
                                      const pf_native_request_v1* request,
                                      uint64_t* successor) {
    return strategy_native_replace_ext_v1(s, incarnation, request, successor, nullptr);
}

PF_API int strategy_native_cancel_v1(pf_strategy_t s, uint64_t incarnation) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        const auto* run = run_identity(*host);
        if (!run) return PF_NATIVE_E_STATE;
        const auto result = host->cancel(no::RequestHandle{*run, incarnation});
        switch (result.status) {
        case no::CancelStatus::Cancelled: return PF_NATIVE_OK;
        case no::CancelStatus::NotWorking: return PF_NATIVE_E_NOT_WORKING;
        case no::CancelStatus::InvalidHandle: return PF_NATIVE_E_INVALID_TARGET;
        }
        return PF_NATIVE_E_EXCEPTION;
    });
}

PF_API int strategy_native_cancel_all_v1(pf_strategy_t s) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        return static_cast<int>(host->cancel_all());
    });
}

PF_API int strategy_native_cancel_where_v1(pf_strategy_t s, const char* text, uint32_t field) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        // NULL is not the empty string here: "" is the text every request
        // that carries no comment or label matches, so a caller that meant
        // one and passed the other would cancel a different set.
        if (!text) return PF_NATIVE_E_ARGUMENT;
        pineforge::NativeRequestField selector = pineforge::NativeRequestField::Comment;
        switch (field) {
        case PF_NATIVE_FIELD_COMMENT:
            selector = pineforge::NativeRequestField::Comment;
            break;
        case PF_NATIVE_FIELD_LABEL:
            selector = pineforge::NativeRequestField::Label;
            break;
        default:
            return PF_NATIVE_E_TAG;
        }
        return static_cast<int>(host->cancel_where(std::string_view(text), selector));
    });
}

PF_API int strategy_native_execute_current_v1(pf_strategy_t s, uint64_t incarnation,
                                              uint32_t price_rule, uint32_t* refusal) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        const auto* run = run_identity(*host);
        if (!run) return PF_NATIVE_E_STATE;
        pineforge::NativeCurrentExecution selection;
        selection.target = no::RequestHandle{*run, incarnation};
        switch (price_rule) {
        case PF_NATIVE_PRICE_AS_PRESENTED:
            selection.price_rule = pineforge::NativeCurrentPriceRule::AsPresented;
            break;
        case PF_NATIVE_PRICE_NEAREST_TICK:
            selection.price_rule = pineforge::NativeCurrentPriceRule::NearestTick;
            break;
        default:
            return PF_NATIVE_E_TAG;
        }
        const auto result = host->execute_current(selection);
        return std::visit([&](const auto& outcome) -> int {
            using T = std::decay_t<decltype(outcome)>;
            if constexpr (std::is_same_v<T, pineforge::NativeCurrentRefusal>) {
                if (refusal) *refusal = c_word(outcome);
                return PF_NATIVE_E_REFUSED;
            } else if constexpr (std::is_same_v<T, no::ExecutionAppliedEvent>) {
                return PF_NATIVE_EXECUTED_APPLIED;
            } else if constexpr (std::is_same_v<T, no::NoEffectEvent>) {
                return PF_NATIVE_EXECUTED_NO_EFFECT;
            } else if constexpr (std::is_same_v<T, no::MatchRejectedEvent>) {
                return PF_NATIVE_EXECUTED_MATCH_REJECTED;
            } else {
                return PF_NATIVE_EXECUTED_CANCELLED;
            }
        }, result);
    });
}

PF_API int strategy_native_position_v1(pf_strategy_t s, double* signed_units,
                                       double* average_price, uint64_t* lots) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        const auto position = host->physical_position();
        if (signed_units) *signed_units = position.signed_units;
        if (average_price) *average_price = position.average_price;
        if (lots) *lots = static_cast<std::uint64_t>(position.lot_count);
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_working_len_v1(pf_strategy_t s) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        host->working_cache() = host->native_working_requests();
        return static_cast<int>(host->working_cache().size());
    });
}

PF_API int strategy_native_working_get_v1(pf_strategy_t s, int index,
                                          pf_native_working_v1* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        /* Two published layouts: the base one the L13 lane first shipped and
         * the current one with the arm-presence tail. A base-layout caller's
         * struct ends at `comment`, so the row is written exactly that far. */
        const std::uint32_t struct_size = out->struct_size;
        if (struct_size != sizeof(pf_native_working_v1)
            && struct_size != PF_NATIVE_WORKING_V1_BASE_SIZE) {
            return PF_NATIVE_E_STRUCT;
        }
        const auto& cache = host->working_cache();
        if (index < 0 || static_cast<std::size_t>(index) >= cache.size()) {
            return PF_NATIVE_E_ARGUMENT;
        }
        pf_native_working_v1 row;
        fill_working(cache[static_cast<std::size_t>(index)], row);
        row.struct_size = struct_size;
        std::memcpy(out, &row, struct_size);
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_open_lot_count_v1(pf_strategy_t s, double mark) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        host->open_lot_cache() = host->native_open_lots(mark);
        return static_cast<int>(host->open_lot_cache().size());
    });
}

PF_API int strategy_native_open_lot_get_v1(pf_strategy_t s, int index,
                                           pf_native_open_lot_v1* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        if (out->struct_size != sizeof(pf_native_open_lot_v1)) return PF_NATIVE_E_STRUCT;
        const auto& cache = host->open_lot_cache();
        if (index < 0 || static_cast<std::size_t>(index) >= cache.size()) {
            return PF_NATIVE_E_ARGUMENT;
        }
        fill_open_lot(cache[static_cast<std::size_t>(index)], *out);
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_events_v1(pf_strategy_t s, uint64_t after_ordinal,
                                     pf_native_event_v1* out, int cap) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (cap < 0 || (cap > 0 && !out)) return PF_NATIVE_E_ARGUMENT;
        const auto events = host->native_events(after_ordinal);
        int written = 0;
        std::size_t consumed = 0;
        for (; consumed < events.size() && written < cap; ++consumed) {
            pf_native_event_v1 row;
            if (!translate_event(events[consumed], row)) continue;
            out[written++] = row;
        }
        /* Ordinals are non-decreasing, not strictly increasing: an applied
         * execution and the account observation it produced share one. A
         * caller resumes from the last returned ordinal, so a page that ended
         * in the middle of such a group would lose its siblings forever.
         * Give the whole group back on the next page instead. A cap below the
         * group size (2 today) cannot honour that and is left as written. */
        if (written == cap && written > 1) {
            const std::uint64_t tail = out[written - 1].ordinal;
            for (std::size_t next = consumed; next < events.size(); ++next) {
                pf_native_event_v1 probe;
                if (!translate_event(events[next], probe)) continue;
                if (probe.ordinal == tail) {
                    while (written > 1 && out[written - 1].ordinal == tail) --written;
                }
                break;
            }
        }
        return written;
    });
}

PF_API int strategy_native_state_v1(pf_strategy_t s, pf_native_state_v1* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        if (out->struct_size != sizeof(pf_native_state_v1)) return PF_NATIVE_E_STRUCT;
        const auto state = host->native_state();
        const std::uint32_t struct_size = out->struct_size;
        std::memset(out, 0, sizeof(*out));
        out->struct_size = struct_size;
        out->version = PF_NATIVE_API_VERSION;
        out->lifecycle = c_word(state.kind);
        out->failure_code = c_word(state.failure.code);
        out->failure_operation = c_word(state.failure.operation);
        out->failure_discriminator = state.failure.discriminator;
        out->failure_ordinal = state.failure.ordinal;
        out->consumed_high_water = state.consumed_high_water;
        out->decision_floor_ms = state.decision_floor_ms;
        out->phase = c_word(state.phase);
        out->completion = c_word(state.completion);
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_declare_subscriptions_ext_v1(pf_strategy_t s,
                                                        const pf_native_subscription_v1* rows,
                                                        int n, const uint32_t* sources,
                                                        uint32_t* error, uint32_t* field) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (n < 0 || (n > 0 && !rows)) return PF_NATIVE_E_ARGUMENT;
        std::vector<pineforge::NativeTimeframeSubscription> declared;
        /* NULL sources build every series from the input, exactly as the
         * run spec's own subscription block does without its source column. */
        if (int rc = translate_subscriptions(rows, static_cast<std::uint32_t>(n), sources,
                                             declared);
            rc != PF_NATIVE_OK) {
            return rc;
        }
        const auto result = host->declare_timeframe_subscriptions_result(std::move(declared));
        write_validation(result.validation, error, field);
        return result.status == pineforge::NativeSetupStatus::Applied ? PF_NATIVE_OK
                                                                      : PF_NATIVE_E_STATE;
    });
}

/* The established spelling: the call above with no source column and no
 * words written. */
PF_API int strategy_native_declare_subscriptions_v1(pf_strategy_t s,
                                                    const pf_native_subscription_v1* rows,
                                                    int n) {
    return strategy_native_declare_subscriptions_ext_v1(s, rows, n, nullptr, nullptr, nullptr);
}

PF_API int strategy_native_declare_auxiliary_feed_v1(pf_strategy_t s, const char* tf,
                                                     const pf_bar_t* bars, int32_t n,
                                                     uint32_t* error, uint32_t* field) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (n < 0 || (n > 0 && !bars)) return PF_NATIVE_E_ARGUMENT;
        std::optional<pineforge::NativeAuxiliaryFeed> feed;
        if (tf) {
            pineforge::NativeAuxiliaryFeed declared;
            declared.tf = tf;
            const auto* begin = reinterpret_cast<const Bar*>(bars);
            declared.bars.assign(begin, begin + n);
            feed = std::move(declared);
        } else if (n != 0 || bars) {
            /* A withdrawal names no feed, so it can carry no bars either. */
            return PF_NATIVE_E_ARGUMENT;
        }
        const auto result = host->declare_auxiliary_feed_result(std::move(feed));
        write_validation(result.validation, error, field);
        return result.status == pineforge::NativeSetupStatus::Applied ? PF_NATIVE_OK
                                                                      : PF_NATIVE_E_STATE;
    });
}

PF_API int strategy_native_partial_bar_v1(pf_strategy_t s, pf_bar_t* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        const auto bar = host->current_partial_bar();
        if (!bar) return PF_NATIVE_ABSENT;
        std::memcpy(out, &*bar, sizeof(pf_bar_t));
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_recalculations_v1(pf_strategy_t s, uint64_t* driven,
                                             uint64_t* skipped) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (driven) *driven = host->native_recalculation_count();
        if (skipped) *skipped = host->native_recalculations_skipped();
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_trail_state_v1(pf_strategy_t s, uint64_t incarnation,
                                          pf_native_trail_state_v1* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        if (out->struct_size != sizeof(pf_native_trail_state_v1)) return PF_NATIVE_E_STRUCT;
        const auto* run = run_identity(*host);
        if (!run) return PF_NATIVE_E_STATE;
        const auto state = host->trail_state(no::RequestHandle{*run, incarnation});
        if (!state) return PF_NATIVE_ABSENT;
        const std::uint32_t struct_size = out->struct_size;
        std::memset(out, 0, sizeof(*out));
        out->struct_size = struct_size;
        out->version = PF_NATIVE_API_VERSION;
        out->activated = state->activated ? 1u : 0u;
        out->best_price = state->best_price;
        out->current_level = state->current_level;
        out->activation_ordinal = state->activation_ordinal;
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_series_bar_v1(pf_strategy_t s, uint32_t subscription,
                                         pf_bar_t* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        const auto bar = host->native_series_bar(static_cast<std::size_t>(subscription));
        if (!bar) return PF_NATIVE_ABSENT;
        std::memcpy(out, &*bar, sizeof(pf_bar_t));
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_marked_equity_v1(pf_strategy_t s, double mark, double* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        *out = host->native_marked_equity(mark);
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_liquidation_price_v1(pf_strategy_t s, double* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        const auto price = host->native_liquidation_price();
        if (!price) {
            *out = kNaN;
            return PF_NATIVE_ABSENT;
        }
        *out = *price;
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_sized_units_v1(pf_strategy_t s, const pf_native_request_v1* sized,
                                          double price, double equity, double fx,
                                          double* units) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!sized || !units) return PF_NATIVE_E_ARGUMENT;
        /* The sizing block is read by the one translation a submit uses. A
         * SIZED request names no handle, so before configure -- when the run
         * has no identity yet -- an empty one serves, and the kernel's own
         * answer for an unconfigured run is the empty. */
        const auto* run = run_identity(*host);
        const no::RunIdentity none{};
        no::Request translated;
        if (int rc = translate_request(*sized, run ? *run : none, translated);
            rc != PF_NATIVE_OK) {
            return rc;
        }
        const auto* basis = std::get_if<no::Sized>(&translated.intent);
        if (!basis) return PF_NATIVE_E_ARGUMENT;
        const auto answer = host->native_sized_units(*basis, price, equity, fx);
        if (!answer) {
            *units = kNaN;
            return PF_NATIVE_ABSENT;
        }
        *units = *answer;
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_risk_state_v1(pf_strategy_t s, pf_native_risk_state_v1* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        if (out->struct_size != sizeof(pf_native_risk_state_v1)) return PF_NATIVE_E_STRUCT;
        const auto state = host->native_risk_state();
        const std::uint32_t struct_size = out->struct_size;
        std::memset(out, 0, sizeof(*out));
        out->struct_size = struct_size;
        out->version = PF_NATIVE_API_VERSION;
        out->blocked = state.blocked ? 1u : 0u;
        if (state.reason) {
            out->has_reason = 1u;
            out->reason = c_word(*state.reason);
        }
        out->has_day = state.has_day ? 1u : 0u;
        out->consecutive_loss_days = state.consecutive_loss_days;
        out->day_ordinal = state.day_ordinal;
        out->fills_today = state.fills_today;
        out->peak_equity = state.peak_equity;
        out->day_open_equity = state.day_open_equity;
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_margin_call_v1(pf_strategy_t s, uint64_t ordinal,
                                          pf_native_margin_call_v1* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        if (out->struct_size != sizeof(pf_native_margin_call_v1)) return PF_NATIVE_E_STRUCT;
        if (ordinal == 0u) return PF_NATIVE_ABSENT;
        /* The history is ordered by ordinal, so the call is the first row at
         * or past it, or it is not there at all. */
        for (const auto& event : host->native_events(ordinal - 1u)) {
            if (event.ordinal > ordinal) break;
            if (event.kind != pineforge::NativeEventKind::Command || !event.command) continue;
            const auto* call = std::get_if<no::MarginCallEvent>(&*event.command);
            if (!call || call->ordinal != ordinal) continue;
            *out = margin_call_economics(*call);
            return PF_NATIVE_OK;
        }
        return PF_NATIVE_ABSENT;
    });
}

PF_API int strategy_native_continuation_hash_v1(pf_strategy_t s, uint64_t* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        *out = host->native_continuation_hash();
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_cohort_open_v1(pf_strategy_t s, uint64_t* cohort) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!cohort) return PF_NATIVE_E_ARGUMENT;
        *cohort = host->cohort_open().value;
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_cohort_add_v1(pf_strategy_t s, uint64_t cohort,
                                         uint64_t incarnation) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        const auto* run = run_identity(*host);
        if (!run) return PF_NATIVE_E_STATE;
        if (cohort == 0u) return PF_NATIVE_E_ARGUMENT;
        host->cohort_add(no::CohortHandle{cohort}, no::RequestHandle{*run, incarnation});
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_cohort_remove_v1(pf_strategy_t s, uint64_t cohort,
                                            uint64_t incarnation) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        const auto* run = run_identity(*host);
        if (!run) return PF_NATIVE_E_STATE;
        if (cohort == 0u) return PF_NATIVE_E_ARGUMENT;
        host->cohort_remove(no::CohortHandle{cohort}, no::RequestHandle{*run, incarnation});
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_configure_native_ext_v1(pf_strategy_t s,
                                            const pf_native_run_spec_v1* base,
                                            const pf_native_run_spec_ext_v1* ext) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!base || !ext) return PF_NATIVE_E_ARGUMENT;
        if (base->struct_size != sizeof(pf_native_run_spec_v1)) return PF_NATIVE_E_STRUCT;
        /* Four published layouts, and only four: the base one the lane
         * first shipped, that plus L9's risk tail, that plus N8's intrabar /
         * policy tail, and the current one with the auxiliary-feed tail
         * behind it. Anything else is a caller this runtime cannot read. */
        const bool has_auxiliary_tail = ext->struct_size == sizeof(pf_native_run_spec_ext_v1);
        const bool has_policy_tail =
            has_auxiliary_tail || ext->struct_size == PF_NATIVE_RUN_SPEC_EXT_V1_POLICY_SIZE;
        const bool has_risk_tail =
            has_policy_tail || ext->struct_size == PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE;
        if ((!has_risk_tail && ext->struct_size != PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE)
            || ext->version != PF_NATIVE_API_VERSION) {
            return PF_NATIVE_E_STRUCT;
        }
        /* The kernel configures a host exactly once and FAILS it on a second
         * attempt (native_execution_consumer.cpp, "configure refused while
         * ready"), so the readiness check happens here, before the kernel
         * sees anything: a refused extension must leave the handle usable. */
        if (host->native_state().kind != pineforge::NativeLifecycleKind::Unconfigured) {
            return PF_NATIVE_E_STATE;
        }
        pineforge::NativeRunSpec spec;
        if (int rc = translate_base_spec(*base, spec); rc != PF_NATIVE_OK) return rc;
        if (int rc = apply_spec_ext(spec, *ext, has_risk_tail, has_policy_tail,
                                    has_auxiliary_tail);
            rc != PF_NATIVE_OK) {
            return rc;
        }
        /* The kernel FAILS a host whose specification its validation
         * refuses, so the same validation runs here first: a rejected
         * specification is refused before the kernel sees it and the handle
         * stays usable. configure_native judges the same value again and
         * cannot disagree. */
        if (!pineforge::validate_native_run_spec(spec)) return PF_NATIVE_E_ARGUMENT;
        return host->configure_native(spec).status == pineforge::NativeSetupStatus::Applied
            ? PF_NATIVE_OK
            : PF_NATIVE_E_ARGUMENT;
    });
}

PF_API int strategy_native_append_auxiliary_bars_ext_v1(pf_strategy_t s, const pf_bar_t* bars,
                                                        int32_t n, uint32_t* error,
                                                        int32_t* index) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (n < 0 || (n > 0 && !bars)) return PF_NATIVE_E_ARGUMENT;
        const auto result = host->append_auxiliary_bars_result(
            reinterpret_cast<const Bar*>(bars), static_cast<std::size_t>(n));
        if (error) *error = c_word(result.error);
        /* The bar of THIS call, which a count of int32_t bars bounds. */
        if (index) *index = static_cast<std::int32_t>(result.index);
        return result.status == pineforge::NativeSetupStatus::Applied ? PF_NATIVE_OK
                                                                      : PF_NATIVE_E_STATE;
    });
}

/* The established spelling: the call above with no words written. */
PF_API int strategy_native_append_auxiliary_bars_v1(pf_strategy_t s, const pf_bar_t* bars,
                                                    int32_t n) {
    return strategy_native_append_auxiliary_bars_ext_v1(s, bars, n, nullptr, nullptr);
}

PF_API int strategy_native_declare_opened_lot_entry_bar_mask_v1(pf_strategy_t s,
                                                                uint64_t entry_incarnation,
                                                                const pf_bar_t* entry_bar,
                                                                uint32_t fill_point) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!entry_bar) return PF_NATIVE_E_ARGUMENT;
        pineforge::OpenedLotFillPoint point = pineforge::OpenedLotFillPoint::OnPath;
        switch (fill_point) {
        case PF_NATIVE_OPENED_LOT_FILL_POINT_ON_PATH:
            point = pineforge::OpenedLotFillPoint::OnPath;
            break;
        case PF_NATIVE_OPENED_LOT_FILL_POINT_AFTER_PATH:
            point = pineforge::OpenedLotFillPoint::AfterPath;
            break;
        default:
            return PF_NATIVE_E_TAG;
        }
        return host->declare_entry_bar_mask(entry_incarnation,
                                            *reinterpret_cast<const Bar*>(entry_bar), point)
            ? PF_NATIVE_OK
            : PF_NATIVE_E_STATE;
    });
}

}  /* extern "C" */

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
