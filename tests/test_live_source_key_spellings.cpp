// Exercise replacement spellings through public runs and trades.
// Keep the dynamic delayed-parent run through the opening after its release:
// ending on the parent's fill bar never admits the parked spelling.
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
constexpr double na = std::numeric_limits<double>::quiet_NaN();
int failures = 0;
template <typename Actual, typename Expected>
void check_equal(const Actual& actual, const Expected& expected,
                 const char* expression, const char* expectation, int line) {
    if (actual == expected) return;
    ++failures;
    std::ostringstream values;
    values.precision(std::numeric_limits<double>::max_digits10);
    values << "expected=[" << expected << "] actual=[" << actual << ']';
    std::fprintf(stderr, "FAIL %s:%d: %s == %s; %s\n", __FILE__, line,
                 expression, expectation, values.str().c_str());
}
#define CHECK_EQ(actual, expected) \
    check_equal((actual), (expected), #actual, #expected, __LINE__)

source::PineStrategyConfig probe_config() {
    source::PineStrategyConfig value;
    value.initial_capital = 10000.0;
    value.default_qty_value = 1.0;
    value.pyramiding = 2;
    value.margin_long = value.margin_short = 0.0;
    value.commission_value = 0.0;
    return value;
}

std::vector<Bar> flat_bars() {
    return {
        {100, 102, 98, 100, 1, 1700000040000LL},
        {100, 102, 98, 100, 1, 1700000100000LL},
        {100, 102, 98, 100, 1, 1700000160000LL},
        {100, 102, 98, 100, 1, 1700000220000LL},
    };
}

class DelayedParent : public source::PineStrategyHost {
public:
    explicit DelayedParent(bool explicit_qty) : explicit_qty_(explicit_qty) {
        configure_pine_strategy(probe_config());
        set_syminfo_mintick(1.0);
    }
    double live_qty() const { return position_qty_; }
    void on_source_bar(const Bar&) override {
        if (bar_index_ != 0) return;
        strategy_entry("E", true, na, na, explicit_qty_ ? 2.0 : 1.0);
        if (!explicit_qty_) strategy_entry("F", true, na, na, 1.0);
        strategy_exit("X", "E", 120.0, 105.0, na, na, na, 100.0, {},
                      explicit_qty_ ? 1.0 : na);
    }
private:
    bool explicit_qty_;
};

void delayed_parent(bool explicit_qty) {
    DelayedParent probe(explicit_qty);
    auto bars = flat_bars();
    probe.run(bars.data(), static_cast<int>(bars.size()));
    if (!probe.last_error().empty()) {
        std::fprintf(stderr, "delayed_parent(%s): %s\n",
                     explicit_qty ? "explicit" : "dynamic", probe.last_error().c_str());
    }
    CHECK_EQ(probe.last_error(), std::string{});
    // Hand oracle, layout estimate from source, not measured. Rules:
    // docs/pages/pine-to-native.md, "Submission is not execution";
    // docs/pages/native-engine.md, "Acceptance is not a fill" and the
    // calculate-on-fill remaining-path rule; docs/pages/fill-model.md,
    // "Current dispatch sequence". strategy_exit's first two numeric
    // arguments are limit_price=120 and stop_price=105, not profit/loss ticks.
    // Bar 0: submit E=2, or E=1 plus F=1; submit X against pending E.
    // Bar 1: market entries fill at open=100. A long sell stop tests <=105,
    // already true at 100 (indeed high=102<105); the limit needs >=120,
    // impossible with high=102. Quantity is one E unit in either variant.
    // Dynamic: on_applied parks the prearmed leg beside sibling F.
    // Bar 2's opening release selects explicit brackets only; the dynamic
    // leg instead reaches release_delayed_orders from flush_pending_entries
    // at bar 2's close. That admission cannot replay bar 2's opening.
    // Bar 3: its first eligible open is 100<=105, so it sells at 100.
    // Explicit: submit_leg stages the new origin's leg until E applies.
    // materialize_pending_bracket_legs admits it on bar 1, stamping that
    // admission bar; it is not the already-live, signal-bar leg parked by
    // on_applied's partial-prearmed scan. The remaining path begins at 100,
    // so the marketable stop sells at 100 on bar 1 itself. Bars 2/3 do nothing.
    // Thus entry bar=1; exit bar=3 dynamic / 1 explicit, never a shared 2.
    // Both outcomes: (100-100)*1=0 PnL, with 2-1=1 unit still open.
    const int expected_exit_bar = explicit_qty ? 1 : 3;
    CHECK_EQ(probe.trade_count(), 1);
    CHECK_EQ(probe.live_qty(), 1.0);
    if (probe.trade_count() != 1) return;
    const auto& trade = probe.get_trade(0);
    CHECK_EQ(trade.entry_id, "E");
    CHECK_EQ(trade.exit_id, "X");
    CHECK_EQ(trade.entry_bar_index, 1);
    CHECK_EQ(trade.exit_bar_index, expected_exit_bar);
    CHECK_EQ(trade.entry_price, 100.0);
    CHECK_EQ(trade.exit_price, 100.0);
    CHECK_EQ(trade.qty, 1.0);
    CHECK_EQ(trade.pnl, 0.0);
}

class DelayedParentCancellation final : public DelayedParent {
public:
    explicit DelayedParentCancellation(bool cancel) : DelayedParent(false), cancel_(cancel) {}
    void on_source_bar(const Bar& bar) override {
        DelayedParent::on_source_bar(bar);
        if (bar_index_ == 3 && cancel_) strategy_exit_cancel_bracket("X", "E");
        if (bar_index_ == 4) strategy_close_all();
    }
private:
    bool cancel_;
};

void delayed_parent_cancellation(bool cancel) {
    DelayedParentCancellation probe(cancel);
    auto bars = flat_bars();
    for (const int i : {2, 3}) {
        bars[i].open = bars[i].close = 110.0;
        bars[i].high = 112.0;
        bars[i].low = 108.0;
    }
    bars.push_back({100, 102, 98, 100, 1, 1700000280000LL});
    bars.push_back({100, 102, 98, 100, 1, 1700000340000LL});
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK_EQ(probe.last_error(), std::string{});
    // Same admission/birth rules as delayed_parent; cancellation semantics:
    // docs/pages/native-engine.md, "Acceptance is not a fill" (Cancelled
    // retires the live incarnation). Hand oracle, layout estimate from source,
    // not measured: E/F open one unit each at 100 on bar 1. X is parked, then
    // admitted at bar 2's close under X<US>E<US>ExitStop. On bars 2/3,
    // low=108>105 and high=112<120, so neither leg can fill. At bar 3's close
    // cancel X/E through its original public spelling, or leave it as a
    // control. Bar 4 gaps to 100: the uncancelled stop sells E at min(100,105)
    // =100; cancellation must prevent that fill. close_all signalled at bar
    // 4 closes F, and E if still open, at bar 5's open=100, label "__close__".
    // Two one-unit trades each earn (100-100)*1=0, then flat. A missed cancel
    // changes E's exit id and bar even though all prices and PnL agree.
    CHECK_EQ(probe.trade_count(), 2);
    CHECK_EQ(probe.live_qty(), 0.0);
    if (probe.trade_count() != 2) return;
    for (int i = 0; i < 2; ++i) {
        const auto& trade = probe.get_trade(i);
        const bool cleanup = cancel || i == 1;
        CHECK_EQ(trade.entry_id, i == 0 ? "E" : "F");
        CHECK_EQ(trade.exit_id, cleanup ? "__close__" : "X");
        CHECK_EQ(trade.entry_bar_index, 1);
        CHECK_EQ(trade.exit_bar_index, cleanup ? 5 : 4);
        CHECK_EQ(trade.entry_price, 100.0);
        CHECK_EQ(trade.exit_price, 100.0);
        CHECK_EQ(trade.qty, 1.0);
        CHECK_EQ(trade.pnl, 0.0);
    }
}

class TokenClose final : public source::PineStrategyHost {
public:
    TokenClose(bool any, bool pooc, std::uint64_t token, bool frozen) : token_(token) {
        auto value = probe_config();
        if (frozen) value.pyramiding = 1;
        value.close_entries_rule_any = any;
        value.process_orders_on_close = pooc;
        configure_pine_strategy(value);
        set_syminfo_mintick(1.0);
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("E", true, na, na, 1.0);
        if (bar_index_ == 1) strategy_close("E", {}, na, na, false, token_);
    }
private:
    std::uint64_t token_;
};

void token_close(bool any, bool pooc, std::uint64_t token, bool frozen = false) {
    TokenClose probe(any, pooc, token, frozen);
    auto bars = flat_bars();
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK_EQ(probe.last_error(), std::string{});
    // Both entry and close prints are 100, independently of FIFO/ANY, POOC,
    // the empty replacement key, or the call site's decimal token.
    CHECK_EQ(probe.trade_count(), 1);
    if (probe.trade_count() != 1) return;
    const auto& trade = probe.get_trade(0);
    CHECK_EQ(trade.entry_id, "E");
    CHECK_EQ(trade.entry_price, 100.0);
    CHECK_EQ(trade.exit_price, 100.0);
    CHECK_EQ(trade.qty, 1.0);
    CHECK_EQ(trade.pnl, 0.0);
}

class SameIdCover final : public source::PineStrategyHost {
public:
    SameIdCover() {
        configure_pine_strategy(probe_config());
        set_syminfo_mintick(1.0);
    }
    double live_qty() const { return position_qty_; }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("L", true, na, na, 1.0);
            strategy_exit("X", "L", 110.0, na, na, na, na, 100.0, {}, 1.0);
        }
        if (bar_index_ == 1) strategy_entry("L", true, na, na, 1.0);
    }
};

