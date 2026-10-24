// CR-277: public pending-order, native-event and trade observations.
//
// The probe rows preserve the draft Pine sequence. Their expected replacement
// result comes from the engine's same-id contract, NOT a TradingView export:
// TV equality for entry/close/re-entry is still owed.
//
// close("L") never reaches the faulty close() push: that push is inside
// id.empty(). Pyramiding=1 batches the first X, so close_all() transfers it
// through the already-correct PendingSameBarCommand sibling. The site rows
// use pyramiding=3 with nonzero margins, prove that X was accepted and then
// cancelled by the full close, and keep the close so its fill releases X.
// A same-id reissue must replace X in pending_entries_ BEFORE choosing a
// same-bar batch or appending another entry behind the close. Otherwise both
// the correct and the moved-from replacement key go unused.
//
// Row classes (measured on the lab, three trees: the fix, only the key capture
// reverted, the fully unfixed adapter):
//   reversal-close-all-capture   CAPTURE WITNESS: red with only the capture
//                                reverted and on the fully unfixed adapter.
//   lookup-queued-replacement    LOOKUP WITNESS: red on the fully unfixed
//                                adapter only; green with only the capture
//                                reverted (it does not witness the capture).
//   lookup-different-id          GUARD for the lookup (a different id must not
//                                be replaced): green on all three trees, red
//                                only if the lookup matches beyond the id.
// Additional incarnation rows (lab verification pending):
//   incarnation-superseded-definition  INCARNATION WITNESS: the queued old X
//                                must never consume a request number.
//   incarnation-different-id     INCARNATION CONTROL: both queued ids survive;
//                                the counting rule also holds without the fix.
// Both read the sequence origin from the earlier L control request, count
// accepted submit/replace events, and check absolute open-lot incarnations,
// including a later Z. Comments distinguish definitions in public events;
// an unexpected old X cannot increase its own expected incarnation.
// The older site rows (probe-*, control-*, site-*) keep their roles.
// The two-close_all sequence (close_all; entry X; close_all; entry X qty 2) is
// NOT asserted here: the engine's outcome differs from the Pine statement
// above and is carded separately (CX-CR277-DIVERGE).
//
// release-time-key was withdrawn: the lab measured it GREEN with the capture
// reverted and on the fully unfixed adapter. reversal-close-all-capture uses
// seed 837832's market S; priced S with OCA reduce; close_all sequence instead.
// Pine's last same-id unfilled command modifies one order; pyramiding=1 gives
// no second same-direction entry slot. Thus the release must open one S lot
// of the declared default quantity, independently of the seed's trade counts.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <variant>
#include <vector>

using namespace pineforge;

namespace {

int failures = 0;
const char* row_name = "";

#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d [%s] %s\n", __FILE__, __LINE__,         \
                     row_name, #condition);                                    \
        ++failures;                                                            \
    }                                                                          \
} while (0)

constexpr double missing = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t start = 1'736'121'600'000LL;
constexpr double seed_units = 1.0;
constexpr double reissue_units = 2.0;

enum class Close { None, Named, Empty, All };

bool full_close(Close close) {
    return close == Close::Empty || close == Close::All;
}

const char* close_label(Close close) {
    return close == Close::Empty ? "__pine_close_all" : "__close__";
}

struct Row {
    const char* name;
    Close close;
    int pyramiding;
};

constexpr Row rows[] = {
    {"probe-close", Close::Named, 1},
    {"probe-close-all", Close::All, 1},
    {"control-close", Close::None, 1},
    {"control-close-all", Close::None, 1},
    {"site-close-empty", Close::Empty, 3},
    {"site-close-all", Close::All, 3},
    {"site-control", Close::None, 3},
};

std::vector<pf_pending_order_v1_t> pending_x(source::PineStrategyHost& host) {
    std::vector<pf_pending_order_v1_t> result;
    const int count = strategy_pending_orders_len(&host);
    CHECK(count >= 0);
    for (int i = 0; i < count; ++i) {
        pf_pending_order_v1_t row{};
        const int status = strategy_pending_order_get(&host, i, &row, sizeof row);
        CHECK(status == 0);
        if (status == 0 && std::strcmp(row.id, "X") == 0) result.push_back(row);
    }
    return result;
}

class ReissueHost final : public source::PineStrategyHost {
public:
    using BacktestEngine::open_trade_entry_id;
    using BacktestEngine::open_trade_entry_price;
    using BacktestEngine::open_trade_size;

