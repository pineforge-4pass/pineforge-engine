// A pending close must not consume a new entry's exit-sizing basis.
// Expected quantities come from the order inputs. Each reversal has a
// no-close control; the old trade proves that the close was executed.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;
const char* scenario = "";
const char* variant = "";

void check(bool ok, const char* expr, int line) {
    ++checks;
    if (ok) return;
    ++failures;
    std::printf("  FAIL  [%s/%s] %s:%d  %s\n",
                scenario, variant, __FILE__, line, expr);
}

#define CHECK(expr) check((expr), #expr, __LINE__)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kT = 1700000000000LL;
constexpr int kOldEntryBar = 1;
constexpr int kSignalBar = 3;
constexpr int kTpBar = kSignalBar + 1;
constexpr int kStopBar = kTpBar + 2;
constexpr double kTpPercent = 50.0;
constexpr double kTpDistance = 2.0;
constexpr double kStopDistance = 3.0;

Bar mk(int index, double o, double h, double l, double c) {
    Bar bar;
    bar.open = o; bar.high = h; bar.low = l; bar.close = c;
    bar.volume = 1.0;
    bar.timestamp = kT + static_cast<std::int64_t>(index) * 60000;
    return bar;
}

struct Scenario {
    const char* name;
    double old_qty;
    double new_qty;
    bool new_is_long;
    double absolute_tp_qty = kNaN;
    bool limit_only = false;
    bool default_quantity = false;
    bool process_orders_on_close = true;

    double direction() const { return new_is_long ? 1.0 : -1.0; }
    bool old_is_long() const { return !new_is_long; }
    std::string old_id() const { return new_is_long ? "Short" : "Long"; }
    std::string new_id() const { return new_is_long ? "Long" : "Short"; }
    int fill_bar(int signal_bar) const {
        return signal_bar + (process_orders_on_close ? 0 : 1);
    }
    int tp_bar() const { return kTpBar + (process_orders_on_close ? 0 : 1); }
    int stop_bar() const { return kStopBar + (process_orders_on_close ? 0 : 1); }
    double fill_price(const std::vector<Bar>& bars, int signal_bar) const {
        const auto& bar = bars[fill_bar(signal_bar)];
        return process_orders_on_close ? bar.close : bar.open;
    }
};

std::vector<Bar> feed(bool new_is_long, bool process_orders_on_close) {
    // Prices are binary-exact multiples of 0.25 on the 0.01 price grid.
    // With process_orders_on_close, entries fill at their signal bars' closes.
    std::vector<Bar> bars = {
        mk(0, 100.0, 100.0, 100.0, 100.0),
        mk(1, 100.0, 100.0, 100.0, 100.0),  // old entry
        mk(2, 100.0, 100.0, 100.0, 100.0),  // old position held
        mk(3, 100.0, 100.0, 100.0, 100.0),  // close, reversal, TP, SL
        mk(4, 100.0, 103.0,  99.5, 102.5),  // TP only
        mk(5, 102.5, 102.75, 100.0, 100.5),
        mk(6, 100.5, 100.75,  96.5, 97.5),  // SL only
        mk(7,  97.5,  97.5,   97.5, 97.5),
    };
    if (!process_orders_on_close) {
        // Market orders fill at the next open. The reversal open equals the
        // signal close, so the script can set its bracket before the fill.
        // Neither level is touched on the fill bar; TP precedes SL.
        bars = {
            mk(0, 100.0, 100.0, 100.0, 100.0),
            mk(1, 100.0, 100.0, 100.0, 100.0),  // old entry signal
            mk(2,  99.5, 100.25, 99.25, 100.0), // old entry fills
            mk(3, 100.0, 100.0, 100.0, 100.0),  // close, reversal, TP, SL
            mk(4, 100.0, 101.0,  99.0, 100.5),  // close and reversal fill
            mk(5, 100.5, 102.25, 100.25, 101.5), // TP only
            mk(6, 101.5, 101.75, 100.0, 100.5),
            mk(7, 100.5, 100.75, 96.75, 97.5),  // SL only
            mk(8,  97.5,  97.5,   97.5, 97.5),
        };
    }
    if (!new_is_long) {
        const double reflection = bars[kSignalBar].close * 2.0;
        for (auto& bar : bars) {
            const double high = bar.high;
            bar.open = reflection - bar.open;
            bar.high = reflection - bar.low;
            bar.low = reflection - high;
            bar.close = reflection - bar.close;
        }
    }
    return bars;
}

