// R5 lane E19: the adapter's own excursion arithmetic walks the run's
// DECLARED leg order, the same one the kernel's entry-bar mask is derived on.
//
// E15 finding 1. `PineStrategyHost::closed_lot_excursion` asked
// `internal::bar_path_uses_high_first`, which reads the sampler's thread-local
// override. Only `NativePathOrderScope` installs that override, and never
// around a host callback, so this arithmetic answered AUTO (the open-proximity
// rule) under every declared `NativeRunSpec::path_order`. The kernel's
// `BacktestEngine::declare_opened_lot_entry_bar_mask` had already been moved to
// the declared order (E15), so a same-bar entry and exit under a forced order
// combined a forced-frame mask with an AUTO-frame path position.
//
// The witnesses below pin the observable consequence — the closed row's
// MFE/MAE — on bars whose forced order is the OPPOSITE of the open-proximity
// verdict, so the two frames cannot coincide:
//
//   1. forced HIGH_FIRST on a bar AUTO calls low-first: a long stopped out
//      after the high. The high precedes the fill on the run's path, so it is
//      the trade's favorable excursion. AUTO's frame puts the high after the
//      fill and folded nothing (mfe 0.00 before the fix).
//   2. forced LOW_FIRST on a bar AUTO calls high-first: the mirror, a short
//      stopped out after the low.
//   3. the AUTO control: the same shape with no declared order. The seam
//      answers the open-proximity rule, so this row is the same before and
//      after the fix — the byte-identity of the default, at test scope.
//   4. the margin-call prefix sample, reached directly with owner facts (the
//      l11b pattern) after a forced-order run has configured the spec. The
//      prefix walk stops at the liquidation waypoint, so which extremes it
//      has already traversed is the declared order's question too.
//
// Fills are unchanged by the fix in every row: the matcher has always walked
// the declared order (`NativeExecutionConsumer`'s own resolution). Only the
// excursion columns move.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int passed = 0;
int failed = 0;

#define CHECK(x) do { \
    if (x) { ++passed; } \
    else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } \
} while (0)

bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) <= tol; }

Bar mk(std::int64_t t, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, t};
}

constexpr std::int64_t T0 = 1743638400000LL;
constexpr std::int64_t kStep = 900000LL;

// |H-O| = 15 is NOT < |O-L| = 10, so the open-proximity rule walks LOW first.
const Bar kLowFirstBar = mk(T0 + kStep, 100.00, 115.00, 90.00, 100.00);
// |H-O| = 10 IS < |O-L| = 15, so the open-proximity rule walks HIGH first.
const Bar kHighFirstBar = mk(T0 + kStep, 100.00, 110.00, 85.00, 100.00);
const Bar kFlatBefore = mk(T0, 100.00, 100.50, 99.50, 100.00);
const Bar kFlatAfter = mk(T0 + 2 * kStep, 100.00, 100.50, 99.50, 100.00);

source::PineStrategyConfig cfg() {
    source::PineStrategyConfig c;
    c.initial_capital = 1000000.0;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = 0;
    c.process_orders_on_close = false;
    c.commission_value = 0.0;
    c.slippage = 0;
    return c;
}

// One bracketed entry armed on the signal bar, so the entry and its priced
// exit both fill on bar 1. The entry's stop is already marketable at that
// bar's open, so its own fill point (path position 0, both masks clear) is
// the same under either leg order: only the EXIT's position on the path, and
// which extreme precedes it, is the declared order's question.
class SameBarBracket : public source::PineStrategyHost {
public:
    SameBarBracket(int path_order_mode, bool is_long,
                   double entry_stop, double exit_limit, double exit_stop)
        : is_long_(is_long), entry_stop_(entry_stop),
          exit_limit_(exit_limit), exit_stop_(exit_stop) {
        configure_pine_strategy(cfg());
        if (path_order_mode != 0) set_path_order(path_order_mode);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        strategy_entry("E", is_long_, kNaN, entry_stop_);
        strategy_exit("X", "E", exit_limit_, exit_stop_);
    }
private:
    bool is_long_;
    double entry_stop_;
    double exit_limit_;
    double exit_stop_;
};

