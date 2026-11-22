// Public synthetic native proof for issue #810 Case A.
//
// The exact row submits a market entry, a 50% tick-profit exit and a full
// price-stop exit in one process_orders_on_close calculation.  The controls
// retain the absolute-price A counterpart and the two Case B reach controls.
// All observations below come from native applied events and closed trades.

#include <pineforge/source/pine_native_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kEntryPrice = 43915.71;
constexpr double kHalfPrice = 44415.71;
constexpr double kRestPrice = 42915.71;

enum class Mode {
    Exact,
    AbsoluteControl,
    BPendingEntryControl,
    BMarketAddControl,
};

const char* mode_name(Mode mode) {
    switch (mode) {
        case Mode::Exact: return "exact";
        case Mode::AbsoluteControl: return "a_absolute_control";
        case Mode::BPendingEntryControl: return "b_pending_entry_control";
        case Mode::BMarketAddControl: return "b_market_add_control";
    }
    return "unknown";
}

struct Applied {
    std::string label;
    int bar = -1;
    double opened = 0.0;
    double closed = 0.0;
    double price = kNaN;
    double position_after = 0.0;
};

Bar bar(int index, double open, double high, double low, double close) {
    return {open, high, low, close, 1000.0,
            1'704'067'200'000LL + static_cast<std::int64_t>(index) * 3'600'000LL};
}

class Probe final : public source::PineNativeHost {
public:
    explicit Probe(Mode mode) : mode_(mode) {
        source::PineStrategyConfig config;
        config.initial_capital = 10'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 0.02;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.pyramiding = mode == Mode::BMarketAddControl ? 2 : 1;
        config.process_orders_on_close =
            mode == Mode::Exact || mode == Mode::AbsoluteControl;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_mintick(0.01);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", 0.01);
    }

    void on_source_bar(const Bar&) override {
        switch (mode_) {
            case Mode::Exact:
            case Mode::AbsoluteControl:
                on_case_a_bar();
                return;
            case Mode::BPendingEntryControl:
                on_case_b_pending_bar();
                return;
            case Mode::BMarketAddControl:
                on_case_b_market_add_bar();
                return;
        }
    }

    void on_native_applied_event(const no::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext& context) override {
        if (event.opened_units == 0.0 && event.closed_units == 0.0) return;
        applied_.push_back({event.request().label,
                            static_cast<int>(context.coordinate.interval_index),
                            event.opened_units, event.closed_units,
                            event.resolved_price,
                            physical_position().signed_units});
    }

    double trade_size(int index) const { return closed_trade_size(index); }
    std::string trade_entry_id(int index) const { return closed_trade_entry_id(index); }
    std::string trade_exit_id(int index) const { return closed_trade_exit_id(index); }
    double trade_entry_price(int index) const {
        return closed_trade_entry_price(index);
    }
    double trade_exit_price(int index) const { return closed_trade_exit_price(index); }

    const std::vector<Applied>& applied() const { return applied_; }
    bool b_relative_shadow_seen() const { return b_relative_shadow_seen_; }
    std::uint64_t b_relative_shadow_rank() const { return b_relative_shadow_rank_; }

private:
    void exit(const char* id, const char* from, double limit, double stop,
              double qty_percent, double qty, double profit_ticks,
              const char* comment) {
        strategy_exit(id, from, limit, stop, kNaN, kNaN, kNaN, qty_percent,
                      comment, qty, "", profit_ticks, kNaN);
    }

    void on_case_a_bar() {
        if (pine_bar_index() != 0) return;
        strategy_entry("Long", true, kNaN, kNaN, 0.02, "case-a-entry");
        if (mode_ == Mode::Exact) {
            exit("Half", "Long", kNaN, kNaN, 50.0, kNaN, 50000.0,
                 "case-a-tick-half");
        } else {
            exit("Half", "Long", kHalfPrice, kNaN, 50.0, kNaN, kNaN,
                 "case-a-price-half");
        }
        exit("Rest", "Long", kNaN, kRestPrice, 100.0, kNaN, kNaN,
             "case-a-price-rest");
    }