// Submit the close, reversing entry and named exits in the same execution.
class ReversingCloseProbe final : public source::PineStrategyHost {
public:
    ReversingCloseProbe(const Scenario& scenario, bool with_close, double signal_price)
        : scenario_(scenario), with_close_(with_close) {
        source::PineStrategyConfig config;
        config.process_orders_on_close = scenario.process_orders_on_close;
        config.initial_capital = 100000.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        if (!scenario.process_orders_on_close) {
            config.pyramiding = 1;
            config.calc_on_order_fills = false;
            config.default_qty_type = static_cast<int>(QtyType::FIXED);
            config.close_entries_rule_any = false;
            // Risk limits and intrabar sampling remain unset.
        }
        if (scenario.default_quantity) {
            // CASH keeps the close executable: a default FIXED reversal
            // consumes the queued FIFO close as its own closing leg.
            config.default_qty_type = static_cast<int>(QtyType::CASH);
            config.default_qty_value = scenario.new_qty * signal_price;
        }
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
        set_syminfo_metadata("qty_step", 0.25);
        margin_call_enabled_ = false;
    }

    double signed_units() const noexcept {
        return position_qty_
            * (position_side_ == PositionSide::SHORT ? -1.0 : 1.0);
    }

    double units_at_signal = kNaN;
    double units_after_stop = kNaN;
    double units_after_old_fill = kNaN;
    double units_after_entry_fill = kNaN;
    double units_after_tp = kNaN;

    void on_source_bar(const Bar&) override {
        const int bar = bar_index_;
        if (bar == kOldEntryBar) {
            strategy_entry(scenario_.old_id(), scenario_.old_is_long(),
                           kNaN, kNaN, scenario_.old_qty, "", "", 0, -1);
        }
        if (bar == kSignalBar) {
            units_at_signal = signed_units();
            const double limit = current_bar_.close + scenario_.direction() * kTpDistance;
            const double stop = current_bar_.close - scenario_.direction() * kStopDistance;
            if (with_close_) {
                strategy_close(scenario_.old_id(), "", kNaN, kNaN, false, 30064771091ULL);
            }
            strategy_entry(scenario_.new_id(), scenario_.new_is_long,
                           kNaN, kNaN, scenario_.default_quantity ? kNaN : scenario_.new_qty,
                           "", "", 0, -1);
            strategy_exit(std::string("TP"), scenario_.new_id(), limit,
                          scenario_.limit_only ? kNaN : stop,
                          kNaN, kNaN, kNaN,
                          std::isfinite(scenario_.absolute_tp_qty) ? kNaN : kTpPercent,
                          "", scenario_.absolute_tp_qty, "", kNaN, kNaN);
            if (!scenario_.limit_only) {
                strategy_exit(std::string("SL"), scenario_.new_id(), kNaN, stop,
                              kNaN, kNaN, kNaN, 100.0, "", kNaN, "", kNaN, kNaN);
            }
        }
        if (bar == scenario_.stop_bar()) units_after_stop = signed_units();
        if (!scenario_.process_orders_on_close) {
            if (bar == scenario_.fill_bar(kOldEntryBar)) units_after_old_fill = signed_units();
            if (bar == scenario_.fill_bar(kSignalBar)) units_after_entry_fill = signed_units();
            if (bar == scenario_.tp_bar()) units_after_tp = signed_units();
        }
    }

private:
    const Scenario& scenario_;
    bool with_close_;
};

struct Row {
    std::int64_t entry_time;
    std::int64_t exit_time;
    double entry_price;
    double exit_price;
    double qty;
    double pnl;
    int is_long;
    int open_at_end;
};

struct Observed {
    std::vector<Row> rows;
    std::vector<Trade> closed_trades;
    std::string error;
    double units_at_signal;
    double units_after_stop;
    double final_units;
    double units_after_old_fill;
    double units_after_entry_fill;
    double units_after_tp;
};

