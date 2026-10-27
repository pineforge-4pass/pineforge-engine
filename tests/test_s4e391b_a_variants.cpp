#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace {

using pineforge::Bar;
using pineforge::CommissionType;
using pineforge::QtyType;
using pineforge::source::PineStrategyConfig;
using pineforge::source::PineStrategyHost;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kFirstTimestamp = 1736121600000LL;

enum class Variant {
    Stops,
    SharedExitIds,
    Replacement,
    FillAndCancel,
    Reversal,
    Bracket
};

std::vector<Bar> tape() {
    const double opens[] = {100.0, 100.0, 100.0, 100.0, 100.0};
    const double highs[] = {100.0, 103.0, 104.0, 106.0, 106.0};
    const double lows[] = {100.0, 99.0, 99.0, 99.0, 99.0};
    const double closes[] = {100.0, 101.0, 102.0, 105.0, 105.0};
    std::vector<Bar> result;
    for (int i = 0; i < 5; ++i) {
        Bar bar{};
        bar.timestamp = kFirstTimestamp + static_cast<std::int64_t>(i) * 60000;
        bar.open = opens[i];
        bar.high = highs[i];
        bar.low = lows[i];
        bar.close = closes[i];
        bar.volume = 1.0;
        result.push_back(bar);
    }
    return result;
}

class VariantHost final : public PineStrategyHost {
public:
    explicit VariantHost(Variant variant) : variant_(variant) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(1.0);
        PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 8;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 0.0;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        switch (variant_) {
        case Variant::Stops:
            stops(bar);
            break;
        case Variant::SharedExitIds:
            shared_exit_ids(bar);
            break;
        case Variant::Replacement:
            replacement(bar);
            break;
        case Variant::FillAndCancel:
            fill_and_cancel(bar);
            break;
        case Variant::Reversal:
            reversal(bar);
            break;
        case Variant::Bracket:
            bracket(bar);
            break;
        }
    }

    double position() const { return signed_position_size(); }

    std::size_t working_label(const std::string& label) const {
        std::size_t count = 0;
        for (const auto& row : native_working_requests()) {
            if (row.definition && row.definition->request.label == label) ++count;
        }
        return count;
    }

private:
    void stops(int bar) {
        if (bar != 0) return;
        strategy_entry("near-long", true, kNaN, 102.0, 1.0);
        strategy_entry("far-long-a", true, kNaN, 110.0, 1.0);
        strategy_entry("far-long-b", true, kNaN, 120.0, 1.0);
        strategy_entry("far-short", false, kNaN, 90.0, 1.0);
    }

    void shared_exit_ids(int bar) {
        if (bar == 0) {
            strategy_entry("A", true);
            strategy_entry("B", true);
        } else if (bar == 1) {
            strategy_exit("shared", "A", 150.0, kNaN);
            strategy_exit("shared", "B", 151.0, kNaN);
        }
    }

    void replacement(int bar) {
        if (bar == 0) {
            strategy_entry("old-a", true, 1.0, kNaN, 1.0);
            strategy_entry("old-b", true, 1.0, kNaN, 1.0);
            strategy_entry("replace", true, 1.0, kNaN, 1.0);
        } else if (bar == 1) {
            strategy_entry("replace", true, 1.0, kNaN, 1.0);
        }
    }

    void fill_and_cancel(int bar) {
        if (bar == 0) {
            strategy_entry("fill", true, kNaN, 102.0, 1.0);
            // Bar 1 reaches 102, but its high 103 cannot reach this stop.
            // The close callback can cancel it before bar 2's high 104.
            strategy_entry("cancel", true, kNaN, 104.0, 1.0);
            strategy_entry("far", true, 1.0, kNaN, 1.0);
        } else if (bar == 1) {
            strategy_cancel("cancel");
        }
    }

    void reversal(int bar) {
        if (bar == 0) {
            strategy_entry("base", true);
            strategy_entry("resting", true, 1.0, kNaN, 1.0);
        } else if (bar == 1) {
            strategy_entry("reverse", false);
        }
    }

    void bracket(int bar) {
        if (bar == 0) {
            strategy_entry("parent", true);
            strategy_entry("other-a", true, 1.0, kNaN, 1.0);
            strategy_entry("other-b", true, 1.0, kNaN, 1.0);
        } else if (bar == 1 && live_position_size() > 0.0) {
            strategy_exit("bracket", "parent", 150.0, 90.0);
            strategy_exit("global", "", 151.0, 89.0);
        }
    }

    Variant variant_;
};

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "s4e391b A variant failed: %s\n", message);
        std::abort();
    }
}

