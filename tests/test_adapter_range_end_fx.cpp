/*
 * test_adapter_range_end_fx.cpp -- R5 lane B-ADAPTER, item 3 (AUDIT3-opus2 H7).
 *
 * A position still open when the Pine feed ends is reported as the rows a
 * close at the terminal bar's close would book, dated on the equity curve's
 * last point (PineStrategyHost::scheduler_record_range_end, through the
 * kernel's producer NativeExecutionConsumer::append_open_position_report_rows
 * and BacktestEngine::build_close_trade_with_costs). Until this lane the host
 * saved, rewrote and restored the presented clock (current_bar_.timestamp)
 * around that call so the producer's implicit FX reads converted at the mark;
 * E21's ruling is to thread the rate, never the clock.
 *
 * Section 1, the economics: a stepped account-currency FX curve whose step
 * sits at each of seven instants around the range end -- a bar before the
 * mark, just before it, at it, just after it, a bar after it, and on an
 * aggregated chart whose last script bar is partial -- and every range-end
 * row's P&L, P&L percent, commission and both excursions must equal the
 * explicit-rate producer's arithmetic at account_currency_fx_at(mark time),
 * restated here from the builder: (fill - entry) * qty * point value * fx
 * less the entry share and an exit fee at the same rate, the excursions the
 * owner's range-end branch answers (the carried extremes against the mark)
 * converted at that rate. Section 2, the clock: no source-layer file writes
 * current_bar_.timestamp (the grep the lane's acceptance names).
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "oracle_fixture_config_shim.hpp"

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
        }                                                                      \
    } while (0)

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

Bar mk_bar(std::int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o;
    b.high = h;
    b.low = l;
    b.close = c;
    b.volume = 1.0;
    b.timestamp = ts;
    return b;
}

// A 10-lot long entered on the first bar and held to the end of the feed, a
// 0.1 % commission, on a curve 1.25 until `step_ms` and 2.5 from it.
class Host : public pineforge::source::PineStrategyHost {
public:
    Host() {
        initial_capital_ = 10000.0;
        syminfo_mintick_ = 0.01;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 10.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.1;
        pyramiding_ = 1;
        margin_call_enabled_ = false;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true);
    }
    using BacktestEngine::account_currency_fx_at;
    using BacktestEngine::equity_curve_;
    using BacktestEngine::pyramid_entries_;
    using BacktestEngine::range_end_trades_;
};

struct Case {
    const char* name;
    std::int64_t step_offset_ms;   // from the mark: the range-end row's date
    bool aggregated;               // 1-minute input, 5-minute script bars
};

void the_range_end_rows_convert_at_the_mark() {
    const Case cases[] = {
        {"step a bar before the mark", -300000, false},
        {"step just before the mark", -1, false},
        {"step at the mark", 0, false},
        {"step just after the mark", 1, false},
        {"step a bar after the mark", 300000, false},
        {"aggregated, step at the mark", 0, true},
        {"aggregated, step just after", 1, true},
        // In the partial tail the feed ends in: the last input bar's own rate.
        {"aggregated, step in the tail", 360000, true},
    };
    for (const Case& c : cases) {
        std::vector<Bar> bars;
        const std::int64_t step = c.aggregated ? 60000 : 300000;
        // Enough bars for an open lot to be marked; on the aggregated chart
        // the feed ends two minutes into the last 5-minute bucket.
        const int count = c.aggregated ? 27 : 6;
        for (int i = 0; i < count; ++i) {
            const double base = 100.0 + 0.5 * i;
            bars.push_back(mk_bar(step * i, base, base + 2.0, base - 1.0, base + 0.25));
        }
        // The mark is the last script bar the chart completed: the last bar
        // itself, or on the aggregated chart the 20-minute bucket (the feed
        // ends two minutes into the 25-minute one).
        const std::int64_t anchor = c.aggregated ? 1200000 : bars.back().timestamp;
        Host host;
        const std::int64_t fx_ts[] = {0, anchor + c.step_offset_ms};
        const double fx_rate[] = {1.25, 2.5};
        CHECK(host.set_account_currency_fx_series(fx_ts, fx_rate, 2));
        if (c.aggregated) host.run(bars.data(), count, "1", "5", false);
        else host.run(bars.data(), count);
        CHECK(host.last_error().empty());
        CHECK(host.range_end_trades_.size() == 1);
        CHECK(host.pyramid_entries_.size() == 1);
        CHECK(!host.equity_curve_.empty());
        if (host.range_end_trades_.size() != 1 || host.pyramid_entries_.size() != 1
            || host.equity_curve_.empty()) {
            continue;
        }
        const Trade& row = host.range_end_trades_[0];
        const PyramidEntry& lot = host.pyramid_entries_[0];
        const std::int64_t mark_time = host.equity_curve_.back().time_ms;
        const double fx = host.account_currency_fx_at(mark_time);
        // The mark is the terminal bar's close on the chart tick.
        const double fill = row.exit_price;
        const double pv = 1.0;
        const double entry_fee = lot.entry_commission_account;
        const double exit_fee = fill * lot.qty * pv * fx * (0.1 / 100.0);
        double pnl = (fill - lot.price) * lot.qty * pv * fx;
        pnl -= entry_fee + exit_fee;
        const double entry_cost = lot.price * lot.qty * pv * fx;
        const double pnl_pct = (pnl / entry_cost) * 100.0;
        // The owner's range-end branch: the carried extremes against the mark.
        double fill_fav = (fill - lot.price) * lot.qty;
        if (std::abs(fill - lot.price) < 1e-9) fill_fav = 0.0;
        const double favorable = std::max(lot.max_runup, fill_fav);
        const double adverse = std::max(lot.max_drawdown, -fill_fav);
        const double runup = std::max(0.0, favorable * pv * fx - entry_fee);
        const double drawdown = std::max(0.0, adverse * pv * fx + entry_fee);
        std::printf("  [%-30s] mark %lld fx %.17g | pnl %.17g (want %.17g) commission %.17g"
                    " (want %.17g) runup %.17g (want %.17g)\n",
                    c.name, static_cast<long long>(mark_time), fx, row.pnl, pnl, row.commission,
                    entry_fee + exit_fee, row.max_runup, runup);
        CHECK(mark_time == anchor);
        CHECK(row.open_at_end);
        CHECK(row.exit_time == mark_time);
        CHECK(bits(row.pnl) == bits(pnl));
        CHECK(bits(row.pnl_pct) == bits(pnl_pct));
        CHECK(bits(row.commission) == bits(entry_fee + exit_fee));
        CHECK(bits(row.max_runup) == bits(runup));
        CHECK(bits(row.max_drawdown) == bits(drawdown));
    }
}

#if defined(PINEFORGE_B_ADAPTER_SOURCE_DIR)
std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}
#endif

void the_source_layer_writes_no_clock() {
#if defined(PINEFORGE_B_ADAPTER_SOURCE_DIR)
    const std::string dir = PINEFORGE_B_ADAPTER_SOURCE_DIR;
    int writes = 0;
    for (const char* file : {"pine_strategy_host.cpp", "pine_adapter.cpp",
                             "pine_scheduler_native.cpp", "pine_scheduler.cpp",
                             "pine_security_eval.cpp", "pine_aux_security.cpp",
                             "pine_state_hash.cpp", "pine_strategy_commands.cpp",
                             "pine_path_resolve.cpp", "market_admission.cpp"}) {
        const std::string text = read_file(dir + "/" + file);
        CHECK(!text.empty());
        std::size_t at = 0;
        while ((at = text.find("current_bar_.timestamp =", at)) != std::string::npos) {
            ++writes;
            std::printf("  %s writes current_bar_.timestamp at offset %zu\n", file, at);
            ++at;
        }
    }
    CHECK(writes == 0);
#else
    std::printf("  PINEFORGE_B_ADAPTER_SOURCE_DIR undefined\n");
    CHECK(false);
#endif
}

void test(const char* name, void (*fn)()) {
    const int before = failures;
    std::printf("-- %s\n", name);
    fn();
    if (failures != before) std::printf("   ^^ %d failure(s)\n", failures - before);
}

}  // namespace

int main() {
    test("the range-end rows convert at the mark's rate",
         the_range_end_rows_convert_at_the_mark);
    test("the source layer writes no clock", the_source_layer_writes_no_clock);
    std::printf("R5 B-ADAPTER range-end FX: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
