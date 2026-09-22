// R5 L8: the generic instrument price grid (NativeRunSpec::price_grid).
// Every NativePriceGrid::None fill expectation is a clean-main witness captured
// from a 7daa511 build of these exact scenarios, so the default path is
// pinned bit-for-bit while the opt-in modes carry the new behaviour. Its
// hash-neutrality guard is portable by construction: the spec fold is pinned
// through native_run_spec_digest and every continuation hash is compared
// between two runs in this process, never against a constant (a continuation
// hash folds the machine's resolved timezone resources). The adapter twin
// (tests/test_native_price_grid_twin.cpp, source-bound) keeps TradingView's
// own tick rule: it is the identity diff that proves the kernel grid never
// reached src/source. This TU is source-free and runs in the kernel-only
// profile.
#include "native_price_grid_fixture.hpp"

#include <pineforge/native_host.hpp>

#include "../src/native_matching.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {
using namespace l8_fixture;

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

// The continuation-hash column this row carried is gone: a raw
// native_continuation_hash() constant folds the run's resolved timezone
// identity — it passed locally and failed on both of PR #260's CI runners
// (macOS and ubuntu), which is what retired it. R5 lane E23 made that identity
// the zone's CONTENT rather than the host's paths, so those runners would
// agree today; a literal would still pin the installed tzdata release. The default path's identity is now
// witnessed by the fills below plus, in L8-1 and L8-6, in-process hash equality
// against the same spec with the grid spelled out at its defaults, and the spec
// fold itself is pinned through native_run_spec_digest.
struct Row {
    const char* key;
    no::Request request;
    double raw;
    double resolved;
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
        booked.push_back(out.resolved[0]);
    }
    return booked;
}

// --- 1. the default grid is the clean-main path ---------------------------
void grid_none_is_clean_main() {
    scenario = "L8-1 None keeps the clean-main fills";
    const std::vector<Row> rows = {
        {"g-market",     tx(1, "market"),                100.10, 100.10},
        {"g-buy-limit",  limit(1, 99.80, "buy-limit"),     99.80,  99.80},
        {"g-sell-limit", limit(-1, 100.30, "sell-limit"), 100.30, 100.30},
        {"g-buy-stop",   stop(1, 100.30, "buy-stop"),     100.30, 100.30},
        {"g-sell-stop",  stop(-1, 99.70, "sell-stop"),     99.70,  99.70},
    };
    check_rows(spec(""), rows, /*expect_on_grid=*/false);

    // Each of those runs is byte-identical to the same run with the grid
    // spelled out at its defaults: same booked price, same continuation
    // identity. This is the in-process form of the neutrality the removed
    // per-row hash constants pinned (see the Row comment above).
    for (const auto& row : rows) {
        auto defaulted = spec(row.key);
        defaulted.price_grid = NativePriceGrid::None;
        defaulted.grid_rounding = NativeGridRounding::HalfUp;
        auto implicit_spec = spec(row.key);
        const auto implied = execute(implicit_spec, row.request, kBar);
        const auto restated = execute(defaulted, row.request, kBar);
        CHECK(native_run_spec_digest(defaulted) == native_run_spec_digest(implicit_spec));
        CHECK(restated.resolved.size() == implied.resolved.size());
        if (restated.resolved.size() != implied.resolved.size()) continue;
        for (std::size_t i = 0; i < implied.resolved.size(); ++i) {
            CHECK(same_bits(restated.raw[i], implied.raw[i]));
            CHECK(same_bits(restated.resolved[i], implied.resolved[i]));
        }
        CHECK(restated.hash == implied.hash);
    }

    // Slippage is whole ticks carried on the raw, unquantized basis.
    auto slipped = spec("g-slip");
    slipped.slippage_ticks = 2;
    const auto out = execute(slipped, tx(1, "market"), kBar);
    CHECK(out.resolved.size() == 1);
    if (out.resolved.size() == 1) CHECK(same_bits(out.resolved[0], 100.60));

    // A stop above the raw high is not reached without the quantized path.
    const auto unreachable =
        execute(spec("g-half-tick"), stop(1, 100.50, "half-tick-stop"), kHalfTickBar);
    CHECK(unreachable.error.empty());
    CHECK(unreachable.resolved.empty());
}

