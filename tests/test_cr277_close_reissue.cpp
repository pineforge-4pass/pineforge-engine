// CR-277: public pending-order, native-event and trade observations.
//
// The probe rows preserve the draft Pine sequence. Their expected replacement
// result comes from the engine's same-id contract, NOT a TradingView export:
// TV equality for entry/close/re-entry is still owed.
//
// On b3d483d6c1 close("L") never reaches the faulty close() push: that push is
// inside id.empty(). Also, pyramiding=1 batches the first X, so close_all()
// transfers it through the already-correct PendingSameBarCommand sibling.
// The site rows therefore use pyramiding=3 to reach the live-entry pushes,
// close("") for the first site, and withdraw the close AFTER reissue. That
// lets both entries reach a common flush before either fills, exposing the
// lost key as two accepted/live X orders instead of one replacement.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

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

enum class Close { None, Named, Empty, All };

struct Row {
    const char* name;
    Close close;
    int pyramiding;
    bool withdraw_close;
};

constexpr Row rows[] = {
    {"probe-close", Close::Named, 1, false},
    {"probe-close-all", Close::All, 1, false},
    {"control-close", Close::None, 1, false},
    {"control-close-all", Close::None, 1, false},
    {"site-close-empty", Close::Empty, 3, true},
    {"site-close-all", Close::All, 3, true},
    {"site-control", Close::None, 3, false},
};

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
        config.default_qty_value = 1.0;
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

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true);
        if (pine_bar_index() != 1) return;
        ++sequence_calls;
        seed_ready = physical_position().signed_units == 1.0
            && physical_position().lot_count == 1
            && open_trade_entry_id(0) == "L";
        for (const auto& event : native_events(0))
            flush_boundary = event.ordinal;
        strategy_entry("X", false);
        switch (row_.close) {
        case Close::None: break;
        case Close::Named: strategy_close("L"); break;
        case Close::Empty: strategy_close(""); break;
        case Close::All: strategy_close_all(); break;
        }
        if (row_.close != Close::None) {
            for (const auto& event : native_events(0))
                flush_boundary = event.ordinal;
        }
        strategy_entry("X", false, missing, missing, 2.0);
        if (row_.withdraw_close) {
            // Source cancellation retires the adapter's parent as well as
            // the native request. It leaves the queued X definitions intact.
            strategy_cancel(row_.close == Close::Empty
                ? "__pine_close_all" : "__close__");
        }
    }

private:
    Row row_;
};

std::vector<pf_pending_order_v1_t> pending_x(ReissueHost& host) {
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

int accepted_x_after_boundary(const ReissueHost& host) {
    int count = 0;
    for (const auto& event : host.native_events(host.flush_boundary)) {
        if (!event.command) continue;
        const auto* accepted = std::get_if<native_order::AcceptedEvent>(&*event.command);
        if (accepted && accepted->request().label == "X") ++count;
    }
    // ReplacedEvent is an amendment of that pending order, not another
    // independently accepted order. With a close, the old pre-close X was
    // cancelled before the boundary. Without a close, include the first
    // accepted X because it remains the replacement's target.
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

    // stream_begin has flushed the final historical callback but has not
    // consumed the following open. Inspect actual pending instructions here.
    const auto pending = pending_x(host);
    const int flushed = accepted_x_after_boundary(host);
    std::fprintf(stderr, "[%s] staged X=%zu, newly accepted X=%d, qty:",
                 row_name, pending.size(), flushed);
    for (const auto& order : pending) std::fprintf(stderr, " %.17g", order.qty);
    std::fprintf(stderr, "\n");
    CHECK(pending.size() == 1);
    if (pending.size() == 1) CHECK(pending[0].qty == 2.0);
    CHECK(flushed == 1);

    // The stream avoids end-of-range liquidation/report artifacts. The first
    // tick is exactly the next bar's open, at the same price as the callback.
    CHECK(host.stream_push_tick(TradeTick{start + 120'000, 1, 100.0, 1.0}));
    CHECK(host.last_error().empty());
    int x_fills = 0;
    double x_opened = 0.0;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        const auto* fill = std::get_if<native_order::ExecutionAppliedEvent>(&*event.command);
        if (!fill || fill->request().label != "X") continue;
        ++x_fills;
        x_opened += fill->opened_units;
    }
    // With the close kept, close_all flattens L first and the sole X opens
    // two short units. Otherwise the replacement transacts two against the
    // held long of one, leaving one short (the existing entry/entry rule).
    const bool close_first = row.close == Close::All && !row.withdraw_close;
    const double expected_short = close_first ? 2.0 : 1.0;
    std::fprintf(stderr, "[%s] X fills=%d, opened=%.17g, position=%.17g, lots=%zu\n",
                 row_name, x_fills, x_opened, host.physical_position().signed_units,
                 host.physical_position().lot_count);
    CHECK(accepted_x_after_boundary(host) == 1);
    CHECK(x_fills == 1);
    CHECK(x_opened == expected_short);
    CHECK(host.physical_position().signed_units == -expected_short);
    CHECK(host.physical_position().lot_count == 1);
    CHECK(host.open_trade_entry_id(0) == "X");
    CHECK(host.open_trade_size(0) == expected_short);
    CHECK(host.open_trade_entry_price(0) == 100.0);
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const auto& trade = host.get_trade(0);
        CHECK(trade.entry_id == "L");
        CHECK(trade.qty == 1.0);
        CHECK(trade.entry_price == 100.0);
        CHECK(trade.exit_price == 100.0);
        CHECK(trade.exit_id == (close_first ? "__close__" : "X"));
        CHECK(trade.entry_time == start + 60'000);
        CHECK(trade.exit_time == start + 120'000);
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
        check_row(row);
    }
    if (!matched) {
        std::fprintf(stderr, "unknown CR-277 row\n");
        return 2;
    }
    return failures == 0 ? 0 : 1;
}
