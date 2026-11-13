// Synthetic reduction: a fixed half-entry exit is called on every execution,
// including calc_on_order_fills callbacks, while its entry still has units.
// The two-exit expectations express the measured TradingView rule: a fills
// once, b takes half the remaining position, and close_all takes the rest.
// R-one-exit is an EXTRAPOLATION until the public TradingView reading; it is
// not a measured rule. R-control calls a once and checks the same harness.
// Model: test_coof_reused_id_reentry.cpp (PineStrategyHost configuration/run).
// Unlike that model's command body, this body also runs after every fill.
// No market data or fixture is read. Baseline outcomes require a separate run;
// in particular, the per-origin consumed-leg guard may keep R-one-exit green.

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace pineforge;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kOpeningQty = 256.0;
constexpr double kEntryPrice = 100.0;
constexpr double kLimit = kEntryPrice + 8.0;
constexpr double kStopB = kEntryPrice - 4.0;
constexpr double kStopA = kEntryPrice - 12.0;
constexpr double kTick = 1.0;
constexpr double kLot = 1.0;
constexpr std::int64_t kStart = 1'735'689'600'000LL;
constexpr std::int64_t kInterval = 15 * 60'000LL;
constexpr int kEntryCallBar = 0;
constexpr int kEntryFillBar = kEntryCallBar + 1;
constexpr int kLimitBar = kEntryFillBar + 2;
constexpr int kStopBBar = 8;
constexpr int kStopABar = 10;
constexpr int kCloseCallBar = 13;
constexpr int kCloseFillBar = kCloseCallBar + 1;
constexpr int kBarCount = kCloseFillBar + 2;
static_assert(kLimitBar < kStopBBar && kStopBBar < kStopABar
              && kStopABar < kCloseCallBar, "exit levels precede the final close");
constexpr const char* kEntryId = "e";
// strategy_close_all()'s report label, not an additional strategy.exit.
constexpr const char* kFinalExitId = "__close__";

struct Row {
    const char* name;
    bool is_long;
    bool two_exits;
    bool call_once;
};

constexpr Row kRows[] = {
    {"R-two-exits", true, true, false},
    {"R-two-exits-short", false, true, false},
    {"R-one-exit", true, false, false},
    {"R-control", true, false, true},
};

double price_for(const Row& row, double long_price) {
    return row.is_long ? long_price : 2.0 * kEntryPrice - long_price;
}

std::vector<Bar> synthetic_bars(const Row& row) {
    struct Prices { double open, high, low, close; };
    const std::array<Prices, kBarCount> prices{{
        {100, 102, 98, 100},                         // 0: signal
        {kEntryPrice, 102, 98, 100},                 // 1: market entry
        {101, 104, 100, 103},                        // 2: neither exit
        {104, kLimit + 2, 103, kLimit + 1},          // 3: a's first limit
        {kLimit + 4, 114, kLimit + 3, 113},          // 4: beyond the limit
        {113, 115, kLimit + 3, 114},                 // 5: beyond the limit
        {114, 116, kLimit + 4, 115},                 // 6: beyond the limit
        {102, 104, 99, 100},                         // 7: neither level
        {100, 101, kStopB - 1, kStopB + 1},          // 8: b's stop
        {97, 99, kStopB - 2, 98},                    // 9: above a's stop
        {96, 98, kStopA - 1, kStopA + 2},            // 10: a's stop
        {kStopA - 2, kStopA + 1, kStopA - 3, 87},   // 11: past that stop
        {90, 93, 89, 92},                            // 12
        {92, 95, 91, 94},                            // 13: close_all call
        {94, 97, 93, 95},                            // 14: market close
        {95, 98, 94, 96},                            // 15: trailing bar
    }};
    std::vector<Bar> bars;
    for (int i = 0; i < kBarCount; ++i) {
        const Prices& p = prices[static_cast<std::size_t>(i)];
        bars.push_back({
            price_for(row, p.open),
            price_for(row, row.is_long ? p.high : p.low),
            price_for(row, row.is_long ? p.low : p.high),
            price_for(row, p.close), 1000.0, kStart + i * kInterval});
    }
    return bars;
}

class ExitProbe final : public source::PineStrategyHost {
public:
    explicit ExitProbe(const Row& row) : row_(row) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = true;
        cfg.process_orders_on_close = false;
        cfg.pyramiding = 0;
        cfg.initial_capital = 10'000'000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = kOpeningQty;
        cfg.margin_long = 100.0;
        cfg.margin_short = 100.0;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        configure_pine_strategy(cfg);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(kTick);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == kEntryCallBar && !entry_sent_) {
            entry_sent_ = true;
            strategy_entry(kEntryId, row_.is_long, kNaN, kNaN, kOpeningQty);
        }
        const double held = std::abs(signed_position_size());
        if (held == 0.0) return;
        // No ordinary-calculation filter: repeat the script's exits in every
        // fill callback as well as in each bar's normal calculation.
        if (!row_.call_once || a_calls == 0) {
            ++a_calls;
            if (first_a_call_bar < 0) first_a_call_bar = pine_bar_index();
            strategy_exit("a", kEntryId, price_for(row_, kLimit),
                          price_for(row_, kStopA), kNaN, kNaN, kNaN,
                          100.0, {}, kOpeningQty / 2.0);
        }
        if (row_.two_exits) {
            strategy_exit("b", kEntryId, kNaN, price_for(row_, kStopB),
                          kNaN, kNaN, kNaN, 100.0, {}, held / 2.0);
        }
        if (pine_bar_index() == kCloseCallBar && !close_sent_) {
            close_sent_ = true;
            strategy_close_all();
        }
    }

    double final_size() const { return signed_position_size(); }
    int a_calls = 0;
    int first_a_call_bar = -1;

