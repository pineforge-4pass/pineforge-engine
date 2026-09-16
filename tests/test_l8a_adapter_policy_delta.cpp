// R4-D L8a native-route witnesses ported from REVIEW-FABLE-DELTA.md and its
// executed /tmp/fable-delta probes.  Each selector is registered as its own
// CTest row so a regression reports the exact reviewed finding.
#include <pineforge/source/pine_native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
const char* active_case = "aggregate";
int checks = 0;
int failures = 0;

#define CHECK(expr) do {                                                        \
    ++checks;                                                                   \
    if (!(expr)) {                                                              \
        ++failures;                                                             \
        std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n", active_case,           \
                     __FILE__, __LINE__, #expr);                                \
    }                                                                           \
} while (false)

bool near(double actual, double expected, double tolerance = 1e-9) {
    return std::isfinite(actual) && std::abs(actual - expected) <= tolerance;
}

Bar flat(std::int64_t timestamp, double price = 100.0) {
    return {price, price, price, price, 1.0, timestamp};
}

Bar ohlc(std::int64_t timestamp, double open, double high,
         double low, double close) {
    return {open, high, low, close, 1.0, timestamp};
}

SymInfo symbol(double quantity_step = 0.0) {
    SymInfo info;
    info.mintick = 0.01;
    info.pointvalue = 1.0;
    info.qty_step = quantity_step;
    info.timezone = "UTC";
    info.session = "24x7";
    return info;
}

void run_rich(source::PineNativeHost& host, const std::vector<Bar>& bars,
              const SymInfo& info = symbol()) {
    InputsMap inputs;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", inputs, info);
    CHECK(host.last_error().empty());
}

class DrawdownCloseFold final : public source::PineNativeHost {
public:
    explicit DrawdownCloseFold(double threshold) {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1'000.0;
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        set_pine_risk_max_drawdown(threshold, false);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("A", true, kNaN, kNaN, 1'000.0);
        if (pine_bar_index() == 2) strategy_close_all();
        if (pine_bar_index() == 4)
            strategy_entry("B", true, kNaN, kNaN, 1'000.0);
    }
};

void drawdown_once() {
    // /tmp/fable-delta/laneD1/probe/drawdown2.cpp row B.  The bar-1 close
    // marks only a 1,000 drawdown; its transient open is 60,000 below peak.
    DrawdownCloseFold host(10'000.0);
    const std::vector<Bar> tape = {
        ohlc(0, 100, 100, 100, 100),
        ohlc(60'000, 40, 100, 40, 99),
        ohlc(120'000, 99, 100, 99, 100),
        flat(180'000), flat(240'000), flat(300'000),
    };
    run_rich(host, tape);
    CHECK(host.trade_count() == 1);
    CHECK(near(host.live_position_size(), 1'000.0));
}

void risk_latch_scope() {
    // Unlike the first witness, this close mark really breaches the rule.
    // Legacy check_risk_allow_entry still permits the later close_all.
    DrawdownCloseFold host(10'000.0);
    const std::vector<Bar> tape = {
        flat(0, 100),
        ohlc(60'000, 100, 100, 40, 40),
        flat(120'000, 40), flat(180'000, 40), flat(240'000, 40),
    };
    run_rich(host, tape);
    CHECK(host.trade_count() == 1);
    CHECK(near(host.live_position_size(), 0.0));
}

class CapPrefillProbe final : public source::PineNativeHost {
public:
    enum class Shape { Reversal, Add };

    CapPrefillProbe(Shape shape, bool starts_long)
        : shape_(shape), starts_long_(starts_long) {
        source::PineStrategyConfig config;
        config.initial_capital = 100'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = shape == Shape::Add ? 2 : 1;
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        set_pine_risk_max_intraday_filled_orders(2);
        set_syminfo_metadata("intraday_cap_skip_noop_market_fills", 1.0);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("FIRST", starts_long_);
        if (pine_bar_index() == 1) {
            strategy_entry(shape_ == Shape::Add ? "ADD" : "REVERSE",
                           shape_ == Shape::Add ? starts_long_ : !starts_long_);
        }
    }

private:
    Shape shape_;
    bool starts_long_ = true;
};

void cap_prefill() {
    const std::vector<Bar> tape = {
        ohlc(1'743'379'200'000LL, 100, 102, 98, 101),
        ohlc(1'743'380'100'000LL, 101, 103, 99, 102),
        ohlc(1'743'381'000'000LL, 102, 104, 100, 103),
    };
    for (const auto shape : {CapPrefillProbe::Shape::Reversal,
                             CapPrefillProbe::Shape::Add}) {
        for (const bool starts_long : {true, false}) {
            CapPrefillProbe host(shape, starts_long);
            run_rich(host, tape);
            CHECK(host.trade_count() == 2);
            CHECK(near(host.live_position_size(), 0.0));
        }
    }
}

class CapPhaseProbe final : public source::PineNativeHost {
public:
    void on_source_bar(const Bar&) override {
        calculations.push_back(fixture_cap_calculation());
    }

    std::vector<compat::pine::Calculation> calculations;
};

void cap_stream_phase() {
    CapPhaseProbe host;
    const Bar warmup = flat(0);
    CHECK(host.stream_begin(&warmup, 1, "1", "1"));
    CHECK(host.stream_push_bar(flat(60'000, 101)));
    CHECK(host.stream_end(false));
    CHECK(host.calculations.size() >= 2U);
    if (host.calculations.size() >= 2U) {
        CHECK(host.calculations.front().stream_warmup);
        CHECK(host.calculations.front().stream_idle);
        CHECK(!host.calculations.back().stream_warmup);
        CHECK(!host.calculations.back().stream_idle);
    }
}

class DirectionGateProbe final : public source::PineNativeHost {
public:
    enum class Change { SetBeforeFill, ClearBeforeFill };

    explicit DirectionGateProbe(Change change) : change_(change) {
        source::PineStrategyConfig config;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 2;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("L", true, kNaN, kNaN, 1.0);
        if (pine_bar_index() == 1) {
            if (change_ == Change::ClearBeforeFill) set_pine_risk_direction(1);
            strategy_entry("S", false, 150.0, kNaN, 1.0);
        }
        if (pine_bar_index() == 2) {
            set_pine_risk_direction(
                change_ == Change::SetBeforeFill ? 1 : 0);
        }
    }

private:
    Change change_;
};

void direction_gate() {
    const std::vector<Bar> spike = {
        flat(1'000), flat(2'000), flat(3'000),
        flat(4'000, 200), flat(5'000, 200), flat(6'000, 200),
    };
    DirectionGateProbe set(DirectionGateProbe::Change::SetBeforeFill);
    run_rich(set, spike);
    CHECK(near(set.live_position_size(), 0.0));
    CHECK(set.trade_count() == 1);

    DirectionGateProbe cleared(DirectionGateProbe::Change::ClearBeforeFill);
    run_rich(cleared, spike);
    CHECK(near(cleared.live_position_size(), -1.0));
    CHECK(cleared.trade_count() == 1);
}

class ExplicitPercentSizing final : public source::PineNativeHost {
public:
    ExplicitPercentSizing(CommissionType type, double value) {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.commission_type = static_cast<int>(type);
        config.commission_value = value;
        config.pyramiding = 2;
        config.process_orders_on_close = true;
        config.margin_long = 0.0;
        config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_margin_call_enabled(false);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("A", false, kNaN, kNaN, 1.0);
        if (pine_bar_index() == 1) {
            strategy_entry("B", false, kNaN, kNaN, 100.0,
                           "", "", 0,
                           static_cast<int>(QtyType::PERCENT_OF_EQUITY));
        }
    }
};

void explicit_percent_sizing() {
    const std::vector<Bar> tape = {flat(2'000), flat(62'000), flat(122'000)};
    ExplicitPercentSizing cash(CommissionType::CASH_PER_ORDER, 5.0);
    run_rich(cash, tape);
    CHECK(near(cash.live_position_size(), -11.0, 1e-10));

    ExplicitPercentSizing percent(CommissionType::PERCENT, 1.0);
    run_rich(percent, tape);
    CHECK(near(percent.live_position_size(), -10.8910891089109, 1e-10));
}

class MarginLatchProbe final : public source::PineNativeHost {
public:
    MarginLatchProbe() {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.process_orders_on_close = true;
        config.margin_long = 50.0;
        config.margin_short = 50.0;
        config.pyramiding = 2;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("FAKE", true, kNaN, kNaN, 1.0);
        if (pine_bar_index() == 1)
            strategy_close("FAKE", "Margin call");
        if (pine_bar_index() == 2) {
            strategy_entry("HELD", true, kNaN, kNaN, 20.0);
            strategy_entry("COMPETING", true, 50.0, kNaN, 1.0);
        }
    }
};

void margin_call_latch() {
    MarginLatchProbe host;
    const std::vector<Bar> tape = {
        flat(0, 100), flat(60'000, 100), flat(120'000, 100),
        ohlc(180'000, 100, 100, 90, 100), flat(240'000, 100),
    };
    run_rich(host, tape);
    CHECK(host.trade_count() >= 2);
    CHECK(host.live_position_size() < 20.0);
    if (host.trade_count() >= 1)
        CHECK(host.get_trade(0).exit_comment == "Margin call");
}

class AffordabilitySurplusProbe final : public source::PineNativeHost {
public:
    explicit AffordabilitySurplusProbe(bool process_on_close = false) {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0022196;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.0;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.pyramiding = 1;
        config.process_orders_on_close = process_on_close;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.00001);
        qty_step_ = 0.01;
    }

    void on_source_bar(const Bar& bar) override {
        if (bar.timestamp == 1'743'495'300'000LL)
            strategy_entry("A", true);
        if (bar.timestamp == 1'743'498'900'000LL)
            strategy_entry("B", false);
        if (bar.timestamp == 1'743'508'800'000LL)
            strategy_close_all();
    }
};

void affordability_surplus() {
    // The Fable P1-15 discriminator is the legacy famr-adm-revL-L23 tape.
    // Its signal-close margin event removes exactly one long unit; the next
    // bar's close-only reversal must retain that frozen one-unit surplus as a
    // new short. These are the literal TV/ab9714be rows recorded by the tape.
    const std::vector<Bar> bars = {
        ohlc(1'743'494'400'000LL, 1.08017, 1.08078, 1.08006, 1.08064),
        ohlc(1'743'495'300'000LL, 1.08065, 1.08103, 1.08035, 1.08094),
        ohlc(1'743'496'200'000LL, 1.08094, 1.08151, 1.08079, 1.08108),
        ohlc(1'743'497'100'000LL, 1.08109, 1.08182, 1.08104, 1.08178),
        ohlc(1'743'498'000'000LL, 1.08176, 1.08292, 1.08166, 1.08240),
        ohlc(1'743'498'900'000LL, 1.08241, 1.08245, 1.08166, 1.08228),
        ohlc(1'743'499'800'000LL, 1.08228, 1.08248, 1.08166, 1.08168),
        ohlc(1'743'500'700'000LL, 1.08166, 1.08213, 1.08148, 1.08174),
        ohlc(1'743'501'600'000LL, 1.08178, 1.08186, 1.07902, 1.07969),
        ohlc(1'743'502'500'000LL, 1.07972, 1.08075, 1.07960, 1.08056),
        ohlc(1'743'503'400'000LL, 1.08058, 1.08088, 1.08022, 1.08024),
        ohlc(1'743'504'300'000LL, 1.08025, 1.08084, 1.07982, 1.07982),
        ohlc(1'743'505'200'000LL, 1.07982, 1.07986, 1.07908, 1.07949),
        ohlc(1'743'506'100'000LL, 1.07950, 1.07954, 1.07864, 1.07874),
        ohlc(1'743'507'000'000LL, 1.07873, 1.07892, 1.07783, 1.07826),
        ohlc(1'743'507'900'000LL, 1.07825, 1.07886, 1.07798, 1.07812),
        ohlc(1'743'508'800'000LL, 1.07812, 1.07889, 1.07812, 1.07878),
        ohlc(1'743'509'700'000LL, 1.07880, 1.07906, 1.07830, 1.07902),
    };
    AffordabilitySurplusProbe host;
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    if (host.trade_count() != 3) {
        std::fprintf(stderr, "affordability_surplus rows=%d pos=%.12f\n",
                     host.trade_count(), host.live_position_size());
        for (int index = 0; index < host.trade_count(); ++index) {
            const auto& trade = host.get_trade(index);
            std::fprintf(stderr,
                         "  row %d side=%s qty=%.12f entry=%.12f exit=%.12f comment=%s\n",
                         index, trade.is_long ? "long" : "short", trade.qty,
                         trade.entry_price, trade.exit_price,
                         trade.exit_comment.c_str());
        }
    }
    CHECK(host.trade_count() == 3);
    if (host.trade_count() == 3) {
        CHECK(host.get_trade(0).is_long);
        CHECK(near(host.get_trade(0).qty, 1.0, 1e-9));
        CHECK(near(host.get_trade(0).exit_price, 1.08228, 1e-9));
        CHECK(host.get_trade(0).exit_comment == "Margin call");
        CHECK(host.get_trade(1).is_long);
        CHECK(near(host.get_trade(1).qty, 925'119.73, 1e-6));
        CHECK(!host.get_trade(2).is_long);
        CHECK(near(host.get_trade(2).qty, 1.0, 1e-9));
        CHECK(near(host.get_trade(2).entry_price, 1.08228, 1e-9));
    }

    // pine_fills.cpp:6408 excludes POOC from the one-unit surplus branch.
    // The same tape therefore closes the carried long without creating the
    // third, one-unit short row.
    AffordabilitySurplusProbe pooc(true);
    pooc.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(pooc.last_error().empty());
    CHECK(pooc.trade_count() == 2);
    for (int index = 0; index < pooc.trade_count(); ++index)
        CHECK(pooc.get_trade(index).is_long);
}

class CapResidualProbe final : public source::PineNativeHost {
public:
    void on_source_bar(const Bar&) override {
        calculations.push_back(fixture_cap_calculation());
    }
    std::vector<compat::pine::Calculation> calculations;
};

class CapBrokerBarProbe final : public source::PineNativeHost {
public:
    CapBrokerBarProbe() {
        source::PineStrategyConfig config;
        config.initial_capital = 100'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
        set_pine_risk_max_intraday_filled_orders(1);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("STOP", true, kNaN, 105.0, 1.0);
    }
};

void cap_residuals() {
    // Inactive sampler arguments install an inert generic intrabar path. They
    // never enabled the legacy Pine bar magnifier and must not disable the
    // cap's ordinary-bar branch.
    CapResidualProbe host;
    const std::vector<Bar> bars = {flat(0), flat(60'000)};
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1",
             false, 5, MagnifierDistribution::COSINE);
    CHECK(host.last_error().empty());
    CHECK(!host.calculations.empty());
    for (const auto& calculation : host.calculations)
        CHECK(!calculation.magnifier);

    // pine_fills.cpp:6287 supplied the actual broker sub-bar O/H/L to the
    // cap's high/low promotion. The surrounding script bar reaches 150, but
    // the sub-bar which fills this long stop reaches only 106.
    CapBrokerBarProbe broker_bar;
    const std::vector<Bar> lower = {
        ohlc(0, 100, 101, 99, 100),
        ohlc(60'000, 100, 101, 99, 100),
        ohlc(120'000, 100, 106, 99, 100),
        ohlc(180'000, 100, 150, 90, 100),
    };
    broker_bar.run(lower.data(), static_cast<int>(lower.size()), "1", "2",
                   true, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(broker_bar.last_error().empty());
    CHECK(broker_bar.trade_count() == 1);
    if (broker_bar.trade_count() == 1) {
        CHECK(near(broker_bar.get_trade(0).entry_price, 105.0));
        CHECK(near(broker_bar.get_trade(0).exit_price, 106.0));
        CHECK(broker_bar.get_trade(0).exit_comment
              == "Close Position (Max number of filled orders in one day)");
    }
}

class ProductDayKeyProbe final : public source::PineNativeHost {
public:
    void on_source_bar(const Bar&) override {}

    std::int64_t fixture_key(std::int64_t timestamp) const {
        return fixture_chart_day_key(timestamp);
    }
    std::int64_t product_key(std::int64_t timestamp) const {
        return adapter_.chart_day_key(timestamp);
    }
    void mutate_live_timezone_without_restaging(const std::string& timezone) {
        chart_timezone_ = timezone;
    }
};

void product_day_key() {
    constexpr std::int64_t kUtc1530 = 1'743'435'000'000LL;
    constexpr std::int64_t kUtc1600 = 1'743'436'800'000LL;
    ProductDayKeyProbe host;
    host.set_chart_timezone("Asia/Taipei");
    const std::vector<Bar> bars = {flat(kUtc1530), flat(kUtc1600)};
    run_rich(host, bars);
    // Distinguish the retained product staging from the A20 helper's former
    // second implementation. The production ledger must remain Taipei-keyed.
    host.mutate_live_timezone_without_restaging("UTC");
    CHECK(host.product_key(kUtc1530) == 3103);
    CHECK(host.product_key(kUtc1600) == 104);
    CHECK(host.fixture_key(kUtc1530) == host.product_key(kUtc1530));
    CHECK(host.fixture_key(kUtc1600) == host.product_key(kUtc1600));
}

struct Case {
    const char* name;
    void (*run)();
};

constexpr Case cases[] = {
    {"drawdown_once", drawdown_once},
    {"risk_latch_scope", risk_latch_scope},
    {"cap_prefill", cap_prefill},
    {"cap_stream_phase", cap_stream_phase},
    {"direction_gate", direction_gate},
    {"explicit_percent_sizing", explicit_percent_sizing},
    {"margin_call_latch", margin_call_latch},
    {"affordability_surplus", affordability_surplus},
    {"cap_residuals", cap_residuals},
    {"product_day_key", product_day_key},
};

} // namespace

int main(int argc, char** argv) {
    bool selected = argc == 1;
    for (const auto& item : cases) {
        if (argc > 1 && std::strcmp(argv[1], item.name) != 0) continue;
        selected = true;
        active_case = item.name;
        item.run();
    }
    if (!selected) {
        std::fprintf(stderr, "unknown L8a witness: %s\n", argv[1]);
        return 2;
    }
    std::printf("R4-D L8a adapter policy delta: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