    void on_case_b_pending_bar() {
        if (pine_bar_index() == 0) {
            strategy_entry("B", true, kNaN, kNaN, 0.02, "case-b-entry");
            exit("BHalf", "B", kNaN, kNaN, 50.0, kNaN, 100.0,
                 "case-b-relative-pending-half");
            for (const auto& row : adapter_.fixture_pending_snapshots()) {
                if (row.snapshot.source_id == "BHalf"
                    && row.snapshot.from_entry == "B" && row.staged) {
                    b_relative_shadow_seen_ = true;
                    b_relative_shadow_rank_ = row.snapshot.chain_origin_sequence;
                }
            }
            return;
        }
    }

    void on_case_b_market_add_bar() {
        if (pine_bar_index() == 0) {
            strategy_entry("B", true, kNaN, kNaN, 0.02, "case-b-base");
            return;
        }
        if (pine_bar_index() == 1 && physical_position().signed_units > 0.0) {
            exit("BHalf", "B", 1000.0, kNaN, 100.0, 0.01, kNaN,
                 "case-b-control-half");
            exit("BRest", "B", 2000.0, kNaN, 100.0, kNaN, kNaN,
                 "case-b-control-rest");
            strategy_entry("Add", true, kNaN, kNaN, 0.02, "case-b-market-add");
            return;
        }
        if (pine_bar_index() == 4 && physical_position().signed_units > 0.0)
            strategy_close("Add", "case-b-add-close");
        if (pine_bar_index() == 5 && physical_position().signed_units > 0.0)
            strategy_close("B", "case-b-base-close");
    }

    Mode mode_;
    std::vector<Applied> applied_;
    bool b_relative_shadow_seen_ = false;
    std::uint64_t b_relative_shadow_rank_ = 0;
};

void print_applied(const Probe& probe) {
    for (const auto& row : probe.applied()) {
        std::printf("APPLIED label=%s bar=%d opened=%.17g closed=%.17g price=%.17g "
                    "position_after=%.17g\n",
                    row.label.c_str(), row.bar, row.opened, row.closed, row.price,
                    row.position_after);
    }
}

bool near(double left, double right) {
    return std::isfinite(left) && std::fabs(left - right) <= 1e-9;
}

bool has_applied(const Probe& probe, const char* label, double closed,
                 double opened, int* bar_out = nullptr) {
    for (const auto& row : probe.applied()) {
        if (row.label == label && near(row.closed, closed) && near(row.opened, opened)) {
            if (bar_out) *bar_out = row.bar;
            return true;
        }
    }
    return false;
}

const Applied* find_applied(const Probe& probe, const char* label, double closed,
                            double opened) {
    for (const auto& row : probe.applied()) {
        if (row.label == label && near(row.closed, closed) && near(row.opened, opened))
            return &row;
    }
    return nullptr;
}

bool check(bool condition, const char* mode, const char* name,
           double actual = kNaN, double expected = kNaN) {
    std::printf("CHECK mode=%s name=%s status=%s actual=%.17g expected=%.17g\n",
                mode, name, condition ? "PASS" : "FAIL", actual, expected);
    return condition;
}