    explicit ReissueHost(const Row& row) : row_(row) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.initial_capital = 100'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = seed_units;
        config.pyramiding = row.pyramiding;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.0;
        config.slippage = 0;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(1.0);
        fixture_retain_all_events();
    }

    bool seed_ready = false;
    int sequence_calls = 0;
    std::uint64_t flush_boundary = 0;
    native_order::RequestHandle first_x{};
    native_order::RequestHandle parent_close{};
    int first_x_acceptances = 0;
    int first_x_cancellations = 0;

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true);
        if (pine_bar_index() != 1) return;
        ++sequence_calls;
        seed_ready = physical_position().signed_units == seed_units
            && physical_position().lot_count == 1
            && open_trade_entry_id(0) == "L";
        for (const auto& event : native_events(0))
            flush_boundary = event.ordinal;
        strategy_entry("X", false);
        for (const auto& event : native_events(flush_boundary)) {
            if (!event.command) continue;
            const auto* accepted = std::get_if<native_order::AcceptedEvent>(&*event.command);
            if (accepted && accepted->request().label == "X") {
                first_x = accepted->handle();
                ++first_x_acceptances;
            }
        }
        const auto first = pending_x(*this);
        CHECK(first.size() == 1);
        if (first.size() == 1) {
            CHECK(std::isnan(first[0].qty));
            CHECK((first[0].incarnation != 0) == (row_.pyramiding > 1));
        }
        switch (row_.close) {
        case Close::None: break;
        case Close::Named: strategy_close("L"); break;
        case Close::Empty: strategy_close(""); break;
        case Close::All: strategy_close_all(); break;
        }
        if (full_close(row_.close)) {
            for (const auto& event : native_events(flush_boundary)) {
                if (!event.command) continue;
                if (const auto* accepted = std::get_if<native_order::AcceptedEvent>(&*event.command)) {
                    if (accepted->request().label == close_label(row_.close)) {
                        parent_close = accepted->handle();
                        CHECK(std::holds_alternative<native_order::Flatten>(accepted->request().intent));
                    }
                }
                if (const auto* cancelled = std::get_if<native_order::CancelledEvent>(&*event.command)) {
                    if (first_x.incarnation != 0 && cancelled->handle() == first_x)
                        ++first_x_cancellations;
                }
            }
            CHECK(parent_close.incarnation != 0);
            // With these configurations, cancellation of the just-accepted,
            // unpriced X inside this full-close call is the live-entry push.
            // Pyramiding=1 instead transfers its unaccepted batch sibling.
            const int live_transfer = row_.pyramiding > 1 ? 1 : 0;
            CHECK(first_x_acceptances == live_transfer);
            CHECK(first_x_cancellations == live_transfer);
            const auto transferred = pending_x(*this);
            std::fprintf(stderr, "[%s] before reissue: X accepted=%d, cancelled=%d, transferred=%zu\n",
                         row_name, first_x_acceptances, first_x_cancellations, transferred.size());
            CHECK(transferred.size() == 1);
            if (transferred.size() == 1) {
                CHECK(std::isnan(transferred[0].qty));
                CHECK(transferred[0].incarnation == 0);
            }
        }
        if (row_.close != Close::None) {
            for (const auto& event : native_events(0))
                flush_boundary = event.ordinal;
        }
        strategy_entry("X", false, missing, missing, reissue_units);
    }

private:
    Row row_;
};

int accepted_x_after_boundary(const ReissueHost& host) {
    int count = 0;
    for (const auto& event : host.native_events(host.flush_boundary)) {
        if (!event.command) continue;
        const auto* accepted = std::get_if<native_order::AcceptedEvent>(&*event.command);
        if (accepted && accepted->request().label == "X") ++count;
    }
    // ReplacedEvent is an amendment, not an independently accepted order.
    // A full close cancels/transfers X before this boundary; its fill must
    // release exactly one new X. Without a close, include the first accepted
    // X because it remains the replacement's target.
    return count;
}