// The l11b DeclaresOwnership pattern: reach the owner's excursion model
// directly with chosen facts. The run below is what puts the declared order
// into the consumer's spec, which is where the seam reads it from.
class MarginPrefixProbe : public source::PineStrategyHost {
public:
    explicit MarginPrefixProbe(int path_order_mode) {
        configure_pine_strategy(cfg());
        if (path_order_mode != 0) set_path_order(path_order_mode);
    }
    void on_source_bar(const Bar&) override {}
    void arm_margin_prefix(const Bar& bar) {
        current_bar_ = bar;
        excursion_margin_call_ = true;
        excursion_margin_prefix_ = true;
        excursion_margin_fill_only_ = false;
    }
};

void expect_row(const char* tag, const Trade& t, bool is_long,
                double entry_px, double exit_px, double fav, double adv) {
    std::printf("%s %s @%.2f->%.2f mfe=%.6f mae=%.6f (want @%.2f->%.2f mfe=%.6f mae=%.6f)\n",
                tag, is_long ? "L" : "S", t.entry_price, t.exit_price,
                t.max_runup, t.max_drawdown, entry_px, exit_px, fav, adv);
    CHECK(t.is_long == is_long);
    CHECK(near(t.entry_price, entry_px));
    CHECK(near(t.exit_price, exit_px));
    CHECK(near(t.max_runup, fav));
    CHECK(near(t.max_drawdown, adv));
}

void run_bracket(const char* tag, int mode, const Bar& shaped, bool is_long,
                 double entry_stop, double exit_limit, double exit_stop,
                 double entry_px, double exit_px, double fav, double adv) {
    SameBarBracket host(mode, is_long, entry_stop, exit_limit, exit_stop);
    const std::vector<Bar> bars = {kFlatBefore, shaped, kFlatAfter};
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() != 1) return;
    expect_row(tag, host.get_trade(0), is_long, entry_px, exit_px, fav, adv);
}

} // namespace