void run_stops() {
    VariantHost host(Variant::Stops);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    require(host.last_error().empty(), "stop variant reports no error");
    // Hand oracle: the only reached entry level is the long stop at 102 on
    // bar 1. The 110 and 120 long stops and the 90 short stop remain
    // untouched, so the final signed position is +1.
    require(host.position() == 1.0, "only the reached stop entry acts");
}

void run_shared_exit_ids() {
    VariantHost host(Variant::SharedExitIds);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    require(host.last_error().empty(), "from-entry variant reports no error");
    // Hand oracle: A and B each open one unit at the bar-1 open. The two
    // strategy.exit calls share an exit id but name different parents, and
    // their 150/151 limits are above every high in this tape. Two working
    // rows must therefore survive.
    require(host.working_label("shared") == 2,
            "same exit id keeps distinct from-entry legs");
}

void run_replacement() {
    VariantHost host(Variant::Replacement);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    require(host.last_error().empty(), "replacement variant reports no error");
    // Hand oracle: the second "replace" call replaces exactly one prior
    // source id. The two unrelated far limits remain, so the replacement
    // source contributes one and only one working row.
    require(host.working_label("replace") == 1,
            "same-id replacement has one live row");
    require(host.working_label("old-a") == 1 && host.working_label("old-b") == 1,
            "same-id replacement leaves unrelated rows");
}

void run_fill_and_cancel() {
    VariantHost host(Variant::FillAndCancel);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    require(host.last_error().empty(), "fill/cancel variant reports no error");
    // Hand oracle: bar 1's high 103 fills the 102 "fill" stop. Its close
    // callback cancels the untouched 104 stop before bar 2 reaches 104.
    // The 1.0 limit never reaches: exactly one unit opens, "cancel" is absent.
    require(host.position() == 1.0 && host.working_label("cancel") == 0,
            "fill and cancellation change the indexed live classes");
    require(host.working_label("far") == 1, "the untouched far row survives");
}

void run_reversal() {
    VariantHost host(Variant::Reversal);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    require(host.last_error().empty(), "reversal variant reports no error");
    // Hand oracle: "base" opens +1 at bar 1; "reverse" opens -1 at bar 2,
    // closing the one base unit and leaving -1. The far "resting" limit does
    // not reach, so it remains a working row.
    require(host.position() == -1.0, "opposite entry reverses the held unit");
    require(host.working_label("resting") == 1,
            "reversal leaves the unrelated standing order");
}

void run_bracket() {
    VariantHost host(Variant::Bracket);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    require(host.last_error().empty(), "bracket variant reports no error");
    // Hand oracle: only "parent" is filled at bar 1; the other two entries
    // stay at limit 1. Each exit call emits one limit and one stop at
    // 150/151 and 89/90, outside every later bar, so two rows per call
    // remain and no unrelated parent receives the named bracket.
    require(host.working_label("bracket") == 2,
            "named bracket has its two legs on one parent");
    require(host.working_label("global") == 2,
            "strategy.exit without from_entry has two observable legs");
}

} // namespace

int main() {
    run_stops();
    run_shared_exit_ids();
    run_replacement();
    run_fill_and_cancel();
    run_reversal();
    run_bracket();
    return 0;
}
