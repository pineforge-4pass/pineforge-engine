#pragma once

#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
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
using pineforge::NativePathPhase;
using pineforge::NativePriceProvenance;
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

enum class ClosePoint { Calculation, AfterCalculation };

struct Fill {
    std::string id;
    int bar;
    std::uint64_t incarnation, ordinal;
    double price, opened, closed;
    NativePathPhase phase;
    NativePriceProvenance provenance, decision_provenance;
    int charged_slots;

    bool at_close(ClosePoint point) const {
        // A synchronous fill has its own cursor but uses the calculation's
        // quote. The ordinary market matcher runs at the later close tick.
        if (point == ClosePoint::Calculation) {
            return phase == NativePathPhase::None
                && provenance == NativePriceProvenance::CurrentExecution
                && decision_provenance == NativePriceProvenance::Calculation;
        }
        return phase == NativePathPhase::Close
            && provenance == NativePriceProvenance::AfterCalculationClose
            && decision_provenance == NativePriceProvenance::AfterCalculationClose;
    }
};

// Change exactly one field of one real fill and use the row's full comparison.
template<std::size_t N, class Compare, class Change>
bool rejects_each_change(std::array<const Fill*, N> observed, Compare compare, Change change) {
    static_assert(N > 0);
    for (auto& fill : observed) {
        const auto* original = fill;
        Fill changed = *original;
        change(changed);
        fill = &changed;
        if (compare(observed)) return false;
        fill = original;
    }
    return true;
}

template<std::size_t N, class Compare>
bool rejects_wrong_close(const std::array<const Fill*, N>& observed, Compare compare) {
    return rejects_each_change(observed, compare, [](Fill& f) { f.phase = NativePathPhase::High; })
        && rejects_each_change(observed, compare, [](Fill& f) {
               f.provenance = NativePriceProvenance::Confirmed;
           })
        && rejects_each_change(observed, compare, [](Fill& f) {
               f.decision_provenance = NativePriceProvenance::ObservedPrint;
           });
}

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
                                 const NativeDecisionContext& context) override {
        fills.push_back({e.request().label, e.interval_index(), e.handle().incarnation,
                         e.ordinal, e.resolved_price, e.opened_units, e.closed_units,
                         e.cursor.point.path_phase, e.cursor.point.provenance,
                         context.coordinate.provenance, adapter_.cap.budget().charged_slots()});
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
