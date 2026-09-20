// R5 lane L7b: the kernel's anchored bracket legs (native_order::FromOwnerFill)
// as first-class, hostable bracket children.
//
// Four generic, opt-in additions, each with a default that reproduces the
// pre-lane tree bit for bit:
//   1. the materialized level snaps onto the run's price tick ladder with a
//      per-anchor NativeAnchorRounding (Raw keeps fill + offset exactly);
//   2. a host hook, resolve_anchored_level, consulted once at the
//      materialization and free to restate the installed level;
//   3. an opt-in book visibility on the owner relation that arms,
//      NativeArmVisibility::PendingUntilArmed, under which an unarmed leg
//      is not a working order;
//   4. a measurement of which non-level facts resolve at the arm.
//
// Neutrality is pinned the portable way: the run-spec fold through
// native_run_spec_digest(), the event count and a digest over the finished
// report, all observed on the clean parent tree (engine main 3f6fd57) by
// compiling this same translation unit with -DPINEFORGE_L7B_HARVEST, which
// prints the observed values and stops. A raw native_continuation_hash()
// constant is not portable (it folds the machine's timezone resources), so
// every continuation comparison here is between two runs in this process.
#include <pineforge/native_c_api.h>
#include <pineforge/native_toolkit.hpp>

#include "native_current_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace r4_test;
namespace tk = pineforge::native_toolkit;

namespace {

// ── Portable pins (harvested on engine main 3f6fd57, see the header) ────
constexpr std::uint64_t kNeutralSpecDigest = 16941677776193786888ULL;
constexpr std::uint64_t kNeutralReportDigest = 7202933235328973332ULL;
constexpr std::size_t kNeutralEventCount = 72;

// A canonical one-minute slot label. The R4 harness's tape starts at
// 1700000000000 (not a minute slot; the Pine host labels slots leniently),
// so harness times are compared by bar index, (time - base) / 60000, which
// is what its rows pin anyway.
constexpr std::int64_t kT = 1700000040000LL;

Bar ohlc(int index, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, kT + static_cast<std::int64_t>(index) * 60000};
}

// The R4 bracket staircase (tests/test_adapter_brackets_relower.cpp, feed()):
// a rise past the bracket levels, a retrace deep enough to take a stop, a
// flat tail and a second-cycle entry ground. Every price sits on the 0.01
// grid and every level is a binary-exact multiple of it.
std::vector<Bar> staircase() {
    std::vector<Bar> bars;
    auto push = [&](double o, double h, double l, double c) {
        bars.push_back(ohlc(static_cast<int>(bars.size()), o, h, l, c));
    };
    push(100.0, 100.0, 100.0, 100.0);       // 0: entry placed
    push(100.0, 100.5,  99.5, 100.25);      // 1: fills @100
    push(100.25, 101.0, 100.0, 100.75);     // 2
    push(100.75, 102.0, 100.5, 101.75);     // 3
    push(101.75, 103.0, 101.5, 102.75);     // 4: past 102.5 / activation
    push(102.75, 104.0, 102.5, 103.75);     // 5
    push(103.75, 104.5, 103.0, 103.25);     // 6: extreme, then the retrace
    push(103.25, 103.5, 101.0, 101.25);     // 7: deep retrace
    push(101.25, 101.5,  98.0,  98.25);     // 8: through the 98.5 stop
    push(98.25,  99.0,  97.0,  97.5);       // 9: through the 97.5 limit-side
    push(97.5,   98.0,  97.0,  97.5);       // 10
    push(97.5,   99.5,  97.5,  99.25);      // 11: second-cycle entry ground
    push(99.25, 100.5,  99.0, 100.25);      // 12
    push(100.25, 103.5,  100.0, 103.0);     // 13: second cycle target
    push(103.0, 103.5, 102.5, 103.0);       // 14
    return bars;
}

// The R4 probe's account: 10000 initial, no fee, no slippage, a 0.01 tick,
// and the kernel recording the report so the equity metrics exist.
NativeRunSpec bracket_spec(const char* key, double tick = 0.01) {
    NativeRunSpec s;
    s.identity = {key, 1};
    s.input_tf = "1"; s.script_tf = "1";
    s.tickerid = "TEST:L7B"; s.timezone = "UTC"; s.session = "24x7";
    s.initial_capital = 10000; s.point_value = 1; s.account_fx = 1;
    s.price_tick = tick;
    s.fee_kind = NativeFeeKind::CashPerExecution; s.fee_value = 0;
    s.report_policy = NativeReportPolicy::KernelRecorded;
    return s;
}

// ── Report digest (the risk-limits witness's portable fold) ─────────────
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

struct Report {
    ReportC c{};
    explicit Report(const BacktestEngine& engine) { engine.fill_report(&c); }
    ~Report() { BacktestEngine::free_report(&c); }
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
};

std::uint64_t report_digest(const ReportC& report) {
    Fnv1a f;
    f.i(report.total_trades);
    f.i(report.trades_len);
    f.d(report.net_profit);
    for (int i = 0; i < report.trades_len; ++i) {
        const TradeC& t = report.trades[i];
        f.i(t.entry_time); f.i(t.exit_time);
        f.d(t.entry_price); f.d(t.exit_price);
        f.d(t.pnl); f.d(t.pnl_pct);
        f.i(t.is_long);
        f.d(t.max_runup); f.d(t.max_drawdown);
        f.d(t.qty); f.d(t.commission);
        f.i(t.entry_bar_index); f.i(t.exit_bar_index); f.i(t.open_at_end);
    }
    f.i(report.input_bars_processed);
    f.i(report.script_bars_processed);
    f.bytes(&report.metrics, sizeof report.metrics);
    f.i(report.broker_state_hash_len);
    for (std::int64_t i = 0; i < report.broker_state_hash_len; ++i)
        f.u(report.broker_state_hash[i]);
    return f.h;
}

bool same_bits(double actual, double expected) {
    if (std::memcmp(&actual, &expected, sizeof(double)) == 0) return true;
    std::printf("  actual=%.17g expected=%.17g\n", actual, expected);
    return false;
}

// ── Leg shapes ──────────────────────────────────────────────────────────
no::Request owner_close(const char* label) {
    return {no::Reduce{no::OwnerOpenedUnits{}}, label, "bracket"};
}

// The R4 bracket as anchored legs: the harness's absolute levels re-expressed
// as fill-relative tick offsets against the harvested entry fill of 100.0
// (102.5 = +250 ticks, 97.5 = -250, 98.5 = -150, 108.0 = +800).
tk::BracketSpec anchored_bracket(const no::RequestHandle& parent,
                                 double profit_ticks, double loss_ticks) {
    tk::BracketSpec spec_rows;
    spec_rows.parent = parent;
    auto take_profit = owner_close("tp");
    take_profit.trigger = no::Limit{0.0};
    take_profit.anchor = no::FromOwnerFill{profit_ticks, true};
    auto stop_loss = owner_close("sl");
    stop_loss.trigger = no::Stop{0.0};
    stop_loss.anchor = no::FromOwnerFill{-loss_ticks, true};
    spec_rows.take_profit = take_profit;
    spec_rows.stop_loss = stop_loss;
    return spec_rows;
}

const no::ArmedEvent* armed_for(const std::vector<no::ArmedEvent>& rows,
                                const no::RequestHandle& handle) {
    for (const auto& row : rows) {
        if (row.definition && row.definition->handle == handle) return &row;
    }
    return nullptr;
}

std::optional<double> installed_level(const no::Request& request) {
    if (const auto* limit = std::get_if<no::Limit>(&request.trigger)) return limit->price;
    if (const auto* stop = std::get_if<no::Stop>(&request.trigger)) return stop->price;
    if (const auto* trail = std::get_if<no::Trail>(&request.trigger)) return trail->arm_price;
    return std::nullopt;
}

// ── 0. The neutral population: a Raw, Working anchored bracket ──────────
// The R4 "from-entry stop+limit" shape driven through the pre-lane API only
// (no rounding, no hook, no visibility): the fills, the report and the spec
// fold stay exactly where the clean parent tree left them.
struct NeutralRun {
    std::size_t events = 0;
    std::uint64_t report = 0;
    std::uint64_t spec = 0;
    std::uint64_t continuation = 0;
    std::vector<no::ExecutionAppliedEvent> applied;
};

NeutralRun neutral_run(const char* key) {
    Host host;
    tk::BracketReceipt receipt;
    host.beginning = [&](Host& base) {
        const auto parent = put(base, tx(2.0, "L"));
        receipt = tk::submit_bracket(base, anchored_bracket(parent, 250.0, 250.0));
    };
    const auto s = bracket_spec(key);
    REQUIRE(host.configure_native(s).status == NativeSetupStatus::Applied);
    const auto bars = staircase();
    host.run(bars.data(), static_cast<int>(bars.size()));
    if (!host.last_error().empty()) std::printf("  run error: %s\n", host.last_error().c_str());
    completed(host);
    REQUIRE(receipt.take_profit && receipt.stop_loss);
    NeutralRun out;
    out.events = host.native_events(0).size();
    const Report report(host);
    out.report = report_digest(report.c);
    out.spec = native_run_spec_digest(s);
    out.continuation = host.native_continuation_hash();
    out.applied = events<no::ExecutionAppliedEvent>(host);
    return out;
}