void check_row(const Row& row) {
    row_name = row.name;
    ReissueHost host(row);
    const Bar warmup[] = {
        {100, 100, 100, 100, 1, start},
        {100, 100, 100, 100, 1, start + 60'000},
    };
    const bool began = host.stream_begin(warmup, 2, "1", "1");
    CHECK(began);
    CHECK(host.last_error().empty());
    if (!began) return;
    CHECK(host.sequence_calls == 1);
    CHECK(host.seed_ready);
    CHECK(host.physical_position().signed_units == seed_units);

    // stream_begin has flushed the final historical callback but has not
    // consumed the following open. Inspect actual pending instructions here.
    const auto pending = pending_x(host);
    const int flushed = accepted_x_after_boundary(host);
    const bool close_first = full_close(row.close);
    std::fprintf(stderr, "[%s] staged X=%zu, newly accepted X=%d, qty:",
                 row_name, pending.size(), flushed);
    for (const auto& order : pending) std::fprintf(stderr, " %.17g", order.qty);
    std::fprintf(stderr, "\n");
    CHECK(pending.size() == 1);
    if (pending.size() == 1) {
        CHECK(pending[0].qty == reissue_units);
        CHECK(pending[0].is_long == 0);
        CHECK((pending[0].incarnation == 0) == close_first);
    }
    // flush_pending_entries holds a paired entry while its close is live.
    // It is accepted at that close's next-open fill, not at this boundary.
    const int expected_preopen_acceptances = close_first ? 0 : 1;
    CHECK(flushed == expected_preopen_acceptances);
    if (close_first) CHECK(strategy_pending_orders_len(&host) == 2); // close and X

    // The stream avoids end-of-range liquidation/report artifacts. The first
    // tick is exactly the next bar's open, at the same price as the callback.
    CHECK(host.stream_push_tick(TradeTick{start + 120'000, 1, 100.0, 1.0}));
    CHECK(host.last_error().empty());
    int x_fills = 0;
    double x_opened = 0.0;
    double x_closed = 0.0;
    int parent_fills = 0;
    std::uint64_t parent_fill_ordinal = 0;
    std::uint64_t x_acceptance_ordinal = 0;
    std::uint64_t x_fill_ordinal = 0;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        if (close_first && event.ordinal > host.flush_boundary) {
            if (const auto* accepted = std::get_if<native_order::AcceptedEvent>(&*event.command)) {
                if (accepted->request().label == "X") {
                    const auto* transaction = std::get_if<native_order::Transact>(&accepted->request().intent);
                    CHECK(transaction != nullptr);
                    if (transaction) CHECK(transaction->signed_units == -reissue_units);
                    x_acceptance_ordinal = accepted->ordinal;
                }
            }
        }
        const auto* fill = std::get_if<native_order::ExecutionAppliedEvent>(&*event.command);
        if (fill && close_first && fill->handle() == host.parent_close) {
            ++parent_fills;
            parent_fill_ordinal = fill->ordinal;
            CHECK(fill->closed_units == seed_units);
            CHECK(fill->opened_units == 0.0);
        }
        if (!fill || fill->request().label != "X") continue;
        ++x_fills;
        x_opened += fill->opened_units;
        x_closed += fill->closed_units;
        x_fill_ordinal = fill->ordinal;
    }
    // order_action::plan(Transact) subtracts closed opposite units from the
    // transaction and keeps the remainder's sign. The R1 same-id rule uses
    // the reissue's own transaction. A full close consumes the seed first;
    // otherwise X consumes it. These are engine contracts, not a TV verdict.
    const double expected_x_closed = close_first ? 0.0 : seed_units;
    const double expected_short = reissue_units - expected_x_closed;
    std::fprintf(stderr, "[%s] close fills=%d, X fills=%d, closed=%.17g, opened=%.17g, position=%.17g, lots=%zu\n",
                 row_name, parent_fills, x_fills, x_closed, x_opened, host.physical_position().signed_units,
                 host.physical_position().lot_count);
    CHECK(accepted_x_after_boundary(host) == 1);
    CHECK(parent_fills == (close_first ? 1 : 0));
    if (close_first) {
        CHECK(parent_fill_ordinal != 0);
        CHECK(x_acceptance_ordinal > parent_fill_ordinal);
        CHECK(x_fill_ordinal > x_acceptance_ordinal);
    }
    CHECK(x_fills == 1);
    // ExecutionAppliedEvent carries signed opened units. A Transact of -2
    // against +1 closes one unit and opens the negative remainder; after a
    // full close it opens the whole negative transaction instead.
    CHECK(x_opened == -expected_short);
    CHECK(x_closed == expected_x_closed);
    CHECK(host.physical_position().signed_units == -expected_short);
    CHECK(host.physical_position().lot_count == 1);
    CHECK(host.open_trade_entry_id(0) == "X");
    CHECK(host.open_trade_size(0) == expected_short);
    CHECK(host.open_trade_entry_price(0) == 100.0);
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const auto& trade = host.get_trade(0);
        CHECK(trade.entry_id == "L");
        CHECK(trade.qty == seed_units);
        CHECK(trade.entry_price == 100.0);
        CHECK(trade.exit_price == 100.0);
        CHECK(trade.exit_id == (close_first ? close_label(row.close) : "X"));
        CHECK(trade.entry_time == start + 60'000);
        CHECK(trade.exit_time == start + 120'000);
    }
    CHECK(strategy_pending_orders_len(&host) == 0);
}

enum class ScopeCase {
    DifferentId, QueuedReplacement, ReversalCloseAllCapture,
    IncarnationSupersededDefinition, IncarnationDifferentId
};

struct ScopeRow {
    const char* name;
    ScopeCase scenario;
};

constexpr ScopeRow scope_rows[] = {
    {"lookup-different-id", ScopeCase::DifferentId},
    {"lookup-queued-replacement", ScopeCase::QueuedReplacement},
    {"reversal-close-all-capture", ScopeCase::ReversalCloseAllCapture},
    {"incarnation-superseded-definition", ScopeCase::IncarnationSupersededDefinition},
    {"incarnation-different-id", ScopeCase::IncarnationDifferentId},
};

constexpr char queued_definition_comment[] = "queued definition";
constexpr char reissued_definition_comment[] = "reissued definition";

bool incarnation_case(ScopeCase scenario) {
    return scenario == ScopeCase::IncarnationSupersededDefinition
        || scenario == ScopeCase::IncarnationDifferentId;
}

std::vector<native_order::DefinitionRef> accepted_definitions(
        const source::PineStrategyHost& host, std::uint64_t after_ordinal = 0) {
    std::vector<native_order::DefinitionRef> result;
    for (const auto& event : host.native_events(after_ordinal)) {
        if (!event.command) continue;
        if (const auto* accepted = std::get_if<native_order::AcceptedEvent>(&*event.command))
            result.push_back(accepted->definition);
        if (const auto* replaced = std::get_if<native_order::ReplacedEvent>(&*event.command))
            result.push_back(replaced->successor_definition);
    }
    // Every successful replace consumes a sequence number, including a
    // keep_handle re-price. Cancels, executions and rejected commands do not.
    return result;
}

std::vector<pf_pending_order_v1_t> pending_rows(source::PineStrategyHost& host) {
    std::vector<pf_pending_order_v1_t> result;
    const int count = strategy_pending_orders_len(&host);
    CHECK(count >= 0);
    for (int index = 0; index < count; ++index) {
        pf_pending_order_v1_t row{};
        const int status = strategy_pending_order_get(&host, index, &row, sizeof row);
        CHECK(status == 0);
        if (status == 0) result.push_back(row);
    }
    return result;
}

int pending_id_count(const std::vector<pf_pending_order_v1_t>& rows, const char* id) {
    int count = 0;
    for (const auto& row : rows)
        if (std::strcmp(row.id, id) == 0) ++count;
    return count;
}

const pf_pending_order_v1_t* pending_id(
        const std::vector<pf_pending_order_v1_t>& rows, const char* id) {
    for (const auto& row : rows)
        if (std::strcmp(row.id, id) == 0) return &row;
    return nullptr;
}

class ScopeProbeHost final : public source::PineStrategyHost {
public:
    using BacktestEngine::open_trade_entry_id;
    using BacktestEngine::open_trade_size;

    explicit ScopeProbeHost(ScopeCase scenario) : scenario_(scenario) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.initial_capital = 100'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = seed_units;
        config.pyramiding = 3;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.0;
        config.slippage = 0;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        if (scenario == ScopeCase::ReversalCloseAllCapture) {
            config.initial_capital = 1'000'000.0;
            config.pyramiding = 1;
            // Margins stay at 100/100, as in the seed's battery configuration
            // (test_adapter_live_state_equivalence.cpp, margin off). Seed 837832's
            // commission keeps the market S live instead of
            // taking the zero-cost same-bar command batch.
            config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
            config.commission_value = 0.25;
        }
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(1.0);
        if (scenario == ScopeCase::ReversalCloseAllCapture) set_syminfo_mintick(0.25);
        fixture_retain_all_events();
    }

    int capture_sequence_calls = 0;
    int capture_release_checks = 0;
    int incarnation_sequence_calls = 0;
    int incarnation_reissue_calls = 0;
    int incarnation_next_calls = 0;
    std::uint64_t incarnation_origin = 0;
    std::uint64_t incarnation_release_boundary = 0;
    std::uint64_t incarnation_next_boundary = 0;
    native_order::RequestHandle incarnation_parent_close{};
    std::vector<native_order::DefinitionRef> incarnation_prefix;

    void stage_reversal_close_all_capture();
    void check_reversal_close_all_release();
    void stage_incarnation_sequence();

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("L", true);
            return;
        }
        if (pine_bar_index() == 1) {
            switch (scenario_) {
            case ScopeCase::DifferentId:
                strategy_entry("X", false);
                strategy_close_all();
                strategy_entry("Y", false);
                break;
            case ScopeCase::QueuedReplacement:
                strategy_close_all();
                strategy_entry("X", false);
                strategy_entry("X", false, missing, missing, reissue_units);
                break;
            case ScopeCase::ReversalCloseAllCapture:
                stage_reversal_close_all_capture();
                break;
            case ScopeCase::IncarnationSupersededDefinition:
            case ScopeCase::IncarnationDifferentId:
                stage_incarnation_sequence();
                break;
            }
            return;
        }
        if (pine_bar_index() == 2 && scenario_ == ScopeCase::ReversalCloseAllCapture)
            check_reversal_close_all_release();
        if (pine_bar_index() == 2 && incarnation_case(scenario_)) {
            ++incarnation_next_calls;
            for (const auto& event : native_events(0))
                incarnation_next_boundary = event.ordinal;
            strategy_entry("Z", false, missing, missing, seed_units);
        }
    }

private:
    ScopeCase scenario_;
};

int open_id_count(const ScopeProbeHost& host, const char* id) {
    int count = 0;
    for (std::size_t index = 0; index < host.physical_position().lot_count; ++index) {
        if (host.open_trade_entry_id(static_cast<int>(index)) == id) ++count;
    }
    return count;
}

double open_id_units(const ScopeProbeHost& host, const char* id) {
    double units = 0.0;
    for (std::size_t index = 0; index < host.physical_position().lot_count; ++index) {
        if (host.open_trade_entry_id(static_cast<int>(index)) == id)
            units += host.open_trade_size(static_cast<int>(index));
    }
    return units;
}

int fill_id_count(const ScopeProbeHost& host, const char* id) {
    int count = 0;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        const auto* fill = std::get_if<native_order::ExecutionAppliedEvent>(&*event.command);
        if (fill && fill->request().label == id) ++count;
    }
    return count;
}

