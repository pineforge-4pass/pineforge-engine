// R5 lane F3: contracts the kernel documents for EVERY host, which until this
// lane only the Pine adapter honoured (AUDIT3 §3.1 "What breaks G1", §7 F3).
// Each section below is the witness of one of them. Each was run against the
// lane's base (fd785928) before its change landed and failed there for the
// reason its header names; the lane report carries those runs.
//
// Source-free: this TU runs in the kernel-only profile.
#include <pineforge/native_calendar.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/native_toolkit.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int passed = 0;
int failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++passed;                                                            \
        } else {                                                                 \
            ++failed;                                                            \
            std::fprintf(stderr, "CHECK FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

std::vector<std::string> split_paths(const char* joined) {
    std::vector<std::string> out;
    std::string current;
    for (const char* c = joined; *c; ++c) {
        if (*c == '|') {
            if (!current.empty()) out.push_back(current);
            current.clear();
        } else {
            current.push_back(*c);
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The code of a C/C++ file with its comments blanked out. String and character
// literals are kept whole, so a literal that looks like a comment stays code.
std::string code_only(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    enum class Mode { Code, Line, Block, String, Char } mode = Mode::Code;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';
        switch (mode) {
        case Mode::Code:
            if (c == '/' && next == '/') { mode = Mode::Line; ++i; continue; }
            if (c == '/' && next == '*') { mode = Mode::Block; ++i; continue; }
            if (c == '"') mode = Mode::String;
            if (c == '\'') mode = Mode::Char;
            out.push_back(c);
            break;
        case Mode::Line:
            if (c == '\n') { mode = Mode::Code; out.push_back(c); }
            break;
        case Mode::Block:
            if (c == '*' && next == '/') { mode = Mode::Code; ++i; }
            break;
        case Mode::String:
        case Mode::Char:
            out.push_back(c);
            if (c == '\\' && next != '\0') { out.push_back(next); ++i; continue; }
            if ((mode == Mode::String && c == '"') || (mode == Mode::Char && c == '\''))
                mode = Mode::Code;
            break;
        }
    }
    return out;
}

constexpr std::int64_t kT0 = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000;

NativeRunSpec base_spec(const char* key, std::uint64_t run_number = 1) {
    NativeRunSpec s;
    s.identity = {key, run_number};
    s.input_tf = "1";
    s.script_tf = "1";
    s.ticker = "MOCK";
    s.tickerid = "TEST:MOCK";
    s.type = "crypto";
    s.currency = "USD";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 10000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    return s;
}

std::vector<Bar> flat_tape(int n, double price = 100.0) {
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i)
        bars.push_back(Bar{price, price, price, price, 1.0, kT0 + i * kMinute});
    return bars;
}

bool typed(const NativeSetupResult& r, NativeRunSpecError error, NativeRunSpecField field) {
    return r.status == NativeSetupStatus::Failed && r.validation.error == error
        && r.validation.field == field;
}

// ─── item 6: configure_native refused for its PHASE answers WrongPhase ─────
// NativeRunSpecError::WrongPhase is "the CALL was refused before any field was
// judged, because the host was not in the phase that call is legal in. Only a
// setup call answers it" (native_run_spec.hpp). configure_native is the setup
// call, yet it answered None for a Ready or Running host and CalendarFailure
// for a Failed one. Fails at the base on all three.
struct Idle : NativeStrategyHost {
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

struct ConfiguresWhileRunning : NativeStrategyHost {
    std::optional<NativeSetupResult> inner;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (!inner) inner = configure_native(base_spec("f3-phase-running", 2));
    }
};

void configure_phase_refusal_is_wrong_phase() {
    {
        // Ready: the second call is refused, the host latches Contract (the
        // documented rule is unchanged), and the answer names the phase.
        Idle host;
        CHECK(host.configure_native(base_spec("f3-phase-ready")).status
              == NativeSetupStatus::Applied);
        const auto second = host.configure_native(base_spec("f3-phase-ready"));
        CHECK(typed(second, NativeRunSpecError::WrongPhase, NativeRunSpecField::None));
        const auto state = host.native_state();
        CHECK(state.kind == NativeLifecycleKind::Failed);
        CHECK(state.failure.code == NativeFailureCode::Contract);
    }
    {
        // Failed: a validation refusal fails the host; any later configure is
        // refused for the phase, not for a calendar.
        Idle host;
        NativeRunSpec bad = base_spec("f3-phase-failed");
        bad.initial_capital = 0.0;
        const auto first = host.configure_native(bad);
        CHECK(typed(first, NativeRunSpecError::NotFinitePositive,
                    NativeRunSpecField::InitialCapital));
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        const auto again = host.configure_native(base_spec("f3-phase-failed"));
        CHECK(typed(again, NativeRunSpecError::WrongPhase, NativeRunSpecField::None));
        CHECK(host.native_state().failure.code == NativeFailureCode::InvalidSpecification);
    }
    {
        // Running: a configure from inside a callback is refused for the
        // phase and latches Contract, as a Running refusal always did.
        ConfiguresWhileRunning host;
        CHECK(host.configure_native(base_spec("f3-phase-running")).status
              == NativeSetupStatus::Applied);
        const auto bars = flat_tape(3);
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.inner.has_value());
        if (host.inner) {
            CHECK(typed(*host.inner, NativeRunSpecError::WrongPhase, NativeRunSpecField::None));
        }
        const auto state = host.native_state();
        CHECK(state.kind == NativeLifecycleKind::Failed);
        CHECK(state.failure.code == NativeFailureCode::Contract);
    }
    {
        // The legal phases still apply: Unconfigured, and a Completed run
        // re-configured with a larger run number.
        Idle host;
        CHECK(host.configure_native(base_spec("f3-phase-legal")).status
              == NativeSetupStatus::Applied);
        const auto bars = flat_tape(2);
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        const auto next = host.configure_native(base_spec("f3-phase-legal", 2));
        CHECK(next.status == NativeSetupStatus::Applied);
        CHECK(next.validation.error == NativeRunSpecError::None);
    }
}

// ─── item 3: the report's magnifier flag is the run spec's intrabar path ───
// pf_report_t::bar_magnifier_enabled is "1 if magnifier was active for this
// run" (pineforge.h). The kernel runs that path whenever the spec declares one
// (NativeRunSpec::intrabar), but only the Pine adapter ever wrote the flag, so
// a bare host with a synthesized path read 0. Fails at the base on the two
// declared paths; the flag is re-derived per run, so a reused host that drops
// its path reads 0 again.
struct EntersOnce : NativeStrategyHost {
    int bars = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (bars++ == 0) (void)submit({no::Transact{1.0}, "entry", ""});
    }
};

int report_magnifier_flag(NativeStrategyHost& host) {
    ReportC report{};
    host.fill_report(&report);
    const int flag = report.bar_magnifier_enabled;
    BacktestEngine::free_report(&report);
    return flag;
}

void magnifier_flag_follows_the_spec() {
    std::vector<Bar> bars;
    for (int i = 0; i < 4; ++i) {
        const double p = 100.0 + i;
        bars.push_back(Bar{p, p + 2.0, p - 1.0, p + 1.0, 4.0, kT0 + i * 5 * kMinute});
    }
    const auto run = [&](NativeStrategyHost& host, const NativeRunSpec& spec) {
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        return report_magnifier_flag(host);
    };
    const auto five = [](const char* key, std::uint64_t run_number) {
        NativeRunSpec s = base_spec(key, run_number);
        s.input_tf = "5";
        s.script_tf = "5";
        return s;
    };
    {
        EntersOnce host;
        CHECK(run(host, five("f3-magnifier-none", 1)) == 0);
    }
    {
        EntersOnce host;
        NativeRunSpec s = five("f3-magnifier-synthesized", 1);
        IntrabarPath::synthesized path;
        path.samples = 4;
        s.intrabar.value = path;
        CHECK(run(host, s) == 1);
        // The same host, re-configured without a path: the flag is this
        // run's, not a leftover of the last one.
        CHECK(run(host, five("f3-magnifier-synthesized", 2)) == 0);
    }
    {
        EntersOnce host;
        NativeRunSpec s = five("f3-magnifier-lower", 1);
        IntrabarPath::lower_tf path;
        path.tf = "1";
        for (const Bar& bar : bars) {
            for (int m = 0; m < 5; ++m) {
                const double p = bar.open + 0.1 * m;
                path.bars.push_back(Bar{p, p + 0.5, p - 0.5, p, 1.0, bar.timestamp + m * kMinute});
            }
        }
        s.intrabar.value = path;
        CHECK(run(host, s) == 1);
    }
}

// ─── item 1: an armed contingent close records CloseCause::Bracket ─────────
// closed_trade_close_cause distinguishes "a script close, a bracket leg, a
// liquidation, a risk flatten and the range end" (pine-to-native.md), and a
// closer that knows why it closed records execution::CloseCause on the row.
// The kernel knows a bracket leg: a closing request its owner's fill armed
// (WaitForApplied -- the relation native_toolkit::submit_bracket builds, and
// the only one an anchored FromOwnerFill level may use). Yet only the Pine
// adapter ever wrote the bracket fact (Trade::exit_from_bracket), so a bare
// host's take-profit read 1 SCRIPT. Fails at the base on the two armed legs;
// the three boundary rows (a market close, an unowned resting stop, an armed
// OPENING transaction that reverses the book) read SCRIPT before and after.
struct Scripted : NativeStrategyHost {
    std::function<void(Scripted&, int)> script;
    std::optional<no::RequestHandle> entry;
    int bars = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        script(*this, bars++);
    }
};

