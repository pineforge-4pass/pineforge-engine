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
    std::uint64_t incarnation = 0;

    void print(const char* shape, const char* order) const {
        std::fprintf(stderr, "%s %s: index=%d rc=%d qty=%.17g close_only=%d partition=%d "
                     "predecessor=%d incarnation=%llu\n", shape, order, index, rc, qty,
                     close_only, partition, predecessor, static_cast<unsigned long long>(incarnation));
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
private:
    bool market_first_;
};

void test_same_bar_predecessor(bool market_first) {
    const Bar bars[] = {
        mk_bar(0, 100, 100, 100, 100, 60'000),
        mk_bar(1, 100, 100, 100, 100, 60'000),
        mk_bar(2, 100, 101, 94, 96, 60'000),
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
        CHECK(near(first, seed) && after == 0.0);
    } else {
        CHECK(first > seed && after < 0.0);
    }
}

}  // namespace

int main() {
    std::printf("-- a default entry's fill-quantity probe answers its booked quantity\n");
    a_default_entry_probe_answers_its_booked_quantity();
    std::printf("-- the money band's affordable price is not the kernel conversion (SZ10)\n");
    the_money_band_price_is_not_the_kernel_conversion();
    test_same_bar_predecessor(true);
    test_same_bar_predecessor(false);
    test_short_seed_replay(QtyType::FIXED);
    test_short_seed_replay(QtyType::PERCENT_OF_EQUITY);
    std::printf("R5 B-ADAPTER fill-quantity probe: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