void ScopeProbeHost::stage_incarnation_sequence() {
    ++incarnation_sequence_calls;
    const auto control = accepted_definitions(*this);
    CHECK(control.size() == 1);
    if (!control.empty()) {
        CHECK(control.front()->request.label == "L");
        incarnation_origin = control.front()->handle.incarnation;
        CHECK(incarnation_origin != 0);
    }
    const auto seed = native_open_lots(missing);
    CHECK(seed.size() == 1);
    if (seed.size() == 1) {
        CHECK(seed[0].entry_label == "L");
        CHECK(seed[0].signed_units == seed_units);
        CHECK(seed[0].entry_incarnation == incarnation_origin);
    }

    // The lookup-queued-replacement sequence, with comments identifying the
    // old and new definitions without supplying any incarnation oracle.
    strategy_close_all();
    incarnation_prefix = accepted_definitions(*this);
    for (const auto& definition : incarnation_prefix) {
        if (definition->request.label != "__close__") continue;
        incarnation_parent_close = definition->handle;
        CHECK(std::holds_alternative<native_order::Flatten>(definition->request.intent));
    }
    CHECK(incarnation_parent_close.incarnation != 0);
    for (const auto& event : native_events(0))
        incarnation_release_boundary = event.ordinal;
    strategy_entry("X", false, missing, missing, missing, queued_definition_comment);

    // Prove the second call really sees an unaccepted definition behind a
    // still-live full close. These preconditions hold on both adapter trees.
    const auto queued = pending_rows(*this);
    CHECK(pending_id_count(queued, "__close__") == 1);
    const auto* parent = pending_id(queued, "__close__");
    CHECK(parent != nullptr);
    if (parent) CHECK(parent->incarnation == incarnation_parent_close.incarnation);
    CHECK(pending_id_count(queued, "X") == 1);
    const auto* x = pending_id(queued, "X");
    CHECK(x != nullptr);
    if (x) {
        CHECK(x->incarnation == 0);
        CHECK(std::isnan(x->qty));
        CHECK(std::isnan(x->limit_price));
        CHECK(std::isnan(x->stop_price));
        CHECK(x->is_long == 0);
    }
    CHECK(accepted_definitions(*this).size() == incarnation_prefix.size());
    CHECK(fill_id_count(*this, "__close__") == 0);
    CHECK(fill_id_count(*this, "X") == 0);
    CHECK(physical_position().signed_units == seed_units);

    const char* reissued_id = scenario_ == ScopeCase::IncarnationSupersededDefinition ? "X" : "Y";
    strategy_entry(reissued_id, false, missing, missing, reissue_units, reissued_definition_comment);
    ++incarnation_reissue_calls;
}