std::vector<Bar> bracket_tape() {
    return {
        Bar{100.0, 100.0, 100.0, 100.0, 1.0, kT0},
        Bar{100.0, 102.0, 100.0, 101.8, 1.0, kT0 + kMinute},
        Bar{101.5, 103.0, 97.5, 100.8, 1.0, kT0 + 2 * kMinute},
        Bar{100.8, 100.8, 100.8, 100.8, 1.0, kT0 + 3 * kMinute},
    };
}

struct CauseRun {
    int trades = 0;
    std::string exit_id;
    int cause = -2;
    execution::CloseCause recorded = execution::CloseCause::Unspecified;
};

CauseRun run_cause(const char* key, std::function<void(Scripted&, int)> script) {
    Scripted host;
    host.script = std::move(script);
    CauseRun out;
    if (host.configure_native(base_spec(key)).status != NativeSetupStatus::Applied) return out;
    const auto bars = bracket_tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    out.trades = host.report_trade_count();
    if (out.trades > 0) {
        const Trade& row = host.get_report_trade(0);
        out.exit_id = row.exit_id;
        out.cause = host.closed_trade_close_cause(0);
        out.recorded = row.close_cause;
    }
    return out;
}

void enter_long(Scripted& host) {
    const auto placed = host.submit({no::Transact{1.0}, "entry", ""});
    CHECK(placed.status == no::SubmitStatus::Accepted && placed.handle.has_value());
    host.entry = placed.handle;
}