#ifdef PINEFORGE_L7B_HARVEST

void harvest() {
    const auto run = neutral_run("l7b-neutral");
    std::printf("// harvested on this tree: paste over the pins\n");
    std::printf("constexpr std::uint64_t kNeutralSpecDigest = %lluULL;\n",
                static_cast<unsigned long long>(run.spec));
    std::printf("constexpr std::uint64_t kNeutralReportDigest = %lluULL;\n",
                static_cast<unsigned long long>(run.report));
    std::printf("constexpr std::size_t kNeutralEventCount = %zu;\n", run.events);
    std::printf("// applied rows: %zu\n", run.applied.size());
    for (const auto& row : run.applied) {
        std::printf("//   %s @ %.17g closed=%.17g opened=%.17g bar=%d\n",
                    row.request().label.c_str(), row.resolved_price, row.closed_units,
                    row.opened_units, row.cursor.point.interval_index);
    }
}

#else

void defaults_are_byte_identical() {
    const auto run = neutral_run("l7b-neutral");
    if (run.events != kNeutralEventCount) {
        std::printf("  neutral event count %zu, pinned %zu\n", run.events, kNeutralEventCount);
    }
    CHECK(run.events == kNeutralEventCount);
    if (run.report != kNeutralReportDigest) {
        std::printf("  neutral report digest %llu, pinned %llu\n",
                    static_cast<unsigned long long>(run.report),
                    static_cast<unsigned long long>(kNeutralReportDigest));
    }
    CHECK(run.report == kNeutralReportDigest);
    if (run.spec != kNeutralSpecDigest) {
        std::printf("  neutral spec digest %llu, pinned %llu\n",
                    static_cast<unsigned long long>(run.spec),
                    static_cast<unsigned long long>(kNeutralSpecDigest));
    }
    CHECK(run.spec == kNeutralSpecDigest);
    // The shape really ran: the entry filled at 100 and the limit leg took
    // the profit at 102.5 on bar 4, exactly the R4 "from-entry-bracket" row.
    REQUIRE(run.applied.size() == 2);
    CHECK(same_bits(run.applied[0].resolved_price, 100.0));
    CHECK(same_bits(run.applied[1].resolved_price, 102.5));
    CHECK(run.applied[1].cursor.point.interval_index == 4);

    // Spelling the defaults out is the same run: the same events, the same
    // report, the same continuation.
    Host stated;
    stated.beginning = [&](Host& base) {
        const auto parent = put(base, tx(2.0, "L"));
        auto bracket = anchored_bracket(parent, 250.0, 250.0);
        bracket.take_profit->anchor = no::FromOwnerFill{250.0, true, no::NativeAnchorRounding::Raw};
        bracket.stop_loss->anchor = no::FromOwnerFill{-250.0, true, no::NativeAnchorRounding::Raw};
        bracket.take_profit->owner = no::WaitForApplied{parent, no::NativeArmVisibility::Working};
        bracket.stop_loss->owner = no::WaitForApplied{parent, no::NativeArmVisibility::Working};
        tk::submit_bracket(base, bracket);
    };
    REQUIRE(stated.configure_native(bracket_spec("l7b-neutral")).status
            == NativeSetupStatus::Applied);
    const auto bars = staircase();
    stated.run(bars.data(), static_cast<int>(bars.size()));
    completed(stated);
    CHECK(stated.native_events(0).size() == run.events);
    const Report restated(stated);
    CHECK(report_digest(restated.c) == run.report);
    CHECK(stated.native_continuation_hash() == run.continuation);
}

// ── 1. Rounding snaps the materialized level onto the tick ladder ───────
// One flat entry at 100, a leg per rounding and trigger kind on a
// non-power-of-ten tick (0.03125 = 2^-5, so every ladder point is a
// binary-exact double and every expectation below is hand-computed).
struct LegProbe {
    const char* label;
    no::Trigger trigger;
    double offset;
    bool ticks;
    no::NativeAnchorRounding rounding;
    double expected;
};

void probe_rounding(bool long_side, const std::vector<LegProbe>& probes,
                              double tick, const char* key) {
    Host host;
    std::vector<no::RequestHandle> legs;
    host.beginning = [&](Host& base) {
        const auto parent = put(base, tx(long_side ? 1.0 : -1.0, "entry"));
        for (const auto& probe : probes) {
            auto leg = owner_close(probe.label);
            leg.trigger = probe.trigger;
            leg.owner = no::WaitForApplied{parent};
            leg.anchor = no::FromOwnerFill{probe.offset, probe.ticks, probe.rounding};
            legs.push_back(put(base, leg));
        }
    };
    REQUIRE(host.configure_native(bracket_spec(key, tick)).status == NativeSetupStatus::Applied);
    // Flat bars: the parent fills at 100 and no leg can reach its level, so
    // every leg is still working with its materialized level.
    const std::vector<Bar> bars = {ohlc(0, 100.0, 100.0, 100.0, 100.0),
                                   ohlc(1, 100.0, 100.0, 100.0, 100.0),
                                   ohlc(2, 100.0, 100.0, 100.0, 100.0)};
    host.run(bars.data(), static_cast<int>(bars.size()));
    completed(host);
    const auto armed = events<no::ArmedEvent>(host);
    REQUIRE(armed.size() == probes.size());
    for (std::size_t i = 0; i < probes.size(); ++i) {
        const auto* row = armed_for(armed, legs[i]);
        REQUIRE(row != nullptr);
        const auto level = installed_level(row->definition->request);
        REQUIRE(level.has_value());
        std::printf("    %s: ", probes[i].label);
        const bool ok = same_bits(*level, probes[i].expected);
        if (ok) std::printf("%.17g\n", *level);
        CHECK(ok);
        CHECK(std::holds_alternative<no::Absolute>(row->definition->request.anchor));
        // The live book reads the same absolute level.
        bool seen = false;
        for (const auto& working : host.native_working_requests()) {
            if (working.definition->handle != legs[i]) continue;
            seen = true;
            const auto live_level = installed_level(working.definition->request);
            CHECK(live_level && same_bits(*live_level, probes[i].expected));
        }
        CHECK(seen);
    }
}

void rounding_snaps_the_level() {
    using R = no::NativeAnchorRounding;
    // Long entry at 100: the legs sell. 100 / 0.03125 = 3200 exactly.
    //   +0.1 -> 100.1  = 3203.2 ticks: HalfUp 3203 (100.09375), sell limit /
    //   trail arm up 3204 (100.125), Raw untouched.
    //   -0.1 -> 99.9   = 3196.8 ticks: HalfUp 3197 (99.90625), sell stop down
    //   3196 (99.875).
    //   +3.5 ticks     = 3203.5: HalfUp ties away from zero -> 3204 (100.125).
    //   -3 ticks       = 3197 exactly (99.90625): every rounding keeps an
    //   on-ladder level, the directional nanotick guard included.
    probe_rounding(true, {
        {"limit-raw", no::Limit{0.0}, 0.1, false, R::Raw, 100.1},
        {"limit-half-up", no::Limit{0.0}, 0.1, false, R::HalfUp, 100.09375},
        {"limit-directional", no::Limit{0.0}, 0.1, false, R::Directional, 100.125},
        {"stop-raw", no::Stop{0.0}, -0.1, false, R::Raw, 99.9},
        {"stop-half-up", no::Stop{0.0}, -0.1, false, R::HalfUp, 99.90625},
        {"stop-directional", no::Stop{0.0}, -0.1, false, R::Directional, 99.875},
        {"trail-half-up", no::Trail{0.0625, 0.0}, 0.1, false, R::HalfUp, 100.09375},
        {"trail-directional", no::Trail{0.0625, 0.0}, 0.1, false, R::Directional, 100.125},
        {"limit-ticks-tie", no::Limit{0.0}, 3.5, true, R::HalfUp, 100.125},
        {"stop-ticks-on-grid", no::Stop{0.0}, -3.0, true, R::Directional, 99.90625},
        {"limit-ticks-on-grid", no::Limit{0.0}, 3.0, true, R::Directional, 100.09375},
    }, 0.03125, "l7b-rounding-long");
    // Short entry at 100: the legs buy, so Directional flips side: a buy
    // limit rounds down and a buy stop up.
    probe_rounding(false, {
        {"buy-limit-directional", no::Limit{0.0}, -0.1, false, R::Directional, 99.875},
        {"buy-limit-half-up", no::Limit{0.0}, -0.1, false, R::HalfUp, 99.90625},
        {"buy-stop-directional", no::Stop{0.0}, 0.1, false, R::Directional, 100.125},
        {"buy-trail-directional", no::Trail{0.0625, 0.0}, -0.1, false, R::Directional, 99.875},
    }, 0.03125, "l7b-rounding-short");
    // A decimal tick with a tick-spelled offset: -10 ticks of 0.01 is the
    // product 100 + (-10 * 0.01) raw, and the ladder point 9990 * 0.01 once
    // snapped (HalfUp and the sell-stop Directional agree on an on-grid
    // level).
    probe_rounding(true, {
        {"cent-raw", no::Stop{0.0}, -10.0, true, R::Raw, 100.0 + (-10.0 * 0.01)},
        {"cent-half-up", no::Stop{0.0}, -10.0, true, R::HalfUp, 9990.0 * 0.01},
        {"cent-directional", no::Stop{0.0}, -10.0, true, R::Directional, 9990.0 * 0.01},
    }, 0.01, "l7b-rounding-cent");
}

