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
#include <pineforge/native_toolkit.hpp>

#include "native_current_fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
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
    std::printf("L7b native anchored legs: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
#endif
}