no::Request armed_close(const char* label, no::Trigger trigger, const no::RequestHandle& parent) {
    no::Request leg{no::Reduce{no::OwnerOpenedUnits{}}, label, ""};
    leg.trigger = trigger;
    leg.owner = no::WaitForApplied{parent};
    return leg;
}

void armed_close_records_bracket() {
    constexpr int kBracket = static_cast<int>(execution::CloseCause::Bracket);
    constexpr int kScript = static_cast<int>(execution::CloseCause::Script);
    {
        // The toolkit's bracket: take-profit +2 and stop-loss -2 anchored on
        // the entry's fill (bar 1's open, 100). Bar 2 reaches 102 first.
        const auto r = run_cause("f3-cause-bracket", [](Scripted& host, int bar) {
            if (bar != 0) return;
            enter_long(host);
            if (!host.entry) return;
            native_toolkit::BracketSpec spec;
            spec.parent = *host.entry;
            no::Request tp{no::Reduce{no::OwnerOpenedUnits{}}, "take-profit", ""};
            tp.trigger = no::Limit{0.0};
            tp.anchor = no::FromOwnerFill{+2.0};
            no::Request sl{no::Reduce{no::OwnerOpenedUnits{}}, "stop-loss", ""};
            sl.trigger = no::Stop{0.0};
            sl.anchor = no::FromOwnerFill{-2.0};
            spec.take_profit = tp;
            spec.stop_loss = sl;
            CHECK(native_toolkit::submit_bracket(host, spec).every_requested_leg_accepted());
        });
        CHECK(r.trades == 1);
        CHECK(r.exit_id == "take-profit");
        CHECK(r.cause == kBracket);
        CHECK(r.recorded == execution::CloseCause::Bracket);
    }
    {
        // An armed trail riding 1.0 behind the best: 102 on bar 1, so bar 2's
        // low crosses 101.
        const auto r = run_cause("f3-cause-trail", [](Scripted& host, int bar) {
            if (bar != 0) return;
            enter_long(host);
            if (!host.entry) return;
            no::Trail trail;
            trail.offset = 1.0;
            CHECK(host.submit(armed_close("trail", trail, *host.entry)).status
                  == no::SubmitStatus::Accepted);
        });
        CHECK(r.trades == 1);
        CHECK(r.exit_id == "trail");
        CHECK(r.cause == kBracket);
        CHECK(r.recorded == execution::CloseCause::Bracket);
    }
    {
        // A market close the host decided: SCRIPT.
        const auto r = run_cause("f3-cause-market", [](Scripted& host, int bar) {
            if (bar == 0) enter_long(host);
            if (bar == 1) (void)host.submit({no::Flatten{}, "flat", ""});
        });
        CHECK(r.trades == 1);
        CHECK(r.cause == kScript);
        CHECK(r.recorded == execution::CloseCause::Unspecified);
    }
    {
        // A resting stop with no owner relation is the host's own close, not
        // an armed leg: SCRIPT.
        const auto r = run_cause("f3-cause-unowned-stop", [](Scripted& host, int bar) {
            if (bar == 0) enter_long(host);
            if (bar == 1) {
                no::Request stop{no::Reduce{no::ExplicitUnits{1.0}}, "stop", ""};
                stop.trigger = no::Stop{99.0};
                CHECK(host.submit(stop).status == no::SubmitStatus::Accepted);
            }
        });
        CHECK(r.trades == 1);
        CHECK(r.exit_id == "stop");
        CHECK(r.cause == kScript);
    }
    {
        // An armed OPENING transaction (sell 2 against the long 1 its owner
        // opened) closes the long as a reversal, which is a script close.
        const auto r = run_cause("f3-cause-armed-reversal", [](Scripted& host, int bar) {
            if (bar != 0) return;
            enter_long(host);
            if (!host.entry) return;
            no::Request flip{no::Transact{-2.0}, "flip", ""};
            flip.trigger = no::Stop{98.0};
            flip.owner = no::WaitForApplied{*host.entry};
            CHECK(host.submit(flip).status == no::SubmitStatus::Accepted);
        });
        CHECK(r.trades >= 1);
        CHECK(r.exit_id == "flip");
        CHECK(r.cause == kScript);
    }
}

