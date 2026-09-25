// R5 lane K-ULP2: a close that spans several lots and ends inside one of them
// is charged exactly its request.
//
// The settlement closes lots in FIFO order (next_close_split,
// src/engine_execution.cpp). Every lot before the one the request ends in
// closes whole, and C is the binary64 sum of what they closed. The lot the
// request ends in -- the absorbing lot -- closes the rest, r = fl(U - C), the
// binary64 value nearest U - C. The closed total was fl(C + r), which can land
// one ulp away from U:
//   above  next_close_split refused the split (`next_remaining < 0`) and the
//          run stopped with "native settlement inspection failed" (code 6,
//          discriminator 5, UnrepresentableQuantity), for a Reduce and for a
//          Transact that closes part of the book;
//   below  the request kept a 2^-51 remainder: a Reduce closed it with a
//          second fill, a later lot gave it up in the same fill, and for a
//          Transact it was an opening beside a surviving lot, which the
//          settlement refuses (code 6, discriminator 5).
// No binary64 row can make that sum U: when fl(C + r) != U, C + r is a tie on
// U's grid that rounds to U's even neighbour, and fl(C + x) steps over U
// between x = prev(r) and x = next(r). Every inexact case checks that. So the
// absorbing lot closes r and the close's total is the request itself.
//
// Every case completes with one terminal fill of the close request: closed
// units and filled_working equal to the request, remaining_after 0. The rows
// are the whole lots, then r; the absorbing lot keeps fl(q - r) (or closes
// whole when r is its full size); every other lot is untouched. The C++ cases
// run on the request core's staged and direct paths, which must agree bit for
// bit, hashes included.
//
//   up-reduce, up-transact       long {fl(106/84.5), 10}, U = fl(277/84.5)
//   down-reduce, down-transact   long {fl(121/84.5), 10}, U = fl(334/84.5)
//   mirrored-up                  the up case short, closed by Transact{+U}
//   three-lot-up                 {fl(90/84.5), fl(94/84.5), 10}, Reduce{fl(523/84.5)}
//   three-lot-down               {fl(90/84.5), fl(96/84.5), 10}, Transact{-fl(526/84.5)}
//   three-lot-mid                {fl(90/84.5), 10, 7}, Reduce{fl(770/84.5)}: the
//                                request ends in the second lot, and the third
//                                lot gave up 2^-49 in the same fill
//   whole-up                     {fl(92/84.5), fl(171/84.5), 10}, U one ulp below
//                                the first two lots' sum: fl(U - a) is the second
//                                lot's full size, so it closes whole
//   scoped-up, scoped-down       BindOpenings over the first and third of three
//                                lots; the middle one is not touched
//   current-up                   up-reduce through execute_current
//   exact-control                long {fl(106/84.5), 10}, U = fl(280/84.5), which
//                                adds up exactly (the same before the fix)
//   c-host-up, c-host-down       the up Reduce and the down Transact through
//                                strategy_native_submit_v1
//   engine-opening-scope         the settlement's own inspect, project and
//                                settle on an opening scope of two lots with one
//                                incarnation around a lot of another
//   battery                      2,000 seeded books of 2-4 lots sized from cash
//                                at tick prices, closed by a Reduce or a
//                                Transact that ends inside a later lot
//   genuine-*                    requests no binary64 split can carry: a
//                                reduction that cannot move the position, a
//                                rest below half an ulp of its lot, a rest that
//                                a whole lot cannot decrement. They still fail
//                                with code 6, discriminator 5, and move nothing.
//
// Fail-before: on db98990c every up case stops with code 6 / discriminator 5,
// every down Reduce books a second 2^-51 fill, every down Transact stops, and
// three-lot-mid books a third row of 2^-49; the lane report records the first
// failing check. Source-free: the kernel-only profile registers the row.
#include "../src/engine_internal.hpp"
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_c_api.h>
#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace pineforge;
namespace no = pineforge::native_order;
namespace ex = pineforge::execution;

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

