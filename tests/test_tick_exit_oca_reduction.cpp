#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
using namespace pineforge;
constexpr double na = std::numeric_limits<double>::quiet_NaN();

enum class Action { Repeat, Reprice, Resize, Percent, Cancel };

struct Row {
    int entry, exit;  // -1 denotes a lot still open at the end.
    double price, qty;
};

struct Case {
    const char* name;
    int second_qty;
    int third_signal;  // -1: none; 2: before the first exit fill; 4: after the second touch.
    bool group;
    Action action;
    int at_second_touch;
    std::vector<Row> rows;
};

double price(bool buy, double long_price) {
    return buy ? long_price : 80 - long_price;
}

std::vector<Bar> bars_for(bool buy) {
    std::vector<Bar> bars;
    const auto bar = [&](double open, double high, double low, double close) {
        const auto time = (1 + static_cast<std::int64_t>(bars.size())) * 60000;
        bars.push_back({price(buy, open), price(buy, buy ? high : low),
                        price(buy, buy ? low : high), price(buy, close), 64, time});
    };
    bar(40, 41, 39, 40);
    bar(40, 44, 39, 43);  // First opening; neither bracket leg is reached.
    bar(44, 46, 43, 45);  // Second opening.
    bar(47, 51, 44, 49);  // First profit at 50; the second profit is at 54.
    bar(49, 55, 48, 52);  // Crosses 54, but not a repriced profit at 57.
    bar(56, 58, 55, 56);  // Third opening when requested.
    bar(56, 67, 55, 62);  // Third profit at 66.
    return bars;
}

class Host final : public source::PineStrategyHost {
public:
    Host(const Case& c, bool buy) : c_(c), buy_(buy) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.pyramiding = 3;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(1);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0 || (c_.third_signal == 2 && i == 2)) strategy_entry("E", buy_, na, na, 1);
        if (i == 1) strategy_entry("E", buy_, na, na, c_.second_qty);
        if (i == 4) at_second_touch = std::abs(signed_position_size());
        // A changed argument or cancellation is tested on the first fill's
        // close. Other rows repeat the original call on every held bar.
        if (i > 0 && signed_position_size() != 0 && (c_.action == Action::Repeat || i <= 3)) {
            const auto action = i == 3 ? c_.action : Action::Repeat;
            if (action == Action::Cancel) strategy_cancel("X");
            strategy_exit("X", "E", na, na, na, na, na, action == Action::Percent ? 50 : 100,
                          "", action == Action::Resize ? 2 : na, c_.group ? "shared" : "",
                          action == Action::Reprice ? 13 : 10, 10);
        }
        // After the call: this opening gets its bracket from the next call.
        if (c_.third_signal == 4 && i == 4) strategy_entry("E", buy_, na, na, 1);
    }

    double at_second_touch = na;
private:
    const Case& c_;
    bool buy_;
};

bool check(const Case& c, bool buy) {
    const auto bars = bars_for(buy);
    Host host(c, buy);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    int closed = 0;
    for (const auto& row : c.rows) closed += row.exit >= 0;
    bool ok = host.last_error().empty() && host.at_second_touch == c.at_second_touch
        && host.trade_count() == closed && host.report_trade_count() == static_cast<int>(c.rows.size());
    for (int i = 0; i < host.report_trade_count() && i < static_cast<int>(c.rows.size()); ++i) {
        const auto& want = c.rows[i];
        const auto got = host.get_report_trade(i);
        ok &= got.entry_id == "E" && got.is_long == buy && got.qty == want.qty
            && got.entry_bar_index == want.entry && got.entry_time == bars[want.entry].timestamp
            && got.entry_price == bars[want.entry].open && got.open_at_end == (want.exit < 0);
        if (want.exit >= 0)
            ok &= got.exit_id == "X" && got.exit_bar_index == want.exit
                && got.exit_time == bars[want.exit].timestamp && got.exit_price == price(buy, want.price);
    }
    std::printf("%s %s %s at-second-touch=%g closed=%d reported=%d\n",
                ok ? "PASS" : "FAIL", c.name, buy ? "long" : "short", host.at_second_touch,
                host.trade_count(), host.report_trade_count());
    if (!host.last_error().empty()) std::fprintf(stderr, "%s\n", host.last_error().c_str());
    return ok;
}
} // namespace

int main() {
    const Case cases[] = {
        {"equal", 1, -1, true, Action::Repeat, 1, {{1, 3, 50, 1}, {2, -1, 0, 1}}},
        // The third bracket fills at its own price and books the second lot.
        {"fresh-opening", 1, 4, true, Action::Repeat, 1,
            {{1, 3, 50, 1}, {2, 6, 66, 1}, {5, -1, 0, 1}}},
        {"three-standing", 1, 2, true, Action::Repeat, 2,
            {{1, 3, 50, 1}, {2, -1, 0, 1}, {3, -1, 0, 1}}},
        {"remainder", 3, -1, true, Action::Repeat, 1,
            {{1, 3, 50, 1}, {2, 4, 54, 2}, {2, -1, 0, 1}}},
        {"independent", 1, 4, false, Action::Repeat, 0,
            {{1, 3, 50, 1}, {2, 4, 54, 1}, {5, 6, 66, 1}}},
        {"new-distance", 1, -1, true, Action::Reprice, 1, {{1, 3, 50, 1}, {2, 5, 57, 1}}},
        {"new-quantity", 4, -1, true, Action::Resize, 2,
            {{1, 3, 50, 1}, {2, 4, 54, 2}, {2, -1, 0, 2}}},
        {"new-percent", 4, -1, true, Action::Percent, 2,
            {{1, 3, 50, 1}, {2, 4, 54, 2}, {2, -1, 0, 2}}},
        {"cancel-and-call", 1, -1, true, Action::Cancel, 0, {{1, 3, 50, 1}, {2, 4, 54, 1}}},
    };
    bool ok = true;
    for (const auto& c : cases)
        for (bool buy : {true, false}) ok &= check(c, buy);
    return ok ? 0 : 1;
}
