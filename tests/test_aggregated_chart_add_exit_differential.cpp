// Aggregated chart versus plain chart: entries and pyramiding adds with their exits book the same closed rows.
//
// Target (one): for the controlled shapes below, a strategy run on a chart aggregated from 1-minute bars (input "1",
// script "15") books closed rows identical, field by field at full precision, to the same strategy on the plain
// 15-minute chart built from the same bars (input "15", script "15"). A seeded integer random walk (splitmix64, a
// fixed seed: no file, no environment) of 1-minute bars (UTC, 24x7) is cut into disjoint windows of kWindow 15-minute
// bars. At window bar kFirst an entry of id "Long" opens with one or two quantity-named exits; at window bar kLater
// the shape adds to the same id (or not) and calls the exits again in the same calculation. The re-issued stop lies
// strictly between the next bar's open and its low, so that bar crosses it after its open; windows where the levels
// cannot be held strictly apart are skipped. Every shape must keep at least kMinScenarios windows and differ in none.
// One more shape has no add and no re-issue: a first entry whose take-profit the entry bar itself crosses after the
// entry fills at that bar's open.
#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1704067200000LL;  // 2024-01-01 00:00 UTC, a 15-minute boundary
constexpr std::int64_t kMinute = 60000LL;
constexpr int kWindow = 24;      // 15-minute bars per scenario window
constexpr int kFirst = 2;        // entry bar inside the window
constexpr int kLater = kFirst + 7;
constexpr int kWindows = 400;    // windows scanned (disjoint)
constexpr int kMinScenarios = 50;  // a shape whose filter keeps fewer windows proves too little

// splitmix64: a fixed seed gives the same walk on every machine.
struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    // Uniform integer in [-r, r].
    std::int64_t step(std::int64_t r) {
        return static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(2 * r + 1)) - r;
    }
};

// Prices in integer cents; every double is cents / 100 computed once, so both constructions see identical values.
std::vector<Bar> one_minute_walk(int minutes) {
    Rng rng{0x1131B0DEULL};
    std::vector<Bar> out;
    out.reserve(static_cast<std::size_t>(minutes));
    std::int64_t close = 10000;  // 100.00
    for (int i = 0; i < minutes; ++i) {
        const std::int64_t open = close + rng.step(3);
        std::int64_t c = open + rng.step(25);
        if (c < 2000) c = 2000 + (2000 - c);  // reflect far above zero
        const std::int64_t hi = std::max(open, c) + static_cast<std::int64_t>(rng.next() % 15);
        const std::int64_t lo = std::min(open, c) - static_cast<std::int64_t>(rng.next() % 15);
        Bar b{};
        b.timestamp = kStart + kMinute * i;
        b.open = static_cast<double>(open) / 100.0;
        b.high = static_cast<double>(hi) / 100.0;
        b.low = static_cast<double>(lo) / 100.0;
        b.close = static_cast<double>(c) / 100.0;
        b.volume = 1.0;
        out.push_back(b);
        close = c;
    }
    return out;
}

std::vector<Bar> fifteen_of(const std::vector<Bar>& one, std::size_t begin, int bars) {
    std::vector<Bar> out;
    for (int k = 0; k < bars; ++k) {
        const Bar* m = &one[begin + static_cast<std::size_t>(k) * 15];
        Bar b = m[0];
        for (int j = 1; j < 15; ++j) {
            b.high = std::max(b.high, m[j].high);
            b.low = std::min(b.low, m[j].low);
            b.close = m[j].close;
            b.volume += m[j].volume;
        }
        out.push_back(b);
    }
    return out;
}

struct Row {
    bool is_long = true;
    bool open_at_end = false;
    std::int64_t entry_ms = 0;
    std::int64_t exit_ms = 0;
    double entry_price = 0.0;
    double exit_price = 0.0;
    double qty = 0.0;
    double pnl = 0.0;
    double runup = 0.0;
    double drawdown = 0.0;
    int entry_bar = 0;
    int exit_bar = 0;
    std::string entry_id;
    std::string exit_id;
};

// two-ids: 2 units, two one-unit exits, a 2-unit add with both exits called again.
// one-id-add: 2 units, one one-unit exit, a 1-unit add with the exit called again.
// no-add: 2 units, two one-unit exits, the exits called again alone.
// one-id-own-lot: 1 unit, one one-unit exit, a 1-unit add with the exit called again.
// one-id-limit-add: one-id-add with the add a buy LIMIT strictly between the next bar's open and the re-issued
//   stop, so the add fills on the Low segment of the next bar's path and the stop is crossed later on that segment.
// one-id-stop-add: one-id-add with the add a buy STOP strictly between the next bar's open and its high, kept only
//   where the next bar's path is open -> high -> low -> close, so the add fills on the High segment and the
//   re-issued stop is crossed on the Low segment after it.
// first-entry-take-profit: 1 unit, one one-unit take-profit at a cent level strictly between the entry bar's open and
//   its high (the shape's `stop` field carries that limit), a far stop, no add and no re-issue: the exit order is
//   placed by the entry's own fill at the open and the entry bar crosses it.
enum class Shape { TwoIds, OneIdAdd, NoAdd, OneIdOwnLot, OneIdLimitAdd, OneIdStopAdd, FirstEntryTakeProfit };