double prev_of(double x) { return std::nextafter(x, -INFINITY); }
double next_of(double x) { return std::nextafter(x, INFINITY); }

// ---- The split the settlement must book -------------------------------------

// Walks the members of the book in FIFO order as the settlement does: a lot
// the rest exceeds closes whole and the rest is recomputed from the sum so
// far; the first lot the rest does not exceed -- or a whole lot whose sum
// reaches the request -- absorbs it and ends the close.
struct Split {
    std::vector<double> rows;       // the closed quantity of each closed lot
    std::vector<double> survivors;  // the book after the close, FIFO
    std::size_t absorbing = 0;      // the lot the request ends in
    double preceding = 0.0;         // C: the sum of the whole lots before it
    double rest = 0.0;              // r = fl(U - C)
    bool ended = false;
    bool inexact() const { return preceding + rest != units; }
    double units = 0.0;
};

Split split_of(const std::vector<double>& lots, const std::vector<bool>& member,
               double units) {
    Split s;
    s.units = units;
    double closed = 0.0;
    for (std::size_t i = 0; i < lots.size(); ++i) {
        if (s.ended || !member[i]) {
            s.survivors.push_back(lots[i]);
            continue;
        }
        const double rest = units - closed;
        const double amount = std::min(lots[i], rest);
        const double sum = closed + amount;
        s.rows.push_back(amount);
        if (amount < lots[i] || sum >= units) {
            s.ended = true;
            s.absorbing = i;
            s.preceding = closed;
            s.rest = amount;
            if (amount < lots[i]) s.survivors.push_back(lots[i] - amount);
            continue;
        }
        closed = sum;
    }
    return s;
}

double fold(const std::vector<double>& values) {
    double sum = 0.0;
    for (double v : values) sum += v;
    return sum;
}

// No binary64 row beside r makes the rows add up to U either: fl(C + x) steps
// over U between prev(r) and next(r).
void check_no_exact_row(const Split& s) {
    const double below = s.preceding + prev_of(s.rest);
    const double above = s.preceding + next_of(s.rest);
    CHECK(below != s.units);
    CHECK(above != s.units);
    CHECK(s.preceding + s.rest != s.units);
    CHECK(below < s.units && above > s.units);
}

void print_split(const char* name, const Split& s) {
    const double sum = s.preceding + s.rest;
    std::printf("%s: U=%a C=%a r=fl(U-C)=%a fl(C+r)=%a (%s) absorbing lot %zu\n", name,
                s.units, s.preceding, s.rest, sum,
                sum > s.units ? "one ulp ABOVE" : sum < s.units ? "one ulp BELOW" : "exact",
                s.absorbing);
}

// ---- A bare C++ host ----------------------------------------------------------

enum class Close { Reduce, Transact };

struct Case {
    const char* name;
    std::vector<double> lots;      // the book, FIFO; every lot on one side
    double units;                  // what the close request takes
    Close close = Close::Reduce;
    bool long_book = true;
    std::vector<bool> scope;       // BindOpenings members; empty = the book
    bool current = false;          // run the close through execute_current
    double price = kPrice;
    double tick = 0.25;
};

std::vector<bool> members_of(const Case& c) {
    return c.scope.empty() ? std::vector<bool>(c.lots.size(), true) : c.scope;
}

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
    std::vector<double> lots;
    std::vector<double> rows;
    std::size_t working = 0;
    std::vector<Fill> fills;
    std::uint64_t continuation = 0, broker = 0;
    bool operator==(const Outcome& o) const {
        return completed == o.completed && failure_code == o.failure_code
            && discriminator == o.discriminator && signed_units == o.signed_units
            && lots == o.lots && rows == o.rows && working == o.working
            && fills == o.fills && continuation == o.continuation && broker == o.broker;
    }
};

NativeRunSpec spec_for(const char* key, double tick) {
    NativeRunSpec s;
    s.event_retention = NativeEventRetention::Full;
    s.identity = {key, 1};
    s.input_tf = "1";
    s.script_tf = "1";
    s.tickerid = "K-ULP2:SPLIT";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 10'000'000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = tick;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    return s;
}