// A rounding needs a positive tick at acceptance, exactly like a tick
// spelling; an unknown rounding is refused rather than read as Raw.
void rounding_is_validated_at_acceptance() {
    Host host;
    no::SubmitResult half_up, unknown, raw;
    host.beginning = [&](Host& base) {
        const auto parent = put(base, tx(1.0, "entry"));
        auto leg = owner_close("leg");
        leg.trigger = no::Stop{0.0};
        leg.owner = no::WaitForApplied{parent};
        leg.anchor = no::FromOwnerFill{-0.1, false, no::NativeAnchorRounding::HalfUp};
        half_up = base.submit(leg);
        leg.anchor = no::FromOwnerFill{-0.1, false, static_cast<no::NativeAnchorRounding>(7)};
        unknown = base.submit(leg);
        leg.anchor = no::FromOwnerFill{-0.1, false, no::NativeAnchorRounding::Raw};
        raw = base.submit(leg);
    };
    // price_tick == 0 is the explicit unquantized run.
    run(host, bracket_spec("l7b-rounding-no-tick", 0.0), {100.0, 100.0});
    CHECK(half_up.status == no::SubmitStatus::Rejected);
    REQUIRE(half_up.reason.has_value());
    CHECK(*half_up.reason == no::RequestRejectReason::InvalidTrigger);
    CHECK(unknown.status == no::SubmitStatus::Rejected);
    REQUIRE(unknown.reason.has_value());
    CHECK(*unknown.reason == no::RequestRejectReason::InvalidTrigger);
    CHECK(raw.status == no::SubmitStatus::Accepted);
    completed(host);
}

// The rounding folds into the continuation identity only when it is set.
void rounding_folds_only_when_set() {
    auto probe = [](no::NativeAnchorRounding rounding) {
        Host host;
        host.beginning = [&](Host& base) {
            const auto parent = put(base, tx(1.0, "entry"));
            auto leg = owner_close("leg");
            leg.trigger = no::Stop{0.0};
            leg.owner = no::WaitForApplied{parent};
            leg.anchor = no::FromOwnerFill{-10.0, true, rounding};
            put(base, leg);
        };
        run(host, bracket_spec("l7b-rounding-fold"), {100.0});
        completed(host);
        return host.native_continuation_hash();
    };
    const auto raw = probe(no::NativeAnchorRounding::Raw);
    Host plain;
    plain.beginning = [&](Host& base) {
        const auto parent = put(base, tx(1.0, "entry"));
        auto leg = owner_close("leg");
        leg.trigger = no::Stop{0.0};
        leg.owner = no::WaitForApplied{parent};
        leg.anchor = no::FromOwnerFill{-10.0, true};
        put(base, leg);
    };
    run(plain, bracket_spec("l7b-rounding-fold"), {100.0});
    completed(plain);
    CHECK(plain.native_continuation_hash() == raw);
    CHECK(probe(no::NativeAnchorRounding::HalfUp) != raw);
    CHECK(probe(no::NativeAnchorRounding::Directional) != raw);
    CHECK(probe(no::NativeAnchorRounding::HalfUp) != probe(no::NativeAnchorRounding::Directional));
}

// ── 2. The arm hook restates the level, once, before the ArmedEvent ─────
// A host that answers resolve_anchored_level owns the installed level; a
// host that answers nullopt keeps the kernel level; a non-representable
// answer fails the run through the existing PreparationError path.
struct HookHost final : Host {
    std::function<std::optional<double>(const NativeAnchoredLevelView&)> policy;
    std::vector<NativeAnchoredLevelView> seen;
    no::RequestHandle parent;
    std::optional<double> resolve_anchored_level(const NativeAnchoredLevelView& view) const override {
        const_cast<HookHost*>(this)->seen.push_back(view);
        return policy ? policy(view) : std::nullopt;
    }
};

struct HookRun {
    std::vector<no::ArmedEvent> armed;
    std::vector<no::ExecutionAppliedEvent> applied;
    std::vector<NativeAnchoredLevelView> seen;
    std::uint64_t continuation = 0;
    NativeStateView state;
    std::string error;
    no::RequestHandle parent, take_profit, stop_loss;
};

HookRun hook_run(const char* key,
                 std::function<std::optional<double>(const NativeAnchoredLevelView&)> policy,
                 double loss_ticks = 250.0) {
    HookHost host;
    host.policy = std::move(policy);
    tk::BracketReceipt receipt;
    host.beginning = [&](Host& base) {
        host.parent = put(base, tx(2.0, "L"));
        receipt = tk::submit_bracket(base, anchored_bracket(host.parent, 250.0, loss_ticks));
    };
    HookRun out;
    REQUIRE(host.configure_native(bracket_spec(key)).status == NativeSetupStatus::Applied);
    const auto bars = staircase();
    host.run(bars.data(), static_cast<int>(bars.size()));
    out.armed = events<no::ArmedEvent>(host);
    out.applied = events<no::ExecutionAppliedEvent>(host);
    out.seen = host.seen;
    out.continuation = host.native_continuation_hash();
    out.state = host.native_state();
    out.error = host.last_error();
    out.parent = host.parent;
    if (receipt.take_profit) out.take_profit = *receipt.take_profit;
    if (receipt.stop_loss) out.stop_loss = *receipt.stop_loss;
    return out;
}

void hook_restates_the_level() {
    // The kernel would arm the profit leg at 102.5 and the stop at 97.5. The
    // host moves the profit leg a tick lower (102.49) and leaves the stop.
    const auto restated = hook_run("l7b-hook", [](const NativeAnchoredLevelView& view) {
        if (view.trigger == NativeAnchoredTrigger::Limit) return std::optional<double>{102.49};
        return std::optional<double>{};
    });
    CHECK(restated.error.empty());
    CHECK(restated.state.kind == NativeLifecycleKind::Completed);
    // Consulted exactly once per materialization: two legs, two views, and
    // every fact in them is the kernel's.
    REQUIRE(restated.seen.size() == 2);
    for (const auto& view : restated.seen) {
        CHECK(view.owner == restated.parent);
        CHECK(view.owner_applied_ordinal != 0);
        CHECK(view.owner_lot_incarnation != 0);
        CHECK(same_bits(view.owner_fill_price, 100.0));
        CHECK(view.owner_cursor.point.interval_index == 0);
        CHECK(view.leg_side == no::Side::Short);
        CHECK(same_bits(view.price_tick, 0.01));
        if (view.trigger == NativeAnchoredTrigger::Limit) {
            CHECK(view.leg == restated.take_profit);
            CHECK(same_bits(view.offset, 250.0 * 0.01));
            CHECK(same_bits(view.kernel_level, 100.0 + 250.0 * 0.01));
        } else {
            CHECK(view.trigger == NativeAnchoredTrigger::Stop);
            CHECK(view.leg == restated.stop_loss);
            CHECK(same_bits(view.offset, -250.0 * 0.01));
            CHECK(same_bits(view.kernel_level, 100.0 + -250.0 * 0.01));
        }
    }
    // The ArmedEvent carries the restated level, and so does the live book.
    REQUIRE(restated.armed.size() == 2);
    const auto* profit = armed_for(restated.armed, restated.take_profit);
    const auto* stop = armed_for(restated.armed, restated.stop_loss);
    REQUIRE(profit != nullptr && stop != nullptr);
    const auto profit_level = installed_level(profit->definition->request);
    const auto stop_level = installed_level(stop->definition->request);
    REQUIRE(profit_level && stop_level);
    CHECK(same_bits(*profit_level, 102.49));
    CHECK(same_bits(*stop_level, 97.5));
    CHECK(std::holds_alternative<no::Absolute>(profit->definition->request.anchor));
    // The restated level is what matches: the profit leg fills at 102.49.
    REQUIRE(restated.applied.size() == 2);
    CHECK(restated.applied[1].handle() == restated.take_profit);
    CHECK(same_bits(restated.applied[1].resolved_price, 102.49));
    CHECK(restated.applied[1].cursor.point.interval_index == 4);

    // nullopt keeps the kernel level: the same run as a host with no hook.
    const auto kept = hook_run("l7b-hook-null", [](const NativeAnchoredLevelView&) {
        return std::optional<double>{};
    });
    const auto plain = neutral_run("l7b-hook-null");
    CHECK(kept.seen.size() == 2);
    CHECK(kept.continuation == plain.continuation);
    REQUIRE(kept.applied.size() == 2);
    CHECK(same_bits(kept.applied[1].resolved_price, 102.5));
    REQUIRE(kept.armed.size() == 2);
    const auto* kept_profit = armed_for(kept.armed, kept.take_profit);
    REQUIRE(kept_profit != nullptr);
    const auto kept_level = installed_level(kept_profit->definition->request);
    CHECK(kept_level && same_bits(*kept_level, 102.5));

    // A non-representable restatement (a negative stop) fails the run through
    // the existing PreparationError path: NonrepresentableQuantity, reported
    // as a settlement failure at the arm, and no leg is armed.
    const auto broken = hook_run("l7b-hook-broken", [](const NativeAnchoredLevelView& view) {
        if (view.trigger == NativeAnchoredTrigger::Stop) return std::optional<double>{-1.0};
        return std::optional<double>{};
    });
    CHECK(broken.state.kind == NativeLifecycleKind::Failed);
    CHECK(broken.state.failure.code == NativeFailureCode::SettlementFailure);
    CHECK(broken.state.failure.operation == NativeFailureOperation::Settlement);
    CHECK(!broken.error.empty());
    CHECK(broken.armed.size() < 2);
    // A NaN answer is the same refusal.
    const auto nan = hook_run("l7b-hook-nan", [](const NativeAnchoredLevelView&) {
        return std::optional<double>{std::numeric_limits<double>::quiet_NaN()};
    });
    CHECK(nan.state.kind == NativeLifecycleKind::Failed);
    CHECK(nan.state.failure.code == NativeFailureCode::SettlementFailure);
}