const char* name(Shape shape) {
    switch (shape) {
        case Shape::TwoIds: return "two-ids";
        case Shape::OneIdAdd: return "one-id-add";
        case Shape::NoAdd: return "no-add";
        case Shape::OneIdOwnLot: return "one-id-own-lot";
        case Shape::OneIdLimitAdd: return "one-id-limit-add";
        case Shape::OneIdStopAdd: return "one-id-stop-add";
        case Shape::FirstEntryTakeProfit: return "first-entry-take-profit";
    }
    return "?";
}

class Host final : public source::PineStrategyHost {
public:
    Host(Shape shape, double stop, double add_level = kNaN) : shape_(shape), stop_(stop), add_level_(add_level) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        source::PineStrategyConfig config;
        config.initial_capital = 100000;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 100;
        config.pyramiding = 2;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        config.slippage = 0;
        config.commission_value = 0.0;
        config.margin_long = 100;
        config.margin_short = 100;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar& bar) override {
        const int i = bar_index_;
        if (shape_ == Shape::FirstEntryTakeProfit) {
            if (i == kFirst) {
                strategy_entry("Long", true, kNaN, kNaN, 1.0);
                strategy_exit("LongTP", "Long", stop_, bar.close - 5.0, kNaN, kNaN, kNaN, 100.0, "TP Exit", 1.0);
            }
            return;
        }
        if (i != kFirst && i != kLater) return;
        const bool later = i == kLater;
        const double stop = later ? stop_ : bar.close - 5.0;
        const double first_qty = shape_ == Shape::OneIdOwnLot ? 1.0 : 2.0;
        const double add_qty = shape_ == Shape::TwoIds ? 2.0 : 1.0;
        if (!later) {
            strategy_entry("Long", true, kNaN, kNaN, first_qty);
        } else if (shape_ == Shape::OneIdLimitAdd) {
            strategy_entry("Long", true, add_level_, kNaN, add_qty);
        } else if (shape_ == Shape::OneIdStopAdd) {
            strategy_entry("Long", true, kNaN, add_level_, add_qty);
        } else if (shape_ != Shape::NoAdd) {
            strategy_entry("Long", true, kNaN, kNaN, add_qty);
        }
        strategy_exit("LongT1", "Long", bar.close + 5.0, stop, kNaN, kNaN, kNaN, 100.0, "T1 Exit", 1.0);
        if (shape_ == Shape::TwoIds || shape_ == Shape::NoAdd) {
            strategy_exit("LongT2", "Long", bar.close + 5.0, stop, kNaN, kNaN, kNaN, 100.0, "T2 Exit", 1.0);
        }
    }

    std::vector<Row> rows() const {
        std::vector<Row> out;
        for (int i = 0; i < trade_count(); ++i) {
            const Trade t = get_trade(i);
            Row row;
            row.is_long = t.is_long;
            row.open_at_end = t.open_at_end;
            row.entry_ms = closed_trade_entry_time(i);
            row.exit_ms = closed_trade_exit_time(i);
            row.entry_price = closed_trade_entry_price(i);
            row.exit_price = closed_trade_exit_price(i);
            row.qty = closed_trade_size(i);
            row.pnl = closed_trade_profit(i);
            row.runup = closed_trade_max_runup(i);
            row.drawdown = closed_trade_max_drawdown(i);
            row.entry_bar = closed_trade_entry_bar_index(i);
            row.exit_bar = closed_trade_exit_bar_index(i);
            row.entry_id = closed_trade_entry_id(i);
            row.exit_id = closed_trade_exit_id(i);
            out.push_back(row);
        }
        return out;
    }

    std::string error() const { return last_error(); }

private:
    Shape shape_;
    double stop_;
    double add_level_;
};

// A cent level at from + (to - from) * num / den, rounded to the cent and returned through its own text so both
// constructions read the identical double.
double cent_level(double from, double to, int num, int den, char (&text)[32]) {
    const double cents = std::floor((from + (to - from) * num / den) * 100.0 + 0.5);
    std::snprintf(text, sizeof text, "%.2f", cents / 100.0);
    return std::strtod(text, nullptr);
}

