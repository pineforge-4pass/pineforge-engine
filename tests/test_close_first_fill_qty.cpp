// Adapter probe with synthetic prices. Observe the broker's applied transaction as
// well as the lot it opened: a turn can have the right net size for the wrong reason.
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
constexpr double missing = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t start = 1'704'067'200'000LL;
constexpr std::int64_t interval = 900'000;
constexpr int bar_count = 7;

enum class Prefix { Flatten, Direct, CanceledFlatten };

struct OrderSizes {
    const char* name;
    double step, base_request, turn_request;
    // Counts of tradable lots, specified independently of the engine's lot floor.
    int base_lots, turn_lots;
    bool use_default;
};

struct Fill {
    std::string id;
    std::int64_t epoch;
    double working, closed, opened;
};

class Probe final : public source::PineStrategyHost {
public:
    Probe(OrderSizes sizes, bool held_long, Prefix prefix, bool retries)
        : sizes(sizes), held_long(held_long), prefix(prefix), retries(retries) {
        source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_value = sizes.base_request;
        configure_pine_strategy(config); // fixed, pyramiding=1, 100% margins, next open
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.01);
        set_syminfo_metadata("qty_step", sizes.step);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i >= 0 && i < bar_count) positions[i] = physical_position().signed_units;
        if (i == 0) strategy_entry("base", held_long, missing, missing,
                                   sizes.use_default ? missing : sizes.base_request);
        if (i == 2) {
            if (prefix != Prefix::Direct) strategy_close("", "");
            if (prefix == Prefix::CanceledFlatten) strategy_cancel_all();
            strategy_entry("turn", !held_long, missing, missing,
                           sizes.use_default ? missing : sizes.turn_request);
            if (retries) {
                strategy_close("base", "c1", missing, missing, false, 1);
                strategy_close("base", "c2", missing, missing, false, 2);
            }
        }
        if (i == 3) strategy_close("turn", "", missing, missing, false, 3);
        if (i == 4) strategy_close("", "end");
    }

    OrderSizes sizes;
    bool held_long;
    Prefix prefix;
    bool retries;
    std::array<double, bar_count> positions{{missing, missing, missing, missing,
                                             missing, missing, missing}};
    std::vector<Fill> fills;

private:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& e,
                                 const NativeDecisionContext&) override {
        fills.push_back({e.request().label, e.interval_open_ms(), e.filled_working,
                         e.closed_units, e.opened_units});
    }
};

struct Checks {
    std::string row;
    int failures = 0;

    void number(const std::string& field, double got, double want) {
        if (got == want) return;
        std::fprintf(stderr, "FAIL %s %s: expected %.17g got %.17g\n",
                     row.c_str(), field.c_str(), want, got);
        ++failures;
    }
    void label(const std::string& field, const std::string& got, const std::string& want) {
        if (got == want) return;
        std::fprintf(stderr, "FAIL %s %s: expected '%s' got '%s'\n",
                     row.c_str(), field.c_str(), want.c_str(), got.c_str());
        ++failures;
    }
};

int check_case(const OrderSizes& sizes, bool held_long, Prefix prefix, bool retries) {
    const bool flatten = prefix == Prefix::Flatten;
    Checks check{std::string(sizes.name) + (held_long ? "/sell" : "/buy")
        + (flatten ? "/flatten" : prefix == Prefix::Direct ? "/direct" : "/canceled")
        + (retries ? "/two-closes" : "/no-closes")};
    std::array<Bar, bar_count> bars;
    for (int i = 0; i < bar_count; ++i) {
        const double open = 100.0 + i;
        bars[i] = {open, open + 1, open - 1, open, 1000.0, start + i * interval};
    }
    Probe host(sizes, held_long, prefix, retries);
    host.run(bars.data(), bar_count, "15", "15", false);
    check.label("engine error", host.last_error(), "");

    const double base = sizes.base_lots * sizes.step, own = sizes.turn_lots * sizes.step;
    const int sign = held_long ? 1 : -1;
    const double turn_closed = flatten ? 0.0 : base;
    const auto fill = [&](const char* id, int bar, double qty, double closed, double opened) {
        int count = 0;
        for (const auto& f : host.fills) {
            if (f.id != id) continue;
            ++count;
            const std::string label(id);
            check.number(label + " booked qty", f.working, qty);
            check.number(label + " closed units", f.closed, closed);
            check.number(label + " signed opened units", f.opened, opened);
            check.number(label + " fill epoch", f.epoch, bars[bar].timestamp);
        }
        check.number(std::string(id) + " fill count", count, 1);
    };
    fill("base", 1, base, 0, sign * base);
    if (flatten) fill("__pine_close_all", 3, base, base, 0);
    fill("turn", 3, own + turn_closed, turn_closed, -sign * own);
    fill("__close__turn", 4, own, own, 0);
    check.number("all applied fills", host.fills.size(), flatten ? 4 : 3);
    const std::array<double, bar_count> want{{0, sign * base, sign * base, -sign * own,
                                              0, 0, 0}};
    for (int i = 0; i < bar_count; ++i)
        check.number("position at bar " + std::to_string(i), host.positions[i], want[i]);

    check.number("closed trades", host.closed_trade_count(), 2);
    const auto trade = [&](int i, const char* entry, const char* exit, bool is_long,
                           double qty, int entry_bar, int exit_bar) {
        if (host.closed_trade_count() <= static_cast<std::size_t>(i)) return;
        const auto t = host.get_trade(i);
        const std::string label(entry);
        check.label(label + " entry ID", t.entry_id, entry);
        check.label(label + " exit ID", t.exit_id, exit);
        check.number(label + " side", t.is_long, is_long);
        check.number(label + " trade qty", t.qty, qty);
        check.number(label + " entry epoch", t.entry_time, bars[entry_bar].timestamp);
        check.number(label + " exit epoch", t.exit_time, bars[exit_bar].timestamp);
        check.number(label + " entry price", t.entry_price, bars[entry_bar].open);
        check.number(label + " exit price", t.exit_price, bars[exit_bar].open);
    };
    trade(0, "base", flatten ? "__pine_close_all" : "turn", held_long, base, 1, 3);
    trade(1, "turn", "__close__turn", !held_long, own, 3, 4);
    std::printf("%s %s\n", check.failures ? "FAIL" : "PASS", check.row.c_str());
    return check.failures;
}
} // namespace

int main() {
    constexpr double step = 1.0;
    constexpr int base_lots = 5, boundary = 11;
    const double base = (base_lots + 0.5) * step, edge = boundary * step;
    const OrderSizes sizes[] = {
        {"default", 0.0001, 1, 1, 10000, 10000, true},
        {"fractional-default", 0.25, 2.5, 2.5, 10, 10, true},
        {"below", step, base, std::nextafter(edge, 0.0), base_lots, boundary - 1, false},
        {"exact", step, base, edge, base_lots, boundary, false},
        {"above", step, base, std::nextafter(edge, edge + step), base_lots, boundary, false},
        {"smaller-turn", 0.25, 3.625, 1.625, 14, 6, false},
    };
    int failures = 0;
    for (const auto& row : sizes) {
        for (bool held_long : {true, false}) {
            for (bool retries : {false, true})
                failures += check_case(row, held_long, Prefix::Flatten, retries);
            // The original side still needs closing. A genuine reversal spends
            // the held lots in addition to its own, even if a flatten was canceled.
            failures += check_case(row, held_long, Prefix::Direct, true);
            failures += check_case(row, held_long, Prefix::CanceledFlatten, true);
        }
    }
    return failures == 0 ? 0 : 1;
}