std::vector<Bar> flat_bars(double price) {
    std::vector<Bar> bars;
    for (int i = 0; i < kBars; ++i) {
        bars.push_back(Bar{price, price, price, price, 10.0, 60'000LL * (i + 1)});
    }
    return bars;
}

class Host final : public NativeStrategyHost {
public:
    Host(const Case& c, bool direct) : case_(c) {
        as_native_consumer(execution_consumer()).set_direct_mutation(direct);
    }

    void on_native_run_begin() override {
        placed_ = false;
        openings_.clear();
        for (double lot : case_.lots) {
            no::Request open{no::Transact{case_.long_book ? lot : -lot}, "open", ""};
            const auto out = submit(open);
            if (out.handle) openings_.push_back(*out.handle);
        }
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        // The close is placed once every lot is booked, so a scope can bind them.
        if (placed_ || pyramid_entries_.size() != case_.lots.size()) return;
        placed_ = true;
        no::Request close{no::Transact{case_.long_book ? -case_.units : case_.units},
                          "close", ""};
        if (case_.close == Close::Reduce) {
            close = no::Request{no::Reduce{no::ExplicitUnits{case_.units}}, "close", ""};
        }
        if (!case_.scope.empty()) {
            std::vector<no::RequestHandle> bound;
            for (std::size_t i = 0; i < case_.scope.size(); ++i) {
                if (case_.scope[i] && i < openings_.size()) bound.push_back(openings_[i]);
            }
            close.owner = no::BindOpenings{bound, position_cycle_seq_};
        }
        const auto out = submit(close);
        if (case_.current && out.handle) {
            try {
                current_ = execute_current(NativeCurrentExecution{*out.handle});
            } catch (const std::exception&) {
                current_threw_ = true;
            }
        }
    }

    std::vector<double> lot_units() const {
        std::vector<double> out;
        for (const auto& lot : pyramid_entries_) out.push_back(lot.qty);
        return out;
    }
    std::vector<double> row_units() const {
        std::vector<double> out;
        for (const auto& trade : trades_) out.push_back(trade.qty);
        return out;
    }

    std::optional<NativeCurrentExecutionResult> current_;
    bool current_threw_ = false;

private:
    Case case_;
    bool placed_ = false;
    std::vector<no::RequestHandle> openings_;
};

Outcome run_cpp(const Case& c, bool direct, Host** keep = nullptr) {
    static std::vector<std::unique_ptr<Host>> kept;
    auto host = std::make_unique<Host>(c, direct);
    Outcome out;
    const auto setup = host->configure_native(spec_for(c.name, c.tick));
    CHECK(setup.status == NativeSetupStatus::Applied);
    const auto bars = flat_bars(c.price);
    host->run(bars.data(), static_cast<int>(bars.size()));
    const auto state = host->native_state();
    out.completed = state.kind == NativeLifecycleKind::Completed;
    out.failure_code = static_cast<unsigned>(state.failure.code);
    out.discriminator = static_cast<unsigned>(state.failure.discriminator);
    out.signed_units = host->physical_position().signed_units;
    out.lots = host->lot_units();
    out.rows = host->row_units();
    out.working = host->native_working_requests().size();
    for (const auto& row : host->native_events(0)) {
        if (!row.command) continue;
        const auto* e = std::get_if<no::ExecutionAppliedEvent>(&*row.command);
        if (!e || e->request().label != "close") continue;
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
    out.continuation = host->native_continuation_hash();
    out.broker = host->broker_state_hash();
    if (keep) {
        *keep = host.get();
        kept.push_back(std::move(host));
    }
    return out;
}

void print_outcome(const char* path, const Outcome& o) {
    std::printf("  %-6s completed=%d code=%u discriminator=%u position=%a lots=%zu rows=%zu "
                "working=%zu\n", path, o.completed ? 1 : 0, o.failure_code, o.discriminator,
                o.signed_units, o.lots.size(), o.rows.size(), o.working);
    for (const auto& f : o.fills) {
        std::printf("         close fill: closed=%a opened=%a filled_working=%a terminal=%d "
                    "remaining_after=%a\n", f.closed, f.opened, f.filled_working,
                    f.terminal ? 1 : 0, f.remaining_after);
    }
    for (double row : o.rows) std::printf("         row %a\n", row);
}

// The outcome every case must reach.
void judge(const Case& c, const Split& s, const Outcome& o) {
    CHECK(o.completed);
    CHECK(o.failure_code == 0);
    CHECK(o.working == 0);
    CHECK(o.fills.size() == 1);
    if (o.fills.size() == 1) {
        const Fill& f = o.fills.front();
        CHECK(f.closed == c.units);
        CHECK(f.opened == 0.0);
        CHECK(f.filled_working == c.units);
        CHECK(f.terminal);
        CHECK(f.remaining_after == 0.0);
    }
    CHECK(o.rows == s.rows);
    CHECK(o.lots == s.survivors);
    for (double lot : o.lots) CHECK(lot > 0.0);
    const double held = fold(s.survivors);
    CHECK(o.signed_units == (c.long_book ? held : -held));
}

void run_cpp_case(const Case& c, bool expect_inexact) {
    scenario = c.name;
    const Split s = split_of(c.lots, members_of(c), c.units);
    print_split(c.name, s);
    CHECK(s.ended);
    CHECK(s.inexact() == expect_inexact);
    if (expect_inexact) check_no_exact_row(s);
    const Outcome staged = run_cpp(c, false);
    const Outcome direct = run_cpp(c, true);
    print_outcome("staged", staged);
    print_outcome("direct", direct);
    judge(c, s, staged);
    judge(c, s, direct);
    CHECK(staged == direct);
}

void run_current_case(const Case& c) {
    scenario = c.name;
    const Split s = split_of(c.lots, members_of(c), c.units);
    print_split(c.name, s);
    CHECK(s.inexact());
    for (bool direct : {false, true}) {
        Host* host = nullptr;
        const Outcome o = run_cpp(c, direct, &host);
        print_outcome(direct ? "direct" : "staged", o);
        judge(c, s, o);
        CHECK(host && !host->current_threw_ && host->current_);
        if (host && host->current_) {
            const auto* e = std::get_if<no::ExecutionAppliedEvent>(&*host->current_);
            CHECK(e != nullptr);
            if (e) {
                CHECK(e->closed_units == c.units);
                CHECK(e->filled_working == c.units);
                CHECK(e->terminal);
            }
        }
    }
}

// ---- The C host -----------------------------------------------------------------

struct CHost {
    pf_strategy_t host = nullptr;
    const Case* c = nullptr;
    int error = 0;
    bool placed = false;
    std::uint64_t close = 0;
    std::vector<pf_native_applied_v1> fills;
};

pf_native_request_v1 c_request(std::uint32_t intent, double value, const char* label) {
    pf_native_request_v1 request;
    std::memset(&request, 0, sizeof(request));
    request.struct_size = static_cast<std::uint32_t>(sizeof(request));
    request.version = PF_NATIVE_API_VERSION;
    request.intent = intent;
    request.reduce_size = PF_NATIVE_REDUCE_EXPLICIT_UNITS;
    request.intent_value = value;
    request.label = label;
    return request;
}

int c_on_run_begin(void* user) {
    auto& s = *static_cast<CHost*>(user);
    for (double lot : s.c->lots) {
        const auto request = c_request(PF_NATIVE_INTENT_TRANSACT,
                                       s.c->long_book ? lot : -lot, "open");
        const int rc = strategy_native_submit_v1(s.host, &request, nullptr, nullptr);
        if (rc != PF_NATIVE_OK && s.error == 0) s.error = rc;
    }
    return 0;
}

int c_on_bar(void* user, const pf_bar_t*, const pf_native_decision_v1*) {
    auto& s = *static_cast<CHost*>(user);
    double signed_units = 0.0, average = 0.0;
    std::uint64_t lots = 0;
    if (s.placed || strategy_native_position_v1(s.host, &signed_units, &average, &lots)
                        != PF_NATIVE_OK
        || lots != s.c->lots.size())
        return 0;
    s.placed = true;
    const auto request = s.c->close == Close::Reduce
        ? c_request(PF_NATIVE_INTENT_REDUCE, s.c->units, "close")
        : c_request(PF_NATIVE_INTENT_TRANSACT,
                    s.c->long_book ? -s.c->units : s.c->units, "close");
    const int rc = strategy_native_submit_v1(s.host, &request, &s.close, nullptr);
    if (rc != PF_NATIVE_OK && s.error == 0) s.error = rc;
    return 0;
}

int c_on_applied(void* user, const pf_native_applied_v1* applied, const pf_native_decision_v1*) {
    auto& s = *static_cast<CHost*>(user);
    if (s.close != 0 && applied->incarnation == s.close) s.fills.push_back(*applied);
    return 0;
}

void run_c_case(const Case& c) {
    scenario = c.name;
    const Split s = split_of(c.lots, members_of(c), c.units);
    print_split(c.name, s);
    CHECK(s.inexact());
    CHost h;
    h.c = &c;
    pf_native_callbacks_v1 table;
    std::memset(&table, 0, sizeof(table));
    table.struct_size = static_cast<std::uint32_t>(sizeof(table));
    table.version = PF_NATIVE_API_VERSION;
    table.user = &h;
    table.on_run_begin = c_on_run_begin;
    table.on_bar = c_on_bar;
    table.on_applied = c_on_applied;
    h.host = strategy_native_host_create_v1(&table);
    CHECK(h.host != nullptr);
    if (!h.host) return;

    pf_native_run_spec_v1 spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.struct_size = static_cast<std::uint32_t>(sizeof(spec));
    spec.session_key = c.name;
    spec.run_number = 1;
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "SPLIT";
    spec.tickerid = "K-ULP2:SPLIT";
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
    spec.price_tick = c.tick;
    spec.allowed_open_directions = 3;
    CHECK(strategy_configure_native_v1(h.host, &spec) == 0);

    std::vector<pf_bar_t> bars(kBars);
    for (int i = 0; i < kBars; ++i) {
        bars[i].open = bars[i].high = bars[i].low = bars[i].close = c.price;
        bars[i].volume = 10.0;
        bars[i].timestamp = 60'000LL * (i + 1);
    }
    const int rc = strategy_native_run_v1(h.host, bars.data(), kBars, nullptr);
    pf_native_state_v1 state;
    std::memset(&state, 0, sizeof(state));
    state.struct_size = static_cast<std::uint32_t>(sizeof(state));
    CHECK(strategy_native_state_v1(h.host, &state) == PF_NATIVE_OK);
    double signed_units = 0.0, average = 0.0;
    std::uint64_t lots = 0;
    CHECK(strategy_native_position_v1(h.host, &signed_units, &average, &lots) == PF_NATIVE_OK);
    std::printf("  c-host completed=%d lifecycle=%u code=%u position=%a lots=%llu\n",
                rc == PF_NATIVE_OK ? 1 : 0, state.lifecycle, state.failure_code, signed_units,
                static_cast<unsigned long long>(lots));
    for (const auto& f : h.fills) {
        std::printf("         close fill: closed=%a opened=%a filled_working=%a terminal=%d\n",
                    f.closed_units, f.opened_units, f.filled_working, f.terminal);
    }

    const double held = fold(s.survivors);
    CHECK(h.error == 0);
    CHECK(rc == PF_NATIVE_OK);
    CHECK(state.lifecycle == PF_NATIVE_LIFECYCLE_COMPLETED);
    CHECK(lots == s.survivors.size());
    CHECK(signed_units == (c.long_book ? held : -held));
    CHECK(h.fills.size() == 1);
    if (h.fills.size() == 1) {
        CHECK(h.fills.front().closed_units == c.units);
        CHECK(h.fills.front().opened_units == 0.0);
        CHECK(h.fills.front().filled_working == c.units);
        CHECK(h.fills.front().terminal == 1);
    }
    strategy_native_host_free(h.host);
}

// ---- The settlement's own calls on an opening scope --------------------------

// Three lots: two of one opening (incarnation 7) around one of another (8).
// The opening scope closes the first lot of 7 whole and ends inside its second
// lot; the lot of 8 is not a member and is not touched.
class EngineHost final : public NativeStrategyHost {
public:
    EngineHost(double a, double m, double b, double u) : a_(a), m_(m), b_(b), u_(u) {}

    void on_native_run_begin() override {
        const ex::PhysicalExecutionContext context{current_bar_.timestamp, bar_index_, {}};
        seeded_ = seed(a_, 7, context) && seed(m_, 8, context) && seed(b_, 7, context);
        if (!seeded_) return;
        const ex::Fill fill{kPrice, "close", "", 9};
        const ex::Action action = order_action::Reduce{u_};
        const ex::OpeningExposure scope{7, position_cycle_seq_};
        const ex::SelectedOpeningSet selected{position_cycle_seq_, {7}};
        inspect = inspect_native_settlement_scoped(action, fill, scope);
        inspect_selected = inspect_native_settlement_selected(action, fill, selected);
        project = project_native_settlement_scoped_v1(action, fill, scope);
        result = settle_native_execution_scoped_at(action, fill, context, scope);
        for (const auto& lot : pyramid_entries_) lots.push_back(lot.qty);
        for (const auto& trade : trades_) rows.push_back(trade.qty);
        position = position_qty_;
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}

    bool seeded_ = false;
    ex::SettlementInspection inspect, inspect_selected;
    ex::AccountEffectProjection project;
    ex::Result result;
    std::vector<double> lots, rows;
    double position = 0.0;

private:
    bool seed(double units, std::uint64_t incarnation, const ex::PhysicalExecutionContext& ctx) {
        return settle_native_execution_at(order_action::Transact{units},
                   ex::Fill{kPrice, "open", "", incarnation}, ctx).status
               == ex::Status::Applied;
    }
    double a_, m_, b_, u_;
};

void run_engine_case() {
    scenario = "engine-opening-scope";
    const double a = 106.0 / kPrice, m = 5.0, b = 10.0, u = 277.0 / kPrice;
    const Split s = split_of({a, m, b}, {true, false, true}, u);
    print_split(scenario.c_str(), s);
    CHECK(s.inexact());
    check_no_exact_row(s);
    EngineHost host(a, m, b, u);
    CHECK(host.configure_native(spec_for("engine-opening-scope", 0.25)).status
          == NativeSetupStatus::Applied);
    const auto bars = flat_bars(kPrice);
    host.run(bars.data(), static_cast<int>(bars.size()));
    std::printf("  inspect status=%d closed=%a; selected status=%d closed=%a; project status=%d "
                "closed=%a; settle status=%d closed=%a rows=%zu\n",
                static_cast<int>(host.inspect.status), host.inspect.closed_units,
                static_cast<int>(host.inspect_selected.status),
                host.inspect_selected.closed_units, static_cast<int>(host.project.status),
                host.project.closed_units, static_cast<int>(host.result.status),
                host.result.closed_units, host.result.closed_trade_count);
    CHECK(host.seeded_);
    CHECK(host.inspect.status == ex::Status::Applied);
    CHECK(host.inspect.closed_units == u);
    CHECK(host.inspect.opened_units == 0.0);
    CHECK(host.inspect.resulting_lot_count == 2);
    CHECK(host.inspect.resulting_abs_units == fold(s.survivors));
    CHECK(host.inspect_selected.status == ex::Status::Applied);
    CHECK(host.inspect_selected.closed_units == u);
    CHECK(host.project.status == ex::Status::Applied);
    CHECK(host.project.closed_units == u);
    CHECK(host.project.signed_units_after == fold(s.survivors));
    CHECK(host.result.status == ex::Status::Applied);
    CHECK(host.result.closed_units == u);
    CHECK(host.result.closed_trade_count == 2);
    CHECK(host.rows == s.rows);
    CHECK(host.lots == s.survivors);
    CHECK(host.position == fold(s.survivors));
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
}

// ---- The randomized battery -----------------------------------------------------

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1) {}
    std::uint64_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    }
    int between(int lo, int hi) {
        return lo + static_cast<int>(next() % static_cast<std::uint64_t>(hi - lo + 1));
    }
};