// ── 3. PendingUntilArmed: not a working order before the arm ────────────
// The pending leg is out of native_working_requests() until its ArmedEvent
// and listed from then on; replace, cancel and cancel_all still address it;
// matching never sees a waiting leg under either visibility; the fold is
// conditional; and the C API's working list agrees by construction.
struct VisibilityHost final : Host {
    no::RequestHandle parent, pending, plain;
    std::vector<std::size_t> working_counts;   // one per calculation
    std::vector<bool> pending_listed, plain_listed;
    void on_native_bar(const Bar& bar, const NativeDecisionContext& ctx) override {
        Host::on_native_bar(bar, ctx);
        const auto rows = native_working_requests();
        working_counts.push_back(rows.size());
        bool saw_pending = false, saw_plain = false;
        for (const auto& row : rows) {
            if (row.definition->handle == pending) saw_pending = true;
            if (row.definition->handle == plain) saw_plain = true;
        }
        pending_listed.push_back(saw_pending);
        plain_listed.push_back(saw_plain);
    }
};

void pending_until_armed_hides_the_leg() {
    VisibilityHost host;
    host.beginning = [&](Host& base) {
        host.parent = put(base, tx(1.0, "entry"));
        // Two legs on the same parent: one pending until armed, one working.
        auto pending = owner_close("pending");
        pending.trigger = no::Stop{0.0};
        pending.owner = no::WaitForApplied{host.parent, no::NativeArmVisibility::PendingUntilArmed};
        pending.anchor = no::FromOwnerFill{-10.0, true};
        host.pending = put(base, pending);
        auto plain = owner_close("plain");
        plain.trigger = no::Limit{0.0};
        plain.owner = no::WaitForApplied{host.parent};
        plain.anchor = no::FromOwnerFill{10.0, true};
        host.plain = put(base, plain);
    };
    // Bar 0: the parent is accepted at run begin and fills at the open, so
    // the calculation at bar 0's close already sees both legs armed. The
    // pre-arm book is therefore read at the moment of submission instead.
    std::size_t at_submit = 0;
    bool pending_at_submit = false, plain_at_submit = false;
    host.calculation = [&](Host&) {};
    Host probe;
    no::RequestHandle probe_parent, probe_pending, probe_plain;
    probe.beginning = [&](Host& base) {
        probe_parent = put(base, tx(1.0, "entry"));
        auto pending = owner_close("pending");
        pending.trigger = no::Stop{0.0};
        pending.owner = no::WaitForApplied{probe_parent, no::NativeArmVisibility::PendingUntilArmed};
        pending.anchor = no::FromOwnerFill{-10.0, true};
        probe_pending = put(base, pending);
        auto plain = owner_close("plain");
        plain.trigger = no::Limit{0.0};
        plain.owner = no::WaitForApplied{probe_parent};
        plain.anchor = no::FromOwnerFill{10.0, true};
        probe_plain = put(base, plain);
        const auto rows = base.native_working_requests();
        at_submit = rows.size();
        for (const auto& row : rows) {
            if (row.definition->handle == probe_pending) pending_at_submit = true;
            if (row.definition->handle == probe_plain) plain_at_submit = true;
        }
    };
    run(probe, bracket_spec("l7b-visibility-probe"), {100.0});
    completed(probe);
    // Before the arm: the parent and the Working leg are the book; the
    // pending leg is live (accepted) but not listed.
    CHECK(at_submit == 2);
    CHECK(!pending_at_submit);
    CHECK(plain_at_submit);
    CHECK(events<no::AcceptedEvent>(probe).size() == 3);

    run(host, bracket_spec("l7b-visibility"), {100.0, 100.0, 100.0});
    completed(host);
    // From the ArmedEvent on, both legs are listed with their materialized
    // levels (the parent is terminal).
    const auto armed = events<no::ArmedEvent>(host);
    REQUIRE(armed.size() == 2);
    REQUIRE(host.working_counts.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(host.working_counts[i] == 2);
        CHECK(host.pending_listed[i]);
        CHECK(host.plain_listed[i]);
    }
    const auto* pending_armed = armed_for(armed, host.pending);
    REQUIRE(pending_armed != nullptr);
    const auto* owner = std::get_if<no::WaitForApplied>(&pending_armed->definition->request.owner);
    REQUIRE(owner != nullptr);
    CHECK(owner->visibility == no::NativeArmVisibility::PendingUntilArmed);

    // Matching: a waiting leg never matches under either visibility. Both
    // legs are placed before the fill and neither fills at the fill's own
    // open print (100 is not past either level), so the only fills are the
    // parent's. The materialized levels are the anchor's.
    CHECK(events<no::ExecutionAppliedEvent>(host).size() == 1);
    CHECK(host.physical_position().signed_units == 1.0);
}

void pending_leg_is_still_addressable() {
    // Replace and cancel address a hidden leg by handle; cancel_all counts it
    // among the requests that left the book; cancel_where finds its comment.
    Host host;
    no::RequestHandle parent, hidden, other;
    no::ReplaceResult replaced;
    no::CancelResult cancelled;
    std::size_t listed_after_replace = 0, listed_after_cancel = 0;
    std::optional<NativeTrailState> trail_of_hidden;
    std::size_t where_count = 0, all_count = 0;
    host.beginning = [&](Host& base) {
        parent = put(base, tx(1.0, "entry"));
        auto leg = owner_close("hidden");
        leg.trigger = no::Stop{0.0};
        leg.owner = no::WaitForApplied{parent, no::NativeArmVisibility::PendingUntilArmed};
        leg.anchor = no::FromOwnerFill{-10.0, true};
        hidden = put(base, leg);
        auto trail = owner_close("hidden-trail");
        trail.trigger = no::Trail{0.05, 0.0};
        trail.owner = no::WaitForApplied{parent, no::NativeArmVisibility::PendingUntilArmed};
        trail.anchor = no::FromOwnerFill{10.0, true};
        other = put(base, trail);
        // A handle-addressed trail read answers for the hidden leg.
        trail_of_hidden = base.trail_state(other);
        // Replace keeps the leg hidden (the successor carries its own
        // visibility) and still succeeds.
        auto successor = leg;
        successor.anchor = no::FromOwnerFill{-20.0, true};
        replaced = base.replace(hidden, successor);
        if (replaced.successor) hidden = *replaced.successor;
        listed_after_replace = base.native_working_requests().size();
        cancelled = base.cancel(hidden);
        listed_after_cancel = base.native_working_requests().size();
        where_count = base.cancel_where("bracket");
        all_count = base.cancel_all();
    };
    run(host, bracket_spec("l7b-visibility-address"), {100.0});
    completed(host);
    CHECK(trail_of_hidden.has_value());
    if (trail_of_hidden) CHECK(!trail_of_hidden->activated);
    CHECK(replaced.status == no::ReplaceStatus::Replaced);
    CHECK(listed_after_replace == 1);      // the parent only
    CHECK(cancelled.status == no::CancelStatus::Cancelled);
    CHECK(listed_after_cancel == 1);
    CHECK(where_count == 1);               // the hidden trail's comment matched
    CHECK(all_count == 1);                 // the parent; nothing else was left
    const auto cancelled_events = events<no::CancelledEvent>(host);
    REQUIRE(cancelled_events.size() == 3);
    CHECK(cancelled_events[0].handle() == hidden);
    CHECK(cancelled_events[1].handle() == other);
    CHECK(cancelled_events[2].handle() == parent);
    CHECK(events<no::ReplacedEvent>(host).size() == 1);
    CHECK(events<no::NotWorkingEvent>(host).empty());
    CHECK(events<no::InvalidHandleEvent>(host).empty());

    // An unknown visibility is refused at acceptance.
    Host rejects;
    no::SubmitResult unknown;
    rejects.beginning = [&](Host& base) {
        const auto p = put(base, tx(1.0, "entry"));
        auto leg = owner_close("leg");
        leg.trigger = no::Stop{0.0};
        leg.owner = no::WaitForApplied{p, static_cast<no::NativeArmVisibility>(9)};
        leg.anchor = no::FromOwnerFill{-10.0, true};
        unknown = base.submit(leg);
    };
    run(rejects, bracket_spec("l7b-visibility-unknown"), {100.0});
    CHECK(unknown.status == no::SubmitStatus::Rejected);
    REQUIRE(unknown.reason.has_value());
    CHECK(*unknown.reason == no::RequestRejectReason::InvalidOwner);
}

