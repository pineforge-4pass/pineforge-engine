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
    return TradeTick{timestamp, id, price, qty};
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

class StopLimitStrategy final : public BacktestEngine {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("SL", true, 103.0, 105.0);
        }
    }

    double position_size() const { return signed_position_size(); }
    double entry_price() const { return position_entry_price_; }
    bool activated() const {
        return pending_orders_.size() == 1
            && pending_orders_.front().stop_limit_activated;
    }
};

class QuietPendingMarketStrategy final : public BacktestEngine {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("quiet", true);
    }

    double position_size() const { return signed_position_size(); }
};

class ProcessOnCloseProvenanceStrategy final : public BacktestEngine {
public:
    ProcessOnCloseProvenanceStrategy() { process_orders_on_close_ = true; }

    void on_bar(const Bar&) override {
        if (bar_index_ == 1) strategy_entry("poc", true);
    }

    double position_size() const { return signed_position_size(); }
    int64_t entry_time() const { return position_entry_time_; }
};

class UnsupportedProfileStrategy final : public BacktestEngine {
public:
    enum class Profile { EveryTick, EveryHistoryTick, OrderFills };

    explicit UnsupportedProfileStrategy(Profile profile) {
        calc_on_every_tick_ = profile == Profile::EveryTick;
        calc_on_every_history_tick_ = profile == Profile::EveryHistoryTick;
        calc_on_order_fills_ = profile == Profile::OrderFills;
    }

    void on_bar(const Bar&) override { ++calls; }
    int calls = 0;
};

class ReplacementIdentityStrategy final : public BacktestEngine {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ <= 1) {
            strategy_entry("same", true, na<double>(), 200.0);
            CHECK(pending_orders_.size() == 1);
            revisions.push_back(pending_orders_.front().command_revision_id);
            legs.push_back(pending_orders_.front().order_leg_id);
            priorities.push_back(pending_orders_.front().priority_sequence);
        }
    }

    std::vector<uint64_t> revisions;
    std::vector<uint64_t> legs;
    std::vector<uint64_t> priorities;
};

class IndependentTrailStrategy final : public BacktestEngine {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 1) {
            strategy_exit("tight", "L", na<double>(), na<double>(),
                          100.0, 50.0, na<double>(), na<double>(), "", 0.5);
            strategy_exit("wide", "L", na<double>(), na<double>(),
                          100.0, 100.0, na<double>(), na<double>(), "", 0.5);
        }
    }

    double position_size() const { return signed_position_size(); }
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

