// Two reductions of one open lot, from the same close callsite. Observe
// committed fills separately from the trade rows and from the remaining lot.
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {
constexpr std::int64_t start = 1'735'689'600'000LL;
constexpr std::int64_t minute = 60'000;
constexpr int chart_minutes = 5;
constexpr int charts = 5;
constexpr int fill_bar = 2;
constexpr int signal_minute = fill_bar * chart_minutes;

struct Scenario {
    const char* id;
    bool long_side;
    double entry, first, second, price_offset, next_open;
    int last_leg_minute;

    double price(double p) const {
        return price_offset + (long_side ? p : 200 - p);
    }
};

struct Fill {
    std::string label, comment;
    int bar;
    std::int64_t sub_open;
    double opened, closed, price;
};

class RepeatClose final : public source::PineStrategyHost {
public:
    explicit RepeatClose(Scenario s) : s_(s) {
        source::PineStrategyConfig c;
        c.initial_capital = 1'000'000;
        c.calc_on_order_fills = true;
        c.process_orders_on_close = false;
        c.margin_long = c.margin_short = 0.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.commission_value = 0.0;
        c.slippage = 0;
        configure_pine_strategy(c);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(1.0);
        set_syminfo_metadata("qty_step", 1.0);
    }

    std::vector<Fill> fills;
    std::vector<double> close_call_positions;
    double remaining() const { return live_position_size(); }

private:
    void on_source_bar(const Bar&) override {
        const double na = std::numeric_limits<double>::quiet_NaN();
        if (pine_bar_index() == fill_bar - 1)
            strategy_entry(s_.id, s_.long_side, na, s_.price(110), s_.entry);
        if (pine_bar_index() != fill_bar || close_call_positions.size() == 2) return;
        const double held = (s_.long_side ? 1 : -1) * live_position_size();
        const bool first = close_call_positions.empty();
        if (held != (first ? s_.entry : s_.entry - s_.first)) return;
        close_call_positions.push_back(live_position_size());
        strategy_close(s_.id, first ? "first" : "second", first ? s_.first : s_.second,
                       na, false, 1);
    }

    void on_native_applied_event(const native_order::ExecutionAppliedEvent& e,
                                 const NativeDecisionContext& ctx) override {
        fills.push_back({e.request().label, e.request().comment, e.interval_index(),
                         ctx.sub_bar_open_ms, e.opened_units, e.closed_units, e.resolved_price});
    }

    const Scenario s_;
};

std::vector<Bar> feed(const Scenario& s) {
    std::vector<Bar> bars;
    // Long tape: on chart 2 the parent low (70) is already behind the entry.
    // The next minute goes 100 -> 99 -> 120 -> 113. The entry stop at 110
    // triggers on the rising leg; the first partial close gets its endpoint.
    // The next minute opens away from both extremes. In the short control
    // the stop minute is the last of the chart bar, so the open is on bar 3.
    for (int i = 0; i < charts * chart_minutes; ++i) {
        Bar b{100, 103, 97, 101, 10, start + i * minute};
        if (i == signal_minute) b = {100, 101, 70, 100, 10, b.timestamp};
        if (i == signal_minute + s.last_leg_minute)
            b = {100, 120, 99, 113, 10, b.timestamp};
        if (i == signal_minute + s.last_leg_minute + 1)
            b = {s.next_open, 119, 101, 107, 10, b.timestamp};
        bars.push_back({s.price(b.open), s.price(s.long_side ? b.high : b.low),
                        s.price(s.long_side ? b.low : b.high), s.price(b.close),
                        b.volume, b.timestamp});
    }
    return bars;
}

bool run(const Scenario& s, bool magnified) {
    const auto bars = feed(s);
    RepeatClose host(s);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "5", magnified, 4,
             MagnifierDistribution::ENDPOINTS);
    const char* mode = magnified ? "on" : "off";
    bool ok = true;
    auto require = [&](bool condition, const std::string& label) {
        if (!condition) {
            std::fprintf(stderr, "FAIL %s/%s: %s\n", mode, s.id, label.c_str());
            ok = false;
        }
    };
    if (!host.last_error().empty()) {
        require(false, "run error: " + host.last_error());
        return false;
    }
    ReportC report{};
    host.fill_report(&report);
    std::printf("%s/%s: input=%lld chart=%lld magnifier=%d sub-bars=%lld ticks=%lld\n",
                mode, s.id, static_cast<long long>(report.input_bars_processed),
                static_cast<long long>(report.script_bars_processed), report.bar_magnifier_enabled,
                static_cast<long long>(report.magnifier_sub_bars_total),
                static_cast<long long>(report.magnifier_sample_ticks_total));
    require(report.input_bars_processed == static_cast<std::int64_t>(bars.size())
            && report.script_bars_processed == charts, "incomplete input or chart population");
    require(report.bar_magnifier_enabled == static_cast<int>(magnified)
            && report.magnifier_sub_bars_total == (magnified ? charts * chart_minutes : 0)
            && report.magnifier_sample_ticks_total == (magnified ? 4 * charts * chart_minutes : 0),
            "wrong magnifier path");
    BacktestEngine::free_report(&report);

    const double sign = s.long_side ? 1.0 : -1.0;
    const std::string close_id = std::string("__close__") + s.id;
    const double residual = sign * (s.entry - s.first - s.second);
    // Without intrabars the chart walks O -> first extreme -> second extreme.
    // A close on that first extreme's forced fill advances to the second.
    const double first_price = s.price(120);
    const double second_price = s.price(magnified ? s.next_open : 70);
    const auto chart_open = bars[signal_minute].timestamp;
    const auto first_sub = chart_open + s.last_leg_minute * minute;
    const auto second_sub = first_sub + minute;
    const int second_bar = magnified ? (signal_minute + s.last_leg_minute + 1) / chart_minutes
                                     : fill_bar;
    std::printf("%s/%s: close calls=%zu; applied=%zu; closed trades=%d; remaining=%g\n",
                mode, s.id, host.close_call_positions.size(), host.fills.size(),
                host.trade_count(), host.remaining());
    for (const auto& f : host.fills)
        std::printf("  applied %s [%s] chart=%d lower_ms=%lld opened=%g closed=%g price=%g\n",
                    f.label.c_str(), f.comment.c_str(), f.bar, static_cast<long long>(f.sub_open),
                    f.opened, f.closed, f.price);
    require(host.close_call_positions == std::vector<double>{sign * s.entry, sign * (s.entry - s.first)},
            "both calls must have been issued on the expected positions");
    require(host.fills.size() == 3, "expected one entry and two separate committed closes");
    const auto check_fill = [&](std::size_t i, const std::string& id, const char* comment,
                                int bar, double opened, double closed, double price, std::int64_t sub) {
        const bool found = host.fills.size() > i;
        if (found) {
            const auto& f = host.fills[i];
            if (f.label == id && f.comment == comment && f.bar == bar
                && f.opened == opened && f.closed == closed && f.price == price
                && (!magnified || f.sub_open == sub)) return;
        }
        require(false, "applied " + id + " [" + comment + "]: expected bar "
            + std::to_string(bar) + ", opened " + std::to_string(opened)
            + ", closed " + std::to_string(closed) + ", price " + std::to_string(price)
            + (found ? " (see observed fill above)" : " (missing)"));
    };
    check_fill(0, s.id, "", fill_bar, sign * s.entry, 0, s.price(110), first_sub);
    check_fill(1, close_id, "first", fill_bar, 0, s.first, first_price, first_sub);
    check_fill(2, close_id, "second", second_bar, 0, s.second, second_price, second_sub);
    require(host.trade_count() == 2, "expected two closed trades, excluding the range-end mark");
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade t = host.get_trade(i);
        const int exit_bar = i == 0 ? fill_bar : second_bar;
        std::printf("  trade %s -> %s [%s] bars=%d/%d qty=%g price=%g\n",
                    t.entry_id.c_str(), t.exit_id.c_str(), t.exit_comment.c_str(),
                    t.entry_bar_index, t.exit_bar_index, t.qty, t.exit_price);
        require(i < 2 && !t.open_at_end && t.entry_id == s.id && t.exit_id == close_id
                && t.is_long == s.long_side && t.entry_bar_index == fill_bar
                && t.exit_bar_index == exit_bar && t.entry_time == chart_open
                && t.exit_time == bars[exit_bar * chart_minutes].timestamp
                && t.exit_comment == (i == 0 ? "first" : "second")
                && t.qty == (i == 0 ? s.first : s.second)
                && t.exit_price == (i == 0 ? first_price : second_price),
                "incorrect settled trade " + std::to_string(i));
    }
    require(host.remaining() == residual, "expected remaining position " + std::to_string(residual));
    return ok;
}
} // namespace

int main() {
    bool ok = true;
    for (const Scenario s : {Scenario{"buy", true, 13, 3, 5, 0, 104, 1},
                             Scenario{"sell", false, 17, 6, 4, 40, 108, 4}}) {
        for (const bool magnified : {false, true})
            if (!run(s, magnified)) ok = false;
    }
    return ok ? 0 : 1;
}
