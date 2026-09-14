#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {
int checks = 0;
int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);   \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

Bar bar(double price, int64_t timestamp, double volume = 1.0) {
    return Bar{price, price, price, price, volume, timestamp};
}

class Probe final : public pineforge::source::PineStrategyHost {
public:
    std::vector<Bar> observed;

    Probe() {
        initial_capital_ = 10000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1;
    }

    void on_source_bar(const Bar& value) override {
        observed.push_back(value);
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 1) strategy_close_all();
    }

    double position() const { return signed_position_size(); }
};

struct Snapshot {
    uint64_t stream_hash;
    uint64_t broker_hash;
    bool realtime;
    double position;
    int trades;
    std::vector<Bar> observed;
    std::vector<StreamOrderAction> actions;
};

Snapshot snapshot(const Probe& engine) {
    Snapshot result{engine.stream_state_hash(), engine.broker_state_hash(),
                    engine.stream_is_realtime(), engine.position(),
                    engine.trade_count(), engine.observed, {}};
    for (int i = 0; i < engine.stream_order_actions_len(); ++i)
        result.actions.push_back(engine.stream_order_action_at(i));
    return result;
}

void same(const Snapshot& actual, const Snapshot& expected) {
    // Error text is a per-call diagnostic, not the economic/stream state.
    CHECK(actual.stream_hash == expected.stream_hash);
    CHECK(actual.broker_hash == expected.broker_hash);
    CHECK(actual.realtime == expected.realtime);
    CHECK(actual.position == expected.position);
    CHECK(actual.trades == expected.trades);
    CHECK(actual.observed.size() == expected.observed.size());
    for (size_t i = 0; i < actual.observed.size() && i < expected.observed.size(); ++i) {
        const auto& a = actual.observed[i];
        const auto& b = expected.observed[i];
        CHECK(a.timestamp == b.timestamp && a.open == b.open && a.high == b.high
              && a.low == b.low && a.close == b.close && a.volume == b.volume);
    }
    CHECK(actual.actions.size() == expected.actions.size());
    for (size_t i = 0; i < actual.actions.size() && i < expected.actions.size(); ++i) {
        const auto& a = actual.actions[i];
        const auto& b = expected.actions[i];
        CHECK(a.sequence == b.sequence && a.timestamp_ms == b.timestamp_ms
              && a.bar_index == b.bar_index && a.is_entry == b.is_entry
              && a.is_long == b.is_long && a.quantity == b.quantity
              && a.price == b.price && a.order_id == b.order_id
              && a.comment == b.comment && a.entry_incarnation == b.entry_incarnation
              && a.closed_trade_index == b.closed_trade_index);
    }
}

void begin(Probe& engine) {
    const Bar warmup[] = {bar(100, 0)};
    CHECK(engine.stream_begin(warmup, 1, "1", "1"));
}

void test_rejected_begin_preserves_live_lifecycle(bool confirmed_bars,
                                                bool invalid_arguments) {
    Probe engine, control;
    begin(engine);
    begin(control);
    if (confirmed_bars) {
        CHECK(engine.stream_push_bar(bar(100, 60000)));
        CHECK(control.stream_push_bar(bar(100, 60000)));
    } else {
        CHECK(engine.stream_push_tick({60100, 10, 100, 1}));
        CHECK(control.stream_push_tick({60100, 10, 100, 1}));
    }
    CHECK(engine.position() == 1);
    CHECK(engine.stream_order_actions_len() == 1);
    const auto before = snapshot(engine);
    const Bar different_warmup[] = {bar(700, 0), bar(800, 60000)};
    CHECK(!engine.stream_begin(invalid_arguments ? nullptr : different_warmup,
                               invalid_arguments ? -1 : 2,
                               invalid_arguments ? "invalid" : "1", "1"));
    CHECK(engine.last_error().find("already realtime") != std::string::npos);
    same(snapshot(engine), before);

    // The rejected setup must neither replay warmup nor disable later output.
    bool continued;
    if (confirmed_bars) {
        continued = engine.stream_push_bar(bar(102, 120000));
        CHECK(control.stream_push_bar(bar(102, 120000)));
    } else {
        continued = engine.stream_push_tick({60200, 11, 101, 1});
        CHECK(control.stream_push_tick({60200, 11, 101, 1}));
        if (continued) {
            CHECK(engine.stream_advance_time(120000));
            CHECK(control.stream_advance_time(120000));
            CHECK(engine.stream_push_tick({120100, 12, 102, 1}));
            CHECK(control.stream_push_tick({120100, 12, 102, 1}));
        }
    }
    CHECK(continued);
    if (!continued) return;  // The baseline failure is already established.
    CHECK(engine.last_error().empty());
    CHECK(engine.position() == 0);
    CHECK(engine.stream_order_actions_len() == 2);
    same(snapshot(engine), snapshot(control));
}

void test_overflow_rejection_preserves_forming_bar_and_cursors() {
    Probe engine, control;
    begin(engine);
    begin(control);
    const double largest = std::numeric_limits<double>::max();
    CHECK(engine.stream_push_tick({60100, 10, 100, largest}));
    CHECK(control.stream_push_tick({60100, 10, 100, largest}));
    const auto before = snapshot(engine);

    // Each quantity is finite; only this interval's aggregate is unrepresentable.
    CHECK(!engine.stream_push_tick({60200, 11, 110, largest}));
    CHECK(engine.last_error().find("volume overflow") != std::string::npos);
    same(snapshot(engine), before);

    // Reuse the rejected sequence and timestamp with a valid quantity. The
    // result must equal a stream that never received the rejected input.
    const bool continued = engine.stream_push_tick({60200, 11, 101, 0});
    CHECK(continued);
    if (!continued) return;
    CHECK(control.stream_push_tick({60200, 11, 101, 0}));
    CHECK(engine.stream_advance_time(120000));
    CHECK(control.stream_advance_time(120000));
    CHECK(engine.observed.size() == 2);
    if (engine.observed.size() == 2) {
        const auto& formed = engine.observed[1];
        CHECK(formed.timestamp == 60000 && formed.open == 100 && formed.high == 101
              && formed.low == 100 && formed.close == 101 && formed.volume == largest);
    }
    CHECK(engine.stream_push_tick({120100, 12, 102, 0}));
    CHECK(control.stream_push_tick({120100, 12, 102, 0}));
    CHECK(engine.stream_order_actions_len() == 2);
    same(snapshot(engine), snapshot(control));
}

void test_new_interval_does_not_add_previous_volume() {
    Probe engine;
    begin(engine);
    const double largest = std::numeric_limits<double>::max();
    CHECK(engine.stream_push_tick({60100, 10, 100, largest}));
    CHECK(engine.stream_push_tick({120100, 11, 101, largest}));
    CHECK(engine.stream_advance_time(180000));
    CHECK(engine.observed.size() == 3);
    if (engine.observed.size() == 3) {
        CHECK(engine.observed[1].volume == largest);
        CHECK(engine.observed[2].volume == largest);
    }
}
}  // namespace

int main() {
    for (bool bars : {false, true})
        for (bool invalid : {false, true})
            test_rejected_begin_preserves_live_lifecycle(bars, invalid);
    test_overflow_rejection_preserves_forming_bar_and_cursors();
    test_new_interval_does_not_add_previous_volume();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