private:
    const Row& row_;
    bool entry_sent_ = false;
    bool close_sent_ = false;
};

bool equal_number(const Row& row, const std::string& field,
                  double actual, double expected) {
    if (actual == expected) return true;
    std::printf("FAIL %s: %s actual=%.17g expected=%.17g\n",
                row.name, field.c_str(), actual, expected);
    return false;
}

bool equal_text(const Row& row, const std::string& field,
                const std::string& actual, const std::string& expected) {
    if (actual == expected) return true;
    std::printf("FAIL %s: %s actual=\"%s\" expected=\"%s\"\n",
                row.name, field.c_str(), actual.c_str(), expected.c_str());
    return false;
}

int exit_count(const ExitProbe& probe, const char* id) {
    int count = 0;
    for (int i = 0; i < probe.trade_count(); ++i) {
        const Trade& trade = probe.get_trade(i);
        if (!trade.open_at_end && trade.entry_id == kEntryId && trade.exit_id == id)
            ++count;
    }
    return count;
}

bool check_leg(const Row& row, const ExitProbe& probe, const char* id,
               double qty, int bar, double price) {
    int matches = 0;
    Trade actual{};
    for (int i = 0; i < probe.trade_count(); ++i) {
        const Trade& trade = probe.get_trade(i);
        if (!trade.open_at_end && trade.entry_id == kEntryId
            && trade.exit_id == id && trade.exit_bar_index == bar) {
            ++matches;
            actual = trade;
        }
    }
    const std::string leg = std::string("leg (") + kEntryId + ", " + id
        + ", bar " + std::to_string(bar) + ")";
    return equal_number(row, leg + " count", matches, 1)
        && equal_number(row, leg + " quantity", actual.qty, qty)
        && equal_number(row, leg + " price", actual.exit_price, price);
}

bool run_row(const Row& row) {
    ExitProbe probe(row);
    const auto bars = synthetic_bars(row);
    probe.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);

    // Print the entire list before any comparison, including an unexpected
    // open-at-end row, so the first failure never hides an extra a fill.
    std::printf("%s: trades=%d a_calls=%d first_a_call_bar=%d\n",
                row.name, probe.trade_count(), probe.a_calls, probe.first_a_call_bar);
    for (int i = 0; i < probe.trade_count(); ++i) {
        const Trade& t = probe.get_trade(i);
        std::printf("  exit=%s entry=%s quantity=%.17g bar=%d price=%.17g open=%d\n",
                    t.exit_id.c_str(), t.entry_id.c_str(), t.qty,
                    t.exit_bar_index, t.exit_price, t.open_at_end ? 1 : 0);
    }
    std::fflush(stdout);
    if (!equal_text(row, "run error", probe.last_error(), "")) return false;
    if (!equal_number(row, "first a call bar", probe.first_a_call_bar, kEntryFillBar))
        return false;
    if (row.call_once && !equal_number(row, "a call count", probe.a_calls, 1))
        return false;
    // Check the setup independently before diagnosing a repeated exit.
    double closed_qty = 0.0;
    for (int i = 0; i < probe.trade_count(); ++i) {
        const Trade& t = probe.get_trade(i);
        const std::string prefix = "trade " + std::to_string(i) + " ";
        if (!equal_text(row, prefix + "entry id", t.entry_id, kEntryId)
            || !equal_number(row, prefix + "entry bar", t.entry_bar_index, kEntryFillBar)
            || !equal_number(row, prefix + "entry price", t.entry_price, kEntryPrice)
            || !equal_number(row, prefix + "long", t.is_long, row.is_long)
            || !equal_number(row, prefix + "open at end", t.open_at_end, false))
            return false;
        closed_qty += t.qty;
    }
    if (!equal_number(row, "total closed quantity", closed_qty, kOpeningQty))
        return false;
    // This is the first behavioural assertion: count ALL of a's closed rows,
    // not just its correct first fill or the total quantity of the entry.
    if (!equal_number(row, "exit a fill count", exit_count(probe, "a"), 1))
        return false;
    if (!check_leg(row, probe, "a", kOpeningQty / 2.0, kLimitBar,
                   price_for(row, kLimit))) return false;
    if (!equal_number(row, "exit b fill count", exit_count(probe, "b"),
                      row.two_exits ? 1 : 0)) return false;
    if (row.two_exits
        && !check_leg(row, probe, "b", kOpeningQty / 4.0, kStopBBar,
                      price_for(row, kStopB))) return false;
    const double final_qty = row.two_exits ? kOpeningQty / 4.0 : kOpeningQty / 2.0;
    if (!equal_number(row, "final close fill count", exit_count(probe, kFinalExitId), 1)
        || !check_leg(row, probe, kFinalExitId, final_qty, kCloseFillBar,
                      bars[kCloseFillBar].open)
        || !equal_number(row, "trade count", probe.trade_count(), row.two_exits ? 3 : 2)
        || !equal_number(row, "final position", probe.final_size(), 0.0))
        return false;
    std::printf("PASS %s\n", row.name);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::printf("Expected zero arguments or one row name.\n");
        return 1;
    }
    int selected = 0;
    int failures = 0;
    for (const Row& row : kRows) {
        if (argc == 2 && std::string(argv[1]) != row.name) continue;
        ++selected;
        if (!run_row(row)) ++failures;
    }
    if (selected == 0) {
        std::printf("Unknown row: %s\n", argv[1]);
        return 1;
    }
    std::printf("%d rows, %d failures\n", selected, failures);
    return failures == 0 ? 0 : 1;
}
