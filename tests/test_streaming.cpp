#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__,     \
                         #cond);                                               \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

Bar flat_bar(double price, int64_t timestamp, double volume = 1.0) {
    return Bar{price, price, price, price, volume, timestamp};
}

TradeTick tick(int64_t timestamp, uint64_t id, double price,
               double qty = 1.0) {
    return TradeTick{timestamp, id, price, qty, false};
}

class ContinuityStrategy final : public BacktestEngine {
public:
    std::vector<bool> saw_islast;

    void on_bar(const Bar&) override {
        saw_islast.push_back(barstate_islast_);
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 1) strategy_close_all();
    }

    double position_size() const { return signed_position_size(); }
    std::size_t pending_count() const { return pending_orders_.size(); }
};

class StopStrategy final : public BacktestEngine {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 1) {
            strategy_entry("stop", true, na<double>(), 103.0);
        }
    }

    double entry_price() const { return position_entry_price_; }
    int64_t entry_time() const { return position_entry_time_; }
    double position_size() const { return signed_position_size(); }
};

class CaptureStrategy final : public BacktestEngine {
public:
    std::vector<Bar> bars;
    std::vector<int> indices;

    void on_bar(const Bar& bar) override {
        bars.push_back(bar);
        indices.push_back(bar_index_);
    }
};

void test_position_pending_order_and_equity_continue() {
    ContinuityStrategy strategy;
    const Bar warmup[] = {
        flat_bar(100.0, 0),
        flat_bar(101.0, 60'000),
    };

    CHECK(strategy.stream_begin(warmup, 2, "1", "1"));
    CHECK(strategy.last_error().empty());
    CHECK(strategy.stream_is_realtime());
    CHECK(near(strategy.position_size(), 1.0));
    CHECK(strategy.pending_count() == 1);
    CHECK(strategy.trade_count() == 0);
    CHECK(strategy.saw_islast.size() == 2);
    CHECK(!strategy.saw_islast[0]);
    CHECK(!strategy.saw_islast[1]);

    // The close order created on the final historical bar fills at the first
    // raw exchange print. A second run() would have erased both the open lot
    // and this pending order, so this is the core lifecycle regression test.
    CHECK(strategy.stream_push_tick(tick(120'123, 1, 110.0, 0.25)));
    CHECK(strategy.trade_count() == 1);
    CHECK(near(strategy.position_size(), 0.0));
    const Trade& trade = strategy.get_trade(0);
    CHECK(near(trade.entry_price, 101.0));
    CHECK(near(trade.exit_price, 110.0));
    CHECK(trade.entry_time == 60'000);
    CHECK(trade.exit_time == 120'123);
    CHECK(trade.entry_bar_index == 1);
    CHECK(trade.exit_bar_index == 2);
    CHECK(near(trade.pnl, 9.0));

    CHECK(strategy.stream_advance_time(180'000));
    CHECK(strategy.saw_islast.size() == 3);
    CHECK(strategy.saw_islast.back());

    ReportC report{};
    strategy.fill_report(&report);
    CHECK(report.input_bars_processed == 3);
    CHECK(report.script_bars_processed == 3);
    CHECK(report.total_trades == 1);
    CHECK(near(report.net_profit, 9.0));
    BacktestEngine::free_report(&report);
    CHECK(strategy.stream_end(false));
}

void test_raw_tick_gap_fill_uses_observed_price_and_time() {
    StopStrategy strategy;
    const Bar warmup[] = {
        flat_bar(100.0, 0),
        flat_bar(100.0, 60'000),
    };
    CHECK(strategy.stream_begin(warmup, 2, "1", "1"));
    CHECK(strategy.stream_push_tick(tick(120'010, 10, 100.0)));
    CHECK(near(strategy.position_size(), 0.0));

    // No synthetic interpolation from 100 to 105: the first observed print
    // beyond the 103 stop is 105, so a stop-market order gaps to 105.
    CHECK(strategy.stream_push_tick(tick(120'250, 11, 105.0)));
    CHECK(near(strategy.position_size(), 1.0));
    CHECK(near(strategy.entry_price(), 105.0));
    CHECK(strategy.entry_time() == 120'250);
}

void test_partial_mtf_aggregator_survives_handoff() {
    CaptureStrategy strategy;
    std::vector<Bar> warmup;
    for (int i = 0; i < 7; ++i) {
        warmup.push_back(flat_bar(static_cast<double>(i), i * 60'000LL));
    }

    CHECK(strategy.stream_begin(
        warmup.data(), static_cast<int>(warmup.size()), "1", "5"));
    CHECK(strategy.bars.size() == 1);
    CHECK(strategy.indices.size() == 1 && strategy.indices[0] == 0);
    CHECK(near(strategy.bars[0].open, 0.0));
    CHECK(near(strategy.bars[0].close, 4.0));

    CHECK(strategy.stream_push_tick(tick(420'000, 20, 7.0)));
    CHECK(strategy.stream_push_tick(tick(480'000, 21, 8.0)));
    CHECK(strategy.stream_push_tick(tick(540'000, 22, 9.0)));
    CHECK(strategy.stream_advance_time(600'000));

    CHECK(strategy.bars.size() == 2);
    CHECK(strategy.indices[1] == 1);
    // Minutes 5 and 6 came from historical OHLCV; 7, 8 and 9 came from raw
    // ticks. One 5-minute candle must span both sources without a reset.
    CHECK(strategy.bars[1].timestamp == 300'000);
    CHECK(near(strategy.bars[1].open, 5.0));
    CHECK(near(strategy.bars[1].close, 9.0));
    CHECK(near(strategy.bars[1].volume, 5.0));
}

void test_clock_materializes_quiet_bars() {
    CaptureStrategy strategy;
    const Bar warmup[] = {flat_bar(42.0, 0, 3.0)};
    CHECK(strategy.stream_begin(warmup, 1, "1", "1"));
    CHECK(strategy.stream_advance_time(240'000));

    CHECK(strategy.bars.size() == 4);
    for (std::size_t i = 1; i < strategy.bars.size(); ++i) {
        CHECK(near(strategy.bars[i].open, 42.0));
        CHECK(near(strategy.bars[i].close, 42.0));
        CHECK(near(strategy.bars[i].volume, 0.0));
    }
}

void test_rejects_replayed_or_out_of_order_ticks() {
    CaptureStrategy strategy;
    const Bar warmup[] = {flat_bar(100.0, 0)};
    CHECK(strategy.stream_begin(warmup, 1, "1", "1"));
    CHECK(strategy.stream_push_tick(tick(60'100, 100, 100.0)));
    CHECK(!strategy.stream_push_tick(tick(60'200, 100, 101.0)));
    CHECK(strategy.last_error().find("trade_id") != std::string::npos);
    CHECK(!strategy.stream_push_tick(tick(60'050, 101, 101.0)));
    CHECK(strategy.last_error().find("backwards") != std::string::npos);
}

}  // namespace

int main() {
    test_position_pending_order_and_equity_continue();
    test_raw_tick_gap_fill_uses_observed_price_and_time();
    test_partial_mtf_aggregator_survives_handoff();
    test_clock_materializes_quiet_bars();
    test_rejects_replayed_or_out_of_order_ticks();

    if (failures == 0) {
        std::puts("test_streaming: OK");
        return 0;
    }
    std::fprintf(stderr, "test_streaming: %d failures\n", failures);
    return 1;
}
