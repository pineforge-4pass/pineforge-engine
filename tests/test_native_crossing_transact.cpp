// R5 lane B-ENGINE (K-ULP1): a Transact that crosses a fractional position is
// charged exactly its units.
//
// The settlement closes the opposite book and opens the rest on the request's
// side, computed in binary64 as units - closed (next_close_split's
// `requested - closed`, taken as the opening). closed + |opened| need not
// recombine to the units. When the sum rounded one ulp above them,
// prepare_execution and its direct twin execution_values refused the fill as
// larger than the working units, and the run stopped with "native
// working-request preparation failed" (code 6, discriminator 9). When it
// rounded one ulp below, the request kept a 2^-49-unit remainder, and a second
// fill opened it as a dust lot.
//
// Every case crosses once and must complete with one lot, whose units are the
// settlement's own opening, and one fill of the crossing request: terminal,
// filled_working equal to the units, remaining_after 0. The C++ cases run on
// the staged path and on the direct one, which must agree bit for bit.
//
//   up-explicit     short c = fl(800/80.25) - 5 met by Transact{+fl(1990/84.5)}
//   up-sized        the same short met by Sized{Long, CashValue{1990}} at 84.5
//   down-explicit   short 2.896242212933992 met by Transact{+15.864594529433946}
//   exact-control   the short c met by Transact{+fl(1990/85.25)}, which
//                   recombines exactly (it passed before the fix)
//   host-sized      HostSized{Open, Long}, the host answering the up units with
//                   the Transact shape
//   two-lot-up      two short lots whose own sum rounds, met by a crossing
//   two-lot-down    Transact that rounds up / down (the chain settlement)
//   mirrored-up     the up case from long to short
//   c-host-up       the up and down cases through strategy_native_submit_v1
//   c-host-down
//
// Fail-before: on 8cf3be58 the up cases stop Failed with code 6 and
// discriminator 9, and the down cases complete with two lots and a second fill
// of 2^-49 units; the lane report records the first failing check.
// Source-free: the kernel-only profile registers the row.
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_c_api.h>
#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace pineforge;
namespace no = pineforge::native_order;

int checks = 0;
int failures = 0;
std::string scenario = "setup";

#define CHECK(condition) do {                                                    \
    ++checks;                                                                    \
    if (!(condition)) {                                                          \
        ++failures;                                                              \
        std::printf("FAIL [%s] line %d: %s\n", scenario.c_str(), __LINE__,      \
                    #condition);                                                 \
    }                                                                            \
} while (0)

constexpr double kPrice = 84.5;
constexpr int kBars = 6;

enum class Cross { Explicit, Sized, HostSized };

struct Case {
    const char* name;
    std::vector<double> lots;  // units of each opening lot, all on the side the cross leaves
    double units;              // what the crossing request trades
    Cross cross = Cross::Explicit;
    bool buys = true;          // the cross buys (the book is short) or sells
};

// The book a crossing leaves, as the settlement computes it: the lots close
// whole in FIFO order and the opening is the units minus the closed sum.
double closed_of(const Case& c) {
    double closed = 0.0;
    for (double lot : c.lots) closed += lot;
    return closed;
}
double opened_of(const Case& c) { return c.units - closed_of(c); }

const char* recombination(const Case& c) {
    const double sum = closed_of(c) + opened_of(c);
    return sum > c.units ? "one ulp ABOVE" : sum < c.units ? "one ulp BELOW" : "exact";
}

NativeRunSpec spec_for(const char* key) {
    NativeRunSpec s;
    s.event_retention = NativeEventRetention::Full;
    s.identity = {key, 1};
    s.input_tf = "1";
    s.script_tf = "1";
    s.tickerid = "B-ENGINE:K-ULP1";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 10'000'000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.25;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    return s;
}

