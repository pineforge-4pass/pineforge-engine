#pragma once
/*
 * engine_settlement_stage.hpp -- RUNTIME-PRIVATE header (src/, never installed).
 *
 * The native settlement's stages, BacktestEngine's private nested types that
 * engine.hpp only declares: the staged chain (NativeSettlementStage) and the
 * one-lot pass (NativeSettlementStage::OneLot, R5 lane PERF-L2). Their
 * definitions live here, not in engine_execution.cpp alone, so the consumer
 * can hold a one-lot stage from a fill's precommit preview to its settlement
 * (R5 lane D2-C); engine.hpp's layout is untouched.
 */

#include <pineforge/engine.hpp>

#include "engine_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

namespace pineforge {

struct BacktestEngine::NativeSettlementStage {
    enum class Phase { Invalid, NoEffect, Ready };
    Phase phase = Phase::Invalid;
    execution::Status status = execution::Status::NoEffect;
    bool flatten = false;
    bool scoped = false;
    bool opposite = false;
    bool closes = false;
    bool use_selected = false;
    PositionSide incoming = PositionSide::LONG;
    bool was_long = false;
    double requested = 0.0;
    double selected_held = 0.0;
    double allocation_requested = 0.0;
    double closed = 0.0;
    double opening = 0.0;
    double ticket = 0.0;
    double after_qty = 0.0;
    double after_price = 0.0;
    // The account-currency rate this stage's quoted charges and inspected
    // notional convert at, handed in by whoever staged it.
    double fx = 0.0;
    std::vector<std::size_t> closing_indices;
    std::vector<double> closing_quantities;
    std::vector<double> current_costs;
    std::vector<PyramidEntry> survivors;
    std::unordered_set<std::uint64_t> selected_ids;

    struct OneLot;
};

// The fused settlement (R5 lane PERF-L2; PERF-D1 §8, lane L2): the stage of
// the common case, in one pass and on the stack.
//
// A Flatten, Reduce or Transact on the Book or an opening's scope, with no
// selection and no lifecycle effect, or a ReverseTo, settles in one pass when
// the book holds at most one lot and none of it survives: an opening from
// flat, the whole lot closing, or that close and the opposite opening. Its
// stage is the chain's scalars with the vectors cut to what one closed lot and
// one opening need -- closing_indices {0}, closing_quantities {closing},
// current_costs {close_cost, open_cost}, no survivor, no selection -- and each
// function below is the chain function it names, on those values, with the
// chain's tests in the chain's order:
//   stage    stage_native_settlement, allocate_native_settlement_closes and
//            finish_native_settlement_stage. It answers false for every call
//            of another shape (a selection, two lots, an addition or a partial
//            close that leaves a survivor) and for every call the chain would
//            not stage Ready (a test it fails, or no effect). It changes
//            nothing and consults no host, so a call it declines stages
//            through the chain from scratch.
//   inspect  inspect_native_settlement_stage.
//   project  project_native_settlement_stage.
//   prepare  prepare_native_settlement_commit, after the close row is built.
//   preview  preview_native_settlement_commit: prepare, then project.
//   settle   commit_native_settlement_stage: prepare, preflight, commit.
// The close row is built by build_close_trade_with_costs as often, at the
// same context, as the chain builds it, so a host that owns lot excursions is
// consulted exactly as before. Only without such a host does preview hand its
// row to project instead of building it again at the default context: that
// row is then a function of the lot, the fill and the fee, and project reads
// its pnl and commission, which the context does not enter. The effects go
// through the chain's sinks in the chain's order: record_close_trade,
// reset_position_state_to_flat, open_quoted_position,
// stream_refresh_action_metadata. Nothing here names a host or a language.
struct BacktestEngine::NativeSettlementStage::OneLot {
    PositionSide incoming = PositionSide::LONG;
    bool was_long = false;
    // The one lot closes, whole: closing_indices {0}.
    bool closes = false;
    double closing = 0.0;  // closing_quantities.front()
    double closed = 0.0;
    double opening = 0.0;
    double close_cost = 0.0;  // current_costs.front() when the lot closes
    double open_cost = 0.0;   // current_costs.back()
    double ticket = 0.0;
    double after_qty = 0.0;
    double after_price = 0.0;
    double fx = 0.0;