int main() {
    // 1. Forced HIGH_FIRST over a bar the open-proximity rule calls low-first.
    //    Declared path 100 -> 115 -> 90 -> 100: the long enters at the open
    //    (stop 99.00 already marketable) and its 95.00 stop fills on the
    //    115 -> 90 leg, so the 115.00 high precedes the fill.
    //    favorable = (115.00 - 100.00) * 1 = 15.00, adverse = 5.00.
    run_bracket("forced-high-first-long-stopout", 1, kLowFirstBar, true,
                99.00, kNaN, 95.00, 100.00, 95.00, 15.00, 5.00);

    // 2. The mirror. Forced LOW_FIRST over a bar the rule calls high-first.
    //    Declared path 100 -> 85 -> 110 -> 100: the short enters at the open
    //    and its 105.00 stop fills on the 85 -> 110 leg, so the 85.00 low
    //    precedes the fill. favorable = (100.00 - 85.00) * 1 = 15.00.
    run_bracket("forced-low-first-short-stopout", 2, kHighFirstBar, false,
                101.00, kNaN, 105.00, 100.00, 105.00, 15.00, 5.00);

    // 3. The AUTO control on the same bar as row 1. No order is declared, so
    //    the seam answers the open-proximity rule: path 100 -> 90 -> 115 ->
    //    100, and a 112.00 take-profit on the 90 -> 115 leg has the 90.00 low
    //    behind it. favorable = 12.00 (the fill), adverse = 10.00 (the low).
    run_bracket("auto-long-take-profit", 0, kLowFirstBar, true,
                99.00, 112.00, kNaN, 100.00, 112.00, 12.00, 10.00);

    // 4. The margin-call prefix sample. A long lot opened at 100.00 and
    //    liquidated at the 90.00 low of the same bar: the prefix the slice
    //    inherits is the traversed waypoints, so under the declared
    //    HIGH_FIRST the 115.00 high is already behind the fire price and the
    //    slice's favorable excursion is 15.00; the open-proximity frame would
    //    reach only the open. Adverse is the low either way.
    ClosedLotExcursionFacts facts{};
    facts.entry_bar_index = 1;
    facts.exit_bar_index = 1;
    facts.is_long = true;
    facts.entry_price = 100.00;
    facts.fill_price = 90.00;
    facts.lot_qty = 1.0;
    facts.closed_qty = 1.0;
    {
        MarginPrefixProbe host(1);
        const std::vector<Bar> bars = {kFlatBefore, kFlatAfter};
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        host.arm_margin_prefix(kLowFirstBar);
        const ClosedLotExcursion owned = host.closed_lot_excursion(facts);
        std::printf("forced-high-first-margin-prefix fav=%.6f adv=%.6f (want fav=15.000000 adv=10.000000)\n",
                    owned.favorable, owned.adverse);
        CHECK(near(owned.favorable, 15.00));
        CHECK(near(owned.adverse, 10.00));
    }
    {
        // The AUTO control of row 4: the same bar, no declared order. The
        // prefix stops at the low, which IS the first waypoint under the
        // open-proximity rule, so nothing but the open precedes it.
        MarginPrefixProbe host(0);
        const std::vector<Bar> bars = {kFlatBefore, kFlatAfter};
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        host.arm_margin_prefix(kLowFirstBar);
        const ClosedLotExcursion owned = host.closed_lot_excursion(facts);
        std::printf("auto-margin-prefix fav=%.6f adv=%.6f (want fav=0.000000 adv=10.000000)\n",
                    owned.favorable, owned.adverse);
        CHECK(near(owned.favorable, 0.00));
        CHECK(near(owned.adverse, 10.00));
    }

    // 5. R5 lane F7 (audit M25): the source layer ASKS the kernel for the leg
    //    order instead of keeping copies of the rule. Rows 1-4 pin that the
    //    forced orders reach the adapter; this pins where the answer comes
    //    from: pine_adapter.cpp restates the open-proximity comparison
    //    nowhere (its one `<=` spelling is the flat dual-stop observer's own
    //    tie rule, kept and counted), names no private path-order helper, and
    //    pine_path_resolve.cpp no longer carries the overload that read the
    //    sampler's thread-local override.
#if defined(PINEFORGE_F7_ADAPTER_FILE) && defined(PINEFORGE_F7_PATH_RESOLVE_FILE)
    {
        const auto read = [](const char* path) {
            std::string out;
            if (std::FILE* in = std::fopen(path, "rb")) {
                char buffer[4096];
                std::size_t got = 0;
                while ((got = std::fread(buffer, 1, sizeof buffer, in)) > 0) out.append(buffer, got);
                std::fclose(in);
            }
            std::string collapsed;
            bool space = false;
            for (const char c : out) {
                if (c == ' ' || c == '\n' || c == '\t' || c == '\r') { space = true; continue; }
                if (space && !collapsed.empty()) collapsed.push_back(' ');
                space = false;
                collapsed.push_back(c);
            }
            return collapsed;
        };
        const auto count = [](const std::string& text, const char* needle) {
            int n = 0;
            for (auto at = text.find(needle); at != std::string::npos;
                 at = text.find(needle, at + 1)) {
                ++n;
            }
            return n;
        };
        const std::string adapter = read(PINEFORGE_F7_ADAPTER_FILE);
        const std::string resolve = read(PINEFORGE_F7_PATH_RESOLVE_FILE);
        CHECK(!adapter.empty() && !resolve.empty());
        const int restated = count(adapter, ".open) < std::abs(");
        const int observer_tie = count(adapter, ".open) <= std::abs(");
        const int helper = count(adapter, "source_path_high_first(");
        const int override_reader = count(resolve, "bar_path_uses_high_first(");
        std::printf("source-layer path order: restated=%d observer-tie=%d helper=%d "
                    "override-reader=%d (want 0 1 0 0)\n",
                    restated, observer_tie, helper, override_reader);
        CHECK(restated == 0);
        CHECK(observer_tie == 1);
        CHECK(helper == 0);
        CHECK(override_reader == 0);
    }
#else
    std::printf("PINEFORGE_F7_ADAPTER_FILE / PINEFORGE_F7_PATH_RESOLVE_FILE undefined\n");
    CHECK(false);
#endif

    std::printf("test_e19_excursion_path_order: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