Observed observe(const Scenario& spec, const std::vector<Bar>& bars, bool with_close) {
    ReversingCloseProbe probe(spec, with_close, bars[kSignalBar].close);
    probe.run(bars.data(), static_cast<int>(bars.size()));
    Observed out;
    out.error = probe.last_error();
    out.units_at_signal = probe.units_at_signal;
    out.units_after_stop = probe.units_after_stop;
    out.final_units = probe.signed_units();
    out.units_after_old_fill = probe.units_after_old_fill;
    out.units_after_entry_fill = probe.units_after_entry_fill;
    out.units_after_tp = probe.units_after_tp;
    // Closed source rows supply the exit IDs; the report also includes any
    // uncovered position marked open_at_end.
    for (std::size_t i = 0; i < probe.closed_trade_count(); ++i)
        out.closed_trades.push_back(probe.get_trade(static_cast<int>(i)));
    ReportC report{};
    probe.fill_report(&report);
    for (int i = 0; i < report.trades_len; ++i) {
        const TradeC& t = report.trades[i];
        out.rows.push_back({t.entry_time, t.exit_time, t.entry_price, t.exit_price,
                            t.qty, t.pnl, t.is_long, t.open_at_end});
    }
    BacktestEngine::free_report(&report);
    return out;
}

void check_old_close(const Scenario& spec, const std::vector<Bar>& bars,
                     const Observed& got, bool with_close) {
    // PineExecutionAdapter::close labels the frozen request "__close__" + id
    // at src/source/pine_adapter.cpp:10688; the close-on-close batch uses
    // that same label at src/source/pine_adapter.cpp:9868 and :9920.
    const std::string close_marker = "__close__";
    int old_rows = 0;
    for (const auto& trade : got.closed_trades) {
        // Identify the old entry by its entry time.
        if (trade.entry_time != bars[spec.fill_bar(kOldEntryBar)].timestamp) continue;
        ++old_rows;
        CHECK(trade.entry_id == spec.old_id());
        CHECK(trade.entry_price == spec.fill_price(bars, kOldEntryBar));
        CHECK(trade.is_long == spec.old_is_long());
        CHECK(trade.exit_time == bars[spec.fill_bar(kSignalBar)].timestamp);
        CHECK(trade.exit_price == spec.fill_price(bars, kSignalBar));
        CHECK(!trade.open_at_end);
        CHECK(trade.qty == spec.old_qty);
        CHECK(trade.pnl == (spec.old_is_long() ? 1.0 : -1.0)
            * (trade.exit_price - trade.entry_price) * trade.qty);
        if (with_close) {
            CHECK(trade.exit_id != spec.new_id());
            CHECK(trade.exit_id == close_marker + spec.old_id());
        } else {
            CHECK(trade.exit_id == spec.new_id());
        }
    }
    CHECK(old_rows == 1);
}

