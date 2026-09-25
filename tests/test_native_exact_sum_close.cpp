// R5 lane K-ULP3: a close whose binary64 running sum reaches its request exactly
// at a lot closes that lot whole.
//
// The settlement closes lots in FIFO order (next_close_split,
// src/engine_execution.cpp). Every lot before the one the request ends in
// closes whole, and C is the binary64 sum of what they closed; the lot it ends
// in closes the rest r = fl(U - C). When the request is exactly the binary64
// sum of the lots through that lot -- fl(C + q) == U, as for a Reduce or a
// Transact of the book's own held total, or of a FIFO prefix of it -- r can
// still come out below the lot's size q: U is itself a rounded sum, and U - C
// is rounded again when C is below U / 2. The lot then closed r and kept
// fl(q - r), a dust lot of at most one ulp of U (2^-55 .. 2^-50 in the named
// books below) that no request asked to keep. fl(C + r) == U as well, so the
// charge and the rows' sum were exact and lanes K-ULP1 and K-ULP2 did not
// touch it: a client that closed the whole book by its own quantity was left
// holding a dust position.
//
// Now that lot closes whole: the rows are the whole lots, their binary64 sum is
// U, the close is charged U, and every other lot is untouched. fl(C + q) == U
// implies fl(C + r) == U (K-ULP2: when fl(C + fl(U - C)) != U, no binary64 x
// has fl(C + x) == U), so K-ULP2's charge never meets this rule; every dust
// case checks that, and that the dust is at most one ulp of U.
//
// Every completing case is one terminal fill of the request: closed units and
// filled_working equal to U, remaining_after 0. The C++ cases run on the
// request core's staged and direct paths, which must agree bit for bit, hashes
// included.
//
//   sum-reduce, sum-transact      long {fl(80/84.5), fl(92/84.5)} closed by its
//                                 own sum U: the second lot kept 2^-52
//   sum-mirrored                  the same book short, closed by Transact{+U}
//   sum-sterbenz                  {fl(157/84.5), fl(16/84.5)}: C >= U / 2, so
//                                 r = U - C exactly and the dust is what U itself
//                                 rounded away, 0x1.4p-53
//   sum-wide                      {fl(22/84.5), fl(339/84.5)}: 2^-50
//   three-lot-sum, four-lot-sum   {80, 80, 84} / 84.5 and {80, 80, 80, 85} / 84.5
//   prefix-reduce, prefix-transact
//                                 {fl(80/84.5), fl(92/84.5), 10} closed by the
//                                 first two lots' sum: the third lot untouched
//   scoped-prefix                 BindOpenings over lots 1, 3 and 4 of
//                                 {fl(80/84.5), 5, fl(92/84.5), 10}, closed by
//                                 the sum of lots 1 and 3
//   current-sum                   sum-reduce through execute_current
//   c-host-sum, c-host-prefix     sum-reduce and prefix-transact through
//                                 strategy_native_submit_v1
//   engine-opening-scope          the settlement's own inspect, project and
//                                 settle on an opening scope of two lots around
//                                 a lot of another opening
//   battery                       2,000 seeded books of 2-4 lots sized from cash
//                                 at tick prices, closed by the book's sum, by a
//                                 FIFO prefix's sum, or by the sum of a prefix
//                                 of a bound selection
//   *-control                     the same before and after: Flatten of a dust
//                                 book, a sum whose rest is the lot, a selection
//                                 closed by its whole sum (consumed exactly), a
//                                 one-lot book (the fused settlement)
//   genuine-*                     the refusals of splits binary64 cannot hold:
//                                 still code 6, discriminator 5, nothing moved
//
// Fail-before: on 07249e3b every dust case closes r and keeps fl(q - r) -- a
// dust lot, and a dust position when the request is the whole book -- while the
// controls and the genuine refusals pass; the lane report records the first
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

double next_of(double x) { return std::nextafter(x, INFINITY); }

