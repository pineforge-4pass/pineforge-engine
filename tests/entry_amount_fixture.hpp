#pragma once

#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace entry_amount {
using pineforge::Bar;
using pineforge::NativeDecisionContext;
using pineforge::NativeLifecycleKind;
using pineforge::PositionSide;
using pineforge::QtyType;
namespace source = pineforge::source;
namespace no = pineforge::native_order;

constexpr double missing = std::numeric_limits<double>::quiet_NaN();
enum class Amount { Cash, Equity, DefaultEquity, Fixed, UntypedUnits };

inline const char* name(Amount a) {
    switch (a) {
    case Amount::Cash: return "cash";
    case Amount::Equity: return "equity";
    case Amount::DefaultEquity: return "default equity";
    case Amount::Fixed: return "fixed";
    case Amount::UntypedUnits: return "untyped units";
    }
    return "";
}

inline double operand(Amount a) {
    if (a == Amount::DefaultEquity) return missing;
    if (a == Amount::Cash) return 825;
    if (a == Amount::Equity) return 4.25;
    return 5;
}

inline int type(Amount a) {
    if (a == Amount::Cash) return static_cast<int>(QtyType::CASH);
    if (a == Amount::Equity) return static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    if (a == Amount::Fixed) return static_cast<int>(QtyType::FIXED);
    return -1;
}

inline Bar bar(int index, double open, double high, double low, double close) {
    return {open, high, low, close, 1000, 1'704'067'200'000LL + 60'000LL * index};
}
inline Bar flat(int index, double price) { return bar(index, price, price, price, price); }

struct Fill {
    std::string id;
    int bar;
    std::uint64_t incarnation, ordinal;
    double price, opened, closed;
};

class Host : public source::PineStrategyHost {
public:
    Host(Amount a, bool at_close = false, int pyramiding = 3, double margin = 1)
        : amount(a) {
        source::PineStrategyConfig c;
        c.initial_capital = 20'000;
        c.default_qty_type = static_cast<int>(a == Amount::DefaultEquity
            ? QtyType::PERCENT_OF_EQUITY : QtyType::FIXED);
        c.default_qty_value = a == Amount::DefaultEquity ? 4.25 : 1;
        c.pyramiding = pyramiding;
        c.margin_long = c.margin_short = margin;
        c.process_orders_on_close = at_close;
        configure_pine_strategy(c);
        set_syminfo_session("24x7");
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_mintick(0.25);
        set_syminfo_pointvalue(2);
        set_syminfo_metadata("qty_step", 0.25);
        set_margin_call_enabled(false);
    }
    void entry(const char* id, bool buy, double limit = missing, double stop = missing) {
        strategy_entry(id, buy, limit, stop, operand(amount), "", "", 0, type(amount));
    }
    bool completed(const std::vector<Bar>& bars) {
        run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        return last_error().empty() && native_state().kind == NativeLifecycleKind::Completed
            && last_bar == static_cast<int>(bars.size()) - 1;
    }
    void on_native_applied_event(const no::ExecutionAppliedEvent& e,
                                 const NativeDecisionContext&) override {
        fills.push_back({e.request().label, e.interval_index(), e.handle().incarnation,
                         e.ordinal, e.resolved_price, e.opened_units, e.closed_units});
    }
    const Fill* only_fill(const char* id) const {
        const Fill* found = nullptr;
        for (const auto& f : fills) if (f.id == id) {
            if (found) return nullptr;
            found = &f;
        }
        return found;
    }
    Amount amount;
    std::vector<Fill> fills;
    int last_bar = -1;
};

inline bool report(const char* status, const char* row, Amount a) {
    std::printf("%s %s / %s\n", status, row, name(a));
    return status[0] == 'P';
}
} // namespace entry_amount