std::vector<Bar> flat_bars() {
    std::vector<Bar> bars;
    for (int i = 0; i < kBars; ++i) {
        bars.push_back(Bar{kPrice, kPrice, kPrice, kPrice, 10.0, 60'000LL * (i + 1)});
    }
    return bars;
}

// What one run left: the lifecycle, the book, and every fill of the crossing
// request.
struct Fill {
    double closed = 0.0, opened = 0.0, filled_working = 0.0, remaining_after = -1.0;
    bool terminal = false;
    bool operator==(const Fill& o) const {
        return closed == o.closed && opened == o.opened && filled_working == o.filled_working
            && remaining_after == o.remaining_after && terminal == o.terminal;
    }
};
struct Outcome {
    bool completed = false;
    unsigned failure_code = 0, discriminator = 0;
    double signed_units = 0.0;
    std::size_t lots = 0;
    int trades = 0;
    std::size_t working = 0;
    std::vector<Fill> fills;
    std::uint64_t continuation = 0, broker = 0;
    bool operator==(const Outcome& o) const {
        return completed == o.completed && failure_code == o.failure_code
            && discriminator == o.discriminator && signed_units == o.signed_units
            && lots == o.lots && trades == o.trades && working == o.working
            && fills == o.fills && continuation == o.continuation && broker == o.broker;
    }
};

class Host final : public NativeStrategyHost {
public:
    Host(const Case& c, bool direct) : case_(c) {
        as_native_consumer(execution_consumer()).set_direct_mutation(direct);
    }

    void on_native_run_begin() override {
        bar_ = 0;
        for (double lot : case_.lots) {
            no::Request open{no::Transact{case_.buys ? -lot : lot}, "open", ""};
            submit(open);
        }
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (bar_++ != 0) return;
        const no::Side side = case_.buys ? no::Side::Long : no::Side::Short;
        if (case_.cross == Cross::Explicit) {
            no::Request cross{no::Transact{case_.buys ? case_.units : -case_.units}, "cross", ""};
            submit(cross);
        } else if (case_.cross == Cross::Sized) {
            no::Sized sized;
            sized.side = side;
            sized.basis = no::CashValue{1990.0};
            no::Request cross{sized, "cross", ""};
            submit(cross);
        } else {
            no::Request cross{no::HostSized{no::HostSizedKind::Open, side}, "cross", ""};
            submit(cross);
        }
    }

    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        no::ExecutionTerms terms{facts.default_resolved_price, std::nullopt,
                                 no::OpeningShape::Transact};
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            terms.units = case_.units;
        }
        return terms;
    }

private:
    Case case_;
    int bar_ = 0;
};

Outcome run_cpp(const Case& c, bool direct) {
    Host host(c, direct);
    Outcome out;
    const auto setup = host.configure_native(spec_for(c.name));
    CHECK(setup.status == NativeSetupStatus::Applied);
    const auto bars = flat_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));
    const auto state = host.native_state();
    out.completed = state.kind == NativeLifecycleKind::Completed;
    out.failure_code = static_cast<unsigned>(state.failure.code);
    out.discriminator = static_cast<unsigned>(state.failure.discriminator);
    const auto position = host.physical_position();
    out.signed_units = position.signed_units;
    out.lots = position.lot_count;
    out.trades = host.trade_count();
    out.working = host.native_working_requests().size();
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        const auto* e = std::get_if<no::ExecutionAppliedEvent>(&*row.command);
        if (!e || e->request().label != "cross") continue;
        Fill f;
        f.closed = e->closed_units;
        f.opened = e->opened_units;
        f.filled_working = e->filled_working;
        f.terminal = e->terminal;
        if (const auto* after = std::get_if<no::RemainingProjectionUnits>(&e->remaining_after)) {
            f.remaining_after = after->q;
        }
        out.fills.push_back(f);
    }
    out.continuation = host.native_continuation_hash();
    out.broker = host.broker_state_hash();
    return out;
}

void print_outcome(const char* path, const Outcome& o) {
    std::printf("  %-6s completed=%d code=%u discriminator=%u position=%a lots=%zu trades=%d "
                "working=%zu\n", path, o.completed ? 1 : 0, o.failure_code, o.discriminator,
                o.signed_units, o.lots, o.trades, o.working);
    for (const auto& f : o.fills) {
        std::printf("         cross fill: closed=%a opened=%a filled_working=%a terminal=%d "
                    "remaining_after=%a\n", f.closed, f.opened, f.filled_working,
                    f.terminal ? 1 : 0, f.remaining_after);
    }
}