void visibility_folds_only_when_set() {
    auto probe = [](std::optional<no::NativeArmVisibility> visibility) {
        Host host;
        host.beginning = [&](Host& base) {
            const auto parent = put(base, tx(1.0, "entry"));
            auto leg = owner_close("leg");
            leg.trigger = no::Stop{0.0};
            leg.owner = visibility ? no::WaitForApplied{parent, *visibility}
                                   : no::WaitForApplied{parent};
            leg.anchor = no::FromOwnerFill{-10.0, true};
            put(base, leg);
        };
        run(host, bracket_spec("l7b-visibility-fold"), {100.0});
        completed(host);
        return host.native_continuation_hash();
    };
    const auto plain = probe(std::nullopt);
    CHECK(probe(no::NativeArmVisibility::Working) == plain);
    CHECK(probe(no::NativeArmVisibility::PendingUntilArmed) != plain);
}

// The C API's working list is the same enumeration: a PENDING_UNTIL_ARMED
// child sent with the additive tail is absent before the fill and present
// after it, through strategy_native_working_len_v1 alone.
struct CVisibilityState {
    pf_strategy_t host = nullptr;
    int calculations = 0;
    uint64_t parent = 0;
    uint64_t leg = 0;
    int len_at_submit = -1;
    int len_after_fill = -1;
    int rc_submit = 0;
    int rc_bad_tag = 0;
};

int c_visibility_on_bar(void* user, const pf_bar_t*, const pf_native_decision_v1*) {
    auto* state = static_cast<CVisibilityState*>(user);
    ++state->calculations;
    if (state->calculations == 1) {
        pf_native_request_v1 parent;
        std::memset(&parent, 0, sizeof(parent));
        parent.struct_size = static_cast<uint32_t>(sizeof(parent));
        parent.version = PF_NATIVE_API_VERSION;
        parent.intent = PF_NATIVE_INTENT_TRANSACT;
        parent.intent_value = 1.0;
        parent.label = "c-parent";
        if (strategy_native_submit_v1(state->host, &parent, &state->parent, nullptr)
            != PF_NATIVE_OK) {
            return 1;
        }
        pf_native_request_v1 leg;
        std::memset(&leg, 0, sizeof(leg));
        leg.struct_size = static_cast<uint32_t>(sizeof(leg));
        leg.version = PF_NATIVE_API_VERSION;
        leg.intent = PF_NATIVE_INTENT_REDUCE;
        leg.reduce_size = PF_NATIVE_REDUCE_OWNER_OPENED;
        leg.trigger = PF_NATIVE_TRIGGER_STOP;
        leg.anchor = PF_NATIVE_ANCHOR_FROM_OWNER_FILL;
        leg.anchor_offset = -10.0;
        leg.anchor_offset_in_ticks = 1;
        leg.anchor_rounding = PF_NATIVE_ANCHOR_ROUNDING_DIRECTIONAL;
        leg.owner = PF_NATIVE_OWNER_WAIT_FOR_APPLIED;
        leg.owner_n = 1;
        leg.owner_incarnations = &state->parent;
        leg.visibility = PF_NATIVE_ARM_VISIBILITY_PENDING_UNTIL_ARMED;
        leg.label = "c-leg";
        state->rc_submit = strategy_native_submit_v1(state->host, &leg, &state->leg, nullptr);
        state->len_at_submit = strategy_native_working_len_v1(state->host);
        // A visibility on a non-arming owner is a tag error, not a silent drop.
        pf_native_request_v1 bad = parent;
        bad.visibility = PF_NATIVE_ARM_VISIBILITY_PENDING_UNTIL_ARMED;
        state->rc_bad_tag = strategy_native_submit_v1(state->host, &bad, nullptr, nullptr);
    } else if (state->calculations == 2) {
        state->len_after_fill = strategy_native_working_len_v1(state->host);
    }
    return 0;
}

void c_api_working_list_agrees() {
    CVisibilityState state;
    pf_native_callbacks_v1 table;
    std::memset(&table, 0, sizeof(table));
    table.struct_size = static_cast<uint32_t>(sizeof(table));
    table.version = PF_NATIVE_API_VERSION;
    table.user = &state;
    table.on_bar = &c_visibility_on_bar;
    state.host = strategy_native_host_create_v1(&table);
    REQUIRE(state.host != nullptr);
    pf_native_run_spec_v1 spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.struct_size = static_cast<uint32_t>(sizeof(spec));
    spec.session_key = "l7b-c-visibility";
    spec.run_number = 1;
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "L7B";
    spec.tickerid = "TEST:L7B";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.description = "";
    spec.volumetype = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.allowed_open_directions = 3;
    CHECK(strategy_configure_native_v1(state.host, &spec) == 0);
    pf_bar_t bars[3];
    std::memset(bars, 0, sizeof(bars));
    for (int i = 0; i < 3; ++i) {
        bars[i].open = bars[i].high = bars[i].low = bars[i].close = 100.0;
        bars[i].volume = 1.0;
        bars[i].timestamp = kT + static_cast<int64_t>(i) * 60000;
    }
    CHECK(strategy_native_run_v1(state.host, bars, 3, nullptr) == PF_NATIVE_OK);
    CHECK(state.rc_submit == PF_NATIVE_OK);
    CHECK(state.rc_bad_tag == PF_NATIVE_E_TAG);
    // At submission: the parent only. After the fill (the leg armed at bar
    // 1's open, the parent terminal): the leg only.
    CHECK(state.len_at_submit == 1);
    CHECK(state.len_after_fill == 1);
    if (state.len_after_fill == 1) {
        pf_native_working_v1 row;
        std::memset(&row, 0, sizeof(row));
        row.struct_size = static_cast<uint32_t>(sizeof(row));
        CHECK(strategy_native_working_get_v1(state.host, 0, &row) == PF_NATIVE_OK);
        CHECK(row.incarnation == state.leg);
        CHECK(row.trigger == PF_NATIVE_TRIGGER_STOP);
        CHECK(same_bits(row.p1, 99.9));   // 100 - 10 ticks, Directional keeps the ladder point
    }
    strategy_native_host_free(state.host);
}

