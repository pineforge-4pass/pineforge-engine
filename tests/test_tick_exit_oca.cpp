// Each entry supplies its own price. One fill reduces the other working
// orders. An unfilled bracket can be reissued; a filled one is spent even
// if its working quantity was reduced before it fired.
#include <filesystem>
#define PINEFORGE_ORDER_PRINT_FIXTURE_ROOT (std::filesystem::path(__FILE__).parent_path() / "fixtures").string()
#include "order_print_tape_fixture.hpp"
#undef PINEFORGE_ORDER_PRINT_FIXTURE_ROOT

namespace {
using namespace pineforge;
constexpr double na = order_print_tape::missing;
constexpr double distance = 0.01 * 50000;
constexpr int first = 117, last = 127;

class Host final : public source::PineStrategyHost {
public:
    Host(bool buy, bool group, int entries, bool reissue, bool larger, int newer, bool trim = false)
        : buy_(buy), group_(group), entries_(entries), reissue_(reissue), larger_(larger),
          newer_(newer), trim_(trim) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.pyramiding = entries;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
    }
    void on_source_bar(const Bar&) override {
        const int i = first + pine_bar_index();
        if (i == 117 || i == newer_ - 1 || (entries_ == 3 && i == 122))
            strategy_entry("E", buy_, na, na,
                           ((trim_ && i == 117) || (larger_ && i == newer_ - 1)) ? 2 : 1);
        if (i < (trim_ ? 124 : 126) || reissue_)
            strategy_exit("X", "E", na, na, na, na, na, 100, "", na,
                          group_ ? "shared" : "", 50000, 50000);
        if (trim_ && i == 124) strategy_close("E", "", 1);
        if (i == 126) units_after_touch = std::abs(signed_position_size());
    }
    double units_after_touch = na;
private:
    bool buy_, group_;
    int entries_;
    bool reissue_, larger_;
    int newer_;
    bool trim_;
};

bool check(const std::vector<Bar>& bars, bool buy, bool group, int entries, bool reissue,
           bool larger = false, bool later_touch = false) {
    const int newer = later_touch ? 123 : 124;
    std::vector<int> origins{118};
    if (entries == 3) origins.push_back(123);
    origins.push_back(newer);
    // Descending from the open on bar 126 reaches the higher entry's lower
    // level first. Bar 127 opens above every lower level and reaches them again.
    for (int entry : origins) {
        const double lower = bars.at(entry).open - distance;
        for (int i = entry; i != last + 1; ++i) {
            const int first_touch = entry == 123 ? 127 : 126;
            if (bars[i].high >= lower + 2 * distance || bars[i].open <= lower
                || (bars[i].low <= lower) != (i >= first_touch))
                throw std::runtime_error("unexpected hourly bracket crossings");
        }
    }
    if (!(bars[118].open > bars[124].open && bars[124].open > bars[123].open))
        throw std::runtime_error("expected three distinct lower levels");
    Host host(buy, group, entries, reissue, larger, newer);
    host.run(bars.data() + first, last - first + 1, "60", "60");
    const int closed = group ? 1 + (larger || reissue) : entries;
    const bool restored_two = later_touch && reissue;
    const int reported = entries + (larger && !restored_two ? 1 : 0);
    bool ok = host.last_error().empty() && host.trade_count() == closed
        && host.report_trade_count() == reported
        && host.units_after_touch == (later_touch ? 2 : (group ? entries - 1 : entries - 2));
    for (int i = 0; i < reported && i < host.report_trade_count(); ++i) {
        const auto t = host.get_report_trade(i);
        const int entry = origins.at(larger && i == 2 ? 1 : i);
        ok &= t.entry_bar_index == entry - first && t.entry_time == bars[entry].timestamp
            && t.is_long == buy && t.qty == (restored_two && i == 1 ? 2 : 1)
            && t.entry_id == "E" && std::abs(t.entry_price - bars[entry].open) < 1e-7;
        if (i >= closed) { ok &= t.open_at_end; continue; }
        const int owner = i == 0 ? 118 : (i == 1 ? newer : 123);
        const bool next_bar = (group && i > (larger ? 1 : 0)) || owner == 123;
        ok &= !t.open_at_end && t.exit_id == "X"
            && t.exit_bar_index == (next_bar ? 127 : 126) - first
            && t.exit_time == bars[next_bar ? 127 : 126].timestamp
            && std::abs(t.exit_price - (bars[owner].open - distance)) < 1e-7;
    }
    std::printf("%s %s %s entries=%d %s %s closed=%d/%d after-touch=%.2f %s\n",
                ok ? "PASS" : "FAIL", group ? "shared" : "default", buy ? "long" : "short",
                entries, later_touch ? "later-touch" : larger ? "unequal" : "equal",
                reissue ? "reissue" : "no-reissue",
                host.trade_count(), closed, host.units_after_touch, host.last_error().c_str());
    return ok;
}

bool independent_close(const std::vector<Bar>& bars, bool buy) {
    Host host(buy, true, 2, false, false, 124, true);
    host.run(bars.data() + first, last - first + 1, "60", "60");
    // Of two units at the first open, the market close takes one and its
    // existing bracket can now take only one. Its fill exhausts the second
    // entry's one-unit order; that lot survives the final bar.
    bool ok = host.last_error().empty() && host.trade_count() == 2 && host.report_trade_count() == 3;
    if (ok) {
        const auto close = host.get_trade(0), stop = host.get_trade(1), open = host.get_report_trade(2);
        ok = close.entry_time == bars[118].timestamp && stop.entry_time == bars[118].timestamp
            && std::abs(close.entry_price - bars[118].open) < 1e-7
            && std::abs(stop.entry_price - bars[118].open) < 1e-7
            && close.exit_time == bars[125].timestamp && std::abs(close.exit_price - bars[125].open) < 1e-7
            && close.exit_id == "__close__E" && stop.exit_id == "X" && stop.exit_time == bars[126].timestamp
            && std::abs(stop.exit_price - (bars[118].open - distance)) < 1e-7
            && close.qty == 1 && stop.qty == 1 && open.qty == 1 && open.open_at_end
            && open.entry_time == bars[124].timestamp && std::abs(open.entry_price - bars[124].open) < 1e-7
            && close.is_long == buy && stop.is_long == buy && open.is_long == buy;
    }
    std::printf("%s shared %s independent-close closed=%d/2 %s\n",
                ok ? "PASS" : "FAIL", buy ? "long" : "short", host.trade_count(), host.last_error().c_str());
    return ok;
}
} // namespace

int main() {
    const auto bars = order_print_tape::bars("qty_step_mincontract/btc60/feed");
    if (bars.size() != 1441) throw std::runtime_error("incomplete hourly feed");
    bool ok = true;
    for (bool buy : {true, false}) {
        ok &= check(bars, buy, true, 2, false);
        ok &= check(bars, buy, true, 2, true);
        ok &= check(bars, buy, true, 3, true);
        ok &= check(bars, buy, true, 2, false, true);
        ok &= check(bars, buy, true, 2, true, true);
        ok &= check(bars, buy, true, 2, false, true, true);
        ok &= check(bars, buy, true, 2, true, true, true);
        ok &= independent_close(bars, buy);
    }
    ok &= check(bars, true, false, 3, true);
    return ok ? 0 : 1;
}
