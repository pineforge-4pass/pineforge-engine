// These bars test order rules, not a recorded TradingView trade list. Each
// waiting half of three whole units gets one; a later stop takes at most the
// two (or one) left. Resolving a half cannot cost a resting stop its next bar
// or make a reissue leave the old price working.
#include <pineforge/source/pine_native_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr double tick = 0.25;
constexpr int fixed_units = 3;

enum class Parent { Working, SignalClose, Limit };
struct Example {
    const char* name;
    Parent parent;
    bool buy;
    int stop_percent;
    const char* stop_group = "";
    bool replace = false;
    bool check_binding = false;
    int cash = 0;
    int signal_price = 99;
    int fill_price = 100;

    bool two_halves() const { return parent == Parent::Working; }
    int requested_qty() const { return cash ? cash : fixed_units; }
    int qty_type() const { return cash ? static_cast<int>(QtyType::CASH) : -1; }
    // Unit point value, FX and lot step: integer division floors cash to contracts.
    int opened() const { return cash ? cash / fill_price : fixed_units; }
    int side() const { return buy ? 1 : -1; }
    int entry_bar() const { return parent == Parent::SignalClose ? 0 : 1; }
    double entry_limit() const { return fill_price + side(); }
    double target(int n) const { return fill_price + side() * n * 8 * tick; }
    double stop(int n) const { return fill_price - side() * n * 8 * tick; }
};