void check_exits(const Scenario& spec, const std::vector<Bar>& bars, const Observed& got) {
    CHECK(got.error.empty());
    CHECK(got.units_at_signal == (spec.old_is_long() ? 1.0 : -1.0) * spec.old_qty);
    const double expected_tp = std::isfinite(spec.absolute_tp_qty)
        ? spec.absolute_tp_qty : spec.new_qty * kTpPercent / 100.0;
    const double remaining = spec.new_qty - expected_tp;
    const double expected_sl = spec.limit_only ? 0.0 : remaining;
    const double entry_price = spec.fill_price(bars, kSignalBar);
    if (!spec.process_orders_on_close) {
        CHECK(got.units_after_old_fill == -spec.direction() * spec.old_qty);
        CHECK(got.units_after_entry_fill == spec.direction() * spec.new_qty);
        CHECK(got.units_after_tp == spec.direction() * remaining);
    }
    double tp_qty = 0.0;
    double sl_qty = 0.0;
    int tp_rows = 0;
    int sl_rows = 0;
    for (const auto& trade : got.closed_trades) {
        if (trade.entry_time != bars[spec.fill_bar(kSignalBar)].timestamp) {
            CHECK(trade.entry_time == bars[spec.fill_bar(kOldEntryBar)].timestamp);
            continue;
        }
        CHECK(trade.entry_id == spec.new_id());
        CHECK(trade.entry_time == bars[spec.fill_bar(kSignalBar)].timestamp);
        CHECK(trade.entry_price == entry_price);
        CHECK(trade.is_long == spec.new_is_long);
        CHECK(!trade.open_at_end);
        CHECK(trade.exit_id == "TP" || trade.exit_id == "SL");
        if (trade.exit_id == "TP") {
            ++tp_rows;
            tp_qty += trade.qty;
            CHECK(trade.exit_time == bars[spec.tp_bar()].timestamp);
            CHECK(trade.exit_price == entry_price + spec.direction() * kTpDistance);
        } else if (trade.exit_id == "SL") {
            ++sl_rows;
            sl_qty += trade.qty;
            CHECK(trade.exit_time == bars[spec.stop_bar()].timestamp);
            CHECK(trade.exit_price == entry_price - spec.direction() * kStopDistance);
        }
        CHECK(trade.pnl == spec.direction() * (trade.exit_price - entry_price) * trade.qty);
    }
    CHECK(tp_rows == 1);
    CHECK(sl_rows == (spec.limit_only ? 0 : 1));
    CHECK(tp_qty == expected_tp);
    CHECK(sl_qty == expected_sl);
    if (spec.limit_only) {
        CHECK(tp_qty + remaining == spec.new_qty);
        CHECK(got.units_after_stop == spec.direction() * remaining);
        CHECK(got.final_units == spec.direction() * remaining);
        CHECK(got.rows.size() == got.closed_trades.size() + 1);
        int open_rows = 0;
        for (const auto& row : got.rows) {
            if (!row.open_at_end) continue;
            ++open_rows;
            CHECK(row.open_at_end == 1);
            CHECK(row.entry_time == bars[spec.fill_bar(kSignalBar)].timestamp);
            CHECK(row.entry_price == entry_price);
            CHECK(row.is_long == spec.new_is_long);
            CHECK(row.qty == remaining);
            CHECK(row.exit_time == bars.back().timestamp);
            CHECK(row.exit_price == bars.back().close);
            CHECK(row.pnl == spec.direction() * (bars.back().close - entry_price) * remaining);
        }
        CHECK(open_rows == 1);
    } else {
        CHECK(tp_qty + sl_qty == spec.new_qty);
        CHECK(got.units_after_stop == 0.0);
        CHECK(got.final_units == 0.0);
        CHECK(got.rows.size() == got.closed_trades.size());
        for (const auto& row : got.rows) CHECK(row.open_at_end == 0);
    }
}

void compare(const Observed& with_close, const Observed& control) {
    CHECK(with_close.rows.size() == control.rows.size());
    const std::size_t n = with_close.rows.size() < control.rows.size()
        ? with_close.rows.size() : control.rows.size();
    // Compare the same report projection as the bracket regression tests.
    // Physical entry incarnation numbering can differ between the streams.
    for (std::size_t i = 0; i < n; ++i) {
        const Row& left = with_close.rows[i];
        const Row& right = control.rows[i];
        CHECK(left.entry_time == right.entry_time);
        CHECK(left.exit_time == right.exit_time);
        CHECK(left.entry_price == right.entry_price);
        CHECK(left.exit_price == right.exit_price);
        CHECK(left.qty == right.qty);
        CHECK(left.pnl == right.pnl);
        CHECK(left.is_long == right.is_long);
        CHECK(left.open_at_end == right.open_at_end);
    }
}

void dump(const char* label, const Observed& got) {
    // Print what the engine did when a variant fails.
    for (const auto& t : got.closed_trades) {
        std::printf("  OBSERVED [%s/%s] closed entry_id='%s' entry_time=%lld "
                    "entry_price=%.4f exit_id='%s' exit_time=%lld exit_price=%.4f "
                    "qty=%.4f side=%s\n",
                    scenario, label, t.entry_id.c_str(),
                    static_cast<long long>(t.entry_time), t.entry_price,
                    t.exit_id.c_str(), static_cast<long long>(t.exit_time),
                    t.exit_price, t.qty, t.is_long ? "long" : "short");
    }
    for (const auto& row : got.rows) {
        if (!row.open_at_end) continue;
        std::printf("  OBSERVED [%s/%s] open entry_time=%lld entry_price=%.4f "
                    "qty=%.4f side=%s\n",
                    scenario, label,
                    static_cast<long long>(row.entry_time),
                    row.entry_price, row.qty, row.is_long ? "long" : "short");
    }
    std::printf("  OBSERVED [%s/%s] units after_old_fill=%.4f after_entry_fill=%.4f "
                "at_signal=%.4f after_tp=%.4f after_stop=%.4f final=%.4f error='%s'\n",
                scenario, label, got.units_after_old_fill, got.units_after_entry_fill,
                got.units_at_signal, got.units_after_tp, got.units_after_stop,
                got.final_units, got.error.c_str());
}