// The outcome every crossing case must reach.
void judge(const Case& c, const Outcome& o) {
    const double opened = opened_of(c);
    CHECK(o.completed);
    CHECK(o.failure_code == 0);
    CHECK(o.lots == 1);
    CHECK(o.signed_units == (c.buys ? opened : -opened));
    CHECK(o.trades == static_cast<int>(c.lots.size()));
    CHECK(o.working == 0);
    CHECK(o.fills.size() == 1);
    if (o.fills.size() != 1) return;
    const Fill& f = o.fills.front();
    CHECK(f.closed == closed_of(c));
    CHECK(f.opened == (c.buys ? opened : -opened));
    CHECK(f.filled_working == c.units);
    CHECK(f.terminal);
    CHECK(f.remaining_after == 0.0);
}

void print_case(const Case& c) {
    const double closed = closed_of(c);
    const double opened = opened_of(c);
    std::printf("%s: closed=%a units=%a fl(units-closed)=%a fl(closed+opened)=%a (%s)\n",
                c.name, closed, c.units, opened, closed + opened, recombination(c));
}

void run_cpp_case(const Case& c) {
    scenario = c.name;
    print_case(c);
    const Outcome staged = run_cpp(c, false);
    const Outcome direct = run_cpp(c, true);
    print_outcome("staged", staged);
    print_outcome("direct", direct);
    judge(c, staged);
    judge(c, direct);
    CHECK(staged == direct);
}

// ---- The C host ------------------------------------------------------------

struct CHost {
    pf_strategy_t host = nullptr;
    const Case* c = nullptr;
    int bar = 0;
    int error = 0;
    std::uint64_t cross = 0;
    std::vector<pf_native_applied_v1> fills;
};

pf_native_request_v1 transact(double signed_units, const char* label) {
    pf_native_request_v1 request;
    std::memset(&request, 0, sizeof(request));
    request.struct_size = static_cast<std::uint32_t>(sizeof(request));
    request.version = PF_NATIVE_API_VERSION;
    request.intent = PF_NATIVE_INTENT_TRANSACT;
    request.intent_value = signed_units;
    request.label = label;
    return request;
}

int c_on_run_begin(void* user) {
    auto& s = *static_cast<CHost*>(user);
    for (double lot : s.c->lots) {
        const auto request = transact(s.c->buys ? -lot : lot, "open");
        const int rc = strategy_native_submit_v1(s.host, &request, nullptr, nullptr);
        if (rc != PF_NATIVE_OK && s.error == 0) s.error = rc;
    }
    return 0;
}

int c_on_bar(void* user, const pf_bar_t*, const pf_native_decision_v1*) {
    auto& s = *static_cast<CHost*>(user);
    if (s.bar++ != 0) return 0;
    const auto request = transact(s.c->buys ? s.c->units : -s.c->units, "cross");
    const int rc = strategy_native_submit_v1(s.host, &request, &s.cross, nullptr);
    if (rc != PF_NATIVE_OK && s.error == 0) s.error = rc;
    return 0;
}

int c_on_applied(void* user, const pf_native_applied_v1* applied, const pf_native_decision_v1*) {
    auto& s = *static_cast<CHost*>(user);
    if (s.cross != 0 && applied->incarnation == s.cross) s.fills.push_back(*applied);
    return 0;
}