// ─── item 4: an owned excursion is recorded as the owner answered it ───────
// A host that owns its lots' excursions (RULING A48) answers the closing row's
// two magnitudes: "Facts in, magnitudes out" (native_host.hpp), "the
// magnitudes the closing row then carries" (native_c_api.h). The kernel
// post-processed the answer with TradingView's net-of-commission basis --
// favorable less the entry fee floored at 0, adverse plus the entry fee --
// after scaling it by point value and FX, so {0.5, 3.0} became {0.0, 4.0}
// (AUDIT3 probe_excursion_hook). Fails at the base; with point value 2 and a
// 1.0 cash fee the base records {0.0, 7.0}. The kernel's OWN model keeps its
// net basis: the second row pins it with hand arithmetic, before and after.
struct ExcursionRoundTrip : NativeStrategyHost {
    int bars = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const int index = bars++;
        if (index == 0) (void)submit({no::Transact{2.0}, "entry", ""});
        if (index == 2) (void)submit({no::Flatten{}, "exit", ""});
    }
};

struct OwnsExcursion final : ExcursionRoundTrip {
    mutable std::vector<ClosedLotExcursionFacts> facts;
    bool owns_lot_excursions() const noexcept override { return true; }
    ClosedLotExcursion closed_lot_excursion(const ClosedLotExcursionFacts& f) const override {
        facts.push_back(f);
        return {0.5, 3.0};
    }
};

void owned_excursion_is_recorded_verbatim() {
    // Flat bars at 100, 101, 104, 99, 102: the lot opens at bar 1's open (101)
    // and closes at bar 3's open (99).
    std::vector<Bar> bars;
    const double closes[] = {100.0, 101.0, 104.0, 99.0, 102.0};
    for (int i = 0; i < 5; ++i)
        bars.push_back(Bar{closes[i], closes[i], closes[i], closes[i], 1.0, kT0 + i * kMinute});
    const auto spec = [](const char* key) {
        NativeRunSpec s = base_spec(key);
        s.point_value = 2.0;
        s.fee_kind = NativeFeeKind::CashPerExecution;
        s.fee_value = 1.0;
        return s;
    };
    {
        OwnsExcursion host;
        CHECK(host.configure_native(spec("f3-excursion-owned")).status
              == NativeSetupStatus::Applied);
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.trade_count() == 1);
        if (host.trade_count() == 1) {
            const Trade& row = host.get_trade(0);
            CHECK(row.commission == 2.0);
            CHECK(row.max_runup == 0.5);
            CHECK(row.max_drawdown == 3.0);
        }
        // The owner is handed what it needs to apply a fee basis of its own:
        // the closed slice's share of the entry fee (the whole 1.0 ticket of
        // the one opening execution, for a full close).
        CHECK(!host.facts.empty());
        for (const ClosedLotExcursionFacts& f : host.facts) {
            CHECK(f.closed_qty == 2.0);
            CHECK(f.entry_commission == 1.0);
        }
    }
    {
        // The kernel's own sampler, 2 units from 101 on flat bars that print
        // 104 then 99: favorable (104-101)*2 = 6, adverse (101-99)*2 = 4 in
        // price points x quantity, then x point value 2 and the net basis:
        // max(0, 12 - 1) = 11 and 8 + 1 = 9.
        ExcursionRoundTrip host;
        CHECK(host.configure_native(spec("f3-excursion-kernel")).status
              == NativeSetupStatus::Applied);
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.trade_count() == 1);
        if (host.trade_count() == 1) {
            const Trade& row = host.get_trade(0);
            CHECK(row.entry_price == 101.0);
            CHECK(row.exit_price == 99.0);
            CHECK(row.max_runup == 11.0);
            CHECK(row.max_drawdown == 9.0);
        }
    }
}