std::vector<Bar> prices(const Example& e) {
    std::vector<Bar> bars;
    const auto add = [&](double open, double extreme) {
        bars.push_back({open, std::max(open, extreme), std::min(open, extreme), open,
                        1000.0, 1'704'067'200'000LL + static_cast<std::int64_t>(bars.size()) * 60'000});
    };
    const double open_price = e.fill_price;
    if (e.parent == Parent::Working) add(e.signal_price, e.signal_price);
    if (e.parent == Parent::Limit) add(e.entry_limit() + e.side(), e.entry_limit() + e.side());
    add(open_price, open_price);
    if (e.replace || e.parent == Parent::Limit) add(open_price, open_price);
    // Cross just the old stop. In the reissue case it must no longer fill.
    add(open_price, e.stop(1) - e.side());
    if (e.replace) add(open_price, e.stop(2) - e.side());
    add(open_price, e.target(1) + e.side());
    if (e.two_halves()) add(open_price, e.target(2) + e.side());
    add(open_price, open_price);
    return bars;
}

struct Fill {
    std::string id, comment;
    int bar;
    NativePathPhase phase;
    double price, opened, closed;
    std::uint64_t incarnation;
};

class Probe final : public source::PineNativeHost {
public:
    explicit Probe(Example e) : e_(e) {
        source::PineStrategyConfig config;
        config.process_orders_on_close = e.parent == Parent::SignalClose;
        // At pyramiding one a commission keeps the market entry working at
        // the signal instead of holding it to the end of the calculation.
        config.commission_value = e.two_halves() ? 0.1 : 0.0;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_mintick(tick);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", 1.0);
    }

    void on_source_bar(const Bar&) override {
        const int bar = static_cast<int>(pine_bar_index());
        last_bar = bar;
        if (bar == 0) {
            const bool priced = e_.parent == Parent::Limit;
            strategy_entry("Entry", e_.buy, priced ? e_.entry_limit() : na, na,
                           e_.requested_qty(), e_.name, "", 0, e_.qty_type());
            target("Half", 8);
            if (e_.two_halves()) target("Second", 16);
            stop(1);
            flat_at_signal = physical_position().signed_units == 0.0;
            for (const auto& item : adapter_.fixture_pending_snapshots()) {
                const auto& s = item.snapshot;
                if (s.comment != e_.name) continue;
                if (!item.staged && s.opening && s.source_id == "Entry"
                    && s.is_long == e_.buy && s.requested_qty == e_.requested_qty()
                    && s.qty_type == e_.qty_type()
                    && (priced ? s.exit_levels.limit == e_.entry_limit() : std::isnan(s.exit_levels.limit))
                    && std::isnan(s.exit_levels.stop)) ++parents;
                if (s.from_entry != "Entry") continue;
                if (item.staged && std::isnan(s.exit_levels.limit) && s.qty_percent == 50) {
                    if (s.source_id == "Half" && s.exit_levels.profit_ticks == 8) ++halves;
                    if (s.source_id == "Second" && s.exit_levels.profit_ticks == 16) ++seconds;
                }
                if (!item.staged && s.source_id == "Stop" && s.exit_levels.stop == e_.stop(1)
                    && std::isnan(s.requested_qty) && s.oca_name == e_.stop_group) ++resting;
            }
        } else if (e_.replace && bar == 1) {
            stop(2);
            for (const auto& item : adapter_.fixture_pending_snapshots())
                if (!item.staged && item.snapshot.source_id == "Stop"
                    && item.snapshot.from_entry == "Entry") ++stops_after_reissue;
        } else if (e_.check_binding && bar == e_.entry_bar() + 1) {
            for (const auto& item : adapter_.fixture_pending_snapshots()) {
                const auto& s = item.snapshot;
                if (item.staged || s.source_id != "Half" || s.from_entry != "Entry"
                    || s.comment != e_.name || s.exit_levels.limit != e_.target(1)) continue;
                ++live_halves;
                half_qty = s.requested_qty;
                half_origin = s.bracket_origin.incarnation;
            }
        }
    }

    void on_native_applied_event(const no::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext& ctx) override {
        fills.push_back({event.request().label, event.request().comment,
                         static_cast<int>(ctx.coordinate.interval_index), ctx.coordinate.path_phase,
                         event.resolved_price, event.opened_units, event.closed_units,
                         event.handle().incarnation});
    }

    bool flat_at_signal = false;
    int parents = 0, halves = 0, seconds = 0, resting = 0, last_bar = -1;
    int live_halves = 0, stops_after_reissue = 0;
    double half_qty = na;
    std::uint64_t half_origin = 0;
    std::vector<Fill> fills;

private:
    void target(const char* id, double ticks) {
        strategy_exit(id, "Entry", na, na, na, na, na, 50, e_.name, na, "", ticks, na);
    }
    void stop(int n) {
        strategy_exit("Stop", "Entry", na, e_.stop(n), na, na, na, e_.stop_percent,
                      n == 1 ? e_.name : "replacement", na, e_.stop_group);
    }
    Example e_;
};

bool check(Example e) {
    Probe p(e);
    const auto bars = prices(e);
    p.fixture_retain_all_events();
    p.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    const auto failure = [&](const char* status, const char* reason) {
        std::printf("%s %s: %s\n", status, e.name, reason);
        return false;
    };
    if (!p.last_error().empty() || p.native_state().kind != NativeLifecycleKind::Completed
        || p.last_bar != static_cast<int>(bars.size()) - 1) {
        std::printf("last_bar=%d error=%s\n", p.last_bar, p.last_error().c_str());
        return failure("NOT COMPARED", "incomplete run");
    }
    if (!p.flat_at_signal || p.parents != 1 || p.halves != 1 || p.seconds != int(e.two_halves()))
        return failure("NOT COMPARED", "working parent and waiting definitions at signal");
    // In the two-half case admission of the stop is itself under test.
    if (!e.two_halves() && p.resting != 1)
        return failure("NOT COMPARED", "resting price stop at signal");
    for (const auto& fill : p.fills)
        std::printf("fill %s %s bar=%d price=%g opened=%g closed=%g\n", fill.id.c_str(),
                    fill.comment.c_str(), fill.bar, fill.price, fill.opened, fill.closed);
    if (p.fills.empty()) return failure("NOT COMPARED", "missing entry");
    const auto& entry = p.fills.front();
    const auto phase = e.entry_bar() == 0 ? NativePathPhase::Close : NativePathPhase::Open;
    const int units = e.opened();
    const int half = units * 50 / 100;
    if (entry.id != "Entry" || entry.comment != e.name || entry.incarnation == 0
        || entry.bar != e.entry_bar() || entry.phase != phase || entry.price != e.fill_price
        || entry.opened != e.side() * units || entry.closed != 0)
        return failure("NOT COMPARED", "wrong entry fill");
    bool binding_ok = true;
    if (e.check_binding) {
        const bool priced = e.parent == Parent::Limit;
        if (p.live_halves != 1)
            return failure("NOT COMPARED", "no unique working half at its fill-relative limit");
        if (priced ? !std::isnan(p.half_qty) : p.half_qty != half || p.half_origin != entry.incarnation)
            binding_ok = failure("FAIL", priced ? "priced partial lost percentage sizing"
                                                : "partial not bound to opening units");
    }
    if (e.replace && (p.stops_after_reissue != 1
        || std::any_of(p.fills.begin(), p.fills.end(), [&](const Fill& fill) {
            return fill.id == "Stop" && fill.comment == e.name;
        }))) return failure("FAIL", "reissue did not replace the resting stop");

    const auto closed = [&](const char* id, const char* comment, int bar, double price, int qty) {
        return std::count_if(p.fills.begin(), p.fills.end(), [&](const Fill& fill) {
            return fill.id == id && fill.comment == comment && fill.bar == bar
                && fill.price == price && fill.opened == 0 && fill.closed == qty;
        }) == 1;
    };
    const int stop_bar = e.replace ? 3 : e.entry_bar() + 1 + int(e.parent == Parent::Limit);
    const int left = units - half * (e.two_halves() ? 2 : 1);
    const int stop_units = std::min(units * e.stop_percent / 100, left);
    if (stop_units == 0) {
        if (std::any_of(p.fills.begin(), p.fills.end(), [](const Fill& f) { return f.id == "Stop"; }))
            return failure("FAIL", "stop filled with no unreserved contract");
    } else if (!closed("Stop", e.replace ? "replacement" : e.name, stop_bar,
                       e.stop(e.replace ? 2 : 1), stop_units)) {
        return failure("FAIL", "stop quantity on first crossing");
    }
    if (!closed("Half", e.name, stop_bar + 1, e.target(1), half))
        return failure("FAIL", "first half quantity at its limit");
    if (e.two_halves() && !closed("Second", e.name, stop_bar + 2, e.target(2), half))
        return failure("FAIL", "second half quantity at its limit");
    const unsigned expected_fills = 2u + unsigned(e.two_halves()) + unsigned(stop_units != 0);
    if (p.fills.size() != expected_fills || p.physical_position().signed_units != 0)
        return failure("FAIL", "extra fill or residual position");
    if (!binding_ok) return false;
    std::printf("PASS %s: all opening units accounted for\n", e.name);
    return true;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 1) return check({"two-halves", Parent::Working, true, 100}) ? 0 : 1;
    if (argc != 2) return 2;
    const std::string row = argv[1];
    if (row == "cash") {
        const auto cash_entry = [](const char* name, bool buy, int cash, int signal, int fill) {
            Example e{name, Parent::Working, buy, 100};
            e.cash = cash;
            e.signal_price = signal;
            e.fill_price = fill;
            return check(e);
        };
        const bool buy = cash_entry("cash-long", true, 300, 99, 100);
        const bool sell = cash_entry("cash-short", false, 300, 101, 100);
        // At 75 the cash buys four units; at 76 it buys only three.
        const bool gap = cash_entry("cash-gap-long", true, 300, 75, 76);
        const bool rounded = cash_entry("cash-rounded-short", false, 300, 77, 76);
        const bool odd_cash = cash_entry("cash-extra-dollar", true, 301, 99, 100);
        // The gap reduces three units to two: both halves fill, leaving no stop fill.
        const bool no_room = cash_entry("cash-no-remainder", true, 300, 100, 101);
        return buy && sell && gap && rounded && odd_cash && no_room ? 0 : 1;
    }
    if (row == "resting") {
        const bool named = check({"named-stop", Parent::SignalClose, true, 100, "protection"});
        const bool later = check({"later-stop", Parent::SignalClose, true, 75});
        const bool replaced = check({"reissued-stop", Parent::SignalClose, true, 100, "protection", true});
        return named && later && replaced ? 0 : 1;
    }
    if (row == "sizing") {
        const bool buy = check({"market-long", Parent::SignalClose, true, 100, "", false, true});
        const bool sell = check({"market-short", Parent::SignalClose, false, 100, "", false, true});
        // Price improvement to 100 makes the fill price distinguishable from
        // either limit. Both controls keep the same exit quantities and prices.
        const bool limit_sell = check({"limit-short", Parent::Limit, false, 100, "", false, true});
        const bool limit_buy = check({"limit-long", Parent::Limit, true, 100, "", false, true});
        return buy && sell && limit_sell && limit_buy ? 0 : 1;
    }
    return 2;
}