// ── 4. Which non-level facts resolve at the arm (measurement) ───────────
// For a bracket child (a WaitForApplied leg) on this tree:
//   Sized intent            -> not a child at all: InvalidOwner at submit
//                              (validate_request: a Sized/ReverseTo intent
//                              must be Independent);
//   Reduce{OwnerOpenedUnits}-> at the ARM, from the owner's opened units
//                              (QuantityBoundEvent), i.e. sized against the
//                              owner's fill -- what a bracket child needs;
//   Reduce{ScopeFraction, AtMatch} -> at the candidate, against the bound
//                              scope, which after the arm is the owner's lot;
//   Reduce{ScopeFraction, AtAcceptance} -> frozen at SUBMIT against the
//                              whole book (a waiting child binds the whole
//                              book), so a pre-fill child freezes the
//                              pre-fill book: not the owner's lot;
//   Capacity{PointBudget}   -> the budget is fixed at submit, the per-point
//                              allowance is initialised at each matching
//                              point after the arm from the post-arm
//                              remaining (min(remaining, budget));
//   Group{Member}           -> identity fixed at submit, the effect resolved
//                              at the filling member's applied over the
//                              live members, which for legs of one bracket
//                              is always after their common arm;
//   cancel on owner rejection -> at the owner's terminal event
//                              (MatchRejected, Cancelled, Replaced,
//                              NoEffect or a terminal fill that opened
//                              nothing): CancelReason::OwnerGone.
// The twin (acceptance a) sizes its legs with OwnerOpenedUnits, which is
// already an arm-time fact, so no SizeTime::AtArm is added.
void facts_resolve_at_the_arm() {
    // (i) A Sized child is refused at submit.
    {
        Host host;
        no::SubmitResult sized;
        host.beginning = [&](Host& base) {
            const auto parent = put(base, tx(2.0, "entry"));
            no::Request leg{no::Sized{no::Side::Short, no::CashValue{100.0}}, "sized", ""};
            leg.trigger = no::Stop{0.0};
            leg.owner = no::WaitForApplied{parent};
            leg.anchor = no::FromOwnerFill{-10.0, true};
            sized = base.submit(leg);
        };
        run(host, bracket_spec("l7b-facts-sized"), {100.0});
        CHECK(sized.status == no::SubmitStatus::Rejected);
        REQUIRE(sized.reason.has_value());
        CHECK(*sized.reason == no::RequestRejectReason::InvalidOwner);
    }
    // (ii) OwnerOpenedUnits: unbound at submit, bound at the arm to what the
    //      owner opened; a per-point budget then meters the post-arm
    //      remaining one unit per point.
    {
        Host host;
        no::RequestHandle parent, leg;
        std::optional<no::RemainingProjection> at_submit;
        host.beginning = [&](Host& base) {
            parent = put(base, tx(3.0, "entry"));
            auto close = owner_close("close");
            close.trigger = no::Stop{0.0};
            close.owner = no::WaitForApplied{parent};
            close.anchor = no::FromOwnerFill{50.0, true};   // 100.5: hit by the rise
            close.capacity = no::PointBudget{1.0};
            leg = put(base, close);
            for (const auto& row : base.native_working_requests()) {
                if (row.definition->handle == leg) at_submit = row.remaining;
            }
        };
        run(host, bracket_spec("l7b-facts-owner-opened"), {100.0, 101.0, 101.0, 101.0, 101.0});
        completed(host);
        REQUIRE(at_submit.has_value());
        CHECK(std::holds_alternative<no::RemainingProjectionUnbound>(*at_submit));
        const auto bound = events<no::QuantityBoundEvent>(host);
        REQUIRE(bound.size() == 1);
        CHECK(bound[0].definition && bound[0].definition->handle == leg);
        CHECK(same_bits(bound[0].source_units, 3.0));
        const auto* units = std::get_if<no::RemainingProjectionUnits>(&bound[0].remaining);
        REQUIRE(units != nullptr);
        CHECK(same_bits(units->q, 3.0));
        // Three one-unit fills, one per matching point, all after the arm.
        std::size_t leg_fills = 0;
        for (const auto& row : events<no::ExecutionAppliedEvent>(host)) {
            if (row.handle() != leg) continue;
            ++leg_fills;
            CHECK(same_bits(row.closed_units, 1.0));
        }
        CHECK(leg_fills == 3);
        CHECK(host.physical_position().signed_units == 0.0);
    }
    // (iii) ScopeFraction AtMatch resolves against the owner's lot at the
    //       candidate; AtAcceptance freezes the pre-fill book at submit.
    {
        auto probe = [](no::ScopeBasis basis) {
            Host host;
            no::RequestHandle parent, leg;
            host.beginning = [&](Host& base) {
                parent = put(base, tx(2.0, "entry"));
                no::Request close{no::Reduce{no::ScopeFraction{0.5, no::ScopeClaim::Gross, basis}},
                                  "half", "bracket"};
                close.trigger = no::Stop{0.0};
                close.owner = no::WaitForApplied{parent};
                close.anchor = no::FromOwnerFill{50.0, true};
                leg = put(base, close);
            };
            run(host, bracket_spec("l7b-facts-fraction"), {100.0, 101.0, 101.0});
            double closed = 0.0;
            for (const auto& row : events<no::ExecutionAppliedEvent>(host)) {
                if (row.handle() == leg) closed += row.closed_units;
            }
            return closed;
        };
        // Half of the owner's 2-unit lot, measured at the candidate.
        CHECK(same_bits(probe(no::ScopeBasis::AtMatch), 1.0));
        // Frozen at submit, when the book was flat: nothing to halve, so the
        // leg closes nothing. Measured, not what a bracket child needs.
        CHECK(same_bits(probe(no::ScopeBasis::AtAcceptance), 0.0));
    }
    // (iv) A rejected owner ends its waiting children: OwnerGone at the
    //      owner's MatchRejectedEvent, before any arm.
    {
        Host host;
        no::RequestHandle parent, leg;
        host.beginning = [&](Host& base) {
            parent = put(base, tx(10.0, "entry"));
            auto close = owner_close("close");
            close.trigger = no::Stop{0.0};
            close.owner = no::WaitForApplied{parent, no::NativeArmVisibility::PendingUntilArmed};
            close.anchor = no::FromOwnerFill{-10.0, true};
            leg = put(base, close);
        };
        auto s = bracket_spec("l7b-facts-owner-rejected");
        s.max_abs_units = 5.0;
        run(host, s, {100.0, 100.0});
        completed(host);
        const auto rejected = events<no::MatchRejectedEvent>(host);
        REQUIRE(rejected.size() == 1);
        CHECK(rejected[0].handle() == parent);
        CHECK(rejected[0].reason == no::MatchRejectReason::MaxAbsUnits);
        const auto cancelled = events<no::CancelledEvent>(host);
        REQUIRE(cancelled.size() == 1);
        CHECK(cancelled[0].handle() == leg);
        CHECK(cancelled[0].reason == no::CancelReason::OwnerGone);
        CHECK(events<no::ArmedEvent>(host).empty());
        CHECK(host.native_working_requests().empty());
    }
}

// ── 5. TWIN: the R4 bracket scenarios through anchored legs ─────────────
// A native host places a Market parent with two anchored legs through
// submit_bracket on the 0.01 grid, rounding Directional, and implements the
// adapter's trigger projection inside the arm hook and its fill booking
// inside resolve_execution_terms. The expected rows are the R4 harness's
// R4_PINNED_DATA (tests/test_adapter_brackets_relower.cpp on branch
// r5/R4-adapter-brackets, harvested from the unchanged adapter on engine main
// 04330d4 with -DPINEFORGE_R4_HARVEST), re-harvested identically on this
// tree's parent 3f6fd57 before this lane. Times are compared by bar index.
//
// Adapter policy copied verbatim as TEST policy (never kernel options):
//   directional_tick          src/source/pine_adapter.cpp:297-301
//   source_bar_fill_tick / nearest_tick are not needed on these on-grid levels
//   source_trigger_threshold  src/source/pine_adapter.cpp:328-375
//   the fill booking          src/source/pine_adapter.cpp:9907-9926 (an
//                             ExitLimit books directional_tick(resolved,
//                             !is_buy) clamped to its level; an ExitStop /
//                             ExitTrail books directional_tick(resolved,
//                             is_buy)), i.e. the grid level for a half-tick
//                             shifted threshold crossed intrabar.
namespace twin_policy {

bool finite_positive(double v) { return std::isfinite(v) && v > 0.0; }

// src/source/pine_adapter.cpp:297-301
double directional_tick(double value, double tick, bool upward) noexcept {
    if (!std::isfinite(value) || !finite_positive(tick)) return value;
    const double scaled = value / tick;
    return (upward ? std::ceil(scaled - 1e-9) : std::floor(scaled + 1e-9)) * tick;
}

// src/source/pine_adapter.cpp:280-289 (source_bar_fill_tick), needed by the
// threshold's on-grid test below.
double source_bar_fill_tick(double price, double tick) noexcept {
    if (!std::isfinite(price) || !finite_positive(tick)) return price;
    return std::floor(price / tick + 0.5) * tick;
}

// src/source/pine_adapter.cpp:328-375
double source_trigger_threshold(double level, double tick, bool is_buy, bool is_limit) noexcept {
    if (!std::isfinite(level) || !finite_positive(tick)) return level;
    if (level <= 0.0) return 0.0;
    const bool upward = is_limit ? !is_buy : is_buy;
    double scaled = level / tick;
    const double inverse = 1.0 / tick;
    const double integral_inverse = std::floor(inverse + 0.5);
    if (integral_inverse > 0.0
        && std::abs(inverse - integral_inverse) <= 1e-6 * integral_inverse) {
        scaled = level * integral_inverse;
        const double nearest_index = std::floor(scaled + 0.5);
        if (source_bar_fill_tick(level, tick) == level) scaled = nearest_index;
    }
    const double target_index = upward ? std::ceil(scaled - 1e-12) : std::floor(scaled + 1e-12);
    const double grid = target_index * tick;
    double threshold = grid + (upward ? -0.5 : 0.5) * tick;
    const auto reaches_target = [&](double price) {
        const double rounded_index = std::floor(price / tick + 0.5);
        return upward ? rounded_index >= target_index : rounded_index <= target_index;
    };
    for (int i = 0; i < 16 && !reaches_target(threshold); ++i) {
        threshold = std::nextafter(threshold, upward
            ? std::numeric_limits<double>::infinity()
            : -std::numeric_limits<double>::infinity());
    }
    for (int i = 0; i < 16; ++i) {
        const double candidate = std::nextafter(threshold, upward
            ? -std::numeric_limits<double>::infinity()
            : std::numeric_limits<double>::infinity());
        if (!reaches_target(candidate)) break;
        threshold = candidate;
    }
    return threshold;
}

}  // namespace twin_policy