// --- 2. QuantizeFills, nearest tick ---------------------------------------
void quantize_fills_half_up() {
    scenario = "L8-2 QuantizeFills books the nearest tick";
    // A sell limit whose nearest tick sits below its level keeps limit-or-
    // better: the protection cap is the tick on the order's own side.
    check_rows(grid_spec("", NativePriceGrid::QuantizeFills), {
        {"h-market",     tx(1, "market"),                100.10, 100.00},
        {"h-buy-limit",  limit(1, 99.80, "buy-limit"),     99.80,  99.75},
        {"h-sell-limit", limit(-1, 100.30, "sell-limit"), 100.30, 100.50},
        {"h-buy-stop",   stop(1, 100.30, "buy-stop"),     100.30, 100.25},
        {"h-sell-stop",  stop(-1, 99.70, "sell-stop"),     99.70,  99.75},
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
        {"d-buy-market",  tx(1, "market"),                100.10, 100.25},
        {"d-sell-market", tx(-1, "market"),               100.10, 100.00},
        {"d-buy-limit",   limit(1, 99.80, "buy-limit"),     99.80,  99.75},
        {"d-sell-limit",  limit(-1, 100.30, "sell-limit"), 100.30, 100.50},
        {"d-buy-stop",    stop(1, 100.30, "buy-stop"),     100.30, 100.50},
        {"d-sell-stop",   stop(-1, 99.70, "sell-stop"),     99.70,  99.50},
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
    // R7: the le boundary is the last price that still rounds to 100.50; the
    // exact half tick 100.625 rounds away from zero to 100.75 and lies outside.
    CHECK(same_bits(native_matching::grid_region_threshold(100.50, true, half),
                    std::nextafter(100.625, 0.0)));
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
    // Pinned on this tree, not on clean main: native_run_spec_digest is exactly
    // the consumer's run-spec fold and nothing else, so unlike a raw
    // continuation hash (which folds the installed zone data's content) it is
    // the same number on every machine. The constant guards the
    // fold's field list and order; the neutrality claim is the equalities.
    constexpr std::uint64_t kSpecDigest = 3103595961916934085ULL;
    CHECK(native_run_spec_digest(spec("g-empty")) == kSpecDigest);
    // Spelling both grid fields out at their defaults folds nothing new, and a
    // rounding policy alone is not a behaviour: an unset grid folds nothing.
    CHECK(native_run_spec_digest(grid_spec("g-empty", NativePriceGrid::None)) == kSpecDigest);
    CHECK(native_run_spec_digest(grid_spec("g-empty", NativePriceGrid::None,
                                           NativeGridRounding::Directional)) == kSpecDigest);
    const auto half_digest =
        native_run_spec_digest(grid_spec("g-empty", NativePriceGrid::QuantizeFills));
    const auto directional_digest = native_run_spec_digest(
        grid_spec("g-empty", NativePriceGrid::QuantizeFills, NativeGridRounding::Directional));
    const auto triggers_digest = native_run_spec_digest(
        grid_spec("g-empty", NativePriceGrid::QuantizeFillsAndTriggers));
    CHECK(half_digest != kSpecDigest);
    CHECK(half_digest != directional_digest);
    CHECK(half_digest != triggers_digest);
    CHECK(directional_digest != triggers_digest);

    // The same three facts at run level, compared in process so no constant is
    // needed: an opted-in grid moves the continuation identity, a defaulted one
    // leaves it exactly where an unstated grid did.
    const auto clean = empty_run_hash(spec("g-empty"));
    CHECK(empty_run_hash(grid_spec("g-empty", NativePriceGrid::None,
                                   NativeGridRounding::Directional)) == clean);
    const auto half = empty_run_hash(grid_spec("g-empty", NativePriceGrid::QuantizeFills));
    const auto directional = empty_run_hash(
        grid_spec("g-empty", NativePriceGrid::QuantizeFills, NativeGridRounding::Directional));
    const auto triggers = empty_run_hash(
        grid_spec("g-empty", NativePriceGrid::QuantizeFillsAndTriggers));
    CHECK(half != clean);
    CHECK(half != directional);
    CHECK(half != triggers);
}

// --- 7. L8b: the activation rule under the grid ----------------------------
// Ruling: under QuantizeFillsAndTriggers the tick-quantized print IS the
// reached price. The matcher's verdict is authoritative and the core
// re-validates every activation (stop, stop-limit, trail arm, trail stop) on
// the same grid arithmetic, so a hit the matcher reports is never refused; a
// limit has no core re-validation (its gate is limit-or-better on the resolved
// price, which the grid's cap keeps). The fill books where the mode already
// says a fill books. None and QuantizeFills keep the raw compare, and their
// runs are pinned below by event count and report digest.
struct Fnv1a {
    std::uint64_t h = 1469598103934665603ULL;
    void bytes(const void* data, std::size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    }
    void u(std::uint64_t v) { bytes(&v, sizeof v); }
    void i(std::int64_t v) { bytes(&v, sizeof v); }
    void d(double v) { bytes(&v, sizeof v); }
};

// The trade rows and counts of a run, machine-independent: no timing, no
// padding, no continuation hash (which folds the installed zone data).
std::uint64_t report_digest(const BacktestEngine& engine) {
    ReportC c{};
    engine.fill_report(&c);
    Fnv1a f;
    f.i(c.total_trades);
    f.i(c.trades_len);
    f.d(c.net_profit);
    for (int i = 0; i < c.trades_len; ++i) {
        const TradeC& t = c.trades[i];
        f.i(t.entry_time); f.i(t.exit_time);
        f.d(t.entry_price); f.d(t.exit_price);
        f.d(t.pnl); f.d(t.qty);
        f.i(t.is_long);
        f.i(t.entry_bar_index); f.i(t.exit_bar_index); f.i(t.open_at_end);
    }
    f.i(c.input_bars_processed);
    f.i(c.script_bars_processed);
    BacktestEngine::free_report(&c);
    return f.h;
}

struct Activation {
    no::ActivationKind kind;
    double reached;
    double best_after;  // TrailTrack / TrailActive best, NaN otherwise
};

struct MultiRun {
    std::vector<double> raw;
    std::vector<double> resolved;
    std::vector<Activation> activations;
    std::size_t events = 0;
    std::uint64_t digest = 0;
    int trades = 0;
    std::vector<double> exit_prices;
    std::vector<int> exit_bars;
    std::string error;
};

// A host driven bar by bar: `on_bar(host, bar, index)` runs at each bar's
// calculation, after the bar's own matching.
struct MultiHost final : NativeStrategyHost {
    std::function<void(MultiHost&, const Bar&, int)> on_bar;
    int index = -1;
    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ++index;
        if (on_bar) on_bar(*this, bar, index);
    }
    // The entry fill price once the first execution has been applied.
    std::optional<double> first_fill() const {
        for (const auto& row : native_events(0)) {
            if (!row.command) continue;
            if (const auto* e = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
                return e->resolved_price;
            }
        }
        return std::nullopt;
    }
};

MultiRun execute_bars(const NativeRunSpec& s, const std::vector<Bar>& bars,
                      std::function<void(MultiHost&, const Bar&, int)> on_bar) {
    MultiHost h;
    h.on_bar = std::move(on_bar);
    MultiRun out;
    if (h.configure_native(s).status != NativeSetupStatus::Applied) {
        CHECK(false);
        return out;
    }
    h.run(bars.data(), (int)bars.size());
    const auto events = h.native_events(0);
    out.events = events.size();
    for (const auto& row : events) {
        if (!row.command) continue;
        if (const auto* e = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
            out.raw.push_back(e->raw_price);
            out.resolved.push_back(e->resolved_price);
        } else if (const auto* a = std::get_if<no::ActivatedEvent>(&*row.command)) {
            Activation act{a->kind, a->reached_price, kNaN};
            if (const auto* track = std::get_if<no::TrailTrack>(&a->after)) act.best_after = track->best;
            if (const auto* active = std::get_if<no::TrailActive>(&a->after)) {
                act.best_after = active->best_at_trigger;
            }
            out.activations.push_back(act);
        }
    }
    out.trades = h.trade_count();
    for (int i = 0; i < out.trades; ++i) {
        out.exit_prices.push_back(h.get_trade(i).exit_price);
        out.exit_bars.push_back(h.get_trade(i).exit_bar_index);
    }
    out.digest = report_digest(h);
    out.error = h.last_error();
    return out;
}

// Ladder equality up to the last bits either spelling (k * tick, k / n) may
// carry: the grid's own rounding decides which one a fill books.
bool near_tick(double actual, double expected, double tick) {
    if (std::abs(actual - expected) <= tick * 1e-6) return true;
    std::printf("  actual=%.17g expected=%.17g\n", actual, expected);
    return false;
}

Bar flat(double price, int i) { return Bar{price, price, price, price, 1, T + i * 60000LL}; }
Bar make_bar(double o, double h, double l, double c, int i) { return Bar{o, h, l, c, 1, T + i * 60000LL}; }

struct Mode {
    const char* name;
    NativePriceGrid grid;
    NativeGridRounding rounding;
};
constexpr Mode kNone{"none", NativePriceGrid::None, NativeGridRounding::HalfUp};
constexpr Mode kFills{"fills", NativePriceGrid::QuantizeFills, NativeGridRounding::HalfUp};
constexpr Mode kHalf{"half", NativePriceGrid::QuantizeFillsAndTriggers, NativeGridRounding::HalfUp};
constexpr Mode kDir{"dir", NativePriceGrid::QuantizeFillsAndTriggers, NativeGridRounding::Directional};

NativeRunSpec mode_spec(const char* key, const Mode& mode, double tick) {
    auto s = grid_spec(key, mode.grid, mode.rounding);
    s.price_tick = tick;
    return s;
}

// 7a. R7's reproducer: buy stop 100.50, tick 0.25, the bar opens at 100.40,
// whose nearest tick is 100.50. Under the trigger mode the stop activates at
// the open and books the gapped open on the ladder, 100.50; None and
// QuantizeFills never reach it (raw 100.40 < 100.50) and stay error-free.
void activation_reproducer() {
    scenario = "L8b-1 the B1 reproducer fills";
    const Bar bar{100.40, 100.45, 100.00, 100.10, 1, T};
    CHECK(same_bits(native_matching::grid_round_half_up(bar.open, 0.25), 100.50));
    for (const Mode* mode : {&kNone, &kFills}) {
        const auto out = execute(mode_spec("b1", *mode, 0.25), stop(1, 100.50, "buy-stop"), bar);
        CHECK(out.error.empty());
        CHECK(out.resolved.empty());
    }
    for (const Mode* mode : {&kHalf, &kDir}) {
        const auto out = execute(mode_spec("b1", *mode, 0.25), stop(1, 100.50, "buy-stop"), bar);
        CHECK(out.error.empty());
        CHECK(out.resolved.size() == 1);
        if (out.resolved.size() != 1) continue;
        // The raw print is a fact of the path; the fill is the gapped open on
        // the ladder (nearest tick, and the adverse tick, are both 100.50).
        CHECK(same_bits(out.raw[0], 100.40));
        CHECK(same_bits(out.resolved[0], 100.50));
    }
    // The activation records the quantized print as its reached price.
    const auto both = execute_bars(mode_spec("b1", kHalf, 0.25), {bar},
        [](MultiHost& h, const Bar&, int i) {
            if (i == 0) CHECK(h.submit(stop(1, 100.50, "buy-stop")).status == no::SubmitStatus::Accepted);
        });
    CHECK(both.error.empty());
    // Submitted at bar 0's calculation, the stop needs a later point: give it one.
    const auto later = execute_bars(mode_spec("b1", kHalf, 0.25),
        {flat(100.00, 0), make_bar(bar.open, bar.high, bar.low, bar.close, 1)},
        [](MultiHost& h, const Bar&, int i) {
            if (i == 0) CHECK(h.submit(stop(1, 100.50, "buy-stop")).status == no::SubmitStatus::Accepted);
        });
    CHECK(later.error.empty());
    CHECK(later.activations.size() == 1);
    if (later.activations.size() == 1) {
        CHECK(later.activations[0].kind == no::ActivationKind::Stop);
        CHECK(same_bits(later.activations[0].reached, 100.50));
    }
    CHECK(later.resolved.size() == 1);
    if (later.resolved.size() == 1) CHECK(same_bits(later.resolved[0], 100.50));
}

// 7b. One case per trigger kind, on binary and decimal ticks. Every level is
// the ladder point k / n for a decimal tick (the double a decimal literal
// parses to) and k * tick for a binary one; the discriminating print is 0.4
// of a tick short of the level on the raw side, so its nearest tick and its
// enclosing tick on the region's side are both the level while the raw
// compare never reaches it. Bar 0 is the placement bar (a market entry where
// a position is needed), the request rests from the next bar on.
struct Tick {
    double tick;
    double level;   // the ladder point near 100.50
};

std::vector<Tick> ticks() {
    return {
        {0.25, 402 * 0.25},
        {0.03125, 3216 * 0.03125},
        {0.01, 10050 / 100.0},
        {1e-5, 10050000 / 100000.0},
    };
}

void per_kind_matrix() {
    scenario = "L8b-2 one rule per trigger kind, binary and decimal ticks";
    Fnv1a none_pin;  // every None run's event count and report digest
    for (const Tick& g : ticks()) {
        const double t = g.tick;
        const double L = g.level;
        CHECK(!std::isnan(native_matching::grid_exact_index(L, t)));
        const double below = L - 0.4 * t;  // reads L on the ladder, short of L raw
        const double above = L + 0.4 * t;  // reads L on the ladder, past L raw
        CHECK(same_bits(native_matching::grid_round_half_up(below, t),
                        std::round(below / t) * t));
        CHECK(std::round(below / t) == std::round(L / t));
        CHECK(std::round(above / t) == std::round(L / t));
        char key[64];

        // (a) buy stop at L; the bar is flat at `below`.
        for (const Mode* mode : {&kNone, &kFills, &kHalf, &kDir}) {
            std::snprintf(key, sizeof key, "stop-%s-%g", mode->name, t);
            const auto out = execute_bars(mode_spec(key, *mode, t), {flat(100.0, 0), flat(below, 1)},
                [&](MultiHost& h, const Bar&, int i) {
                    if (i == 0) CHECK(h.submit(stop(1, L, "buy-stop")).status == no::SubmitStatus::Accepted);
                });
            CHECK(out.error.empty());
            const bool triggers = mode->grid == NativePriceGrid::QuantizeFillsAndTriggers;
            CHECK(out.resolved.size() == (triggers ? 1u : 0u));
            if (triggers && out.resolved.size() == 1) {
                CHECK(same_bits(out.raw[0], below));
                CHECK(near_tick(out.resolved[0], L, t));
                CHECK(out.activations.size() == 1);
                if (out.activations.size() == 1) CHECK(near_tick(out.activations[0].reached, L, t));
            }
            if (mode == &kNone) { none_pin.u(out.events); none_pin.u(out.digest); }
        }

        // (b) buy stop-limit: stop L, limit two ticks above; flat at `below`.
        for (const Mode* mode : {&kNone, &kFills, &kHalf, &kDir}) {
            std::snprintf(key, sizeof key, "stoplimit-%s-%g", mode->name, t);
            auto r = tx(1, "buy-stop-limit");
            r.trigger = no::StopLimit{L, L + 2 * t};
            const auto out = execute_bars(mode_spec(key, *mode, t), {flat(100.0, 0), flat(below, 1)},
                [&](MultiHost& h, const Bar&, int i) {
                    if (i == 0) CHECK(h.submit(r).status == no::SubmitStatus::Accepted);
                });
            CHECK(out.error.empty());
            const bool triggers = mode->grid == NativePriceGrid::QuantizeFillsAndTriggers;
            CHECK(out.resolved.size() == (triggers ? 1u : 0u));
            if (triggers && out.resolved.size() == 1) {
                CHECK(same_bits(out.raw[0], below));
                // The stop activates on the quantized print; the limit leg then
                // books the print on the ladder: nearest under HalfUp, the
                // buyer's favourable tick (one below) under Directional.
                CHECK(near_tick(out.resolved[0], mode == &kDir ? L - t : L, t));
                CHECK(out.activations.size() == 1);
                if (out.activations.size() == 1) {
                    CHECK(out.activations[0].kind == no::ActivationKind::StopLimit);
                    CHECK(near_tick(out.activations[0].reached, L, t));
                }
            }
            if (mode == &kNone) { none_pin.u(out.events); none_pin.u(out.digest); }
        }

        // (c) buy limit at L; the bar is flat at `above`.
        for (const Mode* mode : {&kNone, &kFills, &kHalf, &kDir}) {
            std::snprintf(key, sizeof key, "limit-%s-%g", mode->name, t);
            const auto out = execute_bars(mode_spec(key, *mode, t), {flat(101.0, 0), flat(above, 1)},
                [&](MultiHost& h, const Bar&, int i) {
                    if (i == 0) CHECK(h.submit(limit(1, L, "buy-limit")).status == no::SubmitStatus::Accepted);
                });
            CHECK(out.error.empty());
            const bool triggers = mode->grid == NativePriceGrid::QuantizeFillsAndTriggers;
            CHECK(out.resolved.size() == (triggers ? 1u : 0u));
            if (triggers && out.resolved.size() == 1) {
                CHECK(same_bits(out.raw[0], above));
                // Limit-or-better on the ladder: the print's nearest tick and
                // its tick on the buyer's side are both the level, which is
                // also its own cap.
                CHECK(near_tick(out.resolved[0], L, t));
                CHECK(out.resolved[0] <= L);
            }
            CHECK(out.activations.empty());  // a limit has no activation
            if (mode == &kNone) { none_pin.u(out.events); none_pin.u(out.digest); }
        }

        // (d) sell trail with an arm price at L and a one-tick offset, on a
        // long: the arm bar opens at `below` (arms on the ladder, best L) and
        // falls three ticks, through the stop at L - t.
        for (const Mode* mode : {&kNone, &kFills, &kHalf, &kDir}) {
            std::snprintf(key, sizeof key, "trailarm-%s-%g", mode->name, t);
            const std::vector<Bar> bars = {
                flat(100.0, 0), flat(100.0, 1),
                make_bar(below, below, below - 3 * t, below - 3 * t, 2),
            };
            const auto out = execute_bars(mode_spec(key, *mode, t), bars,
                [&](MultiHost& h, const Bar&, int i) {
                    if (i == 0) CHECK(h.submit(tx(1, "long")).status == no::SubmitStatus::Accepted);
                    if (i == 1) {
                        auto r = tx(-1, "sell-trail");
                        r.trigger = no::Trail{t, L};
                        CHECK(h.submit(r).status == no::SubmitStatus::Accepted);
                    }
                });
            CHECK(out.error.empty());
            const bool triggers = mode->grid == NativePriceGrid::QuantizeFillsAndTriggers;
            CHECK(out.trades == (triggers ? 1 : 0));
            if (triggers && out.trades == 1) {
                CHECK(near_tick(out.exit_prices[0], L - t, t));
                CHECK(out.exit_bars[0] == 2);
                CHECK(out.activations.size() == 2);
                if (out.activations.size() == 2) {
                    CHECK(out.activations[0].kind == no::ActivationKind::TrailArm);
                    CHECK(near_tick(out.activations[0].reached, L, t));
                    CHECK(near_tick(out.activations[0].best_after, L, t));
                    CHECK(out.activations[1].kind == no::ActivationKind::TrailTrigger);
                    CHECK(near_tick(out.activations[1].reached, L - t, t));
                }
            }
            if (mode == &kNone) { none_pin.u(out.events); none_pin.u(out.digest); }
        }

        // (e) sell trail stop: armed at L + t (best on the ladder), one-tick
        // offset, so the stop is L; the next bar is flat at `above`.
        for (const Mode* mode : {&kNone, &kFills, &kHalf, &kDir}) {
            std::snprintf(key, sizeof key, "trailstop-%s-%g", mode->name, t);
            const std::vector<Bar> bars = {
                flat(100.0, 0), flat(100.0, 1), flat(L + t, 2), flat(above, 3),
            };
            const auto out = execute_bars(mode_spec(key, *mode, t), bars,
                [&](MultiHost& h, const Bar&, int i) {
                    if (i == 0) CHECK(h.submit(tx(1, "long")).status == no::SubmitStatus::Accepted);
                    if (i == 1) {
                        auto r = tx(-1, "sell-trail");
                        r.trigger = no::Trail{t, std::nullopt};
                        CHECK(h.submit(r).status == no::SubmitStatus::Accepted);
                    }
                });
            CHECK(out.error.empty());
            const bool triggers = mode->grid == NativePriceGrid::QuantizeFillsAndTriggers;
            CHECK(out.trades == (triggers ? 1 : 0));
            CHECK(!out.activations.empty());
            if (!out.activations.empty()) {
                CHECK(out.activations[0].kind == no::ActivationKind::TrailArm);
                CHECK(near_tick(out.activations[0].best_after, L + t, t));
            }
            if (triggers && out.trades == 1) {
                CHECK(near_tick(out.exit_prices[0], L, t));
                CHECK(out.exit_bars[0] == 3);
                CHECK(out.activations.size() == 2);
                if (out.activations.size() == 2) {
                    CHECK(out.activations[1].kind == no::ActivationKind::TrailTrigger);
                    CHECK(near_tick(out.activations[1].reached, L, t));
                }
            }
            if (mode == &kNone) { none_pin.u(out.events); none_pin.u(out.digest); }
        }

        // (f) zero-offset ride: armed at L, then a bar 0.4 ticks below it and
        // one 0.6 ticks below. Raw, any move strictly below L exits (None at
        // the raw print, QuantizeFills at its nearest tick); on the HalfUp
        // ladder the 0.4-tick print still reads L and the ride holds until the
        // 0.6-tick print reads L - t; the Directional excursion of the first
        // print already reaches L - t.
        for (const Mode* mode : {&kNone, &kFills, &kHalf, &kDir}) {
            std::snprintf(key, sizeof key, "trailzero-%s-%g", mode->name, t);
            const std::vector<Bar> bars = {
                flat(100.0, 0), flat(100.0, 1), flat(L, 2), flat(L - 0.4 * t, 3), flat(L - 0.6 * t, 4),
            };
            const auto out = execute_bars(mode_spec(key, *mode, t), bars,
                [&](MultiHost& h, const Bar&, int i) {
                    if (i == 0) CHECK(h.submit(tx(1, "long")).status == no::SubmitStatus::Accepted);
                    if (i == 1) {
                        auto r = tx(-1, "sell-ride");
                        r.trigger = no::Trail{0.0, std::nullopt};
                        CHECK(h.submit(r).status == no::SubmitStatus::Accepted);
                    }
                });
            CHECK(out.error.empty());
            CHECK(out.trades == 1);
            if (out.trades != 1) continue;
            if (mode == &kNone) {
                CHECK(same_bits(out.exit_prices[0], L - 0.4 * t));
                CHECK(out.exit_bars[0] == 3);
            } else if (mode == &kFills) {
                CHECK(near_tick(out.exit_prices[0], L, t));
                CHECK(out.exit_bars[0] == 3);
            } else if (mode == &kHalf) {
                CHECK(near_tick(out.exit_prices[0], L - t, t));
                CHECK(out.exit_bars[0] == 4);
            } else {
                CHECK(near_tick(out.exit_prices[0], L - t, t));
                CHECK(out.exit_bars[0] == 3);
            }
            if (mode == &kNone) { none_pin.u(out.events); none_pin.u(out.digest); }
        }
    }
    // Every None run above, harvested on 764a323 (the lane base) with
    // -DPINEFORGE_L8B_HARVEST: the raw rule is byte-identical here.
#ifdef PINEFORGE_L8B_HARVEST
    std::printf("kMatrixNonePin = %lluULL\n", (unsigned long long)none_pin.h);
#else
    constexpr std::uint64_t kMatrixNonePin = 11271765375752119154ULL;
    CHECK(none_pin.h == kMatrixNonePin);
#endif
}

// 7c. R7's trial aborted nine of the 76 pinned NYSE:F / NASDAQ:AAPL zero-offset
// trail tapes (tests/zero_offset_trail_rides_cases.inc) under the trigger
// mode. Run here natively: a market entry at bar 0, a zero-offset trail armed
// at entry +/- trail_points ticks submitted at bar 1's calculation, tick 0.01,
// QuantizeFillsAndTriggers / HalfUp. Every tape must run to one on-ladder exit;
// TradingView's own exits are per-kind (B2) and are reported, not asserted.
// Adapter-only, not run: the kAlongside tapes (the activation derives from an
// entry that has not filled: an anchored leg), and the probe's per-bar
// re-issue with a new activation (kProbeReissue is armed once, from bar 1).
namespace tapes {
enum class Mode { kLive, kOnce, kAlongside, kProbeReissue };
Bar mk(double o, double h, double l, double c, int64_t ts) {
    Bar b{}; b.open = o; b.high = h; b.low = l; b.close = c; b.volume = 1000.0; b.timestamp = ts;
    return b;
}
struct TapeCase {
    const char* name;
    const char* symbol;
    const char* feed;
    const char* signal_utc;
    bool is_long;
    double trail_points;
    double trail_offset;
    Mode mode;
    double tv_exit_price;
    int tv_exit_bar;
    std::vector<Bar> bars;
};
#include "zero_offset_trail_rides_cases.inc"
}  // namespace tapes

void zero_offset_tapes_natively() {
    scenario = "L8b-3 the zero-offset trail tapes run natively under the grid";
    const double tick = 0.01;
    int run = 0, skipped = 0, tv_agree = 0;
    Fnv1a none_pin;
    for (const tapes::TapeCase& c : tapes::kCases) {
        if (c.mode == tapes::Mode::kAlongside) { ++skipped; continue; }
        ++run;
        for (const Mode* mode : {&kNone, &kHalf}) {
            auto s = mode_spec(c.name, *mode, tick);
            s.initial_capital = 1000000;
            const auto out = execute_bars(s, c.bars, [&](MultiHost& h, const Bar& bar, int i) {
                if (i == 0) {
                    CHECK(h.submit(tx(c.is_long ? 1 : -1, "entry")).status == no::SubmitStatus::Accepted);
                }
                if (i == 1) {
                    const auto entry = h.first_fill();
                    CHECK(entry.has_value());
                    if (!entry) return;
                    const double points = c.mode == tapes::Mode::kProbeReissue
                        ? bar.close * 0.015 / tick : c.trail_points;
                    const double arm = c.is_long ? *entry + points * tick : *entry - points * tick;
                    auto r = tx(c.is_long ? -1 : 1, "trail");
                    r.trigger = no::Trail{c.trail_offset * tick, arm};
                    CHECK(h.submit(r).status == no::SubmitStatus::Accepted);
                }
            });
            if (!out.error.empty()) std::printf("  tape %s [%s]: %s\n", c.name, mode->name, out.error.c_str());
            CHECK(out.error.empty());
            if (mode == &kNone) { none_pin.u(out.events); none_pin.u(out.digest); continue; }
            // The entry fill is the bar-1 open on the ladder, exactly as the
            // tapes report it (11.425 -> 11.43, 12.005 -> 12.01, 198.695 -> 198.70).
            CHECK(out.raw.size() >= 1);
            if (!out.raw.empty()) {
                CHECK(same_bits(out.raw[0], c.bars[1].open));
                CHECK(same_bits(out.resolved[0], native_matching::grid_round_half_up(c.bars[1].open, tick)));
            }
            CHECK(out.trades == 1);
            if (out.trades != 1) continue;
            const double exit = out.exit_prices[0];
            CHECK(near_tick(exit, std::round(exit / tick) * tick, tick));
            if (std::abs(exit - c.tv_exit_price) <= 1e-9 && out.exit_bars[0] == c.tv_exit_bar) ++tv_agree;
        }
    }
    std::printf("  tapes run=%d skipped(kAlongside)=%d tv-agree=%d\n", run, skipped, tv_agree);
    CHECK(run == 73);
    CHECK(skipped == 2);
#ifdef PINEFORGE_L8B_HARVEST
    std::printf("kTapesNonePin = %lluULL\n", (unsigned long long)none_pin.h);
#else
    constexpr std::uint64_t kTapesNonePin = 6765782154696486296ULL;
    CHECK(none_pin.h == kTapesNonePin);
#endif
}

// 7d. The POOC short-close panels (tests/test_pooc_short_close_tick_l4d.cpp,
// round-16 Hariss F): a short entered at a bar's close, a stop and a limit
// re-issued at every close, tested against that close's print. Natively:
// close_execution AfterCalculation, tick 0.01, the two legs submitted at bar
// 1's calculation and replaced at bars 2 and 3. Panels 1 and 2 are the B1
// shape (11.695 reads 11.70 and reaches the 11.698693 stop; 12.495 reads 12.50
// and reaches 12.496973); panel 0's 11.575 reads 11.58 and so skips the
// 11.576782 limit the raw compare reaches (the grid's skip, TradingView's row
// 114), then fills the 11.583440 limit at 11.58. Two facts stay adapter-side:
// the unbound "other parent" variant (bracket ownership), and the bar of the
// exit -- the kernel excludes a request's own birth print, so a leg reissued at
// bar k's calculation is first tested at bar k + 1's open, which prints the
// same 11.695 / 12.495 / 11.575; TradingView books it at bar k's close. The
// panels therefore carry one more bar (flat at the last close) and expect
// TradingView's price one bar later. Before L8b the trigger mode aborted
// panels 1 and 2 at that open.
struct Panel {
    double stops[3];
    double limits[3];
    double expected_exit;
    int expected_bar;  // TradingView's exit bar + 1, see above
    std::vector<Bar> bars;
};

std::vector<Panel> panels() {
    return {
        {{11.747887, 11.746609, 11.743280}, {11.574226, 11.576782, 11.583440}, 11.58, 4, {
            make_bar(11.69, 11.69, 11.69, 11.69, 0), make_bar(11.615, 11.615, 11.595, 11.595, 1),
            make_bar(11.595, 11.595, 11.575, 11.575, 2), make_bar(11.58, 11.58, 11.575, 11.575, 3),
            flat(11.575, 4)}},
        {{11.700131, 11.698693, 11.69900}, {11.519738, 11.522614, 11.52200}, 11.70, 3, {
            make_bar(11.64, 11.64, 11.64, 11.64, 0), make_bar(11.685, 11.70, 11.685, 11.69, 1),
            make_bar(11.69, 11.70, 11.68, 11.695, 2), make_bar(11.695, 11.695, 11.66, 11.665, 3),
            flat(11.665, 4)}},
        {{12.500586, 12.496973, 12.49700}, {12.228828, 12.236054, 12.23600}, 12.50, 3, {
            make_bar(12.41, 12.41, 12.41, 12.41, 0), make_bar(12.46, 12.48, 12.45, 12.48, 1),
            make_bar(12.48, 12.50, 12.48, 12.495, 2), make_bar(12.50, 12.515, 12.47, 12.48, 3),
            flat(12.48, 4)}},
    };
}

void pooc_panels_natively() {
    scenario = "L8b-4 the POOC short-close panels run natively under the grid";
    const double tick = 0.01;
    Fnv1a none_pin;
    int n = 0;
    for (const Panel& p : panels()) {
        const int panel = n++;
        for (const Mode* mode : {&kNone, &kHalf}) {
            char key[32];
            std::snprintf(key, sizeof key, "pooc-%d-%s", panel, mode->name);
            auto s = mode_spec(key, *mode, tick);
            s.close_execution = NativeCloseExecution::AfterCalculation;
            std::optional<no::RequestHandle> stop_h, limit_h;
            const auto out = execute_bars(s, p.bars, [&](MultiHost& h, const Bar&, int i) {
                if (i == 0) {
                    CHECK(h.submit(tx(-1, "short")).status == no::SubmitStatus::Accepted);
                    return;
                }
                if (h.trade_count() > 0) {
                    if (stop_h) { h.cancel(*stop_h); stop_h.reset(); }
                    if (limit_h) { h.cancel(*limit_h); limit_h.reset(); }
                    return;
                }
                const int leg = i - 1 < 2 ? i - 1 : 2;  // old, new, then next
                const auto stop_r = stop(1, p.stops[leg], "X-stop");
                const auto limit_r = limit(1, p.limits[leg], "X-limit");
                if (!stop_h) {
                    const auto a = h.submit(stop_r);
                    CHECK(a.status == no::SubmitStatus::Accepted);
                    stop_h = a.handle;
                    const auto b = h.submit(limit_r);
                    CHECK(b.status == no::SubmitStatus::Accepted);
                    limit_h = b.handle;
                } else {
                    const auto a = h.replace(*stop_h, stop_r);
                    CHECK(a.status == no::ReplaceStatus::Replaced);
                    if (a.successor) stop_h = a.successor;
                    const auto b = h.replace(*limit_h, limit_r);
                    CHECK(b.status == no::ReplaceStatus::Replaced);
                    if (b.successor) limit_h = b.successor;
                }
            });
            if (!out.error.empty()) std::printf("  panel %d [%s]: %s\n", panel, mode->name, out.error.c_str());
            CHECK(out.error.empty());
            if (mode == &kNone) { none_pin.u(out.events); none_pin.u(out.digest); continue; }
            CHECK(out.trades == 1);
            if (out.trades != 1) continue;
            CHECK(near_tick(out.exit_prices[0], p.expected_exit, tick));
            CHECK(out.exit_bars[0] == p.expected_bar);
            // The leg's activation (a stop on panels 1 and 2) records the
            // quantized print; panel 0's limit has none.
            CHECK(out.activations.size() == (panel == 0 ? 0u : 1u));
            if (out.activations.size() == 1) CHECK(near_tick(out.activations[0].reached, p.expected_exit, tick));
            // The entry is the bar-0 close itself, executed after that calculation.
            CHECK(!out.resolved.empty());
            if (!out.resolved.empty()) CHECK(same_bits(out.resolved[0], p.bars[0].close));
        }
    }
#ifdef PINEFORGE_L8B_HARVEST
    std::printf("kPoocNonePin = %lluULL\n", (unsigned long long)none_pin.h);
#else
    constexpr std::uint64_t kPoocNonePin = 16168676078914724703ULL;
    CHECK(none_pin.h == kPoocNonePin);
#endif
}
} // namespace

int main() {
    grid_none_is_clean_main();
    quantize_fills_half_up();
    quantize_fills_directional();
    quantize_triggers();
    grid_requires_a_tick();
    hash_folds_only_when_set();
    activation_reproducer();
    per_kind_matrix();
    zero_offset_tapes_natively();
    pooc_panels_natively();
    std::printf("%s native price grid: %d checks, %d failures\n",
                failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