void run_battery() {
    constexpr int kBooks = 2000;
    constexpr double kTicks[] = {0.01, 0.05, 0.1, 0.25, 0.5};
    Rng rng(20260925);
    int inexact = 0, above = 0, below = 0, whole = 0, failed_books = 0;
    for (int book = 0; book < kBooks; ++book) {
        const double tick = kTicks[rng.between(0, 4)];
        const long steps = rng.between(static_cast<int>(1.0 / tick),
                                       static_cast<int>(5000.0 / tick));
        const double price = static_cast<double>(steps) * tick;
        const int n = rng.between(2, 4);
        const int k = rng.between(1, n - 1);  // the lot the request ends in
        std::vector<int> cash;
        for (int i = 0; i < n; ++i) cash.push_back(rng.between(10, 5000));
        int before = 0;
        for (int i = 0; i < k; ++i) before += cash[i];
        // Every other book is wide: its absorbing lot holds more than the lots
        // before it and the request takes more of it than they hold, so C is
        // below U / 2 and U - C is not exact (Sterbenz), which is where the
        // sum can land off U. The others are uniform.
        const bool wide = book % 2 == 1;
        if (wide) cash[k] = before + rng.between(2, 40000);
        std::vector<double> lots;
        for (int i = 0; i < n; ++i) lots.push_back(static_cast<double>(cash[i]) / price);
        // The request is the cash of the lots before it and part of its own,
        // converted once.
        const int part = rng.between(wide ? before + 1 : 1, cash[k] - 1);
        const double units = static_cast<double>(before + part) / price;

        Case c;
        char name[48];
        std::snprintf(name, sizeof(name), "battery-%04d", book);
        c.name = name;
        c.lots = lots;
        c.units = units;
        c.close = rng.between(0, 1) ? Close::Reduce : Close::Transact;
        c.long_book = rng.between(0, 1) != 0;
        c.price = price;
        c.tick = tick;
        scenario = name;

        const Split s = split_of(lots, std::vector<bool>(lots.size(), true), units);
        CHECK(s.ended);
        if (s.inexact()) {
            ++inexact;
            if (s.preceding + s.rest > units) ++above; else ++below;
            check_no_exact_row(s);
        }
        if (s.ended && s.absorbing < lots.size() && s.rest == lots[s.absorbing]) ++whole;
        const int failures_before = failures;
        const Outcome staged = run_cpp(c, false);
        const Outcome direct = run_cpp(c, true);
        judge(c, s, staged);
        judge(c, s, direct);
        CHECK(staged == direct);
        if (failures != failures_before) {
            ++failed_books;
            if (failed_books <= 3) {
                print_split(name, s);
                print_outcome("staged", staged);
                print_outcome("direct", direct);
            }
        }
    }
    std::printf("battery: %d books, %d whose split adds up inexactly (%d above, %d below), "
                "%d whose rest is the absorbing lot's full size, %d books failed\n",
                kBooks, inexact, above, below, whole, failed_books);
    scenario = "battery";
    CHECK(inexact > 0 && above > 0 && below > 0);
}