enum class TwinShape {
    FromEntryBracket,
    FromEntryStopout,
    TrailReissue,
    TrailZeroOffset,
    ParentRejection,
    CycleRevival,
};

struct TwinRow {
    int entry_bar;
    int exit_bar;
    double entry_price;
    double exit_price;
    double qty;
    double pnl;
    int is_long;
    int open_at_end;
};

struct TwinObserved {
    std::vector<TwinRow> rows;
    double final_equity = 0.0;
    double max_drawdown = 0.0;
    double max_runup = 0.0;
    double position_units = 0.0;
    std::string error;
};

// The R4 rows, harness times converted to bar indexes ((t - 1700000000000) / 60000).
struct TwinExpected {
    const char* name;
    TwinShape shape;
    std::vector<TwinRow> rows;
    double final_equity;
    double max_drawdown;
    double max_runup;
    double position_units;
};

const std::vector<TwinExpected>& twin_expected() {
    static const std::vector<TwinExpected> rows = {
        {"from-entry-bracket", TwinShape::FromEntryBracket,
         {{1, 4, 100.0, 102.5, 2.0, 5.0, 1, 0}}, 10005.0, 0.0, 0.0, 0.0},
        {"from-entry-stopout", TwinShape::FromEntryStopout,
         {{1, 8, 100.0, 98.5, 2.0, -3.0, 1, 0}}, 9997.0, 10.5, 0.0, 0.0},
        {"trail-reissue", TwinShape::TrailReissue,
         {{1, 6, 100.0, 102.0, 2.0, 4.0, 1, 0}}, 10004.0, 3.5, 0.0, 0.0},
        {"trail-zero-offset", TwinShape::TrailZeroOffset,
         {{1, 6, 100.0, 103.75, 2.0, 7.5, 1, 0}}, 10007.5, 0.0, 0.0, 0.0},
        {"parent-rejection", TwinShape::ParentRejection,
         {{1, 6, 100.0, 108.0, 100.0, 800.0, 1, 0}}, 10800.0, 300.0, 0.0, 0.0},
        {"cycle-revival", TwinShape::CycleRevival,
         {{1, 4, 100.0, 103.0, 2.0, 6.0, 1, 0}, {12, 12, 99.25, 99.0, 2.0, -0.5, 1, 0}},
         10005.5, 0.5, 0.0, 0.0},
    };
    return rows;
}

// The R4 harness's parent-rejection tape (KI-54 decline fixture).
std::vector<Bar> rejection_tape() {
    std::vector<Bar> bars;
    auto push = [&](double o, double h, double l, double c) {
        bars.push_back(ohlc(static_cast<int>(bars.size()), o, h, l, c));
    };
    push(100.0, 100.0, 100.0, 100.0);
    push(100.0, 100.0, 100.0, 100.0);   // L fills @100
    push(100.0, 112.0,  99.0, 110.0);   // X + S queued
    push(111.0, 112.0, 104.0, 111.0);   // S declined; 105 touched
    push(111.0, 111.0, 111.0, 111.0);
    push(111.0, 111.0, 104.0, 111.0);   // re-issue arms 108
    push(109.0, 109.0, 100.0, 101.0);   // the fresh 108 stop settles
    push(101.0, 101.0, 101.0, 101.0);
    return bars;
}

class TwinHost final : public NativeStrategyHost {
public:
    explicit TwinHost(TwinShape shape) : shape_(shape) {}
    std::string first_error;

private:
    TwinShape shape_;
    int bar_ = -1;
    double bar_open_ = 0.0;
    no::RequestHandle parent_;
    std::optional<std::int64_t> cycle_;
    std::optional<double> fill_;
    std::optional<no::RequestHandle> trail_;
    std::optional<no::RequestHandle> stop_;
    // The adapter's placement snapshot: the grid level per leg (what the
    // threshold was projected from) and, for the trail re-issue, the
    // retained best and the new distance.
    std::map<std::uint64_t, double> grid_level_;
    std::map<std::uint64_t, bool> leg_is_limit_;
    std::optional<double> zero_trail_;      // handle of the explicit-zero trail
    bool zero_trail_set_ = false;

    tk::BracketSpec bracket(double profit_ticks, double loss_ticks) {
        auto spec_rows = anchored_bracket(parent_, profit_ticks, loss_ticks);
        spec_rows.anchor_rounding = no::NativeAnchorRounding::Directional;
        spec_rows.visibility = no::NativeArmVisibility::PendingUntilArmed;
        return spec_rows;
    }

    void on_native_bar_open(const Bar& bar, const NativeDecisionContext&) override {
        bar_open_ = bar.open;
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        if (event.definition && event.definition->handle == parent_ && event.opened_units != 0.0) {
            cycle_ = event.cycle_after;
            fill_ = event.resolved_price;
        }
    }

    // The adapter's trigger projection, applied to the kernel level (which is
    // already directional_tick's value: the kernel's Directional rounding is
    // the same arithmetic).
    std::optional<double> resolve_anchored_level(const NativeAnchoredLevelView& view) const override {
        const bool is_limit = view.trigger != NativeAnchoredTrigger::Stop;
        const bool is_buy = view.leg_side == no::Side::Long;
        auto* self = const_cast<TwinHost*>(this);
        self->grid_level_[view.leg.incarnation] = view.kernel_level;
        self->leg_is_limit_[view.leg.incarnation] = is_limit;
        return twin_policy::source_trigger_threshold(view.kernel_level, view.price_tick,
                                                     is_buy, is_limit);
    }