// ─── item 2, first half: a per-point continuation read is linear in the feed
// Item 2 below reads the continuation at every report point. The continuation
// folds the spec's digest of its lower-timeframe path, of its declared series'
// bars and of its auxiliary feed, and each of those walked its whole bar array
// on every read, so a host reading the continuation once per bar with a
// lower-timeframe path paid a cost quadratic in the bar count (5 000 script
// bars over 25 000 path bars: 6.3 s recording on, 0.08 s off). The digests are
// now taken once per staged spec. The witness is the price of the reads, in
// process CPU time and best of three as tests/test_native_continuation_digest_tail.cpp
// measures: 2 000 bars read every bar must cost under three times the same run
// read once (about 1.3x once the digests are cached, about 40x while every read
// re-walked 10 000 path bars), and the value, which may not move: every-bar
// reads and a single read end on one digest.
struct LowerPathReader final : NativeStrategyHost {
    bool read_each_bar = false;
    std::uint64_t last_read = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (read_each_bar) last_read = native_continuation_hash();
    }
};

struct ReadCost {
    double seconds = 0.0;
    std::uint64_t digest = 0;
};

ReadCost lower_path_read_cost(int script_bars, bool read_each_bar) {
    std::vector<Bar> bars;
    IntrabarPath::lower_tf path;
    path.tf = "1";
    for (int i = 0; i < script_bars; ++i) {
        const double p = 100.0 + (i % 17) * 0.25;
        const std::int64_t open = kT0 + i * 5 * kMinute;
        bars.push_back(Bar{p, p + 1.0, p - 1.0, p + 0.5, 5.0, open});
        for (int m = 0; m < 5; ++m) {
            const double q = p + 0.1 * m;
            path.bars.push_back(Bar{q, q + 0.2, q - 0.2, q, 1.0, open + m * kMinute});
        }
    }
    NativeRunSpec s = base_spec("f3-lower-path-reads");
    s.input_tf = "5";
    s.script_tf = "5";
    s.intrabar.value = path;
    ReadCost best;
    for (int attempt = 0; attempt < 3; ++attempt) {
        LowerPathReader host;
        host.read_each_bar = read_each_bar;
        CHECK(host.configure_native(s).status == NativeSetupStatus::Applied);
        const std::clock_t start = std::clock();
        host.run(bars.data(), static_cast<int>(bars.size()));
        const double seconds = static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        if (attempt == 0 || seconds < best.seconds) best.seconds = seconds;
        best.digest = host.native_continuation_hash();
        if (read_each_bar) CHECK(host.last_read != 0);
    }
    return best;
}

void continuation_read_is_linear_in_the_feed() {
    const ReadCost every_bar = lower_path_read_cost(2000, true);
    const ReadCost once = lower_path_read_cost(2000, false);
    const double ratio = every_bar.seconds / std::max(once.seconds, 1e-6);
    std::printf("  2000 bars over a lower-timeframe path: continuation read every bar %.4f s,"
                " read once %.4f s, ratio %.2f\n", every_bar.seconds, once.seconds, ratio);
    CHECK(ratio < 3.0);
    CHECK(every_bar.digest == once.digest);
}

// ─── item 2: the last per-bar hash IS the run's final broker-state hash ─────
// pf_report_t::broker_state_hash: "When populated, len ==
// script_bars_processed and the last element equals
// #strategy_broker_state_hash's value at the end of the run" (pineforge.h),
// whether or not recording was on (strategy_broker_state_hash). A row folds
// the continuation at its report point; the scalar folded the continuation
// AFTER the run -- once the consumer had torn the run down into Completed --
// so the two differed for every bare host (AUDIT3: 7/7 configurations). Only
// the Pine adapter latched the continuation at its report points. Fails at
// the base in all eight runs below (seven batch shapes and a stream).
struct HashProbe : NativeStrategyHost {
    int bars = 0;
    int flat_at = 3;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bars;
        if (bars == 1) (void)submit({no::Transact{1.0}, "e", ""});
        if (bars == flat_at) (void)submit({no::Flatten{}, "x", ""});
    }
};

struct HashShape {
    bool open_at_end_row = false;
    bool keep_open = false;
    bool fills = false;
    bool grid = false;
    bool stream = false;
};

struct HashRun {
    std::vector<std::uint64_t> rows;
    std::uint64_t scalar = 0;
    std::int64_t script_bars = 0;
};