// ---- Requests no binary64 split can carry --------------------------------------

void run_genuine_case(const Case& c) {
    scenario = c.name;
    const Outcome staged = run_cpp(c, false);
    const Outcome direct = run_cpp(c, true);
    std::printf("%s: U=%a\n", c.name, c.units);
    print_outcome("staged", staged);
    print_outcome("direct", direct);
    for (const Outcome* o : {&staged, &direct}) {
        CHECK(!o->completed);
        CHECK(o->failure_code == static_cast<unsigned>(NativeFailureCode::SettlementFailure));
        CHECK(o->discriminator
              == static_cast<unsigned>(ex::Status::UnrepresentableQuantity));
        CHECK(o->lots == c.lots);
        CHECK(o->rows.empty());
        CHECK(o->fills.empty());
        CHECK(o->working == 1);
    }
    CHECK(staged == direct);
}

}  // namespace

int main() {
    const double a_up = 106.0 / kPrice, u_up = 277.0 / kPrice;
    const double a_dn = 121.0 / kPrice, u_dn = 334.0 / kPrice;
    const std::vector<std::pair<Case, bool>> cpp_cases = {
        {{"up-reduce", {a_up, 10.0}, u_up, Close::Reduce}, true},
        {{"up-transact", {a_up, 10.0}, u_up, Close::Transact}, true},
        {{"down-reduce", {a_dn, 10.0}, u_dn, Close::Reduce}, true},
        {{"down-transact", {a_dn, 10.0}, u_dn, Close::Transact}, true},
        {{"mirrored-up", {a_up, 10.0}, u_up, Close::Transact, false}, true},
        {{"three-lot-up", {90.0 / kPrice, 94.0 / kPrice, 10.0}, 523.0 / kPrice,
          Close::Reduce}, true},
        {{"three-lot-down", {90.0 / kPrice, 96.0 / kPrice, 10.0}, 526.0 / kPrice,
          Close::Transact}, true},
        {{"three-lot-mid", {90.0 / kPrice, 10.0, 7.0}, 770.0 / kPrice, Close::Reduce}, true},
        {{"whole-up", {92.0 / kPrice, 171.0 / kPrice, 10.0},
          prev_of(92.0 / kPrice + 171.0 / kPrice), Close::Reduce}, true},
        {{"scoped-up", {a_up, 5.0, 10.0}, u_up, Close::Reduce, true, {true, false, true}}, true},
        {{"scoped-down", {a_dn, 5.0, 10.0}, u_dn, Close::Reduce, true, {true, false, true}},
         true},
        {{"exact-control", {a_up, 10.0}, 280.0 / kPrice, Close::Reduce}, false},
    };
    for (const auto& [c, inexact] : cpp_cases) run_cpp_case(c, inexact);
    // whole-up: the rest rounds to the second lot's full size, which closes whole.
    {
        const auto& c = cpp_cases[8].first;
        const Split s = split_of(c.lots, members_of(c), c.units);
        scenario = c.name;
        CHECK(s.absorbing == 1 && s.rest == c.lots[1] && s.survivors.size() == 1);
    }
    // three-lot-mid: the request ends in the second lot, whose rest adds up below U.
    {
        const auto& c = cpp_cases[7].first;
        const Split s = split_of(c.lots, members_of(c), c.units);
        scenario = c.name;
        CHECK(s.absorbing == 1 && s.preceding + s.rest < c.units && s.survivors.size() == 2);
    }

    run_current_case({"current-up", {a_up, 10.0}, u_up, Close::Reduce, true, {}, true});
    run_c_case({"c-host-up", {a_up, 10.0}, u_up, Close::Reduce});
    run_c_case({"c-host-down", {a_dn, 10.0}, u_dn, Close::Transact});
    run_engine_case();
    run_battery();

    // A reduction that cannot move the position (order_action::plan), a rest
    // below half an ulp of its lot (the survivor 2^60 - 1 is not a binary64),
    // and a rest a whole lot cannot decrement (2^60 - 1025 is not one either).
    const double big = std::ldexp(1.0, 60);
    run_genuine_case({"genuine-plan", {big}, 1.0, Close::Reduce, true, {}, false, 1.0, 0.25});
    run_genuine_case({"genuine-survivor", {1024.0, big}, 1025.0, Close::Reduce, true, {}, false,
                      1.0, 0.25});
    run_genuine_case({"genuine-stuck", {1024.0, 1.0, big}, big, Close::Reduce, true, {}, false,
                      1.0, 0.25});

    std::printf("test_native_partial_close_split: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