void ScopeProbeHost::stage_reversal_close_all_capture() {
    ++capture_sequence_calls;
    CHECK(physical_position().signed_units == seed_units);
    CHECK(physical_position().lot_count == 1);
    CHECK(open_id_count(*this, "L") == 1);
    CHECK(open_id_units(*this, "L") == seed_units);
    CHECK(fill_id_count(*this, "S") == 0);

    // Exactly the three order commands of script bar 190. Observations
    // between them prove the live transfer and its already-staged priced peer.
    strategy_entry("S", false);
    const auto market = pending_rows(*this);
    CHECK(pending_id_count(market, "S") == 1);
    const auto* live_s = pending_id(market, "S");
    CHECK(live_s != nullptr);
    std::uint64_t market_incarnation = 0;
    if (live_s) {
        market_incarnation = live_s->incarnation;
        CHECK(market_incarnation != 0); // A live request, not a batch sibling.
        CHECK(std::isnan(live_s->qty));
        CHECK(std::isnan(live_s->limit_price));
        CHECK(std::isnan(live_s->stop_price));
        CHECK(live_s->is_long == 0);
    }

    strategy_entry("S", false, 101.75, missing, missing, {}, "g1", 2);
    const auto before_close = pending_rows(*this);
    CHECK(pending_id_count(before_close, "S") == 2);
    int priced_pending_s = 0;
    int live_market_s = 0;
    for (const auto& order : before_close) {
        if (std::strcmp(order.id, "S") != 0) continue;
        if (order.limit_price == 101.75) {
            ++priced_pending_s;
            CHECK(order.incarnation == 0); // The priced-reversal staging route.
            CHECK(std::isnan(order.qty));
            CHECK(std::isnan(order.stop_price));
            CHECK(order.is_long == 0);
            CHECK(std::strcmp(order.oca_name, "g1") == 0);
            CHECK(order.oca_type == 2);
        } else if (order.incarnation == market_incarnation) {
            ++live_market_s;
        }
    }
    CHECK(priced_pending_s == 1);
    CHECK(live_market_s == 1);

    strategy_close_all();
    const auto staged = pending_rows(*this);
    CHECK(pending_id_count(staged, "__close__") == 1);
    CHECK(pending_id_count(staged, "S") == 2);
    int transferred_s = 0;
    priced_pending_s = 0;
    for (const auto& order : staged) {
        if (std::strcmp(order.id, "S") != 0) continue;
        CHECK(order.incarnation == 0);
        CHECK(std::isnan(order.qty));
        CHECK(std::isnan(order.stop_price));
        CHECK(order.is_long == 0);
        if (std::isnan(order.limit_price)) ++transferred_s;
        if (order.limit_price == 101.75) ++priced_pending_s;
    }
    // Two internal definitions are staged, but exactly one is the transferred
    // market S in pending_entries_. They are not two permitted Pine orders.
    CHECK(transferred_s == 1);
    CHECK(priced_pending_s == 1);
    int market_cancellations = 0;
    for (const auto& event : native_events(0)) {
        if (!event.command) continue;
        const auto* cancelled = std::get_if<native_order::CancelledEvent>(&*event.command);
        if (cancelled && cancelled->handle().incarnation == market_incarnation)
            ++market_cancellations;
    }
    CHECK(market_cancellations == 1);
    CHECK(fill_id_count(*this, "__close__") == 0);
    CHECK(fill_id_count(*this, "S") == 0);
    CHECK(physical_position().signed_units == seed_units);
}