void test_data_driven_gap_policy_skips_tickless_intervals() {
    CaptureStrategy strategy;
    const Bar warmup[] = {flat_bar(42.0, 0, 3.0)};
    CHECK(strategy.stream_set_gap_policy(1));
    CHECK(strategy.stream_begin(warmup, 1, "1", "1"));
    CHECK(strategy.stream_advance_time(240'000));
    CHECK(strategy.bars.size() == 1);
    CHECK(!strategy.stream_set_gap_policy(0));

    CHECK(strategy.stream_push_tick(tick(240'100, 1, 43.0)));
    CHECK(strategy.stream_advance_time(300'000));
    CHECK(strategy.bars.size() == 2);
    CHECK(strategy.bars.back().timestamp == 240'000);
    CHECK(near(strategy.bars.back().close, 43.0));
}

void test_stop_limit_activation_persists_across_trade_events() {
    StopLimitStrategy strategy;
    const Bar warmup[] = {flat_bar(100.0, 0)};
    CHECK(strategy.stream_begin(warmup, 1, "1", "1"));

    // The stop is crossed first, but the buy limit is below this event. The
    // order must become an active limit instead of forgetting activation.
    CHECK(strategy.stream_push_tick(tick(60'100, 1, 106.0)));
    CHECK(near(strategy.position_size(), 0.0));
    CHECK(strategy.activated());

    CHECK(strategy.stream_push_tick(tick(60'200, 2, 103.0)));
    CHECK(near(strategy.position_size(), 1.0));
    CHECK(near(strategy.entry_price(), 103.0));
}

void test_quiet_clock_does_not_fill_pending_market_order() {
    QuietPendingMarketStrategy strategy;
    const Bar warmup[] = {flat_bar(100.0, 0)};
    CHECK(strategy.stream_begin(warmup, 1, "1", "1"));

    CHECK(strategy.stream_advance_time(120'000));
    CHECK(near(strategy.position_size(), 0.0));
    CHECK(strategy.trade_count() == 0);

    // The clock carried the mark but supplied no executable price. The first
    // actual trade is therefore the pending market order's fill event.
    CHECK(strategy.stream_push_tick(tick(120'100, 1, 104.0)));
    CHECK(near(strategy.position_size(), 1.0));
}

void test_process_on_close_uses_last_trade_provenance() {
    ProcessOnCloseProvenanceStrategy strategy;
    const Bar warmup[] = {flat_bar(100.0, 0)};
    CHECK(strategy.stream_begin(warmup, 1, "1", "1"));
    CHECK(strategy.stream_push_tick(tick(60'100, 10, 101.0)));
    CHECK(strategy.stream_push_tick(tick(119'900, 11, 105.0)));
    CHECK(strategy.stream_advance_time(120'000));

    CHECK(near(strategy.position_size(), 1.0));
    CHECK(strategy.entry_time() == 119'900);
}

void test_unsupported_profiles_reject_before_warmup() {
    const Bar warmup[] = {flat_bar(100.0, 0)};
    for (UnsupportedProfileStrategy::Profile profile : {
             UnsupportedProfileStrategy::Profile::EveryTick,
             UnsupportedProfileStrategy::Profile::EveryHistoryTick,
             UnsupportedProfileStrategy::Profile::OrderFills}) {
        UnsupportedProfileStrategy strategy(profile);
        CHECK(!strategy.stream_begin(warmup, 1, "1", "1"));
        CHECK(strategy.last_error().find("stream profile unsupported")
              != std::string::npos);
        CHECK(strategy.calls == 0);
        CHECK(strategy.trade_count() == 0);
    }
}

void test_replacement_gets_fresh_identity_but_keeps_priority() {
    ReplacementIdentityStrategy strategy;
    const Bar warmup[] = {
        flat_bar(100.0, 0),
        flat_bar(101.0, 60'000),
    };
    CHECK(strategy.stream_begin(warmup, 2, "1", "1"));
    CHECK(strategy.revisions.size() == 2);
    CHECK(strategy.legs.size() == 2);
    CHECK(strategy.priorities.size() == 2);
    CHECK(strategy.revisions[0] != strategy.revisions[1]);
    CHECK(strategy.legs[0] != strategy.legs[1]);
    CHECK(strategy.priorities[0] == strategy.priorities[1]);
}

void test_realtime_trailing_state_is_per_order_leg() {
    IndependentTrailStrategy strategy;
    const Bar warmup[] = {
        flat_bar(100.0, 0),
        flat_bar(100.0, 60'000),
    };
    CHECK(strategy.stream_begin(warmup, 2, "1", "1"));
    CHECK(near(strategy.position_size(), 1.0));

    CHECK(strategy.stream_push_tick(tick(120'100, 1, 101.2)));
    CHECK(strategy.stream_push_tick(tick(120'200, 2, 101.5)));
    CHECK(near(strategy.position_size(), 1.0));

    // The 0.50-offset leg fires first. The 1.00-offset sibling retains its
    // own watermark and remains live until the later, deeper retrace.
    CHECK(strategy.stream_push_tick(tick(120'300, 3, 100.9)));
    CHECK(near(strategy.position_size(), 0.5));
    CHECK(strategy.trade_count() == 1);
    CHECK(near(strategy.get_trade(0).exit_price, 100.9));

    CHECK(strategy.stream_push_tick(tick(120'400, 4, 100.4)));
    CHECK(near(strategy.position_size(), 0.0));
    CHECK(strategy.trade_count() == 2);
    CHECK(near(strategy.get_trade(1).exit_price, 100.4));
}

void test_lifecycle_report_is_deterministic_across_batching() {
    const Bar warmup[] = {flat_bar(100.0, 0)};
    const TradeTick events[] = {
        tick(60'100, 1, 106.0),
        tick(60'200, 2, 103.0),
    };
    StopLimitStrategy one_by_one;
    StopLimitStrategy batched;
    CHECK(one_by_one.stream_begin(warmup, 1, "1", "1"));
    CHECK(batched.stream_begin(warmup, 1, "1", "1"));
    CHECK(one_by_one.stream_push_tick(events[0]));
    CHECK(one_by_one.stream_push_tick(events[1]));
    CHECK(batched.stream_push_ticks(events, 2));

    ReportC a{};
    ReportC b{};
    one_by_one.fill_report(&a);
    batched.fill_report(&b);
    CHECK(a.order_event_count == b.order_event_count);
    CHECK(a.order_event_hash == b.order_event_hash);
    CHECK(a.order_event_dropped == 0);
    CHECK(a.order_events_len == static_cast<int64_t>(a.order_event_count));
    CHECK(a.order_events_len >= 3);  // create, activate, fill

    bool saw_activation = false;
    bool saw_fill = false;
    uint64_t leg_id = 0;
    for (int64_t i = 0; i < a.order_events_len; ++i) {
        const pf_order_event_t& event = a.order_events[i];
        CHECK(event.transition_sequence == static_cast<uint64_t>(i + 1));
        CHECK(event.id != nullptr && std::string(event.id) == "SL");
        if (leg_id == 0) leg_id = event.order_leg_id;
        CHECK(event.order_leg_id == leg_id);
        if (event.transition == static_cast<int32_t>(OrderTransition::ACTIVATED)) {
            saw_activation = true;
            CHECK(event.event_sequence == 1);
        }
        if (event.transition == static_cast<int32_t>(OrderTransition::FILLED)) {
            saw_fill = true;
            CHECK(event.fill_id != 0);
            CHECK(event.event_sequence == 2);
        }
    }
    CHECK(saw_activation);
    CHECK(saw_fill);
    BacktestEngine::free_report(&a);
    BacktestEngine::free_report(&b);
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

}  // namespace

int main() {
    test_position_pending_order_and_equity_continue();
    test_raw_tick_gap_fill_uses_observed_price_and_time();
    test_partial_mtf_aggregator_survives_handoff();
    test_clock_materializes_quiet_bars();
    test_clock_skips_out_of_session_intervals();
    test_data_driven_gap_policy_skips_tickless_intervals();
    test_stop_limit_activation_persists_across_trade_events();
    test_quiet_clock_does_not_fill_pending_market_order();
    test_process_on_close_uses_last_trade_provenance();
    test_unsupported_profiles_reject_before_warmup();
    test_replacement_gets_fresh_identity_but_keeps_priority();
    test_realtime_trailing_state_is_per_order_leg();
    test_lifecycle_report_is_deterministic_across_batching();
    test_rejects_replayed_or_out_of_order_ticks();

    if (failures == 0) {
        std::puts("test_streaming: OK");
        return 0;
    }
    std::fprintf(stderr, "test_streaming: %d failures\n", failures);
    return 1;
}
