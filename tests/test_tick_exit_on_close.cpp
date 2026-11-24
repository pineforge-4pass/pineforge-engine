// A market entry at the close cannot use the high or low that preceded it.
#include <filesystem>
#define PINEFORGE_ORDER_PRINT_FIXTURE_ROOT (std::filesystem::path(__FILE__).parent_path() / "fixtures").string()
#include "order_print_tape_fixture.hpp"
#undef PINEFORGE_ORDER_PRINT_FIXTURE_ROOT

#include <stdexcept>
#include <utility>

namespace {
using namespace pineforge;
constexpr double na = order_print_tape::missing;
constexpr double distance = 50000 * 0.01;

struct Entry { int bar; double units; };
struct Slice { int entry; int bracket; double units; };

class Host final : public source::PineStrategyHost {
public:
    Host(int first, bool buy, bool at_close, int cap, const std::vector<Entry>& orders, int edit)
        : first_(first), buy_(buy), orders_(orders), edit_(edit) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.process_orders_on_close = at_close;
        config.pyramiding = cap;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.01);
    }
    void on_source_bar(const Bar&) override {
        for (const auto& order : orders_)
            if (order.bar == first_ + pine_bar_index())
                strategy_entry("E", buy_, na, na, order.units);
        const double ticks = first_ + pine_bar_index() < edit_ ? 50000 : 30000;
        strategy_exit("X", "E", na, na, na, na, na, 100, "", na, "", ticks, ticks);
    }
private:
    int first_;
    bool buy_;
    const std::vector<Entry>& orders_;
    int edit_;
};

// The windows below have unambiguous first touches on both sides of the
// market. A gap through a resting level executes at the open.
std::pair<int, double> touch(const std::vector<Bar>& bars, int first, int last, double basis,
                             int edit) {
    for (int i = first; i <= last; ++i) {
        const auto& b = bars.at(i);
        const double d = i <= edit ? distance : 30000 * 0.01;
        if (std::abs(b.open - basis) >= d) return {i, b.open};
        const bool down = b.low <= basis - d, up = b.high >= basis + d;
        if (down && up) throw std::runtime_error("ambiguous bracket in hourly fixture");
        if (down || up) return {i, basis + (up ? d : -d)};
        if (i == edit && std::abs(b.close - basis) >= 30000 * 0.01) return {i, b.close};
    }
    throw std::runtime_error("hourly fixture did not reach the bracket");
}

bool check(const char* name, const std::vector<Bar>& bars, bool buy, bool at_close, int cap,
           std::vector<Entry> orders, int last, std::vector<Slice> slices,
           int edit = std::numeric_limits<int>::max()) {
    const int first = orders.front().bar;
    Host host(first, buy, at_close, cap, orders, edit);
    host.run(bars.data() + first, last - first + 1, "60", "60");
    const auto price = [&](int entry) {
        const int signal = orders.at(entry).bar;
        return at_close ? bars.at(signal).close : bars.at(signal + 1).open;
    };
    bool ok = host.last_error().empty() && host.trade_count() == static_cast<int>(slices.size())
        && host.report_trade_count() == host.trade_count();
    for (int i = 0; i < static_cast<int>(slices.size()); ++i) {
        const auto& slice = slices[i];
        const int entry = orders.at(slice.entry).bar + (at_close ? 0 : 1);
        const auto [exit, level] = touch(bars, orders.at(slice.bracket).bar + 1, last,
                                         price(slice.bracket), edit);
        if (i >= host.trade_count()) continue;
        const auto trade = host.get_trade(i);
        const bool match = trade.entry_bar_index == entry - first
            && trade.exit_bar_index == exit - first && trade.is_long == buy
            && trade.entry_time == bars[entry].timestamp && trade.exit_time == bars[exit].timestamp
            && trade.entry_id == "E" && trade.exit_id == "X" && trade.qty == slice.units
            && std::abs(trade.entry_price - price(slice.entry)) < 1e-7
            && std::abs(trade.exit_price - level) < 1e-7;
        if (!match) std::fprintf(stderr, "%s trade %d: expected input bars %d -> %d "
                                 "at %.2f; got %d -> %d at %.2f\n", name, i, entry, exit,
                                 level, first + trade.entry_bar_index,
                                 first + trade.exit_bar_index, trade.exit_price);
        ok &= match;
    }
    std::printf("%s %s %s (closed %d/%zu, %s)\n", ok ? "PASS" : "FAIL", name,
                buy ? "long" : "short", host.trade_count(), slices.size(), host.last_error().c_str());
    return ok;
}
} // namespace

int main() {
    const auto bars = order_print_tape::bars("qty_step_mincontract/btc60/feed");
    if (bars.size() != 1441 || bars.at(1359).low >= bars.at(1359).close - distance
        || bars.at(1360).high < bars.at(1359).close + distance)
        throw std::runtime_error("expected a pre-entry low followed by a profit touch");
    bool ok = true;
    for (bool buy : {true, false}) {
        ok &= check("two closes", bars, buy, true, 2, {{117, 1}, {123, 1}}, 126,
                    {{0, 0, 1}, {1, 1, 1}});
        // The middle entry's downside level is reached first. FIFO books it
        // against the first entry; the first bracket then closes the second.
        ok &= check("three closes", bars, buy, true, 3, {{117, 1}, {119, 1}, {123, 1}}, 126,
                    {{0, 1, 1}, {1, 0, 1}, {2, 2, 1}});
        ok &= check("only newer edit marketable", bars, buy, true, 2, {{117, 1}, {119, 1}},
                    126, {{0, 1, 1}, {1, 0, 1}}, 125);
        // The third entry's target consumes the second lot and half of the
        // third. The second bracket still owns one unit when its lot is gone.
        ok &= check("bracket outlives FIFO lot", bars, buy, true, 2,
                    {{552, 1}, {553, 1}, {556, 2}}, 562,
                    {{0, 0, 1}, {1, 2, 1}, {2, 2, 1}, {2, 1, 1}});
        ok &= check("first eligible bar", bars, buy, true, 2, {{1359, 1}}, 1361, {{0, 0, 1}});
    }
    ok &= check("default opens", bars, true, false, 2, {{117, 1}, {123, 1}}, 126,
                {{0, 0, 1}, {1, 1, 1}});
    return ok ? 0 : 1;
}