    // Whether this call takes the one pass: the test switch, then the shape
    // and stage(). The probe counts the answer.
    bool admit(const BacktestEngine& engine, internal::SettlementEntry entry,
               const execution::Action& action, const execution::Fill& fill,
               const execution::CloseScope& book_or_opening,
               const execution::SelectedOpeningSet* selected,
               const execution::LifecycleEffects* lifecycle, double quote_fx);
    bool admit(const BacktestEngine& engine, internal::SettlementEntry entry,
               const execution::ReverseTo& reversal, const execution::Fill& fill,
               const execution::LifecycleEffects* lifecycle, double quote_fx);

    bool stage(const BacktestEngine& engine, const execution::Action& action,
               const execution::Fill& fill, const execution::CloseScope& book_or_opening,
               const execution::LifecycleEffects* lifecycle, double quote_fx);
    bool stage(const BacktestEngine& engine, const execution::ReverseTo& reversal,
               const execution::Fill& fill, const execution::LifecycleEffects* lifecycle,
               double quote_fx);
    execution::SettlementInspection inspect(const BacktestEngine& engine,
                                            const execution::Fill& fill) const;
    execution::AccountEffectProjection project(const BacktestEngine& engine,
                                               const execution::Fill& fill,
                                               const Trade* row) const;
    execution::Status preview(const BacktestEngine& engine, const execution::Fill& fill,
                              const execution::PhysicalExecutionContext& context,
                              execution::AccountEffectProjection& account,
                              std::vector<double>& row_pnl, bool reserve_rows) const;
    execution::Result settle(BacktestEngine& engine, const execution::Fill& fill,
                             const execution::PhysicalExecutionContext& context) const;

    // R5 lane D2-C: one stage per fill. A host with a precommit hook (and a
    // host that owns lot excursions) is shown the settlement's preview before
    // the fill settles, and the settlement then staged the same fill again:
    // the same action, fill, scope and empty lifecycle over the same book.
    // The preview now hands its stage back (preview_keeping, the body of both
    // preview_native_settlement_commit forms), and the consumer settles from
    // it (settle_kept) when no fill has settled since -- its generation stamp,
    // NativeExecutionConsumer::consume_matched_request. The book is the only
    // input of the stage the precommit call could see move: the consumer's
    // fill carries the inspection's pinned ticket (commission_account), which
    // quote() only splits by quantity, so neither the fee nor the account
    // rate enters it. The close row is still built at the settlement, so a
    // host that owns lot excursions is asked as before.
    static execution::Status preview_keeping(
        const BacktestEngine& engine, const execution::Action& action,
        const execution::Fill& fill, const execution::PhysicalExecutionContext& context,
        execution::CloseScope scope, const execution::SelectedOpeningSet* selected,
        execution::AccountEffectProjection& account, std::vector<double>& row_pnl,
        std::optional<OneLot>* kept);
    static execution::Status preview_keeping(
        const BacktestEngine& engine, const execution::ReverseTo& reversal,
        const execution::Fill& fill, const execution::PhysicalExecutionContext& context,
        execution::AccountEffectProjection& account, std::vector<double>& row_pnl,
        std::optional<OneLot>* kept);
    // settle() for a stage kept from this fill's preview, counted as the
    // Settle entry's one pass. nullopt when the test switch now declines the
    // one pass: the caller settles as before.
    std::optional<execution::Result> settle_kept(
        BacktestEngine& engine, const execution::Fill& fill,
        const execution::PhysicalExecutionContext& context) const;

private:
    static void count(internal::SettlementEntry entry, bool fused) noexcept;
    bool finish(const BacktestEngine& engine, const execution::Fill& fill, double quote_fx);
    void quote(const BacktestEngine& engine, const execution::Fill& fill);
    Trade close_row(const BacktestEngine& engine, const execution::Fill& fill,
                    const execution::PhysicalExecutionContext& context) const;
    execution::Status prepare(const BacktestEngine& engine, const Trade* row) const;
};

}  // namespace pineforge
