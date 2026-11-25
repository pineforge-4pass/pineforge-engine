// The bracket belongs to an opening fill, even when FIFO closes another lot
// and the script immediately calls the exit again.
#include <filesystem>
#define PINEFORGE_ORDER_PRINT_FIXTURE_ROOT (std::filesystem::path(__FILE__).parent_path() / "fixtures").string()
#include "order_print_tape_fixture.hpp"
#undef PINEFORGE_ORDER_PRINT_FIXTURE_ROOT

#include <algorithm>
#include <array>

namespace {
using namespace pineforge;
constexpr double na = order_print_tape::missing;

class Host final : public source::PineStrategyHost {
public:
    Host(bool buy, bool recalc, bool edit, bool third, bool refill)
        : buy_(buy), calc_on_fills_(recalc), edit_(edit), third_(third), refill_(refill) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.calc_on_order_fills = recalc;
        config.pyramiding = third ? 3 : 2;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        ++calls.at(i);
        // One entry instruction per signal. Every fill callback still calls
        // the exit, including the callback immediately after the first close.
        if (calls[i] == 1) {
            if (i == 0) strategy_entry("E", buy_, na, na, 2);
            if (third_ && i == 8) strategy_entry("E", buy_, na, na, 1);
        }
        // A refill placed in the opening callback can fill at that same
        // open. The other rows place the add at the ordinary close.
        if (i == 1 && calls[i] == (calc_on_fills_ && !refill_ ? 2 : 1))
            strategy_entry("E", buy_, na, na, 1);
        const auto upper_leg = [&]() -> std::uint64_t {
            for (const auto& w : native_working_requests()) {
                const auto& req = w.definition->request;
                if (req.label == "X" && (buy_ ? std::holds_alternative<native_order::Limit>(req.trigger)
                                               : std::holds_alternative<native_order::Stop>(req.trigger)))
                    return w.definition->handle.incarnation;
            }
            return 0;
        };
        const bool edit_now = edit_ && i == 6;
        const auto before = edit_now ? upper_leg() : 0;
        const double lower_ticks = edit_ && i >= 6 ? 60000 : 50000;
        strategy_exit("X", "E", na, na, na, na, na, 100, "", na, "",
                      buy_ ? 50000 : lower_ticks, buy_ ? lower_ticks : 50000);
        if (edit_now) kept_upper = before != 0 && before == upper_leg();
    }
    std::array<int, 11> calls{};
    bool kept_upper = false;
private:
    bool buy_, calc_on_fills_, edit_, third_, refill_;
};

// None of these brackets gaps or touches both levels on a bar. On the edit
// bar itself the previous definition is still active until the close.
int first_lower(const std::vector<Bar>& bars, int entry, int last, bool edit) {
    for (int i = entry; i <= last; ++i) {
        const double d = (edit && i > 420 ? 60000 : 50000) * 0.01;
        const auto& bar = bars.at(i);
        const double basis = bars.at(entry).open;
        if (bar.open <= basis - d || bar.high >= basis + 50000 * 0.01)
            throw std::runtime_error("unexpected gap or upper-level touch");
        if (bar.low <= basis - d) return i;
    }
    throw std::runtime_error("missing lower-level touch");
}

bool check(const std::vector<Bar>& bars, bool buy, bool recalc, bool edit, bool third,
           bool refill = false) {
    constexpr int first = 414, last = 424;
    Host host(buy, recalc, edit, third, refill);
    host.run(bars.data() + first, last - first + 1, "60", "60");
    // The second bracket spends one unit of the older two-unit FIFO lot.
    // The first bracket is still for two units: it drains that lot and the
    // newer lot. The newer bracket must not be resurrected in its callback.
    struct Slice { int fifo, bracket; };
    std::vector<Slice> slices = refill ? std::vector<Slice>{{415, 415}, {415, 415}}
                                       : std::vector<Slice>{{415, 416}, {415, 415}, {416, 415}};
    if (third) slices.push_back({423, 423});
    bool ok = host.last_error().empty() && static_cast<int>(slices.size()) == host.trade_count()
        && host.report_trade_count() == host.trade_count() && (!edit || host.kept_upper);
    for (int i = 0; i < static_cast<int>(slices.size()); ++i) {
        const auto [entry, origin] = slices[i];
        const int touch = first_lower(bars, origin, last, edit);
        const double d = (edit && touch > 420 ? 60000 : 50000) * 0.01;
        if (i >= host.trade_count()) continue;
        const auto t = host.get_trade(i);
        const double qty = refill && i == 0 ? 2 : 1;
        ok &= t.entry_id == "E" && t.exit_id == "X" && t.is_long == buy && t.qty == qty
            && t.entry_bar_index == entry - first && t.exit_bar_index == touch - first
            && t.entry_time == bars[entry].timestamp && t.exit_time == bars[touch].timestamp
            && std::abs(t.entry_price - bars[entry].open) < 1e-7
            && std::abs(t.exit_price - (bars[origin].open - d)) < 1e-7;
    }
    const int fired = first_lower(bars, refill ? 415 : 416, last, edit) - first;
    if (refill) {
        ok &= host.calls[1] >= 3 && host.calls[fired] > 1;
        if (host.trade_count() == 2)
            ok &= host.get_trade(0).entry_incarnation != host.get_trade(1).entry_incarnation;
    } else if (recalc) ok &= host.calls[1] > 1 && host.calls[2] > 1 && host.calls[fired] > 1;
    else ok &= std::all_of(host.calls.begin(), host.calls.end(), [](int n) { return n == 1; });
    std::printf("%s %s %s %s entries=%d closed=%d/%zu calls-on-first-exit=%d %s\n",
                ok ? "PASS" : "FAIL", recalc ? "recalculate" : "default",
                buy ? "long" : "short", edit ? "edited" : refill ? "open-refill" : "standing",
                third ? 3 : 2,
                host.trade_count(), slices.size(), host.calls[fired], host.last_error().c_str());
    return ok;
}
} // namespace

int main() {
    const auto bars = order_print_tape::bars("qty_step_mincontract/btc60/feed");
    if (bars.size() != 1441) throw std::runtime_error("incomplete hourly feed");
    bool ok = true;
    for (bool buy : {true, false}) {
        ok &= check(bars, buy, true, false, false);
        ok &= check(bars, buy, true, true, true);
        ok &= check(bars, buy, true, false, false, true);
    }
    ok &= check(bars, true, false, false, false);
    return ok ? 0 : 1;
}