void same_id_cover() {
    SameIdCover probe;
    auto bars = flat_bars();
    bars[2].high = 110.0;
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK_EQ(probe.last_error(), std::string{});
    // Hand oracle: the original one-unit bracket closes its older lot at
    // 110. Its same-id MARKET add opened at 100 on that bar; the cover closes
    // it at the bracket's 110 print too. Each PnL is (110-100)*1 = 10.
    CHECK_EQ(probe.trade_count(), 2);
    CHECK_EQ(probe.live_qty(), 0.0);
    for (int i = 0; i < probe.trade_count(); ++i) {
        const auto& trade = probe.get_trade(i);
        CHECK_EQ(trade.entry_id, "L");
        CHECK_EQ(trade.exit_id, "X");
        CHECK_EQ(trade.entry_bar_index, i + 1);
        CHECK_EQ(trade.exit_bar_index, 2);
        CHECK_EQ(trade.entry_price, 100.0);
        CHECK_EQ(trade.exit_price, 110.0);
        CHECK_EQ(trade.qty, 1.0);
        CHECK_EQ(trade.pnl, 10.0);
    }
}
} // namespace

int main() {
    // RED on the review-start diff: the dynamic row releases the origin-free
    // E/F sibling leg as X<US>E<US>family; capture previously rejected it.
    delayed_parent(false);
    // Control: a staged explicit partial bracket retains its origin suffix.
    delayed_parent(true);
    // Keep a released separated-family leg resting, then cancel the same X/E.
    delayed_parent_cancellation(false);
    delayed_parent_cancellation(true);
    same_id_cover();
    for (const bool any : {false, true}) {
        for (const bool pooc : {false, true}) {
            for (const std::uint64_t token :
                 {std::uint64_t{0}, std::uint64_t{7},
                  std::numeric_limits<std::uint64_t>::max()}) {
                token_close(any, pooc, token);
                if (!any && !pooc) token_close(any, pooc, token, true);
            }
        }
    }
    return failures == 0 ? 0 : 1;
}
