// Native literal lifecycle fixtures. No Pine, market files, reference trades,
// or generated expected results. RAW quantity/fee policy is deliberately fixed.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pineforge;
using pineforge::source::PendingOrder;
namespace {
bool pine_fixture_attachment = false;
const double missing = std::numeric_limits<double>::quiet_NaN();
#define REQUIRE(x) do { if (!(x)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #x); } while (false)

Bar point(double price, int index) {
    return {price, price, price, price, 1, int64_t(index) * 60000};
}

// Test-only access to private helper calls, including aliased string arguments.
// Explicit member-pointer instantiation avoids changing the production class
// definition or redefining access keywords in standard/library headers.
template<class Tag, typename Tag::Type Member>
struct PrivateMember {
    friend typename Tag::Type access(Tag) { return Member; }
};
struct Cancel {
#ifdef PF_IDENTITY_BASELINE
    using Type = void (BacktestEngine::*)(const std::string&, const std::string&);
#else
    using Type = void (pineforge::source::PineStrategyHost::*)(std::string, std::string);
#endif
    friend Type access(Cancel);
};
#ifdef PF_IDENTITY_BASELINE
template struct PrivateMember<Cancel, &BacktestEngine::cancel_oca_group>;
#else
template struct PrivateMember<Cancel, &pineforge::source::PineStrategyHost::cancel_oca_group>;
#endif
struct Reduce {
#ifdef PF_IDENTITY_BASELINE
    using Type = void (BacktestEngine::*)(const std::string&, const std::string&, double);
#else
    using Type = void (pineforge::source::PineStrategyHost::*)(std::string, std::string, double);
#endif
    friend Type access(Reduce);
};
#ifdef PF_IDENTITY_BASELINE
template struct PrivateMember<Reduce, &BacktestEngine::reduce_oca_group>;
#else
template struct PrivateMember<Reduce, &pineforge::source::PineStrategyHost::reduce_oca_group>;
#endif
struct Refresh {
    using Type = void (BacktestEngine::*)(size_t, size_t);
    friend Type access(Refresh);
};
template struct PrivateMember<Refresh, &BacktestEngine::stream_refresh_action_metadata>;
#ifndef PF_IDENTITY_BASELINE
struct Retire {
    using Type = void (pineforge::source::PineStrategyHost::*)(std::vector<uint64_t>&, int, uint64_t, bool);
    friend Type access(Retire);
};
template struct PrivateMember<Retire, &pineforge::source::PineStrategyHost::compact_filled_pending_orders>;
#endif

class Book final : public pineforge::source::PineStrategyHost {
public:
    using BacktestEngine::open_trade_entry_id;
    Book() {
#if defined(PINEFORGE_HAS_EXPLICIT_PINE_EXECUTION_ADAPTER_V1)
        if (pine_fixture_attachment) attach_pine_execution_adapter();
#else
        if (pine_fixture_attachment) throw std::runtime_error("Pine attachment unavailable");
#endif
        initial_capital_ = 100000;
        pyramiding_ = 10;
        margin_long_ = margin_short_ = 0;
        current_bar_ = point(100, 0);
        bar_index_ = 0;
    }
    void on_source_bar(const Bar&) override {}
    void seed(bool long_side, double qty = 2) {
        strategy_order("seed", long_side, qty);
        current_bar_ = point(100, 1);
        bar_index_ = 1;
        process_pending_orders(current_bar_);
        REQUIRE(position_qty_ == qty);
        REQUIRE(pending_orders_.empty());
        REQUIRE(broker_fill_event_seq_ == 1);
        stream_observe_actions_ = true;
    }
    uint64_t raw(const std::string& id, bool buy, double qty, double limit,
                 double stop, const std::string& group = "", int oca = 0) {
        strategy_order(id, buy, qty, limit, stop, group, oca);
        auto& o = pending_orders_.back();
        REQUIRE(o.id == id && o.incarnation != 0);
        o.comment = "comment for " + id;
        return o.incarnation;
    }
    uint64_t passive(const std::string& id, bool long_side, double qty,
                     const std::string& group = "", int oca = 0) {
        return raw(id, long_side, qty, missing, long_side ? 120 : 80, group, oca);
    }
    uint64_t winner(bool long_side, int oca, double qty = 2,
                    const std::string& id = "A") {
        return raw(id, !long_side, qty, long_side ? 110 : 90, missing, "G", oca);
    }
    uint64_t unrelated(bool long_side) {
        return raw("C", long_side, 7, long_side ? 80 : 120, missing);
    }
    void entry_winner() {
        strategy_entry("A", true, missing, 110, 1, "comment for A", "G", 1);
    }
    void default_market_entry() {
        strategy_entry("A", true, missing, missing, missing, "comment for A", "G", 1);
    }
    void step(bool coof, bool long_side, int index = 2, uint64_t expected_events = 1) {
        // All priced objects have phase 1; source order remains [B,A,C],
        // while only A is touched. COOF gets a monotonic remaining segment.
        current_bar_ = long_side ? Bar{100, 115, 100, 115, 1, int64_t(index)*60000}
                                 : Bar{100, 100, 85, 85, 1, int64_t(index)*60000};
        bar_index_ = index;
        const auto first_action = stream_order_actions_.size();
        const auto first_trade = trades_.size();
        calc_on_order_fills_ = coof;
        if (coof) {
            coof_scheduler_active_ = true;
            coof_evaluating_path_segment_ = true;
            coof_hist_is_segment_ = true;
            int closed_bar = -1;
            uint64_t closed_incarnation = 0;
            bool was_long = false;
            auto result = process_next_pending_order(current_bar_, true,
                closed_bar, closed_incarnation, was_long);
            REQUIRE(result.filled && result.fill_events == expected_events);
            REQUIRE(result.fill_price == (long_side ? 110 : 90));
            coof_scheduler_active_ = false;
            coof_evaluating_path_segment_ = false;
            coof_hist_is_segment_ = false;
        } else {
            process_pending_orders(current_bar_);
        }
        (this->*access(Refresh{}))(first_action, first_trade);
    }
    void at_point(bool coof, double price, int index) {
        current_bar_ = point(price, index);
        bar_index_ = index;
        calc_on_order_fills_ = coof;
        const auto a = stream_order_actions_.size(), t = trades_.size();
        if (coof) {
            int closed = -1;
            uint64_t incarnation = 0;
            bool side = false;
            process_next_pending_order(current_bar_, true, closed, incarnation, side);
        } else process_pending_orders(current_bar_);
        (this->*access(Refresh{}))(a, t);
    }
    std::vector<std::string> ids() const {
        std::vector<std::string> result;
        for (const auto& o : pending_orders_) result.push_back(o.id);
        return result;
    }
    PendingOrder get(const std::string& id) const {
        for (const auto& o : pending_orders_) if (o.id == id) return o;
        throw std::runtime_error("missing pending " + id);
    }
    uint64_t fills() const { return broker_fill_event_seq_; }
    double size() const { return position_qty_; }
    PositionSide side() const { return position_side_; }
    const std::vector<Trade>& closed() const { return trades_; }
    void expect_A(bool long_side) const {
        REQUIRE(position_side_ == PositionSide::FLAT);
        REQUIRE(broker_fill_event_seq_ == 2);
        REQUIRE(trades_.size() == 1);
        REQUIRE(trades_[0].qty == 2);
        REQUIRE(trades_[0].exit_price == (long_side ? 110 : 90));
        REQUIRE(trades_[0].pnl == 20);
        REQUIRE(trades_[0].exit_id == "A");
        REQUIRE(trades_[0].exit_comment == "comment for A");
        REQUIRE(stream_order_actions_.size() == 1);
        const auto& a = stream_order_actions_.front();
        REQUIRE(!a.is_entry && a.is_long == long_side && a.quantity == 2);
        REQUIRE(a.order_id == "A" && a.comment == "comment for A");
    }
    void direct_cancel(size_t index) {
        (this->*access(Cancel{}))(pending_orders_.at(index).oca_name, pending_orders_.at(index).id);
    }
    void direct_reduce(size_t index, double qty) {
        (this->*access(Reduce{}))(pending_orders_.at(index).oca_name, pending_orders_.at(index).id, qty);
    }
    void cancel(const std::string& id) { strategy_cancel(id); }
    void set_capacity(int value) { pyramiding_ = value; }
    void set_cap(int value) { adapter_.cap = value; }
    void observe() { stream_observe_actions_ = true; }
    void fee(CommissionType kind, double value) { commission_type_ = kind; commission_value_ = value; }
#ifndef PF_IDENTITY_BASELINE
    void retire(const std::vector<uint64_t>& ids) {
        auto owned_ids = ids;
        (this->*access(Retire{}))(owned_ids, -1, 0, false);
    }
#endif
};

void cancel_shape(bool coof, bool long_side, int shape) {
    Book b;
    b.seed(long_side);
    if (shape != 1) b.passive("B", long_side, 1, "G", 1);
    if (shape == 2) b.passive("D", long_side, 1, "G", 1);
    const auto a = b.winner(long_side, 1);
    if (shape == 1) b.passive("B", long_side, 1, "G", 1);
    if (shape == 2) b.passive("E", long_side, 1, "G", 1);
    const auto c = b.unrelated(long_side);
    if (shape == 0) REQUIRE(b.ids() == (std::vector<std::string>{"B", "A", "C"}));
    b.step(coof, long_side);
    b.expect_A(long_side);
    REQUIRE(b.ids() == (std::vector<std::string>{"C"}));
    REQUIRE(b.get("C").incarnation == c && c != a);
    b.at_point(coof, long_side ? 80 : 120, 3);
    REQUIRE(b.ids().empty());
    REQUIRE(b.fills() == 3 && b.size() == 7);
    REQUIRE(b.side() == (long_side ? PositionSide::LONG : PositionSide::SHORT));
    REQUIRE(b.closed().size() == 1); // matched A cannot refill
    REQUIRE(b.stream_order_action_at(1).order_id == "C");
    REQUIRE(b.stream_order_action_at(1).entry_incarnation == c);
}

void reduce_shape(bool coof, bool long_side) {
    Book b;
    b.seed(long_side);
    b.passive("B", long_side, 1, "G", 2);
    b.passive("D", long_side, 2, "G", 2);
    b.winner(long_side, 2);
    const auto e = b.passive("E", long_side, 5, "G", 2);
    b.passive("default", long_side, missing, "G", 2);
    const auto c = b.unrelated(long_side);
    b.step(coof, long_side);
    b.expect_A(long_side);
    REQUIRE(b.ids() == (std::vector<std::string>{"E", "C"}));
    REQUIRE(b.get("E").qty == 3 && b.get("E").incarnation == e);
    REQUIRE(b.get("C").qty == 7 && b.get("C").incarnation == c);
}

void no_effect_before_winner(bool coof) {
    Book b;
    b.seed(true);
    b.set_capacity(1);
    b.raw("B", true, 1, missing, missing, "G", 1); // matched, capped add
    b.raw("A", false, 2, missing, missing, "G", 1);
    b.unrelated(true);
    REQUIRE(b.ids() == (std::vector<std::string>{"B", "A", "C"}));
    b.at_point(coof, 100, 2);
    REQUIRE(b.ids() == (std::vector<std::string>{"C"}));
    REQUIRE(b.fills() == 2 && b.closed().size() == 1);
    REQUIRE(b.closed()[0].exit_id == "A" && b.closed()[0].qty == 2);
    REQUIRE(b.closed()[0].exit_comment == "comment for A");
}

void continue_after_erasure(bool coof) {
    Book b;
    b.seed(true);
    b.set_capacity(1);
    b.raw("B", true, 1, missing, missing, "G", 1);
    b.raw("A", false, 2, missing, missing, "G", 1);
    const auto c = b.raw("C", true, 7, missing, missing);
    b.at_point(coof, 100, 2);
    if (coof) {
        REQUIRE(b.ids() == (std::vector<std::string>{"C"}));
        REQUIRE(b.fills() == 2);
        b.at_point(coof, 100, 2);
    }
    REQUIRE(b.ids().empty() && b.fills() == 3 && b.size() == 7);
    REQUIRE(b.closed().size() == 1 && b.closed()[0].exit_id == "A");
    REQUIRE(b.stream_order_actions_len() == 2);
    REQUIRE(b.stream_order_action_at(1).entry_incarnation == c);
}

void no_effect_oca_then_live_candidate(bool coof) {
    Book b;
    b.set_capacity(1);
    b.raw("seed", true, 2, missing, missing);
    b.raw("B", true, 1, missing, missing, "G", 1);
    b.default_market_entry();
    b.raw("D", true, 1, missing, missing, "G", 1);
    const auto c = b.raw("C", true, 3, missing, 110);
    // Seed through the real one-fill entry point. All later orders were
    // genuinely placed from flat; C's existing priced-add exception admits it.
    b.at_point(true, 100, 1);
    REQUIRE(b.fills() == 1 && b.size() == 2);
    REQUIRE(b.ids() == (std::vector<std::string>{"B", "A", "D", "C"}));
    b.observe();
    // B and A are capped no-effects. Established OCA semantics still cancel
    // B/D for default-sized A; no broker event means COOF must continue to C
    // in this SAME candidate batch after the erase, not return/re-discover it.
    b.step(coof, true);
    REQUIRE(b.fills() == 2 && b.size() == 5 && b.ids().empty());
    REQUIRE(b.closed().empty());
    REQUIRE(b.stream_order_actions_len() == 1);
    REQUIRE(b.stream_order_action_at(0).order_id == "C");
    REQUIRE(b.stream_order_action_at(0).entry_incarnation == c);
    REQUIRE(b.stream_order_action_at(0).quantity == 3);
    REQUIRE(b.stream_order_action_at(0).price == 110);
}

void post_oca_cap_metadata(bool coof) {
    Book b;
    b.observe();
    b.set_cap(1);
    b.passive("B", true, 1, "G", 1);
    b.entry_winner();
    b.passive("C", true, 7);
    REQUIRE(b.ids() == (std::vector<std::string>{"B", "A", "C"}));
    b.step(coof, true, 2, 2);
    REQUIRE(b.ids() == (std::vector<std::string>{"C"}));
    REQUIRE(b.side() == PositionSide::FLAT && b.fills() == 2);
    REQUIRE(b.closed().size() == 1);
    REQUIRE(b.closed()[0].entry_id == "A");
    REQUIRE(b.closed()[0].entry_price == 110 && b.closed()[0].exit_price == 115);
    REQUIRE(b.closed()[0].pnl == 5);
    REQUIRE(b.closed()[0].exit_id.empty());
    REQUIRE(b.closed()[0].exit_comment == "Close Position (Max number of filled orders in one day)");
    REQUIRE(b.stream_order_actions_len() == 2);
    REQUIRE(b.stream_order_action_at(0).order_id == "A");
    REQUIRE(b.stream_order_action_at(0).comment == "comment for A");
    REQUIRE(b.stream_order_action_at(1).price == 115);
}

void direct_helpers(bool reduce) {
    Book b;
    b.passive("earlier", true, 2, "G", 1);
    b.passive("A", true, 2, "G", 1);
    b.passive("other group", true, 9, "H", 1);
    b.passive("later", true, 1, "G", 1);
    b.passive("survivor", true, 5, "G", 1);
    if (reduce) {
        const auto before = b.ids();
        b.direct_reduce(1, 0);
        REQUIRE(b.ids() == before && b.get("earlier").qty == 2);
        b.direct_reduce(1, -1);
        REQUIRE(b.ids() == before);
        b.direct_reduce(1, 2);
        REQUIRE(b.ids() == (std::vector<std::string>{"A", "other group", "survivor"}));
        REQUIRE(b.get("survivor").qty == 3);
    } else {
        b.direct_cancel(1);
        REQUIRE(b.ids() == (std::vector<std::string>{"A", "other group"}));
    }
    // No active-pass switch may delay direct helper visibility.
    REQUIRE(b.get("A").qty == 2 && b.get("other group").qty == 9);
    REQUIRE(b.fills() == 0);
}

void replacement_and_retirement(bool coof) {
    Book b;
    b.seed(true);
    b.passive("B", true, 1, "G", 1);
    const auto old_a = b.winner(true, 1);
    const auto priority = b.get("A").created_seq;
    b.unrelated(true);
    const auto replacement = b.winner(true, 1);
    REQUIRE(replacement != old_a && b.get("A").created_seq == priority);
    // Replacement is appended with retained priority; fill sorting must not
    // mistake declaration/incarnation order for execution/retirement order.
    REQUIRE(b.ids() == (std::vector<std::string>{"B", "C", "A"}));
#ifndef PF_IDENTITY_BASELINE
    b.retire({old_a});
    REQUIRE(b.get("A").incarnation == replacement);
#endif
    b.step(coof, true);
    b.expect_A(true);
    REQUIRE(b.ids() == (std::vector<std::string>{"C"}));
    b.cancel("C");
    const auto fresh = b.raw("A", false, 1, missing, missing);
    REQUIRE(fresh != replacement && fresh != old_a);
#ifndef PF_IDENTITY_BASELINE
    b.retire({replacement, old_a, replacement});
    REQUIRE(b.get("A").incarnation == fresh);
#endif
    b.at_point(coof, 100, 3);
    REQUIRE(b.ids().empty() && b.fills() == 3);
    REQUIRE(b.side() == PositionSide::SHORT && b.size() == 1);
    REQUIRE(b.stream_order_action_at(1).entry_incarnation == fresh);
}

void untouched_ordering(bool coof) {
    Book b;
    b.raw("one", true, 1, missing, missing);
    b.raw("two", true, 2, missing, missing);
    b.raw("three", true, 3, missing, missing);
    b.at_point(coof, 100, 1);
    if (coof) {
        REQUIRE(b.fills() == 1 && b.ids() == (std::vector<std::string>{"two", "three"}));
        b.at_point(coof, 100, 1);
        REQUIRE(b.fills() == 2 && b.ids() == (std::vector<std::string>{"three"}));
        b.at_point(coof, 100, 1);
    }
    REQUIRE(b.fills() == 3 && b.size() == 6 && b.ids().empty());
    REQUIRE(b.open_trade_entry_id(0) == "one");
    REQUIRE(b.open_trade_entry_id(1) == "two");
    REQUIRE(b.open_trade_entry_id(2) == "three");
}

void fee_control(bool coof, CommissionType fee, double expected) {
    Book b;
    b.fee(fee, fee == CommissionType::PERCENT ? 1 : 2);
    b.seed(true);
    b.passive("B", true, 1, "G", 1);
    b.winner(true, 1);
    b.unrelated(true);
    b.step(coof, true);
    REQUIRE(b.ids() == (std::vector<std::string>{"C"}));
    REQUIRE(b.closed().size() == 1 && b.closed()[0].commission == expected);
    REQUIRE(b.closed()[0].pnl == 20 - expected);
    REQUIRE(b.closed()[0].exit_id == "A");
}

class CallbackBook final : public pineforge::source::PineStrategyHost {
public:
    int observed_close_callbacks = 0;
    uint64_t replaced = 0, fresh = 0;
    CallbackBook() {
#if defined(PINEFORGE_HAS_EXPLICIT_PINE_EXECUTION_ADAPTER_V1)
        if (pine_fixture_attachment) attach_pine_execution_adapter();
#else
        if (pine_fixture_attachment) throw std::runtime_error("Pine attachment unavailable");
#endif
        initial_capital_ = 100000;
        pyramiding_ = 10;
        margin_long_ = margin_short_ = 0;
        calc_on_order_fills_ = true;
    }
    void on_source_bar(const Bar&) override {
        if (coof_fill_recalc_active_) {
            if (trades_.size() == 1 && observed_close_callbacks == 0) {
                REQUIRE(position_side_ == PositionSide::FLAT);
                REQUIRE(pending_orders_.size() == 1 && pending_orders_[0].id == "C");
                REQUIRE(trades_[0].exit_id == "A");
                ++observed_close_callbacks;
                strategy_order("A", false, 1); // same label, new object in callback
                fresh = pending_orders_.back().incarnation;
                REQUIRE(fresh != 0 && fresh != replaced);
            }
            return;
        }
        if (bar_index_ == 0) strategy_order("seed", true, 2);
        if (bar_index_ == 1) {
            strategy_order("B", true, 1, missing, 120, "G", 1);
            strategy_order("A", false, 2, 110, missing, "G", 1);
            replaced = pending_orders_.back().incarnation;
            strategy_order("C", true, 7, 80);
        }
    }
    void verify() const {
        REQUIRE(observed_close_callbacks == 1 && fresh != replaced);
        REQUIRE(trades_.size() == 1 && broker_fill_event_seq_ == 3);
        REQUIRE(position_side_ == PositionSide::SHORT && position_qty_ == 1);
        REQUIRE(pending_orders_.size() == 1 && pending_orders_[0].id == "C");
        REQUIRE(pyramid_entries_.size() == 1 && pyramid_entries_[0].entry_incarnation == fresh);
    }
};
void actual_coof_callback() {
    CallbackBook b;
    const Bar bars[] = {point(100, 0), point(100, 1),
        {100, 115, 95, 105, 1, 120000}, point(105, 3)};
    b.run(bars, 4);
    b.verify();
}
} // namespace