// What fl(x + y) rounded away, (x + y) - fl(x + y), exactly (Knuth's TwoSum):
// 0 when the sum is exact.
double two_sum_error(double x, double y) {
    const double sum = x + y;
    const double y_part = sum - x;
    return (x - (sum - y_part)) + (y - y_part);
}

double fold(const std::vector<double>& values) {
    double sum = 0.0;
    for (double v : values) sum += v;
    return sum;
}

// ---- The split the settlement must book -------------------------------------

// Walks the members of the book in FIFO order as the settlement does: a lot the
// rest exceeds closes whole and the rest is recomputed from the sum so far; the
// lot the request ends in closes the rest r = fl(U - C), and closes whole when r
// is its full size or when fl(C + q) == U.
struct Split {
    std::vector<double> rows;       // the closed quantity of each closed lot
    std::vector<double> survivors;  // the book after the close, FIFO
    bool ended = false;
    std::size_t last = 0;           // the lot the request ends in
    double preceding = 0.0;         // C: the sum of the whole lots before it
    double lot = 0.0;               // q: that lot's size
    double rest = 0.0;              // r = fl(U - C)
    double units = 0.0;
    // 07249e3b closed r here and kept fl(q - r).
    bool dust() const { return rest < lot && preceding + lot == units; }
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
        if (amount < lots[i] || sum >= units) {
            s.ended = true;
            s.last = i;
            s.preceding = closed;
            s.lot = lots[i];
            s.rest = amount;
            const bool whole = amount == lots[i] || closed + lots[i] == units;
            s.rows.push_back(whole ? lots[i] : amount);
            if (!whole) s.survivors.push_back(lots[i] - amount);
            continue;
        }
        s.rows.push_back(amount);
        closed = sum;
    }
    return s;
}

// The arithmetic of a dust case: the rest's sum is the request too (so K-ULP2's
// charge never applies), and the dust is positive and at most one ulp of U.
void check_dust_arithmetic(const Split& s) {
    CHECK(s.preceding + s.lot == s.units);
    CHECK(s.preceding + s.rest == s.units);
    CHECK(s.lot - s.rest > 0.0);
    CHECK(s.lot - s.rest <= next_of(s.units) - s.units);
}

void print_split(const char* name, const Split& s) {
    std::printf("%s: U=%a C=%a q=%a r=fl(U-C)=%a fl(C+r)=%a fl(C+q)=%a dust fl(q-r)=%a "
                "(lot %zu)\n", name, s.units, s.preceding, s.lot, s.rest, s.preceding + s.rest,
                s.preceding + s.lot, s.dust() ? s.lot - s.rest : 0.0, s.last);
}

// ---- A bare C++ host ----------------------------------------------------------

enum class Close { Reduce, Transact, Flatten };

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
    s.tickerid = "K-ULP3:SUM";
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
        } else if (case_.close == Close::Flatten) {
            close = no::Request{no::Flatten{}, "close", ""};
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
    for (double lot : o.lots) std::printf("         lot %a\n", lot);
}

// The outcome every exact-sum close must reach: one terminal fill of U, the
// whole lots as rows adding up to U, every other lot untouched.
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
    CHECK(fold(o.rows) == c.units);
    CHECK(o.lots == s.survivors);
    for (double lot : o.lots) CHECK(lot > 0.0);
    const double held = fold(s.survivors);
    CHECK(o.signed_units == (c.long_book ? held : -held));
}

// The members through the lot the request ends in, whole: the rows an
// exact-sum close books.
std::vector<double> whole_prefix(const Case& c, const Split& s) {
    std::vector<double> out;
    const auto member = members_of(c);
    for (std::size_t i = 0; i <= s.last && i < c.lots.size(); ++i) {
        if (member[i]) out.push_back(c.lots[i]);
    }
    return out;
}

