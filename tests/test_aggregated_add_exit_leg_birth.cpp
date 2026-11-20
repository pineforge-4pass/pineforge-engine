// Aggregated chart: an exit order that a fill places or re-arms at the bucket's open starts working at that fill.
//
// Target (two constructions): on a chart aggregated from finer bars, an exit order that the fill of a same-id market
// add re-arms, and an exit order that a first entry's fill places, are born at that fill's point, exactly as on the
// plain chart, so the two constructions book the same rows.
// Construction: four 10-minute buckets (UTC, 24x7) given as forty 1-minute bars (input "1", script "10") and as the
// four equivalent 10-minute bars (input "10", script "10").
// Construction one, the add:
//   bucket 0: entry "Long" 2 units, exit T1 (1 unit, stop 90, limit 120)       -> entry fills at bucket 1's open
//   bucket 1: same-id market add 1 unit, T1 called again with stop 99.00      -> add fills at bucket 2's open 100.00
//   bucket 2: O 100.00 H 100.50 L 98.00 C 100.20; |H-O| < |O-L| so the path is open -> high -> low -> close, and the
//             low segment crosses 99.00 after the open
//   bucket 3: O 98.50 (below the stop) ... C 98.60
// Plain chart: every point of bucket 2 sits at the chart's decision floor, so T1 fills at 99.00 on bar 2.
// Aggregated: input delivery raises the floor to bucket 2's completion before the seal walks its modeled path, whose
// open point is dated at the bucket's first input. Unless make_command_context counts that open point as current,
// the leg the add's fill re-arms is born at the floor, the low segment (dated at the first input as well) is refused
// by the birth gate, and the leg fills later with the low 98.00 sampled into the lot's drawdown first.
// Construction two, the first entry, on the same buckets:
//   bucket 0: entry "Long" 1 unit, exit TP (limit 100.20, stop 90)             -> entry fills at bucket 1's open 100.10
//   bucket 1: O 100.10 H 100.25 L 99.85 C 100.00; |H-O| < |O-L| so the path is open -> high -> low -> close, and the
//             first leg crosses 100.20 right after the open
// Plain chart: the take-profit fills at 100.20 on bar 1, the bar of the entry itself. Aggregated, without the open
// point counting as current, the exit the entry's fill places is born at the floor, bucket 1 cannot fill it, and it
// fills on bucket 2's rise instead.
#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

const double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000LL;

// One-minute (open, high, low, close) in cents, ten per bucket.
struct M { int o, h, l, c; };
const M kMinutes[40] = {
    // bucket 0: 100.00 .. 100.10, H 100.20 L 99.90
    {10000, 10010, 9995, 10005}, {10005, 10015, 9990, 10000}, {10000, 10020, 9995, 10010},
    {10010, 10015, 10000, 10005}, {10005, 10010, 9995, 10000}, {10000, 10010, 9990, 10005},
    {10005, 10015, 10000, 10010}, {10010, 10015, 10000, 10005}, {10005, 10015, 10000, 10010},
    {10010, 10015, 10005, 10010},
    // bucket 1: 100.10 .. 100.00, H 100.25 L 99.85
    {10010, 10025, 10005, 10020}, {10020, 10025, 10010, 10015}, {10015, 10020, 10000, 10005},
    {10005, 10010, 9985, 9990},   {9990, 10000, 9985, 9995},    {9995, 10005, 9990, 10000},
    {10000, 10010, 9995, 10005},  {10005, 10010, 9995, 10000},  {10000, 10005, 9990, 9995},
    {9995, 10005, 9990, 10000},
    // bucket 2: O 100.00 H 100.50 L 98.00 C 100.20 (high in minute 1, low in minute 5)
    {10000, 10020, 9995, 10015},  {10015, 10050, 10010, 10030}, {10030, 10035, 9950, 9960},
    {9960, 9965, 9880, 9890},     {9890, 9900, 9830, 9840},     {9840, 9850, 9800, 9820},
    {9820, 9900, 9815, 9890},     {9890, 9960, 9885, 9950},     {9950, 10010, 9945, 10000},
    {10000, 10025, 9995, 10020},
    // bucket 3: O 98.50 H 98.80 L 98.40 C 98.60
    {9850, 9870, 9840, 9860},     {9860, 9880, 9850, 9870},     {9870, 9875, 9855, 9860},
    {9860, 9865, 9845, 9850},     {9850, 9860, 9840, 9855},     {9855, 9865, 9850, 9860},
    {9860, 9870, 9855, 9865},     {9865, 9875, 9855, 9860},     {9860, 9865, 9850, 9855},
    {9855, 9865, 9850, 9860},
};

std::vector<Bar> minutes() {
    std::vector<Bar> out;
    for (int i = 0; i < 40; ++i) {
        Bar b{};
        b.timestamp = kStart + kMinute * i;
        b.open = kMinutes[i].o / 100.0;
        b.high = kMinutes[i].h / 100.0;
        b.low = kMinutes[i].l / 100.0;
        b.close = kMinutes[i].c / 100.0;
        b.volume = 1.0;
        out.push_back(b);
    }
    return out;
}

