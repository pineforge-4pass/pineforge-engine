/*
 * test_adapter_fill_qty_probe.cpp -- R5 lane B-ADAPTER, item 2 (AUDIT3-opus2 H4).
 *
 * strategy_pending_order_fill_qty (PendingIntentView::probe_fill_qty) promises
 * "the contracts the entry kernel would OPEN if that order filled at
 * fill_price, sized by the engine's own rules so a live runtime never
 * re-implements them" (include/pineforge/pineforge.h). Its partition 3
 * (AT_FILL) is a default cash / percent-of-equity entry the adapter sizes at
 * its fill: resolve_terms books default_sizing_units() there -- the kernel's
 * native_sized_units() (CashValue, the percentage fee reserve) under the
 * source lot floor, on the source's own sizing equity. Until this lane the
 * probe divided locally: no fee reserve, marked equity instead of the sizing
 * equity, the plain grid floor for a percentage. AUDIT3-opus2's probe printed
 * 500 where the run books 499.5 (50 % of 100 000 at 100 with a 0.1 % fee).
 *
 * The differential: every combination of fee 0 / 0.1 %, quantity grid none /
 * 0.001 and default cash / percent, on a default-sized LIMIT entry (and a cash
 * STOP entry carrying slippage) probed at the price it then fills at. The
 * probe must answer the booked quantity bit for bit.
 *
 * The local quotients H4 left beside the kernel's conversion are ruled in
 * docs/design/native-feature-parity.md (SZ10a): the typed quantity's own
 * conversion now goes through the kernel (test_adapter_typed_entry_admission),
 * and the four "affordable price" quotients -- sig10( sig10(E) / notional per
 * price ) at the placement money band, the paired-reversal whole drop, the
 * terms-time default money candidate and the carried money scope -- stay the
 * source's: they are not a units conversion but TradingView's ten-significant-
 * digit whole-drop PRICE (445 famr3 tapes, test_tv_money_band_l4b; 202 famr
 * tapes, test_tv_money_precision_l4b). Section 2 measures why no kernel query
 * can stand in for them: on a seven-digit decimal tie the kernel's conversion
 * and plain binary64 both refuse the lot the source's rule admits.
 *
 * Sections 3 to 5 compare the probe's close_only with the shape the fill of
 * the same row books in the same run: (3) a same-bar opposite market
 * predecessor, (4) the short-seed final Short, and (5) the fill's shapes the
 * probe models, the shape_* tests: a close before a pyramiding stop, a frozen
 * reversal, a host-sized strategy.order, a row probed in the callback that
 * queued it, a priced replacement of a market entry, a sequential pair member
 * and an entry against the allowed risk direction.
 *
 * Section 6 replays issue 910's shapes 1 and 3 as the issue words them -- a
 * stop entry placed behind a same-bar close of a SHORT, and a strategy.order
 * with its OWN quantity placed against a short, alone and in an OCA cancel
 * group -- and compares the report's close_only and quantity with the units
 * the fill of the same row closes and opens and the position it leaves.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/native_order.hpp>
#include <pineforge/native_run_spec.hpp>
#include <pineforge/source/pine_policy_support.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "oracle_fixture_config_shim.hpp"

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
        }                                                                      \
    } while (0)

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

Bar mk_bar(int index, double o, double h, double l, double c, std::int64_t interval_ms = 300'000) {
    Bar b;
    b.open = o;
    b.high = h;
    b.low = l;
    b.close = c;
    b.volume = 1.0;
    b.timestamp = interval_ms * index;
    return b;
}

struct Row {
    const char* name;
    QtyType type;
    double value;
    double fee_percent;
    double qty_step;
    int slippage;
    bool stop;          // a pure STOP entry at `level`, else a LIMIT entry
    double level;
};

class Probe : public pineforge::source::PineStrategyHost {
public:
    explicit Probe(const Row& row) : row_(row) {
        initial_capital_ = 100000.0;
        syminfo_mintick_ = 0.01;
        qty_step_ = row.qty_step;
        default_qty_type_ = row.type;
        default_qty_value_ = row.value;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = row.fee_percent;
        slippage_ = row.slippage;
        pyramiding_ = 1;
        margin_call_enabled_ = false;
    }
    double probed = kNaN;
    int rc = -9;
    int close_only = -1;
    int partition = -9;
    void on_source_bar(const Bar&) override {
        if (bar_index_ != 0) return;
        if (row_.stop) strategy_entry("L", true, kNaN, row_.level);
        else strategy_entry("L", true, row_.level);
        const int index = pending_order_count() - 1;
        rc = probe_fill_qty(index, row_.level, &probed, &close_only, &partition);
    }
    double held() const { return physical_position().signed_units; }
    const TradeLedger& rows() const { return trades_; }
private:
    Row row_;
};

void a_default_entry_probe_answers_its_booked_quantity() {
    std::vector<Row> rows;
    for (const bool percent : {false, true}) {
        for (const double fee : {0.0, 0.1}) {
            for (const double step : {0.0, 0.001}) {
                Row row{};
                row.name = percent ? "percent limit" : "cash limit";
                row.type = percent ? QtyType::PERCENT_OF_EQUITY : QtyType::CASH;
                row.value = percent ? 50.0 : 50000.0;
                row.fee_percent = fee;
                row.qty_step = step;
                row.slippage = 0;
                row.stop = false;
                row.level = 100.0;
                rows.push_back(row);
            }
        }
    }
    // A cash STOP entry is sized at its slipped fill as well (the percentage
    // stop is the frozen default-stop partition, not this one).
    for (const double fee : {0.0, 0.1}) {
        Row row{};
        row.name = "cash stop slip 2";
        row.type = QtyType::CASH;
        row.value = 50000.0;
        row.fee_percent = fee;
        row.qty_step = 0.001;
        row.slippage = 2;
        row.stop = true;
        row.level = 101.0;
        rows.push_back(row);
    }
    // bar 0 places the entry above/below the market; bar 1 reaches the level.
    const std::vector<Bar> bars = {
        mk_bar(0, 100.5, 100.6, 100.4, 100.5),
        mk_bar(1, 100.5, 101.5, 99.5, 100.2),
        mk_bar(2, 100.2, 100.3, 100.1, 100.2),
    };
    for (const Row& row : rows) {
        Probe probe(row);
        probe.run(bars.data(), static_cast<int>(bars.size()));
        const double booked = probe.held();
        std::printf("  [%-16s fee %.1f%% grid %-5g] probe rc=%d partition=%d qty=%.17g"
                    " | booked %.17g%s\n",
                    row.name, row.fee_percent, row.qty_step, probe.rc, probe.partition,
                    probe.probed, booked, probe.last_error().empty() ? "" : " (error)");
        CHECK(probe.last_error().empty());
        CHECK(probe.rc == 0);
        CHECK(probe.partition == 3);
        CHECK(probe.close_only == 0);
        CHECK(booked > 0.0);
        CHECK(probe.rows().empty());
        CHECK(bits(probe.probed) == bits(booked));
    }
}

class CoreHost : public pineforge::NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

// The whole-drop band's arithmetic against the kernel's conversion on one
// decimal tie of the famr3 F7 shape (seven-digit money): a 10 000-lot order at
// tick(close) = 0.9999 with a sizing equity of 9 998.99999996. The source's
// rule rounds the money to ten significant digits first: sig10(E) = 9 999,
// P = sig10(9 999 / 10 000) = 0.9999, which is not below the price -- the
// order stands. The kernel's conversion of the same money at the same price
// buys 9 999.99999996 lots, fewer than 10 000, and so does plain binary64
// (E / Q = 0.999899999996 < 0.9999): neither can answer the source's decision.
void the_money_band_price_is_not_the_kernel_conversion() {
    NativeRunSpec spec;
    spec.identity.session_key = "b-adapter-sz10";
    spec.identity.run_number = 1;
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "forex";
    spec.currency = "USD";
    spec.basecurrency = "EUR";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.0001;
    CoreHost core;
    const auto configured = core.configure_native(spec);
    CHECK(configured.status == NativeSetupStatus::Applied);
    if (configured.status != NativeSetupStatus::Applied) return;
    const double equity = 9998.99999996;
    const double lots = 10000.0;
    const double price = 9999.0 * 0.0001;
    const double band_price =
        source::tv_money_round(source::tv_money_round(equity) / lots);
    native_order::Sized sized;
    sized.basis = native_order::CashValue{equity};
    sized.grid_policy = native_order::ExecutionGridPolicy::ExplicitUnits;
    const auto kernel_lots = core.native_sized_units(sized, price, equity, 1.0);
    CHECK(kernel_lots.has_value());
    if (!kernel_lots) return;
    std::printf("  [sz10 tie] band price %.17g vs tick(close) %.17g -> %s | kernel buys %.17g"
                " of %.0f lots | binary64 E/Q %.17g\n",
                band_price, price, band_price < price ? "drop" : "admit", *kernel_lots, lots,
                equity / lots);
    CHECK(!(band_price < price));
    CHECK(*kernel_lots < lots);
    CHECK(equity / lots < price);
}

bool near(double a, double b) { return std::fabs(a - b) <= 1e-9; }

// mirror_order_type exports the command family: even a market strategy.entry is an ENTRY.
constexpr std::int32_t kEntryRow = 1, kExitRow = 2;

struct PendingProbe {
    int index = -1, rc = -1;
    double qty = kNaN;
    int close_only = -1, partition = -1, predecessor = -1;
    std::uint64_t incarnation = 0, replaced_market = 0;

    void print(const char* shape, const char* order) const {
        std::fprintf(stderr, "%s %s: index=%d rc=%d qty=%.17g close_only=%d partition=%d "
                     "predecessor=%d incarnation=%llu replaced_market=%llu\n", shape, order, index, rc, qty,
                     close_only, partition, predecessor, static_cast<unsigned long long>(incarnation),
                     static_cast<unsigned long long>(replaced_market));
    }
};

PendingProbe probe_order(const source::PineStrategyHost& host, const char* id,
                         std::int32_t type, double price) {
    PendingProbe probe;
    pf_pending_order_v1_t row{};
    for (int i = 0; i < host.pending_order_count(); ++i) {
        if (host.observe_pending_copy_v1(i, &row) != 0 || row.type != type
            || std::strcmp(row.id, id) != 0) continue;
        probe.index = i;
        probe.incarnation = row.incarnation;
        probe.predecessor = row.reverses_same_bar_market_from_flat;
        probe.replaced_market = row.replaced_default_market_incarnation;
        probe.rc = host.probe_fill_qty(i, price, &probe.qty, &probe.close_only, &probe.partition);
        break;
    }
    return probe;
}

// These replays close each named lot once on bar 2.
double closed_lot_qty(const source::PineStrategyHost& host, const char* entry, const char* exit,
                      bool is_long, int entry_bar, double fill_price) {
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade t = host.get_trade(i);
        if (t.entry_id == entry && t.exit_id == exit && t.is_long == is_long
            && t.entry_bar_index == entry_bar && t.exit_bar_index == 2) {
            std::fprintf(stderr, "bar 2 fill %s->%s: qty=%.17g price=%.17g\n",
                         entry, exit, t.qty, t.exit_price);
            CHECK(near(t.exit_price, fill_price));
            return t.qty;
        }
    }
    return kNaN;
}

class SameBarPredecessor final : public pineforge::source::PineStrategyHost {
public:
    explicit SameBarPredecessor(bool market_first) : market_first_(market_first) {
        set_syminfo_mintick(0.01);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        if (market_first_) strategy_entry("L", true, kNaN, kNaN, 1.0);
        strategy_entry("S", false, kNaN, 95.0);
        if (!market_first_) strategy_entry("L", true, kNaN, kNaN, 1.0);
    }
    std::uint64_t filled_s = 0;  // the S row's incarnation at its fill
protected:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        if (event.request().label == "S" && filled_s == 0) filled_s = event.handle().incarnation;
    }
private:
    bool market_first_;
};

void test_same_bar_predecessor(bool market_first) {
    // Bars start at index 10: a same-bar placement time is never the
    // field's default of 0 (placement_script_open_ms).
    const Bar bars[] = {
        mk_bar(10, 100, 100, 100, 100, 60'000),
        mk_bar(11, 100, 100, 100, 100, 60'000),
        mk_bar(12, 100, 101, 94, 96, 60'000),
    };
    SameBarPredecessor p(market_first), f(market_first);
    p.run(bars, 2);
    const auto stop = probe_order(p, "S", kEntryRow, 95.0);
    f.run(bars, 3);
    const double closed = closed_lot_qty(f, "L", "S", true, 1, 95.0);
    const double before = p.live_position_size(), after = f.live_position_size();
    const char* shape = market_first ? "market then stop" : "stop then market";
    stop.print(shape, "S at 95");
    std::fprintf(stderr, "%s fill: L->S closed=%.17g position_before=%.17g position_after=%.17g "
                 "probe_error='%s' fill_error='%s'\n", shape, closed, before, after,
                 p.last_error().c_str(), f.last_error().c_str());
    CHECK(p.last_error().empty() && f.last_error().empty());
    CHECK(stop.index >= 0 && stop.rc == 0);
    // The probed row is the row that fills (same script, same incarnation).
    CHECK(f.filled_s != 0 && stop.incarnation == f.filled_s);
    CHECK(stop.predecessor == (market_first ? 1 : 0));
    CHECK(near(before, 1.0) && closed > 0.0 && near(closed, before));
    CHECK(after <= 0.0);
    CHECK(stop.close_only == (after == 0.0 ? 1 : 0));
    CHECK(near(stop.qty, after == 0.0 ? closed : -after));
}

// At placement the seed is still short. Only after __close__Short fills at the
// next open can the probe see the two long lots the final Short will face.
class ShortSeedReplay final : public pineforge::source::PineStrategyHost {
public:
    explicit ShortSeedReplay(QtyType sizing) {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(sizing);
        config.default_qty_value = sizing == QtyType::FIXED ? 1.0 : 10.0;
        config.pyramiding = 1;
        config.commission_value = 0.0;
        config.slippage = 0;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("Short", false);
        if (pine_bar_index() == 1) {
            strategy_entry("Long", true);
            strategy_entry("Short", false);
            strategy_close("Long");
            strategy_close("Short");
        }
    }
    PendingProbe at_final_short;
    double before_final = kNaN;
    double average_before_final = kNaN;
    std::uint64_t filled_short = 0;

protected:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        if (event.interval_index() != 2) return;
        if (event.request().label == "__close__Short") {
            before_final = live_position_size();
            average_before_final = position_avg_price();
            at_final_short = probe_order(*this, "Short", kEntryRow, event.resolved_price);
        } else if (event.request().label == "Short") {
            filled_short = event.handle().incarnation;
        }
    }
};

void test_short_seed_replay(QtyType sizing) {
    const bool fixed = sizing == QtyType::FIXED;
    const Bar bars[] = {
        mk_bar(1, 100, 100, 100, 100, 600'000),
        fixed ? mk_bar(2, 100, 100, 100, 100, 600'000) : mk_bar(2, 100, 100.5, 89.5, 90, 600'000),
        fixed ? mk_bar(3, 100, 100, 100, 100, 600'000) : mk_bar(3, 90, 90.5, 89.5, 90, 600'000),
    };
    const double price = bars[2].open;
    ShortSeedReplay p(sizing), f(sizing);
    p.run(bars, 2);
    const auto long_entry = probe_order(p, "Long", kEntryRow, price);
    const auto queued_short = probe_order(p, "Short", kEntryRow, price);
    const auto materialize = probe_order(p, "__close__Short", kExitRow, price);
    f.run(bars, 3);
    const auto& final_short = f.at_final_short;
    const double seed = closed_lot_qty(f, "Short", "Long", false, 1, price);
    const double first = closed_lot_qty(f, "Long", "Short", true, 2, price);
    const double second = closed_lot_qty(f, "__close__Short", "Short", true, 2, price);
    const double after = f.live_position_size();
    const char* shape = fixed ? "fixed short seed" : "percent short seed";
    long_entry.print(shape, "Long queued");
    queued_short.print(shape, "Short queued");
    materialize.print(shape, "__close__Short queued");
    final_short.print(shape, "Short after __close__Short");
    std::fprintf(stderr, "%s fill: expected_open=%.17g seed=%.17g Long=%.17g __close__Short=%.17g "
                 "position_before_Short=%.17g avg_price=%.17g position_after_Short=%.17g "
                 "Short_incarnation=%llu probe_error='%s' fill_error='%s'\n", shape, price,
                 seed, first, second, f.before_final, f.average_before_final, after,
                 static_cast<unsigned long long>(f.filled_short), p.last_error().c_str(), f.last_error().c_str());

    // Scenario setup (the collision's own shape is the target of
    // test_short_seed_collision_percent and test_short_seed_close_collision):
    // these lines only confirm the replay reached the shape under probe.
    CHECK(p.last_error().empty() && f.last_error().empty());
    CHECK(p.pine_bar_index() == 1);
    CHECK(long_entry.index >= 0 && long_entry.rc == 0);
    CHECK(queued_short.index >= 0 && queued_short.rc == 0);
    CHECK(materialize.index >= 0 && materialize.rc == 1);
    CHECK(std::isnan(materialize.qty) && materialize.close_only == 0 && materialize.partition == -1);
    CHECK(seed > 0.0 && first > 0.0 && second > 0.0);
    CHECK(near(p.live_position_size(), -seed));
    CHECK(long_entry.close_only == 0 && near(long_entry.qty, first));
    CHECK(near(second, std::min(seed, first)));
    CHECK(near(f.before_final, first + second));
    CHECK(near(f.average_before_final, price));
    CHECK(final_short.index >= 0 && final_short.rc == 0);
    CHECK(f.filled_short != 0 && queued_short.incarnation == f.filled_short
          && final_short.incarnation == f.filled_short);
    // On the seed side the queued Short reports the whole frozen transaction.
    CHECK(queued_short.close_only == 0 && near(queued_short.qty, first + second - after));
    // Against both longs it reports only what that same sell reopens.
    CHECK(after <= 0.0 && near(-after, first - second));
    CHECK(final_short.partition == PF_FILL_QTY_PARTITION_FROZEN_PLACEMENT && near(final_short.qty, -after));
    CHECK(final_short.close_only == (after == 0.0 ? 1 : 0));
    if (fixed) {
        CHECK(near(first, seed) && near(after, 0.0));
    } else {
        CHECK(first > seed && after < 0.0);
    }
}

// The probe's close_only against the shape the fill books, in one run: the
// host probes the row inside a callback (on_native_applied_event, after the
// named fill has moved the position) and records what the row's own fill
// then does to the position. Bars start at a non-zero timestamp so a
// same-bar placement time is never the field's default of 0.
struct ShapeRecord {
    PendingProbe probe;
    double after = kNaN;    // position right after the row fills
    std::uint64_t filled = 0;
};

class ShapeReplay : public pineforge::source::PineStrategyHost {
public:
    ShapeReplay(const char* probe_after, const char* row, std::int32_t type, int pyramiding)
        : probe_after_(probe_after), row_(row), type_(type) {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = pyramiding;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
    }
    ShapeRecord rec;

protected:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        const std::string& label = event.request().label;
        if (label == probe_after_ && rec.probe.index < 0)
            rec.probe = probe_order(*this, row_, type_, kProbePrice);
        if (label == row_ && rec.filled == 0) {
            rec.filled = event.handle().incarnation;
            rec.after = live_position_size();
        }
    }
    static constexpr double kProbePrice = 95.0;

private:
    const char* probe_after_;
    const char* row_;
    std::int32_t type_;
};

const Bar kShapeBars[] = {
    mk_bar(10, 100, 100, 100, 100, 60'000),
    mk_bar(11, 100, 100, 100, 100, 60'000),
    mk_bar(12, 100, 100, 100, 100, 60'000),
    mk_bar(13, 100, 100, 94, 95, 60'000),
    mk_bar(14, 95, 95, 95, 95, 60'000),
};

// (a) pyramiding 2: strategy.close("L") then a default-sized buy stop L2 on
// the same bar is stamped after-close with the LONG created side; a 0.5 short
// opens later. L2's fill is a Transact of its own unit (long 0.5 after it),
// where the prior-cycle predicate alone (LONG != SHORT, priced) says 1.
class CloseThenStop final : public ShapeReplay {
public:
    CloseThenStop() : ShapeReplay("S", "L2", kEntryRow, 2) {}
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true, kNaN, kNaN, 1.0);
        if (pine_bar_index() == 1) {
            strategy_close("L");
            strategy_entry("L2", true, kNaN, 105.0);
        }
        if (pine_bar_index() == 2) strategy_entry("S", false, kNaN, kNaN, 0.5);
    }
};

void shape_close_then_stop_reports_the_fills_transaction() {
    CloseThenStop host;
    const Bar bars[] = {
        mk_bar(10, 100, 100, 100, 100, 60'000),
        mk_bar(11, 100, 100, 100, 100, 60'000),
        mk_bar(12, 100, 100, 100, 100, 60'000),
        mk_bar(13, 100, 106, 100, 105, 60'000),
        mk_bar(14, 105, 105, 105, 105, 60'000),
    };
    host.run(bars, 5);
    const auto& r = host.rec;
    r.probe.print("close then stop", "L2 after S");
    std::printf("  close then stop: L2 position after its fill %.17g, error '%s'\n",
                r.after, host.last_error().c_str());
    CHECK(host.last_error().empty());
    CHECK(r.probe.index >= 0 && r.probe.rc == 0 && r.filled == r.probe.incarnation);
    CHECK(near(r.after, 0.5));
    CHECK(r.probe.close_only == 0);
}

// (c) a host-sized strategy.order against an opposite position books
// CloseOpposite (resolve_source_terms, the Order branch).
class OrderAgainstShort final : public ShapeReplay {
public:
    OrderAgainstShort() : ShapeReplay("S", "O", 3, 1) {}
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        strategy_entry("S", false, kNaN, kNaN, 1.0);
        strategy_order("O", true, kNaN, 95.0);
    }
};

void shape_host_sized_order_reports_close_opposite() {
    OrderAgainstShort host;
    host.run(kShapeBars, 5);
    const auto& r = host.rec;
    r.probe.print("host-sized order", "O after S");
    std::printf("  host-sized order: O position after its fill %.17g, error '%s'\n",
                r.after, host.last_error().c_str());
    CHECK(host.last_error().empty());
    CHECK(r.probe.index >= 0 && r.probe.rc == 0 && r.filled == r.probe.incarnation);
    CHECK(near(r.after, 0.0));
    CHECK(r.probe.close_only == 1);
}

// (d) a priced reversal queued in the callback that placed it (pending_entries_,
// flushed after the script) is probed in that same callback: its queued copy
// still carries the default FLAT created side, but its flush stamps the live
// side, so the fill reverses through it (ReverseTo), never close-only.
class QueuedReversal final : public pineforge::source::PineStrategyHost {
public:
    QueuedReversal() {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
    }
    PendingProbe probe;
    double before = kNaN;   // position in the callback that queued S
    double after = kNaN;    // position right after S's own fill
    std::uint64_t filled = 0;
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true);
        if (pine_bar_index() == 2) {
            strategy_entry("S", false, kNaN, 95.0);
            before = live_position_size();
            probe = probe_order(*this, "S", kEntryRow, 95.0);
        }
    }

protected:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        if (event.request().label == "S" && filled == 0) {
            filled = event.handle().incarnation;
            after = live_position_size();
        }
    }
};

void shape_queued_row_is_probed_on_its_flush_side() {
    QueuedReversal host;
    host.run(kShapeBars, 5);
    host.probe.print("queued reversal", "S in its own callback");
    std::printf("  queued reversal: position before S %.17g, after S's fill %.17g, error '%s'\n",
                host.before, host.after, host.last_error().c_str());
    CHECK(host.last_error().empty());
    CHECK(host.probe.index >= 0 && host.probe.rc == 0 && host.probe.incarnation == 0);
    // Long 1 when S is queued; nothing fills before S (bar 13 low 94 crosses
    // 95). The flush stamps the live LONG side, so S's fill reverses: short 1.
    CHECK(near(host.before, 1.0));
    CHECK(host.filled != 0 && near(host.after, -1.0));
    CHECK(host.probe.close_only == 0);
}

// The 0-to-1 direction of the prior-cycle predicate: a priced entry that
// replaced a same-id market entry (replaced_default_market_incarnation), then an
// opposite market entry on the same bar. Once the short is live the fill of X
// only closes it; the probe says 1.
// A nonzero commission keeps every same-bar market batch scope off
// (same_bar_market_tx_scope needs commission 0, the p2 and fixed-batch scopes
// pyramiding 2): the market X is then submitted live, so the priced X finds it
// in live_by_source_key_ and replaces it (replaced_opening,
// projection_predecessor_market). Under the batch scope the market X was only
// staged, the priced X found no predecessor, and the market X filled long 1.
class ReplacedMarketThenOpposite final : public ShapeReplay {
public:
    ReplacedMarketThenOpposite() : ShapeReplay("S", "X", kEntryRow, 1) {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 1;
        config.commission_value = 0.01;
        configure_pine_strategy(config);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        strategy_entry("X", true, kNaN, kNaN, 1.0);
        strategy_entry("X", true, kNaN, 105.0, 1.0);
        strategy_entry("S", false, kNaN, kNaN, 1.0);
    }
};

void shape_priced_replacement_of_a_market_entry_closes_only() {
    ReplacedMarketThenOpposite host;
    const Bar bars[] = {
        mk_bar(10, 100, 100, 100, 100, 60'000),
        mk_bar(11, 100, 100, 100, 100, 60'000),
        mk_bar(12, 100, 106, 100, 105, 60'000),
        mk_bar(13, 105, 105, 105, 105, 60'000),
    };
    host.run(bars, 4);
    const auto& r = host.rec;
    r.probe.print("replaced market", "X after S");
    std::printf("  replaced market: X position after its fill %.17g, error '%s'\n",
                r.after, host.last_error().c_str());
    CHECK(host.last_error().empty());
    CHECK(r.probe.index >= 0 && r.probe.rc == 0 && r.filled == r.probe.incarnation);
    // The shape was built: the probed row is the replacement of the market X.
    CHECK(r.probe.index >= 0 && r.probe.replaced_market != 0);
    CHECK(near(r.after, 0.0));
    CHECK(r.probe.close_only == 1);
}

// (c1) a frozen reversal transaction (construction of
// test_same_cycle_frozen_transaction_exactly_flattens_long,
// tests/oracle/test_oracle_reversal_close_only.cpp): long 1, a priced FIXED
// short stop S of 1 freezes the transaction 2, a same-bar long add L2 makes
// the book long 2. S's fill books CloseOpposite of the frozen 2
// (resolve_source_terms, frozen_reversal_closes) and ends flat; the probe,
// after L2 has filled, must say close_only 1 and that 2.
class FrozenReversal final : public ShapeReplay {
public:
    FrozenReversal() : ShapeReplay("L2", "S", kEntryRow, 2) {}
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L1", true, kNaN, kNaN, 1.0);
        if (pine_bar_index() == 1) {
            strategy_entry("S", false, kNaN, 95.0, 1.0);
            strategy_entry("L2", true, kNaN, kNaN, 1.0);
        }
    }
};

void shape_frozen_reversal_reports_its_close() {
    FrozenReversal host;
    host.run(kShapeBars, 5);
    const auto& r = host.rec;
    r.probe.print("frozen reversal", "S after L2");
    std::printf("  frozen reversal: S position after its fill %.17g, error '%s'\n",
                r.after, host.last_error().c_str());
    CHECK(host.last_error().empty());
    CHECK(r.probe.index >= 0 && r.probe.rc == 0 && r.filled == r.probe.incarnation);
    CHECK(near(r.after, 0.0));
    CHECK(r.probe.close_only == 1);
    CHECK(near(r.probe.qty, 2.0));
}

// (b) a sequential bracketed pair (construction of RaceProbe,
// tests/test_same_tick_multi_entry_race.cpp: seed short 50, then two default
// market longs on one bar, each with its own full bracket). The first member's
// fill is the pair's Transact of max(|position|, own) = 200 (position +150
// after it), never close-only; the probe, inside the callback that placed the
// pair, reports 0.
class SequentialPair final : public pineforge::source::PineStrategyHost {
public:
    SequentialPair() {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 2.0;
        config.pyramiding = 1;
        config.commission_value = 0.0;
        config.slippage = 0;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
    }
    ShapeRecord rec;
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("Seed", false, kNaN, kNaN, 50.0);
        if (pine_bar_index() != 2) return;
        strategy_entry("Long", true);
        strategy_exit("Long Exit", "Long", 105.0, 90.0, kNaN, kNaN, kNaN, 100.0, "", kNaN, "");
        strategy_entry("Wyckoff Long", true);
        strategy_exit("Wyckoff Long Exit", "Wyckoff Long", 110.0, 88.0, kNaN, kNaN, kNaN,
                      100.0, "", kNaN, "");
        rec.probe = probe_order(*this, "Long", kEntryRow, 100.0);
    }

protected:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        if (event.request().label == "Long" && rec.filled == 0) {
            rec.filled = event.handle().incarnation;
            rec.after = live_position_size();
        }
    }
};

void shape_sequential_pair_member_is_not_close_only() {
    SequentialPair host;
    const Bar bars[] = {
        mk_bar(10, 100, 100.5, 99.5, 100, 900'000),
        mk_bar(11, 100, 100.5, 99.5, 100, 900'000),
        mk_bar(12, 100, 100.5, 99.5, 100, 900'000),
        mk_bar(13, 100, 100.5, 99.5, 100, 900'000),
        mk_bar(14, 100, 100.5, 99.5, 100, 900'000),
    };
    host.run(bars, 5);
    const auto& r = host.rec;
    r.probe.print("sequential pair", "Long in its own callback");
    std::printf("  sequential pair: Long position after its fill %.17g, error '%s'\n",
                r.after, host.last_error().c_str());
    CHECK(host.last_error().empty());
    // The pair groups only against a LIVE prior member (place_entry scans
    // live_handles_), so "Long" is live when the callback probes it: the
    // probed incarnation is the one that fills.
    CHECK(r.probe.index >= 0 && r.probe.rc == 0 && r.probe.incarnation != 0
          && r.filled == r.probe.incarnation);
    CHECK(near(r.after, 150.0));
    CHECK(r.probe.close_only == 0);
}

// An entry against the allowed risk direction (strategy.risk.allow_entry_in
// long only) can only close: live_direction_gate in resolve_source_terms books
// CloseOpposite of the opposite book against an opposite position, and the
// probe's direction branch reports `opposite`. Long 1 by market on bar 0, a
// short stop S at 95 placed on bar 1 against it, probed on bar 2 with S
// resting; bar 13's low 94 fills S, which ends flat.
class DirectionGated final : public pineforge::source::PineStrategyHost {
public:
    DirectionGated() {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 1;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
    }
    ShapeRecord rec;
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            set_pine_risk_direction(1);
            strategy_entry("L", true, kNaN, kNaN, 1.0);
        }
        if (pine_bar_index() == 1) strategy_entry("S", false, kNaN, 95.0, 1.0);
        if (pine_bar_index() == 2) rec.probe = probe_order(*this, "S", kEntryRow, 95.0);
    }

protected:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        if (event.request().label == "S" && rec.filled == 0) {
            rec.filled = event.handle().incarnation;
            rec.after = live_position_size();
        }
    }
};

void shape_direction_gated_entry_closes_only() {
    DirectionGated host;
    host.run(kShapeBars, 5);
    const auto& r = host.rec;
    r.probe.print("direction gated", "S against long only");
    std::printf("  direction gated: S position after its fill %.17g, error '%s'\n",
                r.after, host.last_error().c_str());
    CHECK(host.last_error().empty());
    CHECK(r.probe.index >= 0 && r.probe.rc == 0 && r.filled != 0
          && r.filled == r.probe.incarnation);
    CHECK(near(r.after, 0.0));
    CHECK(r.probe.close_only == 1);
}

// Issue #910's shapes 1 and 3 as the issue words them. Besides the report,
// each replay records what the row's own fill then books: the units it closes
// and opens (ExecutionAppliedEvent) and the position it leaves.
struct BookedFill {
    std::uint64_t incarnation = 0;  // of the row that filled
    double closed = kNaN, opened = kNaN, after = kNaN;

    void record(const native_order::ExecutionAppliedEvent& event, double position) {
        incarnation = event.handle().incarnation;
        closed = event.closed_units;
        opened = event.opened_units;
        after = position;
    }
};

// Shape 1 as worded: the position is SHORT and, on one bar, the script closes
// the short and then places a long stop entry with the default quantity
// (CloseThenStop above closes a LONG, so its L2 is on the held side and is
// submitted at once). Here L2 opposes the position it is placed against: it
// waits behind the close and is submitted when the close fills, stamped
// after-close. The issue's numbers -- a reported quantity of 1 that leaves 0.5
// long -- need a 0.5 short at the fill, which opens on the next bar. L2's fill
// is then a Transact of its own unit: it closes 0.5 and opens 0.5, and the
// report, read while that short is open, says quantity 1 and not close-only.
class CloseShortThenStop final : public pineforge::source::PineStrategyHost {
public:
    CloseShortThenStop() {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 1;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
    }
    PendingProbe probe;
    BookedFill fill;
    double closed_against = kNaN;  // position when the script closes it
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("S", false, kNaN, kNaN, 1.0);
        if (pine_bar_index() == 1) {
            closed_against = live_position_size();
            strategy_close("S");
            strategy_entry("L2", true, kNaN, 105.0);
        }
        if (pine_bar_index() == 2) strategy_entry("S2", false, kNaN, kNaN, 0.5);
    }

protected:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        const std::string& label = event.request().label;
        if (label == "S2" && probe.index < 0) probe = probe_order(*this, "L2", kEntryRow, 105.0);
        if (label == "L2" && fill.incarnation == 0) fill.record(event, live_position_size());
    }
};

void shape_close_short_then_stop_reports_the_fills_transaction() {
    CloseShortThenStop host;
    const Bar bars[] = {
        mk_bar(10, 100, 100, 100, 100, 60'000),
        mk_bar(11, 100, 100, 100, 100, 60'000),
        mk_bar(12, 100, 100, 100, 100, 60'000),
        mk_bar(13, 100, 106, 100, 105, 60'000),
        mk_bar(14, 105, 105, 105, 105, 60'000),
    };
    host.run(bars, 5);
    const auto& fill = host.fill;
    host.probe.print("close short then stop", "L2 after S2");
    std::printf("  close short then stop: L2's fill closed %.17g, opened %.17g, position after "
                "%.17g, error '%s'\n", fill.closed, fill.opened, fill.after,
                host.last_error().c_str());
    CHECK(host.last_error().empty());
    // The shape was built: the script closed a short of 1.
    CHECK(near(host.closed_against, -1.0));
    CHECK(host.probe.index >= 0 && host.probe.rc == 0 && fill.incarnation != 0
          && fill.incarnation == host.probe.incarnation);
    // The fill: a transaction of L2's own unit against the 0.5 short.
    CHECK(near(fill.closed, 0.5) && near(fill.opened, 0.5));
    CHECK(near(fill.after, 0.5));
    // The report: that transaction, which opens.
    CHECK(host.probe.close_only == 0);
    CHECK(near(host.probe.qty, 1.0));
}

// mirror_order_type exports a strategy.order row as 3.
constexpr std::int32_t kOrderRow = 3;

// Shape 3 as worded: the position is short and the script places a long
// strategy.order WITH ITS OWN QUANTITY (OrderAgainstShort above gives none and
// places its order while still flat). The order is placed on bar 1, against the
// short of 1 that bar 0's market entry opened, and is probed on bar 2 while it
// rests; bar 13's low of 94 fills its limit at 95.
//   - Outside an OCA cancel group the request is an explicit Transact: the
//     fill takes no shape decision and the kernel books the units as one
//     signed transaction, closing the short and opening only what exceeds it.
//   - In an OCA cancel group (oca_type 1) the request is host-sized and the
//     fill closes the smaller of its quantity and the short, opening nothing.
class OwnQuantityOrder final : public pineforge::source::PineStrategyHost {
public:
    OwnQuantityOrder(double qty, bool oca_cancel) : qty_(qty), oca_cancel_(oca_cancel) {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 1;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
    }
    PendingProbe probe;
    BookedFill fill;
    double placed_against = kNaN;  // position when the script places O
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("S", false, kNaN, kNaN, 1.0);
        if (pine_bar_index() == 1) {
            placed_against = live_position_size();
            if (oca_cancel_) strategy_order("O", true, qty_, 95.0, kNaN, "G", 1);
            else strategy_order("O", true, qty_, 95.0);
        }
        if (pine_bar_index() == 2) probe = probe_order(*this, "O", kOrderRow, 95.0);
    }

protected:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        if (event.request().label == "O" && fill.incarnation == 0)
            fill.record(event, live_position_size());
    }

private:
    double qty_;
    bool oca_cancel_;
};

// Runs the replay and checks what every variant shares: no error, the order
// was placed against the short of 1, and the probed row is the row that fills,
// reported in the explicit partition.
void run_own_quantity_order(OwnQuantityOrder& host, const char* shape) {
    host.run(kShapeBars, 5);
    host.probe.print(shape, "O against the short");
    std::printf("  %s: O's fill closed %.17g, opened %.17g, position after %.17g, error '%s'\n",
                shape, host.fill.closed, host.fill.opened, host.fill.after,
                host.last_error().c_str());
    CHECK(host.last_error().empty());
    CHECK(near(host.placed_against, -1.0));
    CHECK(host.probe.index >= 0 && host.probe.rc == 0 && host.fill.incarnation != 0
          && host.fill.incarnation == host.probe.incarnation);
    CHECK(host.probe.partition == PF_FILL_QTY_PARTITION_EXPLICIT);
}

// The issue's shape 3: an own quantity equal to the short. The fill only
// closes and ends flat; the report must say so, with that quantity.
void shape_own_quantity_order_reports_its_close() {
    OwnQuantityOrder host(1.0, false);
    run_own_quantity_order(host, "own-quantity order");
    CHECK(near(host.fill.closed, 1.0) && near(host.fill.opened, 0.0));
    CHECK(near(host.fill.after, 0.0));
    CHECK(host.probe.close_only == 1);
    CHECK(near(host.probe.qty, 1.0));
}

// The other branch of the same transaction: an own quantity of 2 against the
// short of 1 closes 1 and opens the remaining 1, so it is not close-only and
// the reported quantity is the whole transaction.
void shape_own_quantity_order_larger_than_the_short_opens_the_remainder() {
    OwnQuantityOrder host(2.0, false);
    run_own_quantity_order(host, "larger own-quantity order");
    CHECK(near(host.fill.closed, 1.0) && near(host.fill.opened, 1.0));
    CHECK(near(host.fill.after, 1.0));
    CHECK(host.probe.close_only == 0);
    CHECK(near(host.probe.qty, 2.0));
}

// The OCA cancel-group variant of shape 3: host-sized, it closes only.
void shape_own_quantity_oca_cancel_order_reports_its_close() {
    OwnQuantityOrder host(1.0, true);
    run_own_quantity_order(host, "own-quantity OCA cancel order");
    CHECK(near(host.fill.closed, 1.0) && near(host.fill.opened, 0.0));
    CHECK(near(host.fill.after, 0.0));
    CHECK(host.probe.close_only == 1);
    CHECK(near(host.probe.qty, 1.0));
}

// An OCA cancel-group order of 2 against the short of 1 still closes only the
// short: the reported quantity is the 1 its fill transacts, not the 2 asked.
void shape_own_quantity_oca_cancel_order_closes_at_most_the_short() {
    OwnQuantityOrder host(2.0, true);
    run_own_quantity_order(host, "larger own-quantity OCA cancel order");
    CHECK(near(host.fill.closed, 1.0) && near(host.fill.opened, 0.0));
    CHECK(near(host.fill.after, 0.0));
    CHECK(host.probe.close_only == 1);
    CHECK(near(host.probe.qty, 1.0));
}

}  // namespace

int main() {
    std::printf("-- a default entry's fill-quantity probe answers its booked quantity\n");
    a_default_entry_probe_answers_its_booked_quantity();
    std::printf("-- the money band's affordable price is not the kernel conversion (SZ10)\n");
    the_money_band_price_is_not_the_kernel_conversion();
    std::printf("-- a priced entry after a same-bar opposite market reverses; without one it closes only\n");
    test_same_bar_predecessor(true);
    // Control: the same target as test_prior_cycle_close_only
    // (test_live_order_derived.cpp), kept here as the replay's other branch.
    test_same_bar_predecessor(false);
    std::printf("-- a short seed's final Short reports what its fill reopens\n");
    test_short_seed_replay(QtyType::FIXED);
    test_short_seed_replay(QtyType::PERCENT_OF_EQUITY);
    std::printf("-- the probe reports the fill's modelled shapes\n");
    shape_close_then_stop_reports_the_fills_transaction();
    shape_frozen_reversal_reports_its_close();
    shape_host_sized_order_reports_close_opposite();
    shape_queued_row_is_probed_on_its_flush_side();
    shape_priced_replacement_of_a_market_entry_closes_only();
    shape_sequential_pair_member_is_not_close_only();
    shape_direction_gated_entry_closes_only();
    std::printf("-- issue 910's shapes 1 and 3 as worded: the report and the fill agree\n");
    shape_close_short_then_stop_reports_the_fills_transaction();
    shape_own_quantity_order_reports_its_close();
    shape_own_quantity_order_larger_than_the_short_opens_the_remainder();
    shape_own_quantity_oca_cancel_order_reports_its_close();
    shape_own_quantity_oca_cancel_order_closes_at_most_the_short();
    std::printf("R5 B-ADAPTER fill-quantity probe: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