void ScopeProbeHost::check_reversal_close_all_release() {
    ++capture_release_checks;
    // Historical matching has already processed this bar's open. Observe
    // here, before range-end finalization; issue no next-bar replacement.
    const int s_fills = fill_id_count(*this, "S");
    const int s_lots = open_id_count(*this, "S");
    std::fprintf(stderr, "[%s] released S fills=%d, open S=%d, position=%.17g, lots=%zu\n",
                 row_name, s_fills, s_lots, physical_position().signed_units,
                 physical_position().lot_count);
    // The same-id modification rule leaves one one-unit short order; the
    // full close consumes L first. Pyramiding=1 cannot grant another S lot.
    CHECK(s_fills == 1);
    CHECK(s_lots == 1);
    CHECK(physical_position().lot_count == 1);
    CHECK(physical_position().signed_units == -seed_units);
    CHECK(open_id_units(*this, "S") == seed_units);
    CHECK(open_id_count(*this, "L") == 0);
    CHECK(fill_id_count(*this, "__close__") == 1);
    CHECK(trade_count() == 1);
    if (trade_count() == 1) {
        CHECK(get_trade(0).entry_id == "L");
        CHECK(get_trade(0).exit_id == "__close__");
        CHECK(get_trade(0).qty == seed_units);
    }
    CHECK(pending_id_count(pending_rows(*this), "S") == 0);
    CHECK(strategy_pending_orders_len(this) == 0);
}

