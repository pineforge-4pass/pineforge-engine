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

enum class ScopeCase { DifferentId, QueuedReplacement, ReleaseTime, ParentMismatch };

struct ScopeRow {
    const char* name;
    ScopeCase scenario;
};

constexpr ScopeRow scope_rows[] = {
    {"lookup-different-id", ScopeCase::DifferentId},
    {"lookup-queued-replacement", ScopeCase::QueuedReplacement},
    {"release-time-key", ScopeCase::ReleaseTime},
};

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
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(1.0);
        fixture_retain_all_events();
    }

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
            case ScopeCase::ReleaseTime:
                // This is the transferred live NaN-quantity entry. There is
                // deliberately no same-callback reissue in this row.
                strategy_entry("X", true);
                strategy_close_all();
                break;
            case ScopeCase::ParentMismatch:
                strategy_close_all();
                strategy_entry("X", false);
                strategy_close_all();
                strategy_entry("X", false, missing, missing, reissue_units);
                break;
            }
            return;
        }
        if (pine_bar_index() == 2 && scenario_ == ScopeCase::ReleaseTime) {
            // The stream invokes this next-bar callback before matching the
            // deferred close. The fixed key lets the release replace this
            // same-id request; an empty key leaves both X requests live.
            strategy_entry("X", true, missing, missing, reissue_units);
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

void check_parent_scope_guard() {
    ScopeProbeHost host(ScopeCase::ParentMismatch);
    const Bar warmup[] = {
        {100, 100, 100, 100, 1, start},
        {100, 100, 100, 100, 1, start + 60'000},
    };
    const bool began = host.stream_begin(warmup, 2, "1", "1");
    CHECK(began);
    CHECK(host.last_error().empty());
    if (!began) return;
    // The first X belongs to the first full close. A second full close is
    // then current, so the second X must append instead of replacing the
    // first one. Removing only the paired-parent conjunct replaces it.
    const auto staged = pending_rows(host);
    CHECK(pending_id_count(staged, "X") == 2);
    CHECK(host.stream_push_tick(TradeTick{start + 120'000, 1, 100.0, 1.0}));
    CHECK(host.last_error().empty());
    CHECK(host.physical_position().signed_units == -reissue_units);
    CHECK(host.physical_position().lot_count == 1);
    CHECK(open_id_count(host, "X") == 1);
    CHECK(open_id_units(host, "X") == reissue_units);
    const auto remaining = pending_rows(host);
    CHECK(pending_id_count(remaining, "X") == 1);
}

void check_scope_row(const ScopeRow& row) {
    row_name = row.name;
    ScopeProbeHost host(row.scenario);
    const Bar warmup[] = {
        {100, 100, 100, 100, 1, start},
        {100, 100, 100, 100, 1, start + 60'000},
    };
    const bool began = host.stream_begin(warmup, 2, "1", "1");
    CHECK(began);
    CHECK(host.last_error().empty());
    if (!began) return;

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
        check_parent_scope_guard();
    } else if (row.scenario == ScopeCase::QueuedReplacement) {
        // The second X is the declared two-unit replacement of the row
        // appended after close_all. The key capture makes it one queued row;
        // the unfixed adapter leaves the first one under key 0 and produces
        // two X fills.
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
    } else {
        // The first X is a same-side default-sized live entry, so close_all
        // transfers it as a NaN-quantity row. The next-bar X is an explicit
        // two-unit command. A same-id release replaces that command and
        // releases exactly the transferred one-unit X.
        CHECK(pending_id_count(staged, "X") == 1);
        const auto* x = pending_id(staged, "X");
        CHECK(x != nullptr && std::isnan(x->qty));
        CHECK(host.stream_push_tick(TradeTick{start + 120'000, 1, 100.0, 1.0}));
        CHECK(host.last_error().empty());
        CHECK(host.physical_position().signed_units == seed_units);
        CHECK(host.physical_position().lot_count == 1);
        CHECK(open_id_count(host, "X") == 1);
        CHECK(open_id_units(host, "X") == seed_units);
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