std::vector<Bar> buckets(const std::vector<Bar>& one) {
    std::vector<Bar> out;
    for (int k = 0; k < 4; ++k) {
        Bar b = one[static_cast<std::size_t>(k * 10)];
        for (int j = 1; j < 10; ++j) {
            const Bar& m = one[static_cast<std::size_t>(k * 10 + j)];
            if (m.high > b.high) b.high = m.high;
            if (m.low < b.low) b.low = m.low;
            b.close = m.close;
            b.volume += m.volume;
        }
        out.push_back(b);
    }
    return out;
}

enum class Mode { Add, FirstEntry };

class Host final : public source::PineStrategyHost {
public:
    explicit Host(Mode mode = Mode::Add) : mode_(mode) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        source::PineStrategyConfig config;
        config.initial_capital = 100000;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1;
        config.pyramiding = 2;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        if (mode_ == Mode::FirstEntry) {
            if (bar_index_ == 0) {
                strategy_entry("Long", true, kNaN, kNaN, 1.0);
                strategy_exit("LongTP", "Long", 100.20, 90.0, kNaN, kNaN, kNaN, 100.0, "TP Exit", 1.0);
            }
            return;
        }
        if (bar_index_ == 0) {
            strategy_entry("Long", true, kNaN, kNaN, 2.0);
            strategy_exit("LongT1", "Long", 120.0, 90.0, kNaN, kNaN, kNaN, 100.0, "T1 Exit", 1.0);
        } else if (bar_index_ == 1) {
            strategy_entry("Long", true, kNaN, kNaN, 1.0);
            strategy_exit("LongT1", "Long", 120.0, 99.0, kNaN, kNaN, kNaN, 100.0, "T1 Exit", 1.0);
        }
    }

private:
    Mode mode_;
};

} // namespace

int main() {
    const std::vector<Bar> one = minutes();
    const std::vector<Bar> ten = buckets(one);
    CHECK(ten[2].open == 100.0 && ten[2].high == 100.5 && ten[2].low == 98.0 && ten[3].open == 98.5);

    Host chart;
    chart.run(ten.data(), static_cast<int>(ten.size()), "10", "10", false);
    Host aggregated;
    aggregated.run(one.data(), static_cast<int>(one.size()), "1", "10", false);
    CHECK(chart.last_error().empty());
    CHECK(aggregated.last_error().empty());

    // The plain chart's own row: the stop leg fills at 99.00 on bar 2 (its low segment).
    CHECK(chart.trade_count() >= 1);
    if (chart.trade_count() >= 1) {
        CHECK(chart.get_trade(0).exit_bar_index == 2);
        CHECK(chart.get_trade(0).exit_price == 99.0);
    }

    CHECK(aggregated.trade_count() == chart.trade_count());
    const int n = chart.trade_count() < aggregated.trade_count() ? chart.trade_count() : aggregated.trade_count();
    for (int i = 0; i < n; ++i) {
        CHECK(aggregated.get_trade(i).exit_bar_index == chart.get_trade(i).exit_bar_index);
        CHECK(aggregated.get_trade(i).exit_price == chart.get_trade(i).exit_price);
        CHECK(aggregated.get_trade(i).max_drawdown == chart.get_trade(i).max_drawdown);
        CHECK(aggregated.get_trade(i).max_runup == chart.get_trade(i).max_runup);
    }

    // Construction two: the exit a first entry's fill places is working on the entry's own bucket, on both charts.
    Host chart_tp(Mode::FirstEntry);
    chart_tp.run(ten.data(), static_cast<int>(ten.size()), "10", "10", false);
    Host aggregated_tp(Mode::FirstEntry);
    aggregated_tp.run(one.data(), static_cast<int>(one.size()), "1", "10", false);
    CHECK(chart_tp.last_error().empty());
    CHECK(aggregated_tp.last_error().empty());
    CHECK(chart_tp.trade_count() >= 1);
    if (chart_tp.trade_count() >= 1) {
        CHECK(chart_tp.get_trade(0).entry_bar_index == 1);
        CHECK(std::fabs(chart_tp.get_trade(0).entry_price - 100.1) < 1e-9);
        CHECK(chart_tp.get_trade(0).exit_bar_index == 1);
        CHECK(std::fabs(chart_tp.get_trade(0).exit_price - 100.2) < 1e-9);
    }
    CHECK(aggregated_tp.trade_count() == chart_tp.trade_count());
    const int m = chart_tp.trade_count() < aggregated_tp.trade_count() ? chart_tp.trade_count()
                                                                        : aggregated_tp.trade_count();
    for (int i = 0; i < m; ++i) {
        CHECK(aggregated_tp.get_trade(i).entry_bar_index == chart_tp.get_trade(i).entry_bar_index);
        CHECK(aggregated_tp.get_trade(i).entry_price == chart_tp.get_trade(i).entry_price);
        CHECK(aggregated_tp.get_trade(i).exit_bar_index == chart_tp.get_trade(i).exit_bar_index);
        CHECK(aggregated_tp.get_trade(i).exit_price == chart_tp.get_trade(i).exit_price);
        CHECK(aggregated_tp.get_trade(i).max_drawdown == chart_tp.get_trade(i).max_drawdown);
        CHECK(aggregated_tp.get_trade(i).max_runup == chart_tp.get_trade(i).max_runup);
    }
    if (failures == 0) std::printf("test_aggregated_add_exit_leg_birth: OK\n");
    return failures == 0 ? 0 : 1;
}