bool check_case_a(Probe& probe, Mode mode) {
    const char* name = mode_name(mode);
    bool ok = true;
    int half_bar = -1;
    int rest_bar = -1;
    const auto* entry = find_applied(probe, "Long", 0.0, 0.02);
    const auto* half = find_applied(probe, "Half", 0.01, 0.0);
    const auto* rest = find_applied(probe, "Rest", 0.01, 0.0);
    ok = check(entry != nullptr, name, "pending_entry_reached") && ok;
    ok = check(half != nullptr, name, "partial_exit_reached") && ok;
    ok = check(rest != nullptr, name, "remainder_exit_reached") && ok;
    if (entry) {
        ok = check(near(entry->price, kEntryPrice), name,
                   "entry_fill_price", entry->price, kEntryPrice) && ok;
        ok = check(near(entry->position_after, 0.02), name,
                   "position_after_entry", entry->position_after, 0.02) && ok;
    }
    if (half) {
        half_bar = half->bar;
        ok = check(near(half->price, kHalfPrice), name,
                   "partial_exit_price", half->price, kHalfPrice) && ok;
        ok = check(near(half->position_after, 0.01), name,
                   "position_after_partial_exit", half->position_after, 0.01) && ok;
    }
    if (rest) {
        rest_bar = rest->bar;
        ok = check(near(rest->price, kRestPrice), name,
                   "remainder_exit_price", rest->price, kRestPrice) && ok;
        ok = check(near(rest->position_after, 0.0), name,
                   "position_after_remainder_exit", rest->position_after, 0.0) && ok;
    }
    ok = check(half_bar == 1, name, "partial_exit_bar", half_bar, 1) && ok;
    ok = check(rest_bar == 2, name, "remainder_exit_bar", rest_bar, 2) && ok;
    ok = check(probe.trade_count() == 2, name, "two_closed_trade_rows",
               probe.trade_count(), 2) && ok;
    if (probe.trade_count() == 2) {
        ok = check(near(probe.trade_size(0), 0.01), name,
                   "first_trade_partial_quantity", probe.trade_size(0), 0.01) && ok;
        ok = check(near(probe.trade_size(1), 0.01), name,
                   "second_trade_remainder_quantity", probe.trade_size(1), 0.01) && ok;
        ok = check(probe.trade_exit_id(0) == "Half", name,
                   "first_trade_is_half") && ok;
        ok = check(probe.trade_exit_id(1) == "Rest", name,
                   "second_trade_is_rest") && ok;
    }
    return ok;
}

bool check_b_pending(Probe& probe) {
    const char* name = mode_name(Mode::BPendingEntryControl);
    bool ok = true;
    int entry_bar = -1;
    int half_bar = -1;
    ok = check(probe.b_relative_shadow_seen(), name,
               "relative_pending_shadow_seen") && ok;
    ok = check(probe.b_relative_shadow_rank() == 0, name,
               "relative_pending_shadow_rank",
               static_cast<double>(probe.b_relative_shadow_rank()), 0) && ok;
    const bool entry_reached = has_applied(probe, "B", 0.0, 0.02, &entry_bar);
    ok = check(entry_reached, name,
               "pending_entry_reached", entry_bar, 1) && ok;
    ok = check(entry_bar == 1, name, "pending_entry_bar",
               static_cast<double>(entry_bar), 1) && ok;
    const bool half_reached = has_applied(probe, "BHalf", 0.01, 0.0, &half_bar);
    ok = check(half_reached, name,
               "relative_pending_partial_reached") && ok;
    // First eligible bar, reasoned from the fixture rather than from the fill:
    // B is submitted on bar 0 without process_orders_on_close, so it fills at
    // bar 1's open (100) and the 100-tick profit at mintick 0.01 is a sell
    // limit at 101.  Bar 1's high is 100; bar 2's high is the first to touch
    // 101, and a limit order fills on touch by default.
    ok = check(half_bar == 2, name, "relative_pending_partial_bar",
               static_cast<double>(half_bar), 2) && ok;
    ok = check(probe.trade_count() == 1, name, "one_closed_trade_row",
               probe.trade_count(), 1) && ok;
    if (probe.trade_count() == 1) {
        ok = check(near(probe.trade_size(0), 0.01), name,
                   "relative_pending_trade_quantity",
                   probe.trade_size(0), 0.01) && ok;
        ok = check(probe.trade_exit_id(0) == "BHalf", name,
                   "relative_pending_trade_exit") && ok;
    }
    ok = check(near(probe.physical_position().signed_units, 0.01), name,
               "relative_pending_position_after_fill",
               probe.physical_position().signed_units, 0.01) && ok;
    return ok;
}

bool check_b_market_add(Probe& probe) {
    const char* name = mode_name(Mode::BMarketAddControl);
    bool ok = true;
    int add_bar = -1;
    ok = check(has_applied(probe, "B", 0.0, 0.02), name,
               "pending_entry_reached") && ok;
    const bool add_reached = has_applied(probe, "Add", 0.0, 0.02, &add_bar);
    ok = check(add_reached, name,
               "separate_market_add_reached", add_bar, 2) && ok;
    ok = check(add_bar == 2, name, "separate_market_add_bar",
               static_cast<double>(add_bar), 2) && ok;
    ok = check(has_applied(probe, "__close__Add", 0.02, 0.0), name,
               "market_add_close_reached") && ok;
    ok = check(has_applied(probe, "__close__B", 0.02, 0.0), name,
               "base_close_reached") && ok;
    return ok;
}