HashRun run_hash_shape(const HashShape& shape, bool record) {
    HashProbe host;
    if (shape.keep_open) host.flat_at = 999;
    NativeRunSpec s = base_spec("f3-hash");
    s.input_tf = "5";
    s.script_tf = "5";
    s.report_policy = NativeReportPolicy::KernelRecorded;
    s.report_open_position_at_end = shape.open_at_end_row;
    if (shape.fills) s.calculation = NativeCalculationTrigger::BarCloseAndFills;
    if (shape.grid) s.price_grid = NativePriceGrid::QuantizeFills;
    HashRun out;
    CHECK(host.configure_native(s).status == NativeSetupStatus::Applied);
    host.set_broker_state_hash_recording(record);
    std::vector<Bar> bars;
    for (int i = 0; i < 5; ++i)
        bars.push_back(Bar{100.0 + i, 101.0 + i, 99.0 + i, 100.5 + i, 1.0, kT0 + i * 5 * kMinute});
    if (shape.stream) {
        CHECK(host.stream_begin(bars.data(), 2, "5", "5"));
        for (int i = 2; i < 5; ++i) CHECK(host.stream_push_bar(bars[i]));
        CHECK(host.stream_end());
    } else {
        host.run(bars.data(), static_cast<int>(bars.size()));
    }
    CHECK(host.last_error().empty());
    ReportC report{};
    host.fill_report(&report);
    for (std::int64_t i = 0; i < report.broker_state_hash_len; ++i)
        out.rows.push_back(report.broker_state_hash[i]);
    out.script_bars = report.script_bars_processed;
    BacktestEngine::free_report(&report);
    out.scalar = host.broker_state_hash();
    return out;
}

void last_row_is_the_final_scalar() {
    const HashShape shapes[] = {
        {false, false, false, false, false},
        {false, true, false, false, false},
        {true, false, false, false, false},
        {true, true, false, false, false},
        {false, false, true, false, false},
        {false, false, false, true, false},
        {false, true, true, true, false},
        {false, false, false, false, true},
    };
    for (const HashShape& shape : shapes) {
        const HashRun recorded = run_hash_shape(shape, true);
        CHECK(recorded.script_bars == 5);
        CHECK(recorded.rows.size() == 5);
        if (!recorded.rows.empty()) {
            const bool equal = recorded.rows.back() == recorded.scalar;
            if (!equal) {
                std::fprintf(stderr,
                    "  shape open_at_end_row=%d keep_open=%d fills=%d grid=%d stream=%d:"
                    " last=%016llx scalar=%016llx\n",
                    shape.open_at_end_row, shape.keep_open, shape.fills, shape.grid,
                    shape.stream, static_cast<unsigned long long>(recorded.rows.back()),
                    static_cast<unsigned long long>(recorded.scalar));
            }
            CHECK(equal);
        }
        // Recording is reporting: the same run with the switch off ends on
        // the same scalar and records no row.
        const HashRun unrecorded = run_hash_shape(shape, false);
        CHECK(unrecorded.rows.empty());
        CHECK(unrecorded.scalar == recorded.scalar);
    }
}

// ─── item 5: a host command does not move the host's presented clock ───────
// E21/E21b made "the bar clock is never written" by a host command true for
// submit, replace and the placement gate. execute_current still wrote it:
// consume_matched_request stamps the presented clock with the execution
// cursor so its resolver, inspection and settlement convert at that instant,
// and nothing put it back. In a bar-open frame the cursor is the decision floor
// (the bar's close), so the command moved the host's clock by a bar, and with
// an FX step in between it moved the account rate and marked_equity the host
// reads inside the same callback (AUDIT3 probe_e21_preopen: +2 -> +3 min,
// marked_equity(110) 1150 -> 1375). Fails at the base. The execution itself
// still converts at its cursor: the new lot's fee is taken at the stepped rate
// before and after.
struct PreOpenCommand final : NativeStrategyHost {
    int bars = 0;
    int opens = 0;
    std::int64_t clock_before = 0;
    std::int64_t clock_after = 0;
    double fx_before = 0.0;
    double fx_after = 0.0;
    double equity_before = 0.0;
    double equity_after = 0.0;
    std::int64_t executed_at = 0;
    bool executed = false;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (bars++ == 0) (void)submit({no::Transact{15.0}, "entry", ""});
    }
    void on_native_bar_open(const Bar& bar, const NativeDecisionContext&) override {
        if (opens++ != 2) return;
        clock_before = current_bar_.timestamp;
        fx_before = active_account_currency_fx();
        equity_before = marked_equity(bar.open);
        const auto placed = submit({no::Transact{1.0}, "mkt", ""});
        if (!placed.handle) return;
        NativeCurrentExecution command;
        command.target = *placed.handle;
        const auto result = execute_current(command);
        if (const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&result)) {
            executed = true;
            executed_at = applied->cursor.point.effective_time_ms;
        }
        clock_after = current_bar_.timestamp;
        fx_after = active_account_currency_fx();
        equity_after = marked_equity(bar.open);
    }
};