void run_c_case(const Case& c) {
    scenario = c.name;
    print_case(c);
    CHost s;
    s.c = &c;
    pf_native_callbacks_v1 table;
    std::memset(&table, 0, sizeof(table));
    table.struct_size = static_cast<std::uint32_t>(sizeof(table));
    table.version = PF_NATIVE_API_VERSION;
    table.user = &s;
    table.on_run_begin = c_on_run_begin;
    table.on_bar = c_on_bar;
    table.on_applied = c_on_applied;
    s.host = strategy_native_host_create_v1(&table);
    CHECK(s.host != nullptr);
    if (!s.host) return;

    pf_native_run_spec_v1 spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.struct_size = static_cast<std::uint32_t>(sizeof(spec));
    spec.session_key = c.name;
    spec.run_number = 1;
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "K-ULP1";
    spec.tickerid = "B-ENGINE:K-ULP1";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "";
    spec.description = "";
    spec.volumetype = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "";
    spec.initial_capital = 10'000'000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.25;
    spec.allowed_open_directions = 3;
    CHECK(strategy_configure_native_v1(s.host, &spec) == 0);

    std::vector<pf_bar_t> bars(kBars);
    for (int i = 0; i < kBars; ++i) {
        bars[i].open = bars[i].high = bars[i].low = bars[i].close = kPrice;
        bars[i].volume = 10.0;
        bars[i].timestamp = 60'000LL * (i + 1);
    }
    const int rc = strategy_native_run_v1(s.host, bars.data(), kBars, nullptr);
    pf_native_state_v1 state;
    std::memset(&state, 0, sizeof(state));
    state.struct_size = static_cast<std::uint32_t>(sizeof(state));
    CHECK(strategy_native_state_v1(s.host, &state) == PF_NATIVE_OK);
    double signed_units = 0.0, average = 0.0;
    std::uint64_t lots = 0;
    CHECK(strategy_native_position_v1(s.host, &signed_units, &average, &lots) == PF_NATIVE_OK);
    std::printf("  c-host completed=%d lifecycle=%u code=%u position=%a lots=%llu\n",
                rc == PF_NATIVE_OK ? 1 : 0, state.lifecycle, state.failure_code, signed_units,
                static_cast<unsigned long long>(lots));
    for (const auto& f : s.fills) {
        std::printf("         cross fill: closed=%a opened=%a filled_working=%a terminal=%d\n",
                    f.closed_units, f.opened_units, f.filled_working, f.terminal);
    }

    const double opened = opened_of(c);
    CHECK(s.error == 0);
    CHECK(rc == PF_NATIVE_OK);
    CHECK(state.lifecycle == PF_NATIVE_LIFECYCLE_COMPLETED);
    CHECK(lots == 1);
    CHECK(signed_units == (c.buys ? opened : -opened));
    CHECK(s.fills.size() == 1);
    if (s.fills.size() == 1) {
        CHECK(s.fills.front().closed_units == closed_of(c));
        CHECK(s.fills.front().filled_working == c.units);
        CHECK(s.fills.front().terminal == 1);
    }
    strategy_native_host_free(s.host);
}

}  // namespace

int main() {
    // L3's seed-8 arithmetic (TRIAGE1 A1): c = fl(800/80.25) - 5, exact by
    // Sterbenz; U = fl(1990/84.5); fl(U - c) + c ties to even one ulp above U.
    const double c_up = 800.0 / 80.25 - 5.0;
    const double u_up = 1990.0 / kPrice;
    const std::vector<Case> cpp_cases = {
        {"up-explicit", {c_up}, u_up, Cross::Explicit, true},
        {"up-sized", {c_up}, u_up, Cross::Sized, true},
        {"down-explicit", {2.896242212933992}, 15.864594529433946, Cross::Explicit, true},
        {"exact-control", {c_up}, 1990.0 / 85.25, Cross::Explicit, true},
        {"host-sized", {c_up}, u_up, Cross::HostSized, true},
        // Lots of 100 and 133 (103 and 100) cash at 84.5: their closed sum
        // rounds, and so does the crossing that follows it.
        {"two-lot-up", {100.0 / kPrice, 133.0 / kPrice}, 2946.0 / kPrice, Cross::Explicit, true},
        {"two-lot-down", {103.0 / kPrice, 100.0 / kPrice}, 882.0 / kPrice, Cross::Explicit, true},
        {"mirrored-up", {c_up}, u_up, Cross::Explicit, false},
    };
    for (const auto& c : cpp_cases) run_cpp_case(c);
    // The two-lot cases are the chain's arithmetic, not the one lot's: the
    // closed part is itself a rounded sum.
    CHECK(closed_of(cpp_cases[5]) - cpp_cases[5].lots[0] != cpp_cases[5].lots[1]
          || closed_of(cpp_cases[5]) - cpp_cases[5].lots[1] != cpp_cases[5].lots[0]);
    CHECK(std::string(recombination(cpp_cases[0])) == "one ulp ABOVE");
    CHECK(std::string(recombination(cpp_cases[2])) == "one ulp BELOW");
    CHECK(std::string(recombination(cpp_cases[3])) == "exact");
    CHECK(std::string(recombination(cpp_cases[5])) == "one ulp ABOVE");
    CHECK(std::string(recombination(cpp_cases[6])) == "one ulp BELOW");

    const std::vector<Case> c_cases = {
        {"c-host-up", {c_up}, u_up, Cross::Explicit, true},
        {"c-host-down", {2.896242212933992}, 15.864594529433946, Cross::Explicit, true},
    };
    for (const auto& c : c_cases) run_c_case(c);

    std::printf("test_native_crossing_transact: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