// The levels of one window for one shape; false when the next bar cannot hold them strictly apart.
bool levels_for(Shape shape, const Bar& next, double& stop, double& add_level, char (&stop_text)[32],
                char (&add_text)[32]) {
    add_level = kNaN;
    std::snprintf(add_text, sizeof add_text, "-");
    const double gap = 0.004;
    if (shape == Shape::FirstEntryTakeProfit) {
        // `next` is the entry bar. The take-profit is a cent level strictly between its open and its high.
        stop = cent_level(next.open, next.high, 1, 2, stop_text);
        return stop > next.open + gap && stop < next.high - gap;
    }
    if (shape == Shape::OneIdLimitAdd) {
        // open > add limit > stop > low, all on the Low segment's descent.
        add_level = cent_level(next.open, next.low, 1, 3, add_text);
        stop = cent_level(next.open, next.low, 2, 3, stop_text);
        return add_level < next.open - gap && stop < add_level - gap && stop > next.low + gap;
    }
    if (shape == Shape::OneIdStopAdd) {
        // high-first path; open < add stop < high, then low < stop < open on the Low segment after the high.
        if (!(std::abs(next.high - next.open) < std::abs(next.open - next.low))) return false;
        add_level = cent_level(next.open, next.high, 1, 2, add_text);
        stop = cent_level(next.open, next.low, 1, 2, stop_text);
        return add_level > next.open + gap && add_level < next.high - gap && stop < next.open - gap
            && stop > next.low + gap;
    }
    // A cent level strictly between the next bar's low and its open.
    const double cents = std::floor((next.open + next.low) / 2.0 * 100.0 + 0.5);
    std::snprintf(stop_text, sizeof stop_text, "%.2f", cents / 100.0);
    stop = std::strtod(stop_text, nullptr);
    return stop < next.open - gap && stop > next.low + gap;
}

std::string differing(const Row& a, const Row& b) {
    std::string out;
    const auto add = [&](bool same, const char* field) {
        if (!same) out += std::string(out.empty() ? "" : ",") + field;
    };
    add(a.is_long == b.is_long, "is_long");
    add(a.open_at_end == b.open_at_end, "open_at_end");
    add(a.entry_ms == b.entry_ms, "entry_ms");
    add(a.exit_ms == b.exit_ms, "exit_ms");
    add(a.entry_price == b.entry_price, "entry_price");
    add(a.exit_price == b.exit_price, "exit_price");
    add(a.qty == b.qty, "qty");
    add(a.pnl == b.pnl, "pnl");
    add(a.runup == b.runup, "runup");
    add(a.drawdown == b.drawdown, "drawdown");
    add(a.entry_bar == b.entry_bar, "entry_bar");
    add(a.exit_bar == b.exit_bar, "exit_bar");
    add(a.entry_id == b.entry_id, "entry_id");
    add(a.exit_id == b.exit_id, "exit_id");
    return out;
}

} // namespace

int main() {
    const int per_window = kWindow * 15;
    const std::vector<Bar> walk = one_minute_walk(per_window * kWindows);
    const Shape shapes[] = {Shape::TwoIds, Shape::OneIdAdd, Shape::NoAdd, Shape::OneIdOwnLot, Shape::OneIdLimitAdd,
                            Shape::OneIdStopAdd, Shape::FirstEntryTakeProfit};
    int failures = 0;
    for (const Shape shape : shapes) {
        int scenarios = 0, different = 0, errors = 0;
        for (int w = 0; w < kWindows; ++w) {
            const std::size_t begin = static_cast<std::size_t>(w) * static_cast<std::size_t>(per_window);
            const std::vector<Bar> one(walk.begin() + static_cast<std::ptrdiff_t>(begin),
                                       walk.begin() + static_cast<std::ptrdiff_t>(begin + per_window));
            const std::vector<Bar> fifteen = fifteen_of(walk, begin, kWindow);
            const int decision_bar = shape == Shape::FirstEntryTakeProfit ? kFirst : kLater;
            const Bar& next = fifteen[static_cast<std::size_t>(decision_bar + 1)];
            char text[32];
            char add_text[32];
            double stop = 0.0;
            double add_level = kNaN;
            if (!levels_for(shape, next, stop, add_level, text, add_text)) continue;
            ++scenarios;
            Host chart(shape, stop, add_level);
            chart.run(fifteen.data(), static_cast<int>(fifteen.size()), "15", "15", false);
            Host aggregated(shape, stop, add_level);
            aggregated.run(one.data(), static_cast<int>(one.size()), "1", "15", false);
            if (!chart.error().empty() || !aggregated.error().empty()) ++errors;
            const std::vector<Row> a = chart.rows();
            const std::vector<Row> b = aggregated.rows();
            bool differs = a.size() != b.size();
            for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
                const std::string fields = differing(a[i], b[i]);
                if (fields.empty()) continue;
                differs = true;
                std::printf("FAIL %s window=%d stop=%s add=%s row=%zu differs in %s\n", name(shape), w, text,
                            add_text, i, fields.c_str());
            }
            if (a.size() != b.size()) {
                std::printf("FAIL %s window=%d rows chart=%zu aggregated=%zu\n", name(shape), w, a.size(),
                            b.size());
            }
            if (differs) ++different;
        }
        std::printf("%s: %d scenarios, %d different, %d run errors\n", name(shape), scenarios, different, errors);
        if (scenarios < kMinScenarios || different != 0 || errors != 0) ++failures;
    }
    if (failures == 0) std::printf("test_aggregated_chart_add_exit_differential: OK\n");
    return failures == 0 ? 0 : 1;
}
