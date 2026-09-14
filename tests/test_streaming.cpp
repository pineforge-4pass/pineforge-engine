#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <limits>
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
    return TradeTick{timestamp, id, price, qty};
}

class ContinuityStrategy final : public pineforge::source::PineStrategyHost {
public:
    std::vector<bool> saw_islast;

    void on_source_bar(const Bar&) override {
        saw_islast.push_back(barstate_islast_);
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 1) strategy_close_all();
    }

    double position_size() const { return signed_position_size(); }
    std::size_t pending_count() const { return pending_orders_.size(); }
};

class StopStrategy final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 1) {
            strategy_entry("stop", true, na<double>(), 103.0);
        }
    }

    double entry_price() const { return position_entry_price_; }
    int64_t entry_time() const { return position_entry_time_; }
    double position_size() const { return signed_position_size(); }
};

class CaptureStrategy final : public pineforge::source::PineStrategyHost {
public:
    std::vector<Bar> bars;
    std::vector<int> indices;

    void on_source_bar(const Bar& bar) override {
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
    // normalized source record. A second run() would have erased both the open
    // lot and this pending order, so this is the core lifecycle regression test.
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

void test_clock_skips_out_of_session_intervals() {
    CaptureStrategy strategy;
    strategy.set_syminfo_timezone("UTC");
    strategy.set_syminfo_session("0000-0001");
    const Bar warmup[] = {flat_bar(42.0, 0, 3.0)};
    CHECK(strategy.stream_begin(warmup, 1, "1", "1"));
    CHECK(strategy.stream_advance_time(240'000));

    // Minute zero is the configured session. Minutes one through three are
    // closed and must not become synthetic tradable bars.
    CHECK(strategy.bars.size() == 1);
}

void test_rejects_replayed_or_out_of_order_ticks() {
    CaptureStrategy strategy;
    const Bar warmup[] = {flat_bar(100.0, 0)};
    CHECK(strategy.stream_begin(warmup, 1, "1", "1"));
    CHECK(strategy.stream_push_tick(tick(60'100, 100, 100.0)));
    CHECK(!strategy.stream_push_tick(tick(60'200, 100, 101.0)));
    CHECK(strategy.last_error().find("sequence") != std::string::npos);
    CHECK(!strategy.stream_push_tick(tick(60'050, 101, 101.0)));
    CHECK(strategy.last_error().find("backwards") != std::string::npos);
}

class LedgerStrategy final : public pineforge::source::PineStrategyHost {
public:
    explicit LedgerStrategy(bool pooc = false) { process_orders_on_close_ = pooc; }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true, na<double>(), na<double>(), 2, "open long");
        if (bar_index_ == 1) strategy_close("L", "partial", 1);
        if (bar_index_ == 2) strategy_entry("S", false, na<double>(), na<double>(), 3, "reverse");
        if (bar_index_ == 3) strategy_close_all();
    }
};

void test_confirmed_bars_match_batch_and_recovery() {
    const Bar bars[] = {flat_bar(100, 0), flat_bar(101, 60'000),
        flat_bar(103, 120'000), flat_bar(102, 180'000), flat_bar(99, 240'000)};
    for (const bool pooc : {false, true}) {
        LedgerStrategy batch(pooc), stream(pooc), replay(pooc);
        batch.run(bars, 5, "1", "1", false);
        CHECK(stream.stream_begin(bars, 1, "1", "1"));
        CHECK(replay.stream_begin(bars, 1, "1", "1"));
        CHECK(stream.stream_order_actions_len() == 0);
        CHECK(stream.stream_state_hash() == replay.stream_state_hash());
        uint64_t sequence = 0;
        for (int i = 1; i < 5; ++i) {
            CHECK(stream.stream_push_bar(bars[i]));
            CHECK(replay.stream_push_bar(bars[i]));
            CHECK(stream.stream_state_hash() == replay.stream_state_hash());
            CHECK(stream.stream_order_actions_len() == replay.stream_order_actions_len());
            for (int j = 0; j < stream.stream_order_actions_len(); ++j) {
                pf_stream_order_action_t action{};
                CHECK(strategy_stream_order_action_get(&stream, j, &action) == 0);
                CHECK(action.sequence == ++sequence);
                CHECK(action.quantity > 0 && std::isfinite(action.price));
                const auto& r = replay.stream_order_action_at(j);
                CHECK(action.order_id == r.order_id);
                CHECK(action.comment == r.comment);
            }
            const auto hash = stream.stream_state_hash();
            stream.stream_order_actions_clear();
            replay.stream_order_actions_clear();
            CHECK(stream.stream_state_hash() == hash);
        }
        CHECK(stream.trade_count() == batch.trade_count());
        CHECK(near(stream.live_position_size(), batch.live_position_size()));
        CHECK(near(stream.live_current_equity(), batch.live_current_equity()));
        for (int i = 0; i < batch.trade_count(); ++i) {
            const Trade& a = stream.get_trade(i), &b = batch.get_trade(i);
            CHECK(a.entry_time == b.entry_time && a.exit_time == b.exit_time);
            CHECK(a.entry_id == b.entry_id && a.exit_id == b.exit_id);
            CHECK(a.entry_comment == b.entry_comment && a.exit_comment == b.exit_comment);
            CHECK(near(a.qty, b.qty) && near(a.entry_price, b.entry_price) && near(a.exit_price, b.exit_price));
        }
        CHECK(sequence >= 4);
        CHECK(stream.stream_end(false));
        CHECK(stream.stream_order_actions_len() == 0); // no range-end fiction
    }
}

void test_order_action_exact_tick_time_and_comments() {
    LedgerStrategy strategy;
    const Bar warmup[] = {flat_bar(100, 0)};
    CHECK(strategy.stream_begin(warmup, 1, "1", "1"));
    CHECK(strategy.stream_push_tick(tick(60'123, 1, 105, 2)));
    CHECK(strategy.stream_order_actions_len() == 1);
    const auto a = strategy.stream_order_action_at(0);
    CHECK(a.sequence == 1 && a.timestamp_ms == 60'123 && a.bar_index == 1);
    CHECK(a.is_entry && a.is_long && a.quantity == 2 && a.price == 105);
    CHECK(a.order_id == "L" && a.comment == "open long");
    CHECK(a.entry_incarnation != 0);
    strategy.stream_order_actions_clear();
    CHECK(strategy.stream_advance_time(120'000));
    CHECK(strategy.stream_push_tick(tick(120'234, 2, 110)));
    CHECK(strategy.stream_order_actions_len() == 1);
    const auto b = strategy.stream_order_action_at(0);
    CHECK(b.sequence == 2 && b.timestamp_ms == 120'234 && b.bar_index == 2);
    CHECK(!b.is_entry && b.is_long && b.quantity == 1 && b.price == 110);
    CHECK(b.order_id == "__close__L" && b.comment == "partial");
    CHECK(b.entry_incarnation == a.entry_incarnation);
}

class SameBarRoundtrip final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("L", true, na<double>(), na<double>(), 2, "entry");
            strategy_exit("bracket", "L", 110, 90, na<double>(), na<double>(),
                na<double>(), 100, "exit");
        }
    }
};

void test_same_input_entry_exit_survives_flat_position() {
    SameBarRoundtrip strategy;
    const Bar warmup[] = {flat_bar(100, 0)};
    CHECK(strategy.stream_begin(warmup, 1, "1", "1"));
    CHECK(strategy.stream_push_bar(Bar{100, 115, 95, 105, 2, 60'000}));
    CHECK(strategy.live_position_size() == 0);
    CHECK(strategy.stream_order_actions_len() == 2);
    const auto& a = strategy.stream_order_action_at(0);
    const auto& b = strategy.stream_order_action_at(1);
    CHECK(a.is_entry && !b.is_entry && a.sequence == 1 && b.sequence == 2);
    CHECK(a.order_id == "L" && b.order_id == "bracket");
    CHECK(a.comment == "entry" && b.comment == "exit");
    CHECK(a.quantity == 2 && b.quantity == 2 && a.price == 100 && b.price == 110);
    CHECK(a.entry_incarnation == b.entry_incarnation);
}

class PyramidClose final : public pineforge::source::PineStrategyHost {
public:
    PyramidClose() { pyramiding_ = 3; }
    void on_source_bar(const Bar&) override {
        if (bar_index_ < 2) strategy_entry("L", true, na<double>(), na<double>(), bar_index_ + 1);
        if (bar_index_ == 2) strategy_close_all();
    }
};

void test_fifo_exit_fragments_keep_physical_order() {
    PyramidClose strategy;
    const Bar warmup[] = {flat_bar(100, 0)};
    CHECK(strategy.stream_begin(warmup, 1, "1", "1"));
    CHECK(strategy.stream_push_bar(flat_bar(101, 60'000)));
    CHECK(strategy.stream_push_bar(flat_bar(102, 120'000)));
    CHECK(strategy.stream_push_bar(flat_bar(103, 180'000)));
    CHECK(strategy.stream_order_actions_len() == 4);
    const auto& a = strategy.stream_order_action_at(0);
    const auto& b = strategy.stream_order_action_at(1);
    const auto& c = strategy.stream_order_action_at(2);
    const auto& d = strategy.stream_order_action_at(3);
    CHECK(a.is_entry && b.is_entry && !c.is_entry && !d.is_entry);
    CHECK(a.quantity == 1 && b.quantity == 2 && c.quantity == 1 && d.quantity == 2);
    CHECK(a.entry_incarnation == c.entry_incarnation);
    CHECK(b.entry_incarnation == d.entry_incarnation);
    CHECK(a.entry_incarnation != b.entry_incarnation);
    CHECK(c.sequence == 3 && d.sequence == 4);
}

class UnsupportedCoof final : public pineforge::source::PineStrategyHost {
public:
    UnsupportedCoof() { calc_on_order_fills_ = true; }
    void on_source_bar(const Bar&) override {}
};

void test_unsupported_stream_configuration_fails_closed() {
    const Bar warmup[] = {flat_bar(100, 0)};
    UnsupportedCoof coof;
    CHECK(!coof.stream_begin(warmup, 1, "1", "1"));
    CHECK(coof.last_error().find("calc_on_order_fills") != std::string::npos);
    CaptureStrategy probe;
    probe.set_realtime_tail(true, 10);
    CHECK(!probe.stream_begin(warmup, 1, "1", "1"));
    CHECK(probe.last_error().find("probe/tail overrides") != std::string::npos);
}

void test_confirmed_mtf_and_rejected_input() {
    CaptureStrategy strategy;
    const Bar warmup[] = {flat_bar(10, 0), flat_bar(11, 60'000)};
    CHECK(strategy.stream_begin(warmup, 2, "1", "3"));
    CHECK(strategy.bars.empty());
    CHECK(strategy.stream_push_bar(Bar{12, 15, 11, 14, 3, 120'000}));
    CHECK(strategy.bars.size() == 1);
    CHECK(strategy.bars[0].timestamp == 0 && strategy.bars[0].open == 10);
    CHECK(strategy.bars[0].high == 15 && strategy.bars[0].low == 10);
    CHECK(strategy.bars[0].close == 14 && strategy.bars[0].volume == 5);
    const auto hash = strategy.stream_state_hash();
    CHECK(!strategy.stream_push_tick(tick(180'000, 1, 14)));
    CHECK(!strategy.stream_advance_time(240'000));
    CHECK(!strategy.stream_push_bar(flat_bar(14, 240'000))); // missing minute
    CHECK(!strategy.stream_push_bar(flat_bar(14, 180'001))); // wrong grid
    CHECK(!strategy.stream_push_bar(Bar{14, 13, 12, 14, 1, 180'000}));
    CHECK(!strategy.stream_push_bar(flat_bar(14, std::numeric_limits<int64_t>::max())));
    CHECK(strategy.stream_state_hash() == hash);
    CHECK(strategy.stream_push_bar(flat_bar(14, 180'000)));
    CHECK(strategy.stream_state_hash() != hash); // partial aggregate is visible
    CaptureStrategy ticks;
    CHECK(ticks.stream_begin(warmup, 2, "1", "3"));
    CHECK(ticks.stream_push_tick(tick(120'000, 1, 12)));
    CHECK(!ticks.stream_push_bar(flat_bar(12, 120'000)));
    CHECK(strategy_stream_api_version() == 1);
    CHECK(strategy_stream_order_actions_len(nullptr) == -1);
    pf_stream_order_action_t out{};
    CHECK(strategy_stream_order_action_get(nullptr, 0, &out) == -1);
    CHECK(strategy_stream_order_action_get(&strategy, -1, &out) == -1);
    CHECK(strategy_stream_order_action_get(&strategy, 0, &out) == -1);
}

}  // namespace

int main() {
    test_position_pending_order_and_equity_continue();
    test_raw_tick_gap_fill_uses_observed_price_and_time();
    test_partial_mtf_aggregator_survives_handoff();
    test_clock_materializes_quiet_bars();
    test_clock_skips_out_of_session_intervals();
    test_rejects_replayed_or_out_of_order_ticks();
    test_confirmed_bars_match_batch_and_recovery();
    test_order_action_exact_tick_time_and_comments();
    test_confirmed_mtf_and_rejected_input();
    test_same_input_entry_exit_survives_flat_position();
    test_fifo_exit_fragments_keep_physical_order();
    test_unsupported_stream_configuration_fails_closed();

    if (failures == 0) {
        std::puts("test_streaming: OK");
        return 0;
    }
    std::fprintf(stderr, "test_streaming: %d failures\n", failures);
    return 1;
}