std::vector<Bar> bars_for(Mode mode) {
    if (mode == Mode::Exact || mode == Mode::AbsoluteControl) {
        return {
            bar(0, kEntryPrice, kEntryPrice, kEntryPrice, kEntryPrice),
            bar(1, kEntryPrice, kHalfPrice, kEntryPrice, kHalfPrice),
            bar(2, kHalfPrice, kHalfPrice, kRestPrice, 43000.0),
        };
    }
    if (mode == Mode::BPendingEntryControl) {
        return {
            bar(0, 100.0, 100.0, 100.0, 100.0),
            bar(1, 100.0, 100.0, 100.0, 100.0),
            bar(2, 100.0, 101.0, 99.0, 100.5),
            bar(3, 101.0, 102.0, 100.0, 101.5),
            bar(4, 101.5, 101.5, 101.5, 101.5),
        };
    }
    return {
        bar(0, 100.0, 100.0, 100.0, 100.0),
        bar(1, 100.0, 100.0, 100.0, 100.0),
        bar(2, 100.0, 100.0, 100.0, 100.0),
        bar(3, 100.0, 100.0, 100.0, 100.0),
        bar(4, 100.0, 100.0, 100.0, 100.0),
        bar(5, 100.0, 100.0, 100.0, 100.0),
        bar(6, 100.0, 100.0, 100.0, 100.0),
    };
}

bool run_mode(Mode mode) {
    const char* name = mode_name(mode);
    Probe probe(mode);
    const auto bars = bars_for(mode);
    probe.fixture_retain_all_events();
    probe.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    std::printf("EXECUTION mode=%s status=%s last_error=%s\n", name,
                probe.last_error().empty() ? "completed" : "failed",
                probe.last_error().c_str());
    print_applied(probe);
    for (int index = 0; index < probe.trade_count(); ++index) {
        std::printf("TRADE mode=%s index=%d entry=%s exit=%s qty=%.17g "
                    "entry_price=%.17g exit_price=%.17g\n",
                    name, index, probe.trade_entry_id(index).c_str(),
                    probe.trade_exit_id(index).c_str(), probe.trade_size(index),
                    probe.trade_entry_price(index), probe.trade_exit_price(index));
    }
    const auto position = probe.physical_position();
    std::printf("POSITION mode=%s signed_units=%.17g lot_count=%zu trades=%d\n",
                name, position.signed_units, position.lot_count, probe.trade_count());
    std::printf("REPORT mode=%s broker_state_hash=%llu\n", name,
                static_cast<unsigned long long>(probe.broker_state_hash()));
    if (!probe.last_error().empty()) return false;
    if (mode == Mode::Exact || mode == Mode::AbsoluteControl)
        return check_case_a(probe, mode);
    if (mode == Mode::BPendingEntryControl) return check_b_pending(probe);
    return check_b_market_add(probe);
}

bool parse_mode(const std::string& value, Mode& out) {
    if (value == "exact") out = Mode::Exact;
    else if (value == "a_absolute_control") out = Mode::AbsoluteControl;
    else if (value == "b_pending_entry_control") out = Mode::BPendingEntryControl;
    else if (value == "b_market_add_control") out = Mode::BMarketAddControl;
    else return false;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) return 2;
    if (argc == 2) {
        Mode mode;
        if (!parse_mode(argv[1], mode)) return 2;
        return run_mode(mode) ? 0 : 1;
    }
    bool ok = true;
    for (const Mode mode : {Mode::Exact, Mode::AbsoluteControl,
                            Mode::BPendingEntryControl, Mode::BMarketAddControl}) {
        ok = run_mode(mode) && ok;
    }
    return ok ? 0 : 1;
}