int main(int argc, char** argv) {
    int failed = 0, passed = 0;
    pine_fixture_attachment = argc > 1 && std::string(argv[1]) == "--pine";
    const std::string filter = argc > 1 && !pine_fixture_attachment ? argv[1] : "";
    auto check = [&](const std::string& name, const std::function<void()>& body) {
        if (!filter.empty() && name.find(filter) == std::string::npos) return;
        try { body(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    for (bool coof : {false, true}) {
        const std::string mode = coof ? "coof" : "normal";
        for (bool long_side : {false, true}) {
            const std::string prefix = mode + (long_side ? "/long" : "/short");
            for (int shape = 0; shape < 3; ++shape)
                check(prefix + "/cancel/" + std::to_string(shape), [&] { cancel_shape(coof, long_side, shape); });
            check(prefix + "/reduce", [&] { reduce_shape(coof, long_side); });
        }
        check(mode + "/no-effect-before-winner", [&] { no_effect_before_winner(coof); });
        check(mode + "/continue-after-erasure", [&] { continue_after_erasure(coof); });
        check(mode + "/no-effect-oca-then-live-candidate", [&] { no_effect_oca_then_live_candidate(coof); });
        check(mode + "/post-oca-cap-metadata", [&] { post_oca_cap_metadata(coof); });
        check(mode + "/replacement-retirement", [&] { replacement_and_retirement(coof); });
        check(mode + "/untouched-order", [&] { untouched_ordering(coof); });
        check(mode + "/fixed-fee-control", [&] { fee_control(coof, CommissionType::CASH_PER_ORDER, 4); });
        check(mode + "/percent-fee-control", [&] { fee_control(coof, CommissionType::PERCENT, 4.2); });
        check(mode + "/per-contract-fee-control", [&] { fee_control(coof, CommissionType::CASH_PER_CONTRACT, 8); });
    }
    check("direct/cancel-borrowed-selectors", [&] { direct_helpers(false); });
    check("direct/reduce-borrowed-selectors", [&] { direct_helpers(true); });
    check("coof/actual-callback-reissue", actual_coof_callback);
    std::cout << passed << " passed; " << failed << " failed\n";
    return failed || !passed ? 1 : 0;
}
