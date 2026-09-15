#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

// Public native-route twin for the legacy margin-call family.  Every position
// begins with a source command in on_source_bar; all observations come from
// closed trades, the physical-position projection, or the restored generated
// margin_liquidation_price surface.  Deliberately synthetic owner snapshots
// from the retired route are itemized in DELETION-LEDGER.md instead.

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int passed = 0;
int failed = 0;

#define CHECK(expr) do {                                                       \
    if (expr) ++passed; else {                                                 \
        ++failed; std::printf("FAIL %d %s\\n", __LINE__, #expr);             \
    }                                                                          \
} while (0)

bool near(double actual, double expected, double tolerance = 1e-6) {
    return std::abs(actual - expected) <= tolerance;
}

Bar bar(std::int64_t timestamp, double open, double high, double low, double close) {
    return {open, high, low, close, 1.0, timestamp};
}

class MarginHost : public source::PineStrategyHost {
public:
    const Trade& row(int index) const { return get_trade(index); }
    double position() const { return physical_position().signed_units; }
    double average() const { return physical_position().average_price; }
    double liquidation_price() const { return margin_liquidation_price(); }
};

class ShortPathHost final : public MarginHost {
public:
    explicit ShortPathHost(bool margin_enabled = true, double qty_step = 0.0) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = true;
        qty_step_ = qty_step;
        syminfo_mintick_ = 0.01;
        set_margin_call_enabled(margin_enabled);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("S", false, kNaN, kNaN, kNaN);
    }
};

class LeveragedLongHost final : public MarginHost {
public:
    LeveragedLongHost() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 20.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 50.0;
        process_orders_on_close_ = true;
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true, kNaN, kNaN, 20.0);
    }
};

class StreamShortHost final : public MarginHost {
public:
    StreamShortHost() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 10.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = true;
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("S", false, kNaN, kNaN, 10.0);
    }
};

int margin_rows(const MarginHost& host) {
    int count = 0;
    for (int index = 0; index < host.trade_count(); ++index) {
        if (host.row(index).exit_comment == "Margin call") ++count;
    }
    return count;
}

