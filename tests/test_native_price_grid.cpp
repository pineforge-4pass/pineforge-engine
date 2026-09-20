// R5 L8: the generic instrument price grid (NativeRunSpec::price_grid).
// Every NativePriceGrid::None expectation is a clean-main witness captured
// from a 7daa511 build of these exact scenarios, so the default path is
// pinned bit-for-bit while the opt-in modes carry the new behaviour. The
// adapter twin at the end keeps TradingView's own tick rule: it is the
// identity diff that proves the kernel grid never reached src/source.
#include "l4a_native_route_guard.hpp"

#include <pineforge/native_host.hpp>

#include "../src/native_matching.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <variant>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {
int checks = 0, failures = 0;
const char* scenario = "setup";
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::printf("FAIL %s:%d %s\n", scenario, __LINE__, #x); } } while (0)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t T = 1736121600000LL;
constexpr double kTick = 0.25;

// Clean-main witnesses are compared bit-for-bit: a quantization that leaked
// into the default path would move a low bit long before a printed decimal.
bool same_bits(double actual, double expected) {
    if (native_matching::double_bits(actual) == native_matching::double_bits(expected)) return true;
    std::printf("  actual=%.17g expected=%.17g\n", actual, expected);
    return false;
}

bool on_grid(double price) { return price == std::round(price / kTick) * kTick; }

struct Host final : NativeStrategyHost {
    std::function<void(Host&)> begin;
    void on_native_run_begin() override { if (begin) begin(*this); }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

NativeRunSpec spec(const char* key) {
    NativeRunSpec s;
    s.identity = {key, 1};
    s.input_tf = "1"; s.script_tf = "1";
    s.tickerid = "TEST:GRID"; s.timezone = "UTC"; s.session = "24x7";
    s.initial_capital = 100000; s.point_value = 1; s.account_fx = 1;
    s.price_tick = kTick;
    s.fee_kind = NativeFeeKind::CashPerExecution; s.fee_value = 0;
    return s;
}

NativeRunSpec grid_spec(const char* key, NativePriceGrid grid,
                        NativeGridRounding rounding = NativeGridRounding::HalfUp) {
    auto s = spec(key);
    s.price_grid = grid;
    s.grid_rounding = rounding;
    return s;
}

no::Request tx(double units, const char* label) { return {no::Transact{units}, label, ""}; }
no::Request limit(double units, double price, const char* label) {
    auto r = tx(units, label);
    r.trigger = no::Limit{price, false};
    return r;
}
no::Request stop(double units, double price, const char* label) {
    auto r = tx(units, label);
    r.trigger = no::Stop{price};
    return r;
}

// Every price on this bar is off the 0.25 ladder except its close.
const Bar kBar{100.10, 100.40, 99.60, 100.00, 1, T};
// The high sits exactly on a half tick, so a stop one half tick above it is
// unreachable on the raw path and reachable on the quantized one.
const Bar kHalfTickBar{100.00, 100.375, 99.90, 100.00, 1, T};
// The mirrored case: a low whose nearest tick is below it, so a buy limit
// between the two is reachable only on the quantized path.
const Bar kLowBar{100.00, 100.10, 99.87, 100.00, 1, T};

struct Run {
    std::vector<double> raw;
    std::vector<double> resolved;
    std::uint64_t hash = 0;
    std::string error;
};

Run execute(const NativeRunSpec& s, const no::Request& request, const Bar& bar) {
    Host h;
    h.begin = [&](Host& x) { CHECK(x.submit(request).status == no::SubmitStatus::Accepted); };
    Run out;
    if (h.configure_native(s).status != NativeSetupStatus::Applied) {
        CHECK(false);
        return out;
    }
    h.run(&bar, 1);
    for (const auto& row : h.native_events(0)) {
        if (!row.command) continue;
        if (const auto* e = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
            out.raw.push_back(e->raw_price);
            out.resolved.push_back(e->resolved_price);
        }
    }
    out.hash = h.native_continuation_hash();
    out.error = h.last_error();
    return out;
}

std::uint64_t empty_run_hash(const NativeRunSpec& s) {
    Host h;
    CHECK(h.configure_native(s).status == NativeSetupStatus::Applied);
    h.run(&kBar, 1);
    CHECK(h.last_error().empty());
    return h.native_continuation_hash();
}

struct Row {
    const char* key;
    no::Request request;
    double raw;
    double resolved;
    std::uint64_t hash;
};

std::vector<double> check_rows(const NativeRunSpec& base, const std::vector<Row>& rows,
                               bool expect_on_grid) {
    std::vector<double> booked;
    for (const auto& row : rows) {
        auto s = base;
        s.identity = {row.key, 1};
        const auto out = execute(s, row.request, kBar);
        CHECK(out.error.empty());
        CHECK(out.resolved.size() == 1);
        if (out.resolved.size() != 1) {
            booked.push_back(kNaN);
            continue;
        }
        // The order's raw modeled price is a fact of the path, never rounded.
        CHECK(same_bits(out.raw[0], row.raw));
        CHECK(same_bits(out.resolved[0], row.resolved));
        CHECK(on_grid(out.resolved[0]) == expect_on_grid);
        if (row.hash) CHECK(out.hash == row.hash);
        booked.push_back(out.resolved[0]);
    }
    return booked;
}

// --- 1. the default grid is the clean-main path ---------------------------
void grid_none_is_clean_main() {
    scenario = "L8-1 None keeps the clean-main fills";
    check_rows(spec(""), {
        {"g-market",     tx(1, "market"),                100.10, 100.10, 14125007403465094750ULL},
        {"g-buy-limit",  limit(1, 99.80, "buy-limit"),     99.80,  99.80,  3140672069948710939ULL},
        {"g-sell-limit", limit(-1, 100.30, "sell-limit"), 100.30, 100.30, 14574835912580645344ULL},
        {"g-buy-stop",   stop(1, 100.30, "buy-stop"),     100.30, 100.30,  2927666990158715397ULL},
        {"g-sell-stop",  stop(-1, 99.70, "sell-stop"),     99.70,  99.70, 11741635604452854407ULL},
    }, /*expect_on_grid=*/false);

    // Slippage is whole ticks carried on the raw, unquantized basis.
    auto slipped = spec("g-slip");
    slipped.slippage_ticks = 2;
    const auto out = execute(slipped, tx(1, "market"), kBar);
    CHECK(out.resolved.size() == 1);
    if (out.resolved.size() == 1) CHECK(same_bits(out.resolved[0], 100.60));
    CHECK(out.hash == 15244465367648976717ULL);

    // A stop above the raw high is not reached without the quantized path.
    const auto unreachable =
        execute(spec("g-half-tick"), stop(1, 100.50, "half-tick-stop"), kHalfTickBar);
    CHECK(unreachable.error.empty());
    CHECK(unreachable.resolved.empty());
    CHECK(unreachable.hash == 3094959961021854686ULL);
}

// --- 2. QuantizeFills, nearest tick ---------------------------------------
void quantize_fills_half_up() {
    scenario = "L8-2 QuantizeFills books the nearest tick";
    // A sell limit whose nearest tick sits below its level keeps limit-or-
    // better: the protection cap is the tick on the order's own side.
    check_rows(grid_spec("", NativePriceGrid::QuantizeFills), {
        {"h-market",     tx(1, "market"),                100.10, 100.00, 0},
        {"h-buy-limit",  limit(1, 99.80, "buy-limit"),     99.80,  99.75, 0},
        {"h-sell-limit", limit(-1, 100.30, "sell-limit"), 100.30, 100.50, 0},
        {"h-buy-stop",   stop(1, 100.30, "buy-stop"),     100.30, 100.25, 0},
        {"h-sell-stop",  stop(-1, 99.70, "sell-stop"),     99.70,  99.75, 0},
    }, /*expect_on_grid=*/true);

    // Hand-rounded: nearest tick, ties away from zero.
    CHECK(same_bits(std::round(100.10 / kTick) * kTick, 100.00));
    CHECK(same_bits(std::round(100.30 / kTick) * kTick, 100.25));
    CHECK(same_bits(native_matching::grid_round_half_up(100.125, kTick), 100.25));
    CHECK(same_bits(native_matching::grid_round_half_up(-100.125, kTick), -100.25));
    // An on-ladder price is its own quantization in every mode.
    CHECK(same_bits(native_matching::grid_round_half_up(100.25, kTick), 100.25));
    CHECK(same_bits(native_matching::grid_round_directional(100.25, kTick, true), 100.25));
    CHECK(same_bits(native_matching::grid_round_directional(100.25, kTick, false), 100.25));
    // The grid is an identity without a ladder or on a nonfinite price.
    CHECK(same_bits(native_matching::grid_round_half_up(100.10, 0.0), 100.10));
    CHECK(std::isnan(native_matching::grid_round_directional(kNaN, kTick, true)));

    // Slippage is carried after the rounding and is itself a whole number
    // of ticks, so the booked price stays on the ladder.
    auto slipped = grid_spec("h-slip", NativePriceGrid::QuantizeFills);
    slipped.slippage_ticks = 2;
    const auto out = execute(slipped, tx(1, "market"), kBar);
    CHECK(out.resolved.size() == 1);
    if (out.resolved.size() == 1) {
        CHECK(same_bits(out.resolved[0], 100.50));
        CHECK(on_grid(out.resolved[0]));
    }
}

// --- 3. QuantizeFills, directional ----------------------------------------
void quantize_fills_directional() {
    scenario = "L8-3 Directional rounds toward each order's own region";
    const auto base = grid_spec("", NativePriceGrid::QuantizeFills, NativeGridRounding::Directional);
    const auto booked = check_rows(base, {
        {"d-buy-market",  tx(1, "market"),                100.10, 100.25, 0},
        {"d-sell-market", tx(-1, "market"),               100.10, 100.00, 0},
        {"d-buy-limit",   limit(1, 99.80, "buy-limit"),     99.80,  99.75, 0},
        {"d-sell-limit",  limit(-1, 100.30, "sell-limit"), 100.30, 100.50, 0},
        {"d-buy-stop",    stop(1, 100.30, "buy-stop"),     100.30, 100.50, 0},
        {"d-sell-stop",   stop(-1, 99.70, "sell-stop"),     99.70,  99.50, 0},
    }, /*expect_on_grid=*/true);
    // The same fills as relations: a limit rounds to its own favourable side,
    // a stop and a market fill to the adverse one.
    CHECK(booked.size() == 6);
    if (booked.size() == 6) {
        CHECK(booked[0] > kBar.open);       // buy market, adverse
        CHECK(booked[1] < kBar.open);       // sell market, adverse
        CHECK(booked[2] < 99.80);           // buy limit rounds down
        CHECK(booked[3] > 100.30);          // sell limit rounds up
        CHECK(booked[4] > 100.30);          // buy stop rounds up
        CHECK(booked[5] < 99.70);           // sell stop rounds down
    }
}

// --- 4. QuantizeFillsAndTriggers ------------------------------------------
void quantize_triggers() {
    scenario = "L8-4 QuantizeFillsAndTriggers tests the quantized path";
    // The level is exactly half a tick beyond the raw high and exactly on the
    // tick the half-up quantized high reaches.
    CHECK(same_bits(100.50 - kHalfTickBar.high, kTick / 2));
    CHECK(same_bits(native_matching::grid_round_half_up(kHalfTickBar.high, kTick), 100.50));

    const auto fills_only = execute(grid_spec("t-fills", NativePriceGrid::QuantizeFills),
                                    stop(1, 100.50, "stop"), kHalfTickBar);
    CHECK(fills_only.error.empty());
    CHECK(fills_only.resolved.empty());

    const auto both = execute(grid_spec("t-both", NativePriceGrid::QuantizeFillsAndTriggers),
                              stop(1, 100.50, "stop"), kHalfTickBar);
    CHECK(both.error.empty());
    CHECK(both.resolved.size() == 1);
    if (both.resolved.size() == 1) {
        CHECK(same_bits(both.raw[0], 100.50));
        CHECK(same_bits(both.resolved[0], 100.50));
    }

    // One tick further is beyond the quantized high as well.
    const auto beyond = execute(grid_spec("t-beyond", NativePriceGrid::QuantizeFillsAndTriggers),
                                stop(1, 100.75, "stop"), kHalfTickBar);
    CHECK(beyond.error.empty());
    CHECK(beyond.resolved.empty());

    // A directional path extends its own excursion to the enclosing tick, so
    // the same level is reached and still booked on the ladder.
    const auto directional = execute(
        grid_spec("t-dir", NativePriceGrid::QuantizeFillsAndTriggers, NativeGridRounding::Directional),
        stop(1, 100.50, "stop"), kHalfTickBar);
    CHECK(directional.error.empty());
    CHECK(directional.resolved.size() == 1);
    if (directional.resolved.size() == 1) CHECK(same_bits(directional.resolved[0], 100.50));

    // The mirrored region: a buy limit under the quantized low.
    CHECK(kLowBar.low > 99.80);
    CHECK(same_bits(native_matching::grid_round_half_up(kLowBar.low, kTick), 99.75));
    const auto raw_limit = execute(grid_spec("t-limit-fills", NativePriceGrid::QuantizeFills),
                                   limit(1, 99.80, "buy-limit"), kLowBar);
    CHECK(raw_limit.error.empty());
    CHECK(raw_limit.resolved.empty());
    const auto grid_limit = execute(
        grid_spec("t-limit-both", NativePriceGrid::QuantizeFillsAndTriggers),
        limit(1, 99.80, "buy-limit"), kLowBar);
    CHECK(grid_limit.error.empty());
    CHECK(grid_limit.resolved.size() == 1);
    if (grid_limit.resolved.size() == 1) {
        CHECK(same_bits(grid_limit.raw[0], 99.80));
        CHECK(same_bits(grid_limit.resolved[0], 99.75));
    }

    // Threshold form, stated directly: half-up opens the region at the
    // enclosing half tick, directional a tick early with the tick itself out.
    const native_matching::GridThreshold half{kTick, true};
    const native_matching::GridThreshold dir{kTick, false};
    CHECK(same_bits(native_matching::grid_region_threshold(100.50, false, half), 100.375));
    CHECK(same_bits(native_matching::grid_region_threshold(100.50, true, half), 100.625));
    CHECK(native_matching::grid_region_threshold(100.50, false, dir) > 100.25);
    CHECK(native_matching::grid_region_threshold(100.50, false, dir) < 100.2501);
    CHECK(native_matching::grid_region_threshold(100.50, true, dir) < 100.75);
    // An inactive threshold is the raw level itself, bit-for-bit.
    CHECK(same_bits(native_matching::grid_region_threshold(100.30, false, {}), 100.30));
    CHECK(same_bits(native_matching::grid_region_threshold(100.30, true, {}), 100.30));
}

// --- 5. validation ---------------------------------------------------------
void grid_requires_a_tick() {
    scenario = "L8-5 a quantizing grid requires a tick ladder";
    auto missing = grid_spec("e-grid", NativePriceGrid::QuantizeFills);
    missing.price_tick = 0.0;
    const auto validation = validate_native_run_spec(missing);
    CHECK(!validation.ok());
    CHECK(validation.error == NativeRunSpecError::GridRequiresPriceTick);
    CHECK(validation.field == NativeRunSpecField::PriceGrid);
    Host h;
    const auto setup = h.configure_native(missing);
    CHECK(setup.status == NativeSetupStatus::Failed);
    CHECK(setup.validation.error == NativeRunSpecError::GridRequiresPriceTick);
    CHECK(setup.validation.field == NativeRunSpecField::PriceGrid);

    // A zero tick without a grid keeps its documented unquantized meaning.
    auto unquantized = spec("e-none");
    unquantized.price_tick = 0.0;
    CHECK(validate_native_run_spec(unquantized).ok());

    auto bad_grid = spec("e-bad-grid");
    bad_grid.price_grid = static_cast<NativePriceGrid>(7);
    CHECK(validate_native_run_spec(bad_grid).error == NativeRunSpecError::UnknownPriceGrid);
    CHECK(validate_native_run_spec(bad_grid).field == NativeRunSpecField::PriceGrid);

    auto bad_rounding = spec("e-bad-rounding");
    bad_rounding.grid_rounding = static_cast<NativeGridRounding>(9);
    CHECK(validate_native_run_spec(bad_rounding).error == NativeRunSpecError::UnknownGridRounding);
    CHECK(validate_native_run_spec(bad_rounding).field == NativeRunSpecField::GridRounding);
}

// --- 6. hash neutrality ----------------------------------------------------
void hash_folds_only_when_set() {
    scenario = "L8-6 the grid folds into the spec hash only when set";
    constexpr std::uint64_t kCleanMain = 11136844009055565705ULL;
    CHECK(empty_run_hash(spec("g-empty")) == kCleanMain);
    // A rounding policy alone is not a behaviour: an unset grid folds nothing.
    CHECK(empty_run_hash(grid_spec("g-empty", NativePriceGrid::None,
                                   NativeGridRounding::Directional)) == kCleanMain);
    const auto half = empty_run_hash(grid_spec("g-empty", NativePriceGrid::QuantizeFills));
    const auto directional = empty_run_hash(
        grid_spec("g-empty", NativePriceGrid::QuantizeFills, NativeGridRounding::Directional));
    const auto triggers = empty_run_hash(
        grid_spec("g-empty", NativePriceGrid::QuantizeFillsAndTriggers));
    CHECK(half != kCleanMain);
    CHECK(half != directional);
    CHECK(half != triggers);
}

// --- twin: one adapter-driven probe through the native lowering ------------
source::PineStrategyConfig cfg() {
    source::PineStrategyConfig c;
    c.initial_capital = 1000000;
    c.default_qty_type = (int)QtyType::FIXED;
    c.default_qty_value = 1;
    c.pyramiding = 1;
    c.process_orders_on_close = false;
    c.commission_value = 0.0;
    c.commission_type = (int)CommissionType::PERCENT;
    c.slippage = 0;
    return c;
}

class MidBarStop final : public source::PineStrategyHost {
public:
    MidBarStop() { configure_pine_strategy(cfg()); set_syminfo_mintick(0.01); }
    void on_source_bar(const Bar& bar) override {
        if (placed_) return;
        strategy_entry("L", true, kNaN, kNaN, 1.0, "entry long");
        strategy_exit("X", "L", kNaN, (bar.open + bar.high) * 0.5, kNaN, kNaN, kNaN, 100.0,
                      "mid-bar stop");
        placed_ = true;
    }
private:
    bool placed_ = false;
};

void adapter_twin_is_untouched() {
    scenario = "L8-twin the adapter keeps its own tick rule";
    const std::vector<Bar> bars = {
        {1804.00, 1813.014, 1803.33, 1811.96, 49634.773, 1743397200000LL},
        {1811.96, 1812.000, 1801.08, 1808.93, 51943.482, 1743398100000LL},
        {1801.93, 1807.960, 1800.92, 1806.37, 37418.258, 1743420600000LL},
        {1806.37, 1819.000, 1805.97, 1812.52, 92807.927, 1743421500000LL},
        {1812.51, 1815.000, 1809.24, 1809.48, 39810.958, 1743422400000LL},
        {1809.49, 1818.800, 1808.15, 1816.41, 45388.380, 1743423300000LL},
    };
    MidBarStop host;
    host.run(bars.data(), (int)bars.size(), "15", "15");
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() != 1) return;
    const auto& t = host.get_trade(0);
    // Diffed by identity against the clean-main witness, not by trade number.
    CHECK(t.is_long);
    CHECK(same_bits(t.qty, 1.0));
    CHECK(t.entry_time == 1743398100000LL);
    CHECK(same_bits(t.entry_price, 1811.96));
    CHECK(t.exit_time == 1743398100000LL);
    // The raw stop level is 1808.507. The adapter floors a long's protective
    // stop onto its own 0.01 ladder; a kernel grid leaking into this path
    // would book the nearest tick, 1808.51, so this price is the identity
    // diff that keeps the two layers apart.
    CHECK(same_bits(t.exit_price, 1808.50));
    CHECK(same_bits(t.pnl, -3.4600000000000364));
}
} // namespace

int main() {
    grid_none_is_clean_main();
    quantize_fills_half_up();
    quantize_fills_directional();
    quantize_triggers();
    grid_requires_a_tick();
    hash_folds_only_when_set();
    adapter_twin_is_untouched();
    std::printf("%s native price grid: %d checks, %d failures\n",
                failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
