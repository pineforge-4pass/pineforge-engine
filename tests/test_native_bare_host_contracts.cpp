// R5 lane F3: contracts the kernel documents for EVERY host, which until this
// lane only the Pine adapter honoured (AUDIT3 §3.1 "What breaks G1", §7 F3).
// Each section below is the witness of one of them. Each was run against the
// lane's base (fd785928) before its change landed and failed there for the
// reason its header names; the lane report carries those runs.
//
// Source-free: this TU runs in the kernel-only profile.
#include <pineforge/native_host.hpp>
#include <pineforge/native_toolkit.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

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
    std::printf("test_native_bare_host_contracts: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