void host_command_keeps_the_presented_clock() {
    std::vector<Bar> bars;
    for (int i = 0; i < 5; ++i) {
        const double p = i < 2 ? 100.0 : 110.0;
        bars.push_back(Bar{p, p, p, p, 1.0, kT0 + i * kMinute});
    }
    NativeRunSpec s = base_spec("f3-presented-clock");
    s.initial_capital = 1000.0;
    s.fee_kind = NativeFeeKind::Percent;
    s.fee_value = 0.1;
    PreOpenCommand host;
    CHECK(host.configure_native(s).status == NativeSetupStatus::Applied);
    CHECK(host.configure_native_fx_curve(NativeFxCurve{{kT0 + 3 * kMinute}, {2.5}}).status
          == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.executed);
    std::printf("  bar-open command: clock %+lld -> %+lld min, fx %.2f -> %.2f,"
                " marked_equity(110) %.6f -> %.6f, executed at %+lld min\n",
                static_cast<long long>((host.clock_before - kT0) / kMinute),
                static_cast<long long>((host.clock_after - kT0) / kMinute),
                host.fx_before, host.fx_after, host.equity_before, host.equity_after,
                static_cast<long long>((host.executed_at - kT0) / kMinute));
    // The bar-open frame of bar 2 presents +2 min; the command executes at the
    // decision floor, +3 min, where the rate has stepped to 2.5.
    CHECK(host.clock_before == kT0 + 2 * kMinute);
    CHECK(host.executed_at == kT0 + 3 * kMinute);
    CHECK(host.fx_before == 1.0);
    // 1000 of capital, 15 units bought at 100 for a 1.5 fee, marked at 110.
    CHECK(std::abs(host.equity_before - 1148.5) < 1e-9);
    CHECK(host.clock_after == host.clock_before);
    CHECK(host.fx_after == host.fx_before);
    // The execution converted at its own cursor: the 1-unit lot's 0.1 % fee on
    // 110 is taken at the stepped rate, 0.11 x 2.5. Marked at the presented
    // clock, the equity moved by exactly that fee (the new unit is flat at 110).
    bool found = false;
    for (const NativeOpenLot& lot : host.native_open_lots(110.0)) {
        if (lot.entry_label != "mkt") continue;
        found = true;
        CHECK(lot.entry_time_ms == kT0 + 3 * kMinute);
        CHECK(std::abs(lot.entry_commission - 0.275) < 1e-12);
        CHECK(std::abs(host.equity_after - (host.equity_before - lot.entry_commission)) < 1e-9);
    }
    CHECK(found);
}

// ─── item 9: a zone whose identity cannot be derived is refused (E23) ───────
// The continuation folds the run's timezone by its identity: the source kind,
// the effective definition and a digest of the zone resources the resolver
// read (native_calendar.hpp, TimezoneIdentityDescriptor). The descriptor is
// nullopt when a named resource cannot be read back or is larger than any zone
// file, or when a POSIX default-DST spec finds no posixrules -- and its own
// contract says "the caller refuses the descriptor rather than hashing a
// hole" (native_calendar.cpp). The consumer ran the spec anyway and folded a
// bare `false`, so two runs over DIFFERENT zone data shared one continuation
// (AUDIT3: two oversized New_York files, 8010893015232274070 both times). Only
// a libc that reads TZDIR lets a test present such a zone, so the refusal rows
// run on glibc (the Linux CI profiles); elsewhere the row pins that the run's
// own zone is derivable and runs. Fails at the base on glibc (the runs
// complete, and the two continuations agree).
struct ZoneProbe final : NativeStrategyHost {
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

struct ZoneRun {
    NativeLifecycleKind kind = NativeLifecycleKind::Unconfigured;
    NativeFailure failure{};
    std::string error;
    std::uint64_t continuation = 0;
};

ZoneRun run_in_zone(const char* key, const char* timezone) {
    ZoneProbe host;
    NativeRunSpec s = base_spec(key);
    s.timezone = timezone;
    ZoneRun out;
    if (host.configure_native(s).status != NativeSetupStatus::Applied) {
        out.error = "configure refused";
        out.kind = host.native_state().kind;
        return out;
    }
    const auto bars = flat_tape(3);
    host.run(bars.data(), static_cast<int>(bars.size()));
    const auto state = host.native_state();
    out.kind = state.kind;
    out.failure = state.failure;
    out.error = host.last_error();
    out.continuation = host.native_continuation_hash();
    return out;
}

#if defined(__GLIBC__)
bool write_zone_file(const std::string& path, std::size_t bytes, char fill) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write("TZif", 4);
    const std::string body(bytes - 4, fill);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    return static_cast<bool>(out);
}

bool refused_for_identity(const ZoneRun& run) {
    return run.kind == NativeLifecycleKind::Failed
        && run.failure.code == NativeFailureCode::Calendar
        && run.failure.operation == NativeFailureOperation::Begin
        && run.error.find("timezone identity") != std::string::npos;
}
#endif