    // The adapter's fill booking: an exit crossed intrabar at a half-tick
    // threshold books the grid level (pine_adapter.cpp:9907-9926); the
    // explicit-zero trail books the next open (resolve_terms
    // explicit_zero_trail, pine_adapter.cpp:9466-9476 and its
    // zero_trail_policy_price).
    no::ExecutionTerms resolve_execution_terms(const NativeExecutionTermsFacts& facts) const override {
        no::ExecutionTerms terms{facts.default_resolved_price, std::nullopt,
                                 no::OpeningShape::Transact};
        const double tick = 0.01;
        const auto grid = grid_level_.find(facts.target.incarnation);
        if (grid != grid_level_.end() && facts.trigger_level
            && facts.cursor.point.path_phase != NativePathPhase::Open) {
            const bool is_limit = leg_is_limit_.at(facts.target.incarnation);
            double booked = twin_policy::directional_tick(
                    facts.default_resolved_price, tick, is_limit ? !facts.is_buy : facts.is_buy);
            if (is_limit) booked = facts.is_buy ? std::min(booked, grid->second)
                                                : std::max(booked, grid->second);
            terms.resolved_price = booked;
            return terms;
        }
        if (zero_trail_set_ && trail_ && facts.target == *trail_) {
            terms.resolved_price = bar_open_;
            return terms;
        }
        return terms;
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bar_;
        const int bar = bar_;
        auto entry = [&](double units) {
            const auto placed = submit({no::Transact{units}, "L", ""});
            if (placed.status == no::SubmitStatus::Accepted && placed.handle) {
                parent_ = *placed.handle;
            } else if (first_error.empty()) {
                first_error = "entry rejected";
            }
        };
        // A leg placed after the fill closes the lot the parent opened:
        // OwnerOpenedUnits is a waiting child's spelling, so the bound leg
        // claims the whole lot as a ScopeFraction (qty_percent = 100).
        auto bound_close = [&](const char* label) {
            no::Request request{no::Reduce{no::ScopeFraction{1.0}}, label, "bracket"};
            if (cycle_) request.owner = no::BindOpening{parent_, *cycle_};
            return request;
        };
        switch (shape_) {
        case TwinShape::FromEntryBracket:
            if (bar == 0) {
                entry(2.0);
                tk::submit_bracket(*this, bracket(250.0, 250.0));
            }
            break;
        case TwinShape::FromEntryStopout:
            if (bar == 0) {
                entry(2.0);
                tk::submit_bracket(*this, bracket(800.0, 150.0));
            }
            break;
        case TwinShape::TrailReissue:
            // strategy.exit("T","L", trail_points=300, trail_offset=100 then
            // 200 from bar 5): a Trail on the open position, re-issued at the
            // new distance with the running extreme retained.
            if (bar == 0) entry(2.0);
            if (bar >= 1 && fill_ && cycle_) {
                auto request = bound_close("T");
                request.trigger = no::Trail{(bar < 5 ? 100.0 : 200.0) * 0.01, *fill_ + 3.0};
                if (!trail_) {
                    const auto placed = submit(request);
                    if (placed.status == no::SubmitStatus::Accepted) trail_ = placed.handle;
                    else if (first_error.empty()) first_error = "trail rejected";
                } else if (bar == 5) {
                    const auto replaced = replace(*trail_, request, no::ReplaceOptions{true});
                    if (replaced.status == no::ReplaceStatus::Replaced) trail_ = replaced.successor;
                    else if (first_error.empty()) first_error = "trail re-issue rejected";
                }
            }
            break;
        case TwinShape::TrailZeroOffset:
            // strategy.exit("T","L", trail_points=100, trail_offset=0) from
            // bar 5: the adapter's half-tick sentinel (0.5 tick) on a live
            // Trail whose activation is already reached.
            if (bar == 0) entry(2.0);
            if (bar >= 5 && fill_ && cycle_ && !trail_) {
                auto request = bound_close("T");
                request.trigger = no::Trail{0.0, *fill_ + 1.0, no::TrailTicks{0.5}};
                const auto placed = submit(request);
                if (placed.status == no::SubmitStatus::Accepted) {
                    trail_ = placed.handle;
                    zero_trail_set_ = true;
                } else if (first_error.empty()) {
                    first_error = "zero trail rejected";
                }
            }
            break;
        case TwinShape::ParentRejection:
            // 100 % of equity long; at bar 2 a priced stop at 105 and a
            // reversal that the account cannot afford at the +1 gap open.
            if (bar == 0) {
                no::Request sized{no::Sized{no::Side::Long, no::EquityFraction{1.0}}, "L", ""};
                const auto placed = submit(sized);
                if (placed.status == no::SubmitStatus::Accepted && placed.handle) parent_ = *placed.handle;
            }
            if (bar == 2 && cycle_) {
                auto stop = bound_close("X");
                stop.trigger = no::Stop{twin_policy::source_trigger_threshold(105.0, 0.01, false, false)};
                const auto placed = submit(stop);
                if (placed.status == no::SubmitStatus::Accepted) stop_ = placed.handle;
                grid_level_[stop_ ? stop_->incarnation : 0] = 105.0;
                leg_is_limit_[stop_ ? stop_->incarnation : 0] = false;
                (void)submit({no::ReverseTo{-100.0}, "S", ""});
            }
            if (bar == 5 && stop_) {
                auto stop = bound_close("X");
                stop.trigger = no::Stop{twin_policy::source_trigger_threshold(108.0, 0.01, false, false)};
                const auto replaced = replace(*stop_, stop);
                if (replaced.status == no::ReplaceStatus::Replaced && replaced.successor) {
                    stop_ = replaced.successor;
                    grid_level_[stop_->incarnation] = 108.0;
                    leg_is_limit_[stop_->incarnation] = false;
                }
            }
            break;
        case TwinShape::CycleRevival:
            // One bracket definition (103 / 99 absolute) across two cycles:
            // the second lot arms fresh legs from its own fill of 99.25
            // (103 = +375 ticks, 99 = -25 ticks).
            if (bar == 0) {
                entry(2.0);
                tk::submit_bracket(*this, bracket(300.0, 100.0));
            }
            if (bar == 11) {
                entry(2.0);
                tk::submit_bracket(*this, bracket(375.0, 25.0));
            }
            break;
        }
    }
};

TwinObserved twin_observe(TwinShape shape) {
    TwinObserved out;
    TwinHost host(shape);
    auto s = bracket_spec("l7b-twin");
    if (shape == TwinShape::ParentRejection) s.initial_margin_fraction = 1.0;
    if (host.configure_native(s).status != NativeSetupStatus::Applied) {
        out.error = "configure: " + host.last_error();
        return out;
    }
    const auto bars = shape == TwinShape::ParentRejection ? rejection_tape() : staircase();
    host.run(bars.data(), static_cast<int>(bars.size()));
    out.error = host.last_error();
    if (out.error.empty() && !host.first_error.empty()) out.error = host.first_error;
    const Report report(host);
    for (int i = 0; i < report.c.trades_len; ++i) {
        const TradeC& t = report.c.trades[i];
        out.rows.push_back({static_cast<int>((t.entry_time - kT) / 60000),
                            static_cast<int>((t.exit_time - kT) / 60000),
                            t.entry_price, t.exit_price, t.qty, t.pnl, t.is_long, t.open_at_end});
    }
    out.final_equity = report.c.equity_curve_len > 0
        ? report.c.equity_curve[report.c.equity_curve_len - 1].equity : 0.0;
    out.max_drawdown = report.c.metrics.equity.max_equity_drawdown;
    out.max_runup = report.c.metrics.equity.max_equity_runup;
    out.position_units = host.physical_position().signed_units;
    return out;
}

// One line per divergence; the first one is the mechanism to report.
std::vector<std::string> twin_diff(const TwinExpected& want, const TwinObserved& got) {
    std::vector<std::string> out;
    char line[256];
    auto row_diff = [&](const char* field, std::size_t i, double a, double b) {
        if (std::memcmp(&a, &b, sizeof(double)) == 0) return;
        std::snprintf(line, sizeof line, "row %zu %s: got %.17g want %.17g", i, field, a, b);
        out.emplace_back(line);
    };
    if (!got.error.empty()) out.push_back("run error: " + got.error);
    if (got.rows.size() != want.rows.size()) {
        std::snprintf(line, sizeof line, "row count: got %zu want %zu", got.rows.size(),
                      want.rows.size());
        out.emplace_back(line);
    }
    const std::size_t n = std::min(got.rows.size(), want.rows.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto& a = got.rows[i];
        const auto& b = want.rows[i];
        if (a.entry_bar != b.entry_bar) {
            std::snprintf(line, sizeof line, "row %zu entry bar: got %d want %d", i, a.entry_bar,
                          b.entry_bar);
            out.emplace_back(line);
        }
        if (a.exit_bar != b.exit_bar) {
            std::snprintf(line, sizeof line, "row %zu exit bar: got %d want %d", i, a.exit_bar,
                          b.exit_bar);
            out.emplace_back(line);
        }
        row_diff("entry_price", i, a.entry_price, b.entry_price);
        row_diff("exit_price", i, a.exit_price, b.exit_price);
        row_diff("qty", i, a.qty, b.qty);
        row_diff("pnl", i, a.pnl, b.pnl);
        if (a.is_long != b.is_long) out.push_back("row is_long differs");
        if (a.open_at_end != b.open_at_end) out.push_back("row open_at_end differs");
    }
    row_diff("final_equity", 0, got.final_equity, want.final_equity);
    row_diff("max_drawdown", 0, got.max_drawdown, want.max_drawdown);
    row_diff("max_runup", 0, got.max_runup, want.max_runup);
    row_diff("position_units", 0, got.position_units, want.position_units);
    return out;
}

void twin_of_the_r4_bracket_scenarios() {
    for (const auto& want : twin_expected()) {
        const auto got = twin_observe(want.shape);
        const auto diff = twin_diff(want, got);
        std::printf("    twin %-20s rows=%zu final_equity=%.17g dd=%.17g runup=%.17g: %s\n",
                    want.name, got.rows.size(), got.final_equity, got.max_drawdown,
                    got.max_runup, diff.empty() ? "MATCH" : "DIVERGES");
        for (const auto& row : got.rows) {
            std::printf("      row entry_bar=%d exit_bar=%d entry=%.17g exit=%.17g qty=%.17g pnl=%.17g\n",
                        row.entry_bar, row.exit_bar, row.entry_price, row.exit_price, row.qty,
                        row.pnl);
        }
        for (const auto& line : diff) std::printf("      - %s\n", line.c_str());
        const bool expected_to_match = want.shape == TwinShape::FromEntryBracket
            || want.shape == TwinShape::FromEntryStopout
            || want.shape == TwinShape::TrailZeroOffset
            || want.shape == TwinShape::CycleRevival;
        if (expected_to_match) CHECK(diff.empty());
    }
}

#endif  // PINEFORGE_L7B_HARVEST

}  // namespace

int main() {
#ifdef PINEFORGE_L7B_HARVEST
    harvest();
    return failures == 0 ? 0 : 1;
#else
    test("defaults are byte-identical", defaults_are_byte_identical);
    test("rounding snaps the level", rounding_snaps_the_level);
    test("rounding is validated at acceptance", rounding_is_validated_at_acceptance);
    test("rounding folds only when set", rounding_folds_only_when_set);
    test("hook restates the level", hook_restates_the_level);
    test("pending until armed hides the leg", pending_until_armed_hides_the_leg);
    test("pending leg is still addressable", pending_leg_is_still_addressable);
    test("visibility folds only when set", visibility_folds_only_when_set);
    test("C API working list agrees", c_api_working_list_agrees);
    test("facts resolve at the arm", facts_resolve_at_the_arm);
    test("twin of the R4 bracket scenarios", twin_of_the_r4_bracket_scenarios);
    std::printf("L7b native anchored legs: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
#endif
}