void run_cpp_case(const Case& c, bool expect_dust) {
    scenario = c.name;
    const Split s = split_of(c.lots, members_of(c), c.units);
    print_split(c.name, s);
    CHECK(s.ended);
    CHECK(s.dust() == expect_dust);
    if (expect_dust) check_dust_arithmetic(s);
    CHECK(s.rows == whole_prefix(c, s));
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
    CHECK(s.dust());
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

// Flatten closes every lot whole with or without the rule.
void run_flatten_control(const Case& c) {
    scenario = c.name;
    const Outcome staged = run_cpp(c, false);
    const Outcome direct = run_cpp(c, true);
    std::printf("%s: Flatten of %zu lots\n", c.name, c.lots.size());
    print_outcome("staged", staged);
    print_outcome("direct", direct);
    for (const Outcome* o : {&staged, &direct}) {
        CHECK(o->completed);
        CHECK(o->failure_code == 0);
        CHECK(o->working == 0);
        CHECK(o->fills.size() == 1);
        if (o->fills.size() == 1) {
            CHECK(o->fills.front().closed == fold(c.lots));
            CHECK(o->fills.front().terminal);
        }
        CHECK(o->rows == c.lots);
        CHECK(o->lots.empty());
        CHECK(o->signed_units == 0.0);
    }
    CHECK(staged == direct);
}

// A one-lot book settles through the fused settlement; its rest is the
// request itself (C = 0), so the rule has nothing to do there.
void run_one_lot_control(const Case& c) {
    scenario = c.name;
    const Split s = split_of(c.lots, members_of(c), c.units);
    print_split(c.name, s);
    CHECK(!s.dust());
    internal::count_settlement_paths(true);
    const Outcome fused = run_cpp(c, false);
    const auto counts = internal::settlement_path_counts();
    internal::count_settlement_paths(false);
    internal::set_fused_settlement(false);
    const Outcome staged = run_cpp(c, false);
    internal::set_fused_settlement(true);
    print_outcome("fused", fused);
    print_outcome("chain", staged);
    const auto settle = static_cast<int>(internal::SettlementEntry::Settle);
    std::printf("  fused settle calls=%llu staged settle calls=%llu\n",
                static_cast<unsigned long long>(counts.fused[settle]),
                static_cast<unsigned long long>(counts.staged[settle]));
    CHECK(counts.fused[settle] > 0);
    judge(c, s, fused);
    judge(c, s, staged);
    CHECK(fused == staged);
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
    CHECK(s.dust());
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
    spec.ticker = "SUM";
    spec.tickerid = "K-ULP3:SUM";
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

// Three lots: two of one opening (incarnation 7) around one of another (8). The
// opening scope's request is the sum of its two lots, so it ends exactly at its
// second lot; the lot of 8 is not a member and is not touched. The selected set
// {7} is the control: the settlement consumes a selection whose request is its
// whole sum exactly, before and after the rule.
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
    const double a = 80.0 / kPrice, m = 5.0, b = 92.0 / kPrice;
    const double u = a + b;
    const Split s = split_of({a, m, b}, {true, false, true}, u);
    print_split(scenario.c_str(), s);
    CHECK(s.dust());
    check_dust_arithmetic(s);
    EngineHost host(a, m, b, u);
    CHECK(host.configure_native(spec_for("engine-opening-scope", 0.25)).status
          == NativeSetupStatus::Applied);
    const auto bars = flat_bars(kPrice);
    host.run(bars.data(), static_cast<int>(bars.size()));
    std::printf("  inspect status=%d closed=%a lots=%zu; selected status=%d closed=%a lots=%zu; "
                "project status=%d closed=%a after=%a; settle status=%d closed=%a rows=%zu\n",
                static_cast<int>(host.inspect.status), host.inspect.closed_units,
                host.inspect.resulting_lot_count,
                static_cast<int>(host.inspect_selected.status),
                host.inspect_selected.closed_units, host.inspect_selected.resulting_lot_count,
                static_cast<int>(host.project.status), host.project.closed_units,
                host.project.signed_units_after, static_cast<int>(host.result.status),
                host.result.closed_units, host.result.closed_trade_count);
    for (double row : host.rows) std::printf("         row %a\n", row);
    for (double lot : host.lots) std::printf("         lot %a\n", lot);
    CHECK(host.seeded_);
    CHECK(host.inspect.status == ex::Status::Applied);
    CHECK(host.inspect.closed_units == u);
    CHECK(host.inspect.opened_units == 0.0);
    CHECK(host.inspect.resulting_lot_count == 1);
    CHECK(host.inspect.resulting_abs_units == m);
    CHECK(host.inspect_selected.status == ex::Status::Applied);
    CHECK(host.inspect_selected.closed_units == u);
    CHECK(host.inspect_selected.resulting_lot_count == 1);
    CHECK(host.inspect_selected.resulting_abs_units == m);
    CHECK(host.project.status == ex::Status::Applied);
    CHECK(host.project.closed_units == u);
    CHECK(host.project.resulting_lot_count == 1);
    CHECK(host.project.signed_units_after == m);
    CHECK(host.result.status == ex::Status::Applied);
    CHECK(host.result.closed_units == u);
    CHECK(host.result.closed_trade_count == 2);
    CHECK(host.rows == (std::vector<double>{a, b}));
    CHECK(host.rows == s.rows);
    CHECK(fold(host.rows) == u);
    CHECK(host.lots == (std::vector<double>{m}));
    CHECK(host.lots == s.survivors);
    CHECK(host.position == m);
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
    const char* const kKinds[] = {"book", "prefix", "scoped"};
    Rng rng(20260926);
    int dust[3] = {0, 0, 0}, books[3] = {0, 0, 0}, failed_books = 0;
    for (int book = 0; book < kBooks; ++book) {
        const double tick = kTicks[rng.between(0, 4)];
        const long steps = rng.between(static_cast<int>(1.0 / tick),
                                       static_cast<int>(5000.0 / tick));
        const double price = static_cast<double>(steps) * tick;
        const int n = rng.between(2, 4);
        std::vector<double> lots;
        for (int i = 0; i < n; ++i) {
            lots.push_back(static_cast<double>(rng.between(10, 5000)) / price);
        }
        // The request is the binary64 sum of the lots it closes, folded in
        // FIFO order as the settlement folds them: the whole book, a FIFO
        // prefix of it, or a prefix of a bound selection (a selection's whole
        // sum is consumed exactly, before and after the rule).
        const int kind = book % 3;
        std::vector<bool> scope;
        std::vector<double> closes;
        if (kind == 0) {
            closes = lots;
        } else if (kind == 1) {
            const int k = rng.between(1, n - 1);
            closes.assign(lots.begin(), lots.begin() + k);
        } else {
            scope.assign(n, false);
            std::vector<double> members;
            for (int i = 0; i < n; ++i) {
                scope[i] = i == 0 || rng.between(0, 1) != 0;
                if (scope[i]) members.push_back(lots[i]);
            }
            if (members.size() < 2) {
                scope[n - 1] = true;
                members.push_back(lots[n - 1]);
            }
            const int k = rng.between(1, static_cast<int>(members.size()) - 1);
            closes.assign(members.begin(), members.begin() + k);
        }
        Case c;
        char name[48];
        std::snprintf(name, sizeof(name), "battery-%04d", book);
        c.name = name;
        c.lots = lots;
        c.units = fold(closes);
        c.close = kind != 2 && rng.between(0, 1) ? Close::Transact : Close::Reduce;
        c.long_book = rng.between(0, 1) != 0;
        c.scope = scope;
        c.price = price;
        c.tick = tick;
        scenario = name;

        const Split s = split_of(lots, members_of(c), c.units);
        ++books[kind];
        CHECK(s.ended);
        CHECK(s.rows == closes);
        if (s.dust()) {
            ++dust[kind];
            check_dust_arithmetic(s);
        }
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
    for (int kind = 0; kind < 3; ++kind) {
        std::printf("battery %s: %d books, %d whose last lot kept dust on 07249e3b\n",
                    kKinds[kind], books[kind], dust[kind]);
    }
    std::printf("battery: %d books, %d failed\n", kBooks, failed_books);
    scenario = "battery";
    CHECK(dust[0] > 0 && dust[1] > 0 && dust[2] > 0);
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
    const double a = 80.0 / kPrice, b = 92.0 / kPrice;
    const double sum = a + b;
    const std::vector<double> three = {80.0 / kPrice, 80.0 / kPrice, 84.0 / kPrice};
    const std::vector<double> four = {80.0 / kPrice, 80.0 / kPrice, 80.0 / kPrice,
                                      85.0 / kPrice};
    const std::vector<double> sterbenz = {157.0 / kPrice, 16.0 / kPrice};
    const std::vector<double> wide = {22.0 / kPrice, 339.0 / kPrice};
    const std::vector<std::pair<Case, bool>> cpp_cases = {
        {{"sum-reduce", {a, b}, sum, Close::Reduce}, true},
        {{"sum-transact", {a, b}, sum, Close::Transact}, true},
        {{"sum-mirrored", {a, b}, sum, Close::Transact, false}, true},
        {{"sum-sterbenz", sterbenz, fold(sterbenz), Close::Reduce}, true},
        {{"sum-wide", wide, fold(wide), Close::Transact}, true},
        {{"three-lot-sum", three, fold(three), Close::Reduce}, true},
        {{"four-lot-sum", four, fold(four), Close::Transact, false}, true},
        {{"prefix-reduce", {a, b, 10.0}, sum, Close::Reduce}, true},
        {{"prefix-transact", {a, b, 10.0}, sum, Close::Transact}, true},
        {{"scoped-prefix", {a, 5.0, b, 10.0}, sum, Close::Reduce, true,
          {true, false, true, true}}, true},
        {{"exact-control", {a, 91.0 / kPrice}, a + 91.0 / kPrice, Close::Reduce}, false},
        {{"selected-control", {a, 5.0, b}, sum, Close::Reduce, true, {true, false, true}},
         true},
    };
    for (const auto& [c, dust] : cpp_cases) run_cpp_case(c, dust);
    // sum-sterbenz: the first lot holds at least half the request, so the rest
    // U - C is exact (Sterbenz) and the dust is exactly what U = fl(q1 + q2)
    // rounded away.
    {
        const auto& c = cpp_cases[3].first;
        const Split s = split_of(c.lots, members_of(c), c.units);
        scenario = c.name;
        CHECK(s.preceding >= c.units / 2);
        CHECK(two_sum_error(c.units, -s.preceding) == 0.0);
        CHECK(s.lot - s.rest == two_sum_error(c.lots[0], c.lots[1]));
        CHECK(s.lot - s.rest == std::ldexp(1.25, -53));
    }
    // sum-wide: the widest dust of these books, 2^-50.
    {
        const auto& c = cpp_cases[4].first;
        const Split s = split_of(c.lots, members_of(c), c.units);
        scenario = c.name;
        CHECK(s.lot - s.rest == std::ldexp(1.0, -50));
    }

    run_current_case({"current-sum", {a, b}, sum, Close::Reduce, true, {}, true});
    run_c_case({"c-host-sum", {a, b}, sum, Close::Reduce});
    run_c_case({"c-host-prefix", {a, b, 10.0}, sum, Close::Transact});
    run_engine_case();
    run_flatten_control({"flatten-control", {a, b}, 0.0, Close::Flatten});
    run_one_lot_control({"one-lot-control", {a}, a, Close::Reduce});
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

    std::printf("test_native_exact_sum_close: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
