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

Bar mk_bar(int index, double o, double h, double l, double c) {
    Bar b;
    b.open = o;
    b.high = h;
    b.low = l;
    b.close = c;
    b.volume = 1.0;
    b.timestamp = 300000LL * index;
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
    const std::vector<Trade>& rows() const { return trades_; }
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

}  // namespace

int main() {
    std::printf("-- a default entry's fill-quantity probe answers its booked quantity\n");
    a_default_entry_probe_answers_its_booked_quantity();
    std::printf("-- the money band's affordable price is not the kernel conversion (SZ10)\n");
    the_money_band_price_is_not_the_kernel_conversion();
    std::printf("R5 B-ADAPTER fill-quantity probe: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