void run_case(const Scenario& spec) {
    scenario = spec.name;
    const auto bars = feed(spec.new_is_long, spec.process_orders_on_close);
    const auto control = observe(spec, bars, false);
    variant = "control";
    const int control_before = failures;
    check_exits(spec, bars, control);
    check_old_close(spec, bars, control, false);
    if (failures > control_before) dump(variant, control);
    const auto with_close = observe(spec, bars, true);
    variant = "with-close";
    const int with_close_before = failures;
    check_exits(spec, bars, with_close);
    check_old_close(spec, bars, with_close, true);
    if (failures > with_close_before) dump(variant, with_close);
    variant = "differential";
    compare(with_close, control);
}

}  // namespace

int main() {
    // A close before a reversal leaves percentage exits sized from the new entry.
    run_case({"long-old-larger", 3.0, 2.0, true});
    run_case({"long-old-smaller", 1.0, 3.0, true});
    run_case({"long-old-equal", 2.0, 2.0, true});
    run_case({"short-old-smaller", 1.0, 3.0, false});

    const Scenario additional[] = {
        // An absolute-quantity take-profit is not clipped by the old position's size.
        {"absolute-qty-exits-long", 3.0, 2.0, true, 1.0},
        {"absolute-qty-exits-short", 3.0, 2.0, false, 1.0},
        {"absolute-qty-exits-old-smaller-long", 2.0, 3.0, true, 1.5},
        {"absolute-qty-exits-old-smaller-short", 2.0, 3.0, false, 1.5},
        // A limit-only exit takes its percentage of the new entry and leaves the rest open.
        {"limit-only-exit-long", 3.0, 2.0, true, kNaN, true},
        {"limit-only-exit-short", 3.0, 2.0, false, kNaN, true},
        // A default-quantity reversal is unaffected by the close.
        {"default-quantity-reversal-long", 3.0, 2.0, true, kNaN, false, true},
        {"default-quantity-reversal-short", 3.0, 2.0, false, kNaN, false, true},
    };
    for (const auto& spec : additional) run_case(spec);

    // Percentage exits use the new short entry's size for either old size.
    run_case({"short-old-larger", 3.0, 2.0, false});
    run_case({"short-old-equal", 2.0, 2.0, false});

    const Scenario next_bar[] = {
        {"next-bar-long-old-larger", 3.0, 2.0, true, kNaN, false, false, false},
        {"next-bar-long-old-smaller", 1.0, 3.0, true, kNaN, false, false, false},
        {"next-bar-long-old-equal", 2.0, 2.0, true, kNaN, false, false, false},
        {"next-bar-short-old-larger", 3.0, 2.0, false, kNaN, false, false, false},
        {"next-bar-short-old-smaller", 1.0, 3.0, false, kNaN, false, false, false},
        {"next-bar-short-old-equal", 2.0, 2.0, false, kNaN, false, false, false},
    };
    for (const auto& spec : next_bar) run_case(spec);

    const Scenario next_bar_absolute_qty[] = {
        {"next-bar-absolute-qty-long-old-larger", 4.0, 2.0, true, 1.0, false, false, false},
        {"next-bar-absolute-qty-long-old-smaller", 2.0, 4.0, true, 2.0, false, false, false},
        {"next-bar-absolute-qty-short-old-larger", 4.0, 2.0, false, 1.0, false, false, false},
        {"next-bar-absolute-qty-short-old-smaller", 2.0, 4.0, false, 2.0, false, false, false},
    };
    for (const auto& spec : next_bar_absolute_qty) run_case(spec);
    std::printf("test_adapter_named_exit_after_reversing_close: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