void test_short_adverse_path() {
    // The unchanged principal tape: a 100%-equity short fills at 100, then
    // the next source bar reaches high 105.  The source policy must issue the
    // 4x deficit slice at that adverse extreme through execute_current.
    const std::vector<Bar> tape = {
        bar(1000, 100.0, 100.0, 99.0, 100.0),
        bar(2000, 100.0, 105.0, 99.5, 104.0),
        bar(3000, 104.0, 130.0, 103.0, 128.0),
        bar(4000, 128.0, 140.0, 127.0, 139.0),
    };
    ShortPathHost host;
    host.run(tape.data(), static_cast<int>(tape.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() >= 1);
    CHECK(margin_rows(host) >= 1);
    CHECK(host.row(0).exit_id == "__margin_call__");
    CHECK(host.row(0).exit_comment == "Margin call");
    CHECK(near(host.row(0).entry_price, 100.0));
    CHECK(near(host.row(0).exit_price, 105.0));
    CHECK(near(host.row(0).qty, 3.80952381, 1e-4));
    CHECK(host.row(0).entry_time == 1000);
    CHECK(host.row(0).exit_time == 2000);
    CHECK(host.position() < 0.0);
    CHECK(std::abs(host.position()) < 10.0);
    CHECK(near(host.average(), 100.0));
}

void test_liquidation_price_and_no_adverse_call() {
    const std::vector<Bar> tape = {
        bar(1000, 100.0, 100.0, 99.0, 100.0),
        bar(2000, 100.0, 100.0, 99.5, 100.0),
    };
    ShortPathHost host;
    host.run(tape.data(), static_cast<int>(tape.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 0);
    CHECK(margin_rows(host) == 0);
    CHECK(near(host.position(), -10.0));
    CHECK(near(host.average(), 100.0));
    CHECK(near(host.liquidation_price(), 100.0));
    CHECK(std::isfinite(host.liquidation_price()));
}

void test_margin_switch_is_observable() {
    const std::vector<Bar> tape = {
        bar(1000, 100.0, 100.0, 99.0, 100.0),
        bar(2000, 100.0, 105.0, 99.5, 104.0),
        bar(3000, 104.0, 200.0, 103.0, 199.0),
    };
    ShortPathHost host(/*margin_enabled=*/false);
    host.run(tape.data(), static_cast<int>(tape.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 0);
    CHECK(margin_rows(host) == 0);
    CHECK(near(host.position(), -10.0));
    CHECK(near(host.average(), 100.0));
    CHECK(std::isfinite(host.liquidation_price()));
}

void test_grid_floor_before_four_x() {
    const std::vector<Bar> tape = {
        bar(1000, 100.0, 100.0, 99.0, 100.0),
        bar(2000, 100.0, 105.0, 99.5, 104.0),
        bar(3000, 104.0, 130.0, 103.0, 128.0),
    };
    ShortPathHost host(/*margin_enabled=*/true, /*qty_step=*/0.5);
    host.run(tape.data(), static_cast<int>(tape.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() >= 2);
    CHECK(host.row(0).exit_comment == "Margin call");
    CHECK(near(host.row(0).qty, 2.0));
    CHECK(near(host.row(0).exit_price, 105.0));
    CHECK(host.row(1).exit_comment == "Margin call");
    CHECK(near(host.row(1).qty, 8.0));
    CHECK(near(host.row(1).exit_price, 130.0));
    CHECK(near(host.position(), 0.0));
}

void test_leveraged_long_adverse_low() {
    const std::vector<Bar> tape = {
        bar(1000, 100.0, 100.0, 100.0, 100.0),
        bar(2000, 100.0, 101.0, 95.0, 96.0),
    };
    LeveragedLongHost host;
    host.run(tape.data(), static_cast<int>(tape.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    CHECK(host.row(0).exit_id == "__margin_call__");
    CHECK(host.row(0).exit_comment == "Margin call");
    CHECK(near(host.row(0).entry_price, 100.0));
    CHECK(near(host.row(0).exit_price, 95.0));
    CHECK(near(host.row(0).qty, 4.2105263157894735, 1e-6));
    CHECK(near(host.position(), 15.789473684210526, 1e-6));
    CHECK(host.liquidation_price() < 100.0);
}

void test_stream_tick_margin_checkpoint() {
    // A28(3): the native tick callback precedes matching.  After a public
    // source-command short has filled from the warmup tape, the adverse print
    // itself must be sufficient to produce the margin slice; no synthetic
    // source bar or retired pending-loop drive is used.
    const Bar warmup[] = {
        bar(0, 100.0, 100.0, 100.0, 100.0),
        bar(60000, 100.0, 100.0, 100.0, 100.0),
    };
    StreamShortHost host;
    CHECK(host.stream_begin(warmup, 2, "1", "1"));
    CHECK(near(host.position(), -10.0));
    const TradeTick adverse{120000, 1, 105.0, 1.0};
    CHECK(host.stream_push_tick(adverse));
    CHECK(host.trade_count() == 1);
    CHECK(host.row(0).exit_id == "__margin_call__");
    CHECK(host.row(0).exit_comment == "Margin call");
    CHECK(near(host.row(0).exit_price, 105.0));
    CHECK(near(host.row(0).qty, 3.80952381, 1e-4));
    CHECK(host.position() < 0.0);
    CHECK(host.stream_end(false));
}

}  // namespace

int main() {
    test_short_adverse_path();
    test_liquidation_price_and_no_adverse_call();
    test_margin_switch_is_observable();
    test_grid_floor_before_four_x();
    test_leveraged_long_adverse_low();
    test_stream_tick_margin_checkpoint();
    std::printf("%d passed, %d failed\\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