void check_incarnation_row(ScopeProbeHost& host, bool same_id) {
    const char* reissued_id = same_id ? "X" : "Y";
    CHECK(host.incarnation_sequence_calls == 1);
    CHECK(host.incarnation_reissue_calls == 1);
    CHECK(host.incarnation_next_calls == 0);
    CHECK(!host.incarnation_prefix.empty());
    const auto staged = pending_rows(host);
    CHECK(pending_id_count(staged, "__close__") == 1);
    CHECK(pending_id_count(staged, "X") == 1);
    CHECK(pending_id_count(staged, "Y") == (same_id ? 0 : 1));
    const auto* reissued = pending_id(staged, reissued_id);
    CHECK(reissued != nullptr);
    if (reissued) {
        CHECK(reissued->incarnation == 0);
        CHECK(reissued->qty == reissue_units);
    }
    CHECK(accepted_definitions(host).size() == host.incarnation_prefix.size());

    CHECK(host.stream_push_tick(TradeTick{start + 120'000, 1, 100.0, 1.0}));
    CHECK(host.last_error().empty());
    const auto released = accepted_definitions(host, host.incarnation_release_boundary);
    std::vector<native_order::DefinitionRef> survivors;
    int x_acceptances = 0;
    int surviving_x_acceptances = 0;
    int reissued_acceptances = 0;
    for (const auto& definition : released) {
        const auto& request = definition->request;
        if (request.label == "X") ++x_acceptances;
        const bool reissued_definition = request.label == reissued_id
            && request.comment == reissued_definition_comment;
        const bool different_id_original = !same_id && request.label == "X"
            && request.comment == queued_definition_comment;
        if (!reissued_definition && !different_id_original) continue;
        survivors.push_back(definition);
        if (request.label == "X") ++surviving_x_acceptances;
        if (reissued_definition) ++reissued_acceptances;
    }
    CHECK(reissued_acceptances == 1);
    CHECK(surviving_x_acceptances == 1);
    CHECK(x_acceptances == surviving_x_acceptances);
    CHECK(released.size() == survivors.size());
    std::fprintf(stderr, "[%s] origin=%llu, accepted before release=%zu, released=%zu, surviving=%zu, X accepted=%d, surviving X=%d\n",
                 row_name, static_cast<unsigned long long>(host.incarnation_origin),
                 host.incarnation_prefix.size(), released.size(), survivors.size(),
                 x_acceptances, surviving_x_acceptances);

    std::uint64_t parent_fill_ordinal = 0;
    for (const auto& event : host.native_events(host.incarnation_release_boundary)) {
        if (!event.command) continue;
        const auto* fill = std::get_if<native_order::ExecutionAppliedEvent>(&*event.command);
        if (fill && fill->handle() == host.incarnation_parent_close) {
            parent_fill_ordinal = event.ordinal;
            CHECK(fill->closed_units == seed_units);
            CHECK(fill->opened_units == 0.0);
        }
    }
    CHECK(parent_fill_ordinal != 0);
    CHECK(fill_id_count(host, "__close__") == 1);
    const auto opened = host.native_open_lots(missing);
    CHECK(opened.size() == survivors.size());
    CHECK(host.physical_position().signed_units == -reissue_units - (same_id ? 0.0 : seed_units));
    // Start at L's observed incarnation, not a literal. The prefix counts
    // every accepted command before release. Only definitions permitted to
    // survive contribute below; counting an unwanted old X here would let
    // its extra sequence number normalize the bug's own expected values.
    std::uint64_t accepted_before = static_cast<std::uint64_t>(host.incarnation_prefix.size());
    for (const auto& definition : survivors) {
        const std::uint64_t expected = host.incarnation_origin + accepted_before;
        CHECK(definition->birth.acceptance_ordinal > parent_fill_ordinal);
        CHECK(definition->handle.incarnation == expected);
        int matching_lots = 0;
        for (const auto& lot : opened) {
            if (lot.entry_label != definition->request.label
                || lot.entry_comment != definition->request.comment) continue;
            ++matching_lots;
            CHECK(lot.entry_incarnation == definition->handle.incarnation);
            CHECK(lot.entry_incarnation == expected);
            const double units = lot.entry_comment == reissued_definition_comment
                ? reissue_units : seed_units;
            CHECK(lot.signed_units == -units);
            std::fprintf(stderr, "[%s] %s lot incarnation=%llu, expected=%llu, accepted before=%llu\n",
                         row_name, lot.entry_label.c_str(),
                         static_cast<unsigned long long>(lot.entry_incarnation),
                         static_cast<unsigned long long>(expected),
                         static_cast<unsigned long long>(accepted_before));
        }
        CHECK(matching_lots == 1);
        ++accepted_before;
    }
    CHECK(strategy_pending_orders_len(&host) == 0);

    // Close bar 2 to issue Z, then read its accepted handle and pending row
    // before its next-open fill. Its absolute expectation uses L and counted
    // accepted definitions, never the observed X incarnation plus one.
    const std::uint64_t expected_next = host.incarnation_origin + accepted_before;
    CHECK(host.stream_advance_time(start + 180'000));
    CHECK(host.last_error().empty());
    CHECK(host.incarnation_next_calls == 1);
    const auto next = accepted_definitions(host, host.incarnation_next_boundary);
    CHECK(next.size() == 1);
    if (next.size() == 1) {
        CHECK(next[0]->request.label == "Z");
        CHECK(next[0]->handle.incarnation == expected_next);
    }
    const auto pending_next = pending_rows(host);
    CHECK(pending_id_count(pending_next, "Z") == 1);
    const auto* z = pending_id(pending_next, "Z");
    CHECK(z != nullptr);
    if (z) CHECK(z->incarnation == expected_next);
    CHECK(host.stream_push_tick(TradeTick{start + 180'000, 2, 100.0, 1.0}));
    CHECK(host.last_error().empty());
    CHECK(fill_id_count(host, "Z") == 1);
    int next_lots = 0;
    for (const auto& lot : host.native_open_lots(missing)) {
        if (lot.entry_label != "Z") continue;
        ++next_lots;
        CHECK(lot.entry_incarnation == expected_next);
        CHECK(lot.signed_units == -seed_units);
        std::fprintf(stderr, "[%s] Z lot incarnation=%llu, expected=%llu, accepted before=%llu\n",
                     row_name, static_cast<unsigned long long>(lot.entry_incarnation),
                     static_cast<unsigned long long>(expected_next),
                     static_cast<unsigned long long>(accepted_before));
    }
    CHECK(next_lots == 1);
    CHECK(strategy_pending_orders_len(&host) == 0);
}

void check_scope_row(const ScopeRow& row) {
    row_name = row.name;
    ScopeProbeHost host(row.scenario);
    if (row.scenario == ScopeCase::ReversalCloseAllCapture) {
        // The host's historical run supports the seed's magnifier mode.
        // These inputs make the sell limit executable at the next open;
        // no observed trade price, handle or trade count is an oracle.
        const Bar tape[] = {
            {100.25, 100.25, 100.25, 100.25, 1, start},
            {101.5, 103.0, 101.5, 102.25, 1, start + 60'000},
            {102.25, 102.25, 102.0, 102.0, 1, start + 120'000},
        };
        host.run(tape, 3, "", "", true, 4, MagnifierDistribution::ENDPOINTS);
        CHECK(host.last_error().empty());
        CHECK(host.capture_sequence_calls == 1);
        CHECK(host.capture_release_checks == 1);
        return;
    }
    const Bar warmup[] = {
        {100, 100, 100, 100, 1, start},
        {100, 100, 100, 100, 1, start + 60'000},
    };
    const bool began = host.stream_begin(warmup, 2, "1", "1");
    CHECK(began);
    CHECK(host.last_error().empty());
    if (!began) return;

    if (incarnation_case(row.scenario)) {
        check_incarnation_row(host, row.scenario == ScopeCase::IncarnationSupersededDefinition);
        return;
    }
    const auto staged = pending_rows(host);
    if (row.scenario == ScopeCase::DifferentId) {
        // X and Y are both default-sized one-unit entries. The different-id
        // request must append behind the close, leaving both entries for the
        // release; dropping only the lookup key predicate makes Y overwrite X.
        CHECK(pending_id_count(staged, "X") == 1);
        CHECK(pending_id_count(staged, "Y") == 1);
        const auto* x = pending_id(staged, "X");
        const auto* y = pending_id(staged, "Y");
        CHECK(x != nullptr && std::isnan(x->qty));
        CHECK(y != nullptr && std::isnan(y->qty));
        CHECK(host.stream_push_tick(TradeTick{start + 120'000, 1, 100.0, 1.0}));
        CHECK(host.last_error().empty());
        CHECK(host.physical_position().signed_units == -2.0);
        CHECK(host.physical_position().lot_count == 2);
        CHECK(open_id_count(host, "X") == 1);
        CHECK(open_id_count(host, "Y") == 1);
        CHECK(open_id_units(host, "X") == seed_units);
        CHECK(open_id_units(host, "Y") == seed_units);
        CHECK(fill_id_count(host, "X") == 1);
        CHECK(fill_id_count(host, "Y") == 1);
    } else if (row.scenario == ScopeCase::QueuedReplacement) {
        // The second X is the declared two-unit replacement of the row
        // appended after close_all. The place_entry LOOKUP (not the key
        // capture) makes it one queued row: measured, this row stays green
        // with only the capture reverted; on the fully unfixed adapter the
        // lookup is absent and two X rows are queued, giving two X fills.
        CHECK(pending_id_count(staged, "X") == 1);
        const auto* x = pending_id(staged, "X");
        CHECK(x != nullptr);
        if (x) CHECK(x->qty == reissue_units);
        CHECK(host.stream_push_tick(TradeTick{start + 120'000, 1, 100.0, 1.0}));
        CHECK(host.last_error().empty());
        CHECK(host.physical_position().signed_units == -reissue_units);
        CHECK(host.physical_position().lot_count == 1);
        CHECK(open_id_count(host, "X") == 1);
        CHECK(open_id_units(host, "X") == reissue_units);
        CHECK(fill_id_count(host, "X") == 1);
    }
    CHECK(strategy_pending_orders_len(&host) == 0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) return 2;
    bool matched = false;
    for (const auto& row : rows) {
        if (argc == 2 && std::strcmp(argv[1], row.name) != 0) continue;
        matched = true;
        const int before = failures;
        check_row(row);
        std::fprintf(stderr, "[%s] %s (%d failed checks)\n",
                     row.name, failures == before ? "PASS" : "FAIL", failures - before);
    }
    for (const auto& row : scope_rows) {
        if (argc == 2 && std::strcmp(argv[1], row.name) != 0) continue;
        matched = true;
        const int before = failures;
        check_scope_row(row);
        std::fprintf(stderr, "[%s] %s (%d failed checks)\n",
                     row.name, failures == before ? "PASS" : "FAIL", failures - before);
    }
    if (!matched) {
        std::fprintf(stderr, "unknown CR-277 row\n");
        return 2;
    }
    return failures == 0 ? 0 : 1;
}