void underivable_zone_identity_is_refused() {
    {
        // Wherever it runs: the zone this suite uses resolves to an identity,
        // and a run over it is not refused.
        CHECK(native_calendar::timezone_identity_descriptor("UTC").has_value());
        const ZoneRun utc = run_in_zone("f3-zone-utc", "UTC");
        CHECK(utc.kind == NativeLifecycleKind::Completed);
    }
#if defined(__GLIBC__)
    char pattern[] = "/tmp/f3-zone-XXXXXX";
    const char* root = mkdtemp(pattern);
    CHECK(root != nullptr);
    if (!root) return;
    const std::string base(root);
    const std::string two = base + "/two", three = base + "/three", bare = base + "/bare";
    for (const std::string& dir : {two, three, bare}) CHECK(mkdir(dir.c_str(), 0700) == 0);
    CHECK(mkdir((two + "/America").c_str(), 0700) == 0);
    CHECK(mkdir((three + "/America").c_str(), 0700) == 0);
    // Two different zone files for one name, both past the 1 MiB resource cap;
    // the two runs are otherwise the same spec over the same bars.
    CHECK(write_zone_file(two + "/America/New_York", std::size_t{2} << 20, '\x07'));
    CHECK(write_zone_file(three + "/America/New_York", std::size_t{3} << 20, '\x05'));
    // A root with a UTC file but no posixrules, for a POSIX default-DST spec.
    {
        std::ifstream in("/usr/share/zoneinfo/UTC", std::ios::binary);
        std::ofstream out(bare + "/UTC", std::ios::binary);
        out << in.rdbuf();
    }
    const char* previous = std::getenv("TZDIR");
    const std::string saved = previous ? previous : "";
    setenv("TZDIR", two.c_str(), 1);
    const ZoneRun oversized_two = run_in_zone("f3-zone-oversized", "America/New_York");
    setenv("TZDIR", three.c_str(), 1);
    const ZoneRun oversized_three = run_in_zone("f3-zone-oversized", "America/New_York");
    setenv("TZDIR", bare.c_str(), 1);
    const ZoneRun no_posixrules = run_in_zone("f3-zone-posix", "XYZ5ABC");
    if (previous) setenv("TZDIR", saved.c_str(), 1); else unsetenv("TZDIR");
    std::printf("  oversized zone (2 MiB): kind=%d code=%d op=%d continuation=%llu error='%s'\n",
                static_cast<int>(oversized_two.kind), static_cast<int>(oversized_two.failure.code),
                static_cast<int>(oversized_two.failure.operation),
                static_cast<unsigned long long>(oversized_two.continuation),
                oversized_two.error.c_str());
    std::printf("  oversized zone (3 MiB): kind=%d continuation=%llu\n",
                static_cast<int>(oversized_three.kind),
                static_cast<unsigned long long>(oversized_three.continuation));
    std::printf("  POSIX default-DST without posixrules: kind=%d error='%s'\n",
                static_cast<int>(no_posixrules.kind), no_posixrules.error.c_str());
    CHECK(refused_for_identity(oversized_two));
    CHECK(refused_for_identity(oversized_three));
    CHECK(refused_for_identity(no_posixrules));
    std::remove((two + "/America/New_York").c_str());
    std::remove((three + "/America/New_York").c_str());
    std::remove((bare + "/UTC").c_str());
    rmdir((two + "/America").c_str());
    rmdir((three + "/America").c_str());
    for (const std::string& dir : {two, three, bare}) rmdir(dir.c_str());
    rmdir(base.c_str());
#else
    std::printf("  zone-identity refusal rows need a libc that reads TZDIR (glibc)\n");
#endif
}

// ─── item 8: the adapter's `__close__` id prefix is not kernel code ─────────
// A strategy.close order id is the source adapter's own spelling. The kernel
// carried a copy of that prefix (`internal::kClosePrefix`) with no reader left
// anywhere in the tree: dead, Pine-shaped code in every object that includes
// the kernel's internal header. Fails at the base: engine_internal.hpp still
// defines it.
void close_prefix_is_not_kernel_code() {
    const auto files = split_paths(PINEFORGE_F3_KERNEL_FILES);
    CHECK(files.size() > 40);
    int readable = 0;
    for (const auto& path : files) {
        const std::string text = read_file(path);
        if (!text.empty()) ++readable;
        const std::string code = code_only(text);
        const bool carries = code.find("\"__close__\"") != std::string::npos;
        if (carries) std::fprintf(stderr, "  __close__ literal in kernel code: %s\n", path.c_str());
        CHECK(!carries);
    }
    CHECK(readable == static_cast<int>(files.size()));
}

}  // namespace

int main() {
    close_prefix_is_not_kernel_code();
    configure_phase_refusal_is_wrong_phase();
    magnifier_flag_follows_the_spec();
    armed_close_records_bracket();
    owned_excursion_is_recorded_verbatim();
    continuation_read_is_linear_in_the_feed();
    last_row_is_the_final_scalar();
    host_command_keeps_the_presented_clock();
    // Last: it points TZDIR at scratch trees, and the zone cache is process-wide.
    underivable_zone_identity_is_refused();
    std::printf("test_native_bare_host_contracts: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
