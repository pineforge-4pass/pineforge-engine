// R5 lane K-ULP4 (audit X1, ruling part b): the opt-in quantity tolerance.
//
// NativeRunSpec::quantity_tolerance = t declares that two quantities within t
// of each other are one quantity to the settlement (tolerant_close_split,
// src/engine_execution.cpp). A close that comes within t of a FIFO boundary
// -- the binary64 sum of the lots through one of them -- ends at that
// boundary, charged its request; a lot of at most t that binary64 cannot take
// off a close's rest, or its running sum, closes whole with the close that
// reaches it; a surviving book of at most t beside an opening stays. Absent,
// the settlement is exact and the spec digest is the one it had before the
// field (pinned below from 91d65ad6).
//
//   snap-down          {fl(1.5 - 1.3), 2.9}, Reduce 0.2: the lot closes whole
//                      and the 2^-54 rest is not taken from the 2.9 lot
//   snap-down-transact the same, Transact{-0.2}
//   snap-up            Reduce prev(fl(1.5 - 1.3)): the lot closes whole, no
//                      2^-55 lot is kept
//   dust-head-reduce   {2^-55, 2.9}, Reduce 1.0: the dust lot closes whole
//                      and the rest 1.0 comes from the 2.9 lot
//   dust-head-open     {2^-55}, open 200 on its side: the dust stays beside it
//   dust-head-cross    {2^-55, 2.9}, Transact{-10}: both close, 7.1 opens
//   trailing-dust      {1, 2^-40}, Reduce 1 + 5e-13: the boundary after the
//                      first lot is within t, and the dust lot behind it is
//                      taken rather than left
//   overshoot          {1}, Transact{-(1 + 1e-12)}: the book closes, no
//                      1e-12 opposite lot opens; the same spelled Reduce is
//                      charged its request
//   one-lot            {1}, Reduce 1 - 1e-12: the fused one-lot settlement
//                      closes it whole
//   exact-control      a close exactly at a boundary, and one far from any
//   still-refused      a request of at most t on a larger lot (zero is not a
//                      boundary to snap to), a rest above t a huge lot
//                      absorbs, and a dust-only book closed by a far larger
//                      request: typed MatchRejected(UnrepresentableQuantity)
//   dust-head-small    a request of at most t on {2^-55, 2.9}: the dust lot's
//                      boundary is the one within t, so only it closes
//   spec               validation (NotFinitePositive on QuantityTolerance),
//                      the digest pin, and the value folded when present
//   c-host             the extension's quantity-tolerance tail, its mask
//                      bit refused from the fifth layout, a bad value refused
//                      with its field word
// Every C++ case runs staged and direct on the request core, and with the
// fused one-lot settlement on and off, and all four must agree bit for bit.
//
// Fail-before: compiled against 91d65ad6 the TU stops at its first use of
// MatchRejectReason::UnrepresentableQuantity (no such enumerator), then at
// every use of NativeRunSpec::quantity_tolerance (no such member).
// Source-free: the kernel-only profile registers the row.
#include "../src/engine_internal.hpp"
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_c_api.h>
#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
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

constexpr double kPrice = 100.0;
constexpr double kTolerance = 1e-10;
constexpr unsigned kUnrepresentable =
    static_cast<unsigned>(no::MatchRejectReason::UnrepresentableQuantity);
const double kSurvivor = 1.5 - 1.3;   // 0x1.9999999999998p-3

// native_run_spec_digest of base_spec() below, with and without a 0.1
// quantity grid, computed on 91d65ad6, before the field existed.
constexpr std::uint64_t kDigestBefore = 6393930142210106639ULL;
constexpr std::uint64_t kDigestGridBefore = 15026561762967368001ULL;

struct Step {
    enum Kind { Transact, Reduce, Flatten } kind = Transact;
    double value = 0.0;
};

struct Case {
    const char* name;
    std::vector<Step> steps;
    std::optional<double> tolerance = kTolerance;
};

struct StepOutcome {
    int submit = -1;
    int terminal = 0;             // 0 none, 1 applied, 2 MatchRejected, 3 NoEffect
    unsigned reason = 0;
    std::vector<double> closed, opened, filled;
    std::vector<double> rows;
    std::vector<double> book;
    bool operator==(const StepOutcome& o) const {
        return submit == o.submit && terminal == o.terminal && reason == o.reason
            && closed == o.closed && opened == o.opened && filled == o.filled
            && rows == o.rows && book == o.book;
    }
};

struct Outcome {
    bool completed = false;
    unsigned code = 0;
    std::vector<StepOutcome> steps;
    std::uint64_t continuation = 0, broker = 0;
    bool operator==(const Outcome& o) const {
        return completed == o.completed && code == o.code && steps == o.steps
            && continuation == o.continuation && broker == o.broker;
    }
};

NativeRunSpec base_spec(const char* key) {
    NativeRunSpec s;
    s.event_retention = NativeEventRetention::Full;
    s.identity = {key, 1};
    s.input_tf = "1";
    s.script_tf = "1";
    s.tickerid = "K-ULP4:TOLERANCE";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 1e9;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    return s;
}

class Host final : public NativeStrategyHost {
public:
    Host(const Case& c, bool direct) : case_(c) {
        as_native_consumer(execution_consumer()).set_direct_mutation(direct);
    }
    void on_native_run_begin() override { bar_ = 0; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const int b = bar_++;
        if (b % 2 != 0) return;
        const std::size_t k = static_cast<std::size_t>(b / 2);
        if (k > 0 && k - 1 < steps.size()) steps[k - 1].book = book();
        if (k >= case_.steps.size()) return;
        const Step& s = case_.steps[k];
        no::Request r;
        switch (s.kind) {
        case Step::Transact: r.intent = no::Transact{s.value}; break;
        case Step::Reduce: r.intent = no::Reduce{no::ExplicitUnits{s.value}}; break;
        case Step::Flatten: r.intent = no::Flatten{}; break;
        }
        r.label = "step";
        StepOutcome out;
        const auto res = submit(r);
        if (res.status != no::SubmitStatus::Accepted || !res.handle) {
            out.submit = res.reason ? static_cast<int>(*res.reason) : -2;
            incarnations.push_back(0);
        } else {
            incarnations.push_back(res.handle->incarnation);
        }
        steps.push_back(out);
    }
    std::vector<double> book() const {
        std::vector<double> out;
        for (const auto& lot : native_open_lots(std::numeric_limits<double>::quiet_NaN())) {
            out.push_back(lot.signed_units);
        }
        return out;
    }
    std::vector<double> row_units(std::size_t first, std::size_t n) const {
        std::vector<double> out;
        for (std::size_t i = first; i < first + n && i < trades_.size(); ++i) {
            out.push_back(trades_[i].qty);
        }
        return out;
    }
    std::vector<StepOutcome> steps;
    std::vector<std::uint64_t> incarnations;

private:
    Case case_;
    int bar_ = 0;
};

Outcome run_cpp(const Case& c, bool direct, bool fused) {
    internal::set_fused_settlement(fused);
    auto host = std::make_unique<Host>(c, direct);
    Outcome out;
    auto spec = base_spec((std::string("k-ulp4-tol-") + c.name).c_str());
    spec.quantity_tolerance = c.tolerance;
    CHECK(host->configure_native(spec).status == NativeSetupStatus::Applied);
    std::vector<Bar> bars;
    const int n = static_cast<int>(2 * c.steps.size() + 3);
    for (int i = 0; i < n; ++i) {
        bars.push_back(Bar{kPrice, kPrice, kPrice, kPrice, 10.0, 60'000LL * (i + 1)});
    }
    host->run(bars.data(), n);
    internal::set_fused_settlement(true);
    const auto state = host->native_state();
    out.completed = state.kind == NativeLifecycleKind::Completed;
    out.code = static_cast<unsigned>(state.failure.code);
    out.steps = host->steps;
    for (std::size_t k = 0; k < out.steps.size(); ++k) {
        auto& step = out.steps[k];
        const std::uint64_t inc = host->incarnations[k];
        if (inc == 0) continue;
        for (const auto& row : host->native_events(0)) {
            if (!row.command) continue;
            if (const auto* a = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
                if (a->handle().incarnation != inc) continue;
                step.closed.push_back(a->closed_units);
                step.opened.push_back(a->opened_units);
                step.filled.push_back(a->filled_working);
                const auto rows = host->row_units(a->first_trade_index, a->closed_trade_count);
                step.rows.insert(step.rows.end(), rows.begin(), rows.end());
                if (a->terminal) step.terminal = 1;
            } else if (const auto* r = std::get_if<no::MatchRejectedEvent>(&*row.command)) {
                if (r->handle().incarnation != inc) continue;
                step.terminal = 2;
                step.reason = static_cast<unsigned>(r->reason);
            } else if (const auto* e = std::get_if<no::NoEffectEvent>(&*row.command)) {
                if (e->handle().incarnation == inc) step.terminal = 3;
            }
        }
    }
    out.continuation = host->native_continuation_hash();
    out.broker = host->broker_state_hash();
    return out;
}

void print_outcome(const Outcome& o) {
    std::printf("  completed=%d code=%u\n", o.completed ? 1 : 0, o.code);
    for (std::size_t k = 0; k < o.steps.size(); ++k) {
        const auto& s = o.steps[k];
        std::printf("    step %zu submit=%d terminal=%d reason=%u", k, s.submit, s.terminal,
                    s.reason);
        for (std::size_t i = 0; i < s.filled.size(); ++i) {
            std::printf(" [closed=%a opened=%a filled=%a]", s.closed[i], s.opened[i], s.filled[i]);
        }
        std::printf(" rows:");
        for (double r : s.rows) std::printf(" %a", r);
        std::printf(" book:");
        for (double lot : s.book) std::printf(" %a", lot);
        std::printf("\n");
    }
}

// All four twins agree and the run completes.
Outcome run_case(const Case& c) {
    scenario = c.name;
    const Outcome a = run_cpp(c, false, true);
    const Outcome b = run_cpp(c, true, true);
    const Outcome d = run_cpp(c, false, false);
    const Outcome e = run_cpp(c, true, false);
    std::printf("%s (quantity_tolerance=%g)\n", c.name, c.tolerance ? *c.tolerance : 0.0);
    print_outcome(a);
    CHECK(a == b);
    CHECK(a == d);
    CHECK(a == e);
    CHECK(a.completed);
    CHECK(a.code == 0);
    CHECK(a.steps.size() == c.steps.size());
    return a;
}

void filled(const Outcome& o, std::size_t k, double closed, double opened, double units,
            const std::vector<double>& rows, const std::vector<double>& book) {
    CHECK(k < o.steps.size());
    if (k >= o.steps.size()) return;
    const auto& s = o.steps[k];
    CHECK(s.submit == -1);
    CHECK(s.terminal == 1);
    CHECK(s.filled.size() == 1);
    if (s.filled.size() == 1) {
        CHECK(s.closed[0] == closed);
        CHECK(s.opened[0] == opened);
        CHECK(s.filled[0] == units);
    }
    CHECK(s.rows == rows);
    CHECK(s.book == book);
}

void refused(const Outcome& o, std::size_t k, const std::vector<double>& book) {
    CHECK(k < o.steps.size());
    if (k >= o.steps.size()) return;
    const auto& s = o.steps[k];
    CHECK(s.submit == -1);
    CHECK(s.terminal == 2);
    CHECK(s.reason == kUnrepresentable);
    CHECK(s.filled.empty() && s.rows.empty());
    CHECK(s.book == book);
}

std::vector<Step> with(std::vector<Step> steps, const std::vector<Step>& more) {
    steps.insert(steps.end(), more.begin(), more.end());
    return steps;
}

void settlement_cases() {
    const double s02 = kSurvivor;
    const std::vector<Step> survivor_book = {
        {Step::Transact, 1.5}, {Step::Reduce, 1.3}, {Step::Transact, 2.9}};
    {
        const auto o = run_case({"snap-down", with(survivor_book, {{Step::Reduce, 0.2}})});
        filled(o, 3, 0.2, 0.0, 0.2, {s02}, {2.9});
    }
    {
        const auto o = run_case({"snap-down-transact", with(survivor_book, {{Step::Transact, -0.2}})});
        filled(o, 3, 0.2, -0.0, 0.2, {s02}, {2.9});
    }
    // Exact, the same request is refused (test_native_unrepresentable_refusal).
    {
        Case c{"snap-down-exact-control", with(survivor_book, {{Step::Reduce, 0.2}}), std::nullopt};
        const auto o = run_case(c);
        refused(o, 3, {s02, 2.9});
    }
    {
        const double u = std::nextafter(s02, 0.0);
        const auto o = run_case({"snap-up", with(survivor_book, {{Step::Reduce, u}, {Step::Reduce, 1.0}})});
        filled(o, 3, u, 0.0, u, {s02}, {2.9});
        filled(o, 4, 1.0, 0.0, 1.0, {1.0}, {2.9 - 1.0});
    }
    {
        const auto o = run_case({"dust-head-reduce",
            {{Step::Transact, 0x1p-55}, {Step::Transact, 2.9}, {Step::Reduce, 1.0}}});
        filled(o, 1, 0.0, 2.9, 2.9, {}, {0x1p-55, 2.9});
        filled(o, 2, 1.0, 0.0, 1.0, {0x1p-55, 1.0}, {2.9 - 1.0});
    }
    {
        const auto o = run_case({"dust-head-open", {{Step::Transact, 0x1p-55}, {Step::Transact, 200.0}}});
        filled(o, 1, 0.0, 200.0, 200.0, {}, {0x1p-55, 200.0});
    }
    {
        const auto o = run_case({"dust-head-cross",
            {{Step::Transact, 0x1p-55}, {Step::Transact, 2.9}, {Step::Transact, -10.0}}});
        filled(o, 2, 0x1p-55 + 2.9, -(10.0 - (0x1p-55 + 2.9)), 10.0, {0x1p-55, 2.9},
               {-(10.0 - (0x1p-55 + 2.9))});
    }
    {
        const double u = 1.0 + 5e-13;
        const auto o = run_case({"trailing-dust",
            {{Step::Transact, 1.0}, {Step::Transact, 0x1p-40}, {Step::Reduce, u}}});
        filled(o, 2, u, 0.0, u, {1.0, 0x1p-40}, {});
    }
    // A rest of at most t before a lot larger than t: that lot is not touched.
    {
        const double u = 1.0 + 5e-13;
        const auto o = run_case({"boundary-before-large-lot",
            {{Step::Transact, 1.0}, {Step::Transact, 3.0}, {Step::Reduce, u}}});
        filled(o, 2, u, 0.0, u, {1.0}, {3.0});
    }
    {
        const double u = 1.0 + 1e-12;
        const auto o = run_case({"overshoot", {{Step::Transact, 1.0}, {Step::Transact, -u}}});
        filled(o, 1, u, -0.0, u, {1.0}, {});
    }
    // The same overshoot spelled Reduce: charged its request, not the held
    // total an exact oversized close is charged.
    {
        const double u = 1.0 + 1e-12;
        const auto o = run_case({"overshoot-reduce", {{Step::Transact, 1.0}, {Step::Reduce, u}}});
        filled(o, 1, u, 0.0, u, {1.0}, {});
    }
    // Exact: the overshoot opens its 1e-12 on the other side.
    {
        const double u = 1.0 + 1e-12;
        Case c{"overshoot-exact-control", {{Step::Transact, 1.0}, {Step::Transact, -u}}, std::nullopt};
        const auto o = run_case(c);
        filled(o, 1, 1.0, -(u - 1.0), u, {1.0}, {-(u - 1.0)});
    }
    {
        const double u = 1.0 - 1e-12;
        const auto o = run_case({"one-lot", {{Step::Transact, 1.0}, {Step::Reduce, u}}});
        filled(o, 1, u, 0.0, u, {1.0}, {});
    }
    // Far from any boundary and exactly at one: what the exact walk books.
    {
        const auto o = run_case({"exact-control-partial",
            {{Step::Transact, 1.0}, {Step::Transact, 3.0}, {Step::Reduce, 2.0}}});
        filled(o, 2, 2.0, 0.0, 2.0, {1.0, 1.0}, {2.0});
    }
    {
        const auto o = run_case({"exact-control-boundary",
            {{Step::Transact, 1.0}, {Step::Transact, 3.0}, {Step::Reduce, 1.0}}});
        filled(o, 2, 1.0, 0.0, 1.0, {1.0}, {3.0});
    }
    // A book of dust closed by a far larger request: the dust lot closes whole,
    // but the rest 1.0 is far above t and the request's units cannot be moved
    // by 2^-55 -- the request core's check, the same typed refusal as exact.
    {
        const auto o = run_case({"dust-only-book", {{Step::Transact, 0x1p-55}, {Step::Reduce, 1.0}}});
        refused(o, 1, {0x1p-55});
    }
    // A request of at most t on a dust head: the dust lot's own boundary is
    // within t of the request, so the close ends there and the 2.9 lot is not
    // touched (a boundary, not zero).
    {
        const auto o = run_case({"dust-head-small-request",
            {{Step::Transact, 0x1p-55}, {Step::Transact, 2.9}, {Step::Reduce, 1e-11}}});
        filled(o, 2, 1e-11, 0.0, 1e-11, {0x1p-55}, {2.9});
    }
    // Zero is not a boundary: 2^-54 of a 2.9 lot cannot move the position.
    {
        const auto o = run_case({"still-refused-dust-request",
            {{Step::Transact, 2.9}, {Step::Reduce, 0x1p-54}}});
        refused(o, 1, {2.9});
    }
    // A rest of 1 is far above t, and the 2^60 lot absorbs it.
    {
        const double big = std::ldexp(1.0, 60);
        const auto o = run_case({"still-refused-huge-lot",
            {{Step::Transact, 1024.0}, {Step::Transact, big}, {Step::Reduce, 1025.0}}});
        refused(o, 2, {1024.0, big});
    }
}

void spec_cases() {
    scenario = "spec";
    auto spec = base_spec("k-ulp4-digest");
    CHECK(native_run_spec_digest(spec) == kDigestBefore);
    spec.quantity_grid = 0.1;
    CHECK(native_run_spec_digest(spec) == kDigestGridBefore);
    spec.quantity_grid.reset();
    spec.quantity_tolerance = 1e-10;
    const auto with_t = native_run_spec_digest(spec);
    CHECK(with_t != kDigestBefore);
    spec.quantity_tolerance = 1e-9;
    CHECK(native_run_spec_digest(spec) != with_t);
    CHECK(native_run_spec_digest(spec) != kDigestBefore);
    for (double bad : {0.0, -0.0, -1e-10, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity()}) {
        spec.quantity_tolerance = bad;
        const auto v = validate_native_run_spec(spec);
        CHECK(v.error == NativeRunSpecError::NotFinitePositive);
        CHECK(v.field == NativeRunSpecField::QuantityTolerance);
    }
    spec.quantity_tolerance = 1e-10;
    CHECK(validate_native_run_spec(spec).ok());
}

// ---- The C spelling -----------------------------------------------------------

struct CHost {
    const Case* c = nullptr;
    pf_strategy_t host = nullptr;
    int bar = 0;
    std::vector<std::uint64_t> incarnations;
};

int c_on_run_begin(void* user) {
    static_cast<CHost*>(user)->bar = 0;
    return 0;
}

int c_on_bar(void* user, const pf_bar_t*, const pf_native_decision_v1*) {
    auto& s = *static_cast<CHost*>(user);
    const int b = s.bar++;
    if (b % 2 != 0) return 0;
    const std::size_t k = static_cast<std::size_t>(b / 2);
    if (k >= s.c->steps.size()) return 0;
    const Step& step = s.c->steps[k];
    pf_native_request_v1 r;
    std::memset(&r, 0, sizeof(r));
    r.struct_size = static_cast<std::uint32_t>(sizeof(r));
    r.version = PF_NATIVE_API_VERSION;
    r.trigger = PF_NATIVE_TRIGGER_MARKET;
    r.label = "step";
    r.comment = "";
    switch (step.kind) {
    case Step::Transact: r.intent = PF_NATIVE_INTENT_TRANSACT; r.intent_value = step.value; break;
    case Step::Reduce:
        r.intent = PF_NATIVE_INTENT_REDUCE;
        r.reduce_size = PF_NATIVE_REDUCE_EXPLICIT_UNITS;
        r.intent_value = step.value;
        break;
    case Step::Flatten: r.intent = PF_NATIVE_INTENT_FLATTEN; break;
    }
    std::uint64_t inc = 0;
    std::uint32_t reject = 0;
    s.incarnations.push_back(
        strategy_native_submit_v1(s.host, &r, &inc, &reject) == PF_NATIVE_OK ? inc : 0);
    return 0;
}

pf_native_run_spec_v1 c_spec(const char* key) {
    pf_native_run_spec_v1 spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.struct_size = static_cast<std::uint32_t>(sizeof(spec));
    spec.session_key = key;
    spec.run_number = 1;
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "TOLERANCE";
    spec.tickerid = "K-ULP4:TOLERANCE";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "";
    spec.description = "";
    spec.volumetype = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "";
    spec.initial_capital = 1e9;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = PF_NATIVE_FEE_CASH_PER_EXECUTION;
    spec.fee_value = 0.0;
    spec.allowed_open_directions = 3;
    return spec;
}

pf_native_run_spec_ext_v1 c_ext(std::uint32_t size, bool with_tolerance, double tolerance) {
    pf_native_run_spec_ext_v1 ext;
    std::memset(&ext, 0, sizeof(ext));
    ext.struct_size = size;
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_EVENT_RETENTION;
    ext.event_retention = PF_NATIVE_EVENT_RETENTION_FULL;
    if (with_tolerance) {
        ext.present_mask |= PF_NATIVE_SPEC_EXT_QUANTITY_TOLERANCE;
        ext.quantity_tolerance = tolerance;
    }
    return ext;
}

void c_cases() {
    scenario = "c-host";
    const double s02 = kSurvivor;
    const Case c{"c-host-snap-down",
                 {{Step::Transact, 1.5}, {Step::Reduce, 1.3}, {Step::Transact, 2.9},
                  {Step::Reduce, 0.2}}};
    CHost h;
    h.c = &c;
    pf_native_callbacks_v1 table;
    std::memset(&table, 0, sizeof(table));
    table.struct_size = static_cast<std::uint32_t>(sizeof(table));
    table.version = PF_NATIVE_API_VERSION;
    table.user = &h;
    table.on_run_begin = c_on_run_begin;
    table.on_bar = c_on_bar;
    h.host = strategy_native_host_create_v1(&table);
    CHECK(h.host != nullptr);
    if (!h.host) return;
    const auto spec = c_spec("k-ulp4-tol-c");
    // The layout, then the value.
    CHECK(PF_NATIVE_RUN_SPEC_EXT_V1_RETENTION_SIZE
          == PF_NATIVE_RUN_SPEC_EXT_V1_AUXILIARY_SIZE + 2u * sizeof(std::uint32_t));
    CHECK(sizeof(pf_native_run_spec_ext_v1) == PF_NATIVE_RUN_SPEC_EXT_V1_RETENTION_SIZE + sizeof(double));
    CHECK(offsetof(pf_native_run_spec_ext_v1, quantity_tolerance)
          == PF_NATIVE_RUN_SPEC_EXT_V1_RETENTION_SIZE);
    auto older = c_ext(PF_NATIVE_RUN_SPEC_EXT_V1_RETENTION_SIZE, true, kTolerance);
    CHECK(strategy_configure_native_ext_v1(h.host, &spec, &older) == PF_NATIVE_E_STRUCT);
    std::uint32_t error = 0, field = 0;
    auto bad = c_ext(static_cast<std::uint32_t>(sizeof(pf_native_run_spec_ext_v1)), true, -1.0);
    CHECK(strategy_configure_native_ext_result_v1(h.host, &spec, &bad, &error, &field)
          != PF_NATIVE_OK);
    CHECK(error == PF_NATIVE_SPEC_ERROR_NOT_FINITE_POSITIVE);
    CHECK(field == PF_NATIVE_SPEC_FIELD_QUANTITY_TOLERANCE);
    auto good = c_ext(static_cast<std::uint32_t>(sizeof(pf_native_run_spec_ext_v1)), true, kTolerance);
    CHECK(strategy_configure_native_ext_v1(h.host, &spec, &good) == PF_NATIVE_OK);
    const int n = static_cast<int>(2 * c.steps.size() + 3);
    std::vector<pf_bar_t> bars(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        auto& bar = bars[static_cast<std::size_t>(i)];
        bar.open = bar.high = bar.low = bar.close = kPrice;
        bar.volume = 10.0;
        bar.timestamp = 60'000LL * (i + 1);
    }
    CHECK(strategy_native_run_v1(h.host, bars.data(), n, nullptr) == PF_NATIVE_OK);
    pf_native_state_v1 state;
    std::memset(&state, 0, sizeof(state));
    state.struct_size = static_cast<std::uint32_t>(sizeof(state));
    state.version = PF_NATIVE_API_VERSION;
    CHECK(strategy_native_state_v1(h.host, &state) == PF_NATIVE_OK);
    CHECK(state.lifecycle == PF_NATIVE_LIFECYCLE_COMPLETED);
    std::vector<pf_native_event_v1> rows(4096);
    for (auto& row : rows) {
        std::memset(&row, 0, sizeof(row));
        row.struct_size = static_cast<std::uint32_t>(sizeof(row));
        row.version = PF_NATIVE_API_VERSION;
    }
    const int got = strategy_native_events_v1(h.host, 0, rows.data(), static_cast<int>(rows.size()));
    int applied = 0;
    double closed = 0.0;
    for (int i = 0; i < got; ++i) {
        const auto& row = rows[static_cast<std::size_t>(i)];
        if (h.incarnations.size() == 4 && row.incarnation == h.incarnations[3]) {
            if (row.kind == PF_NATIVE_EVENT_APPLIED) {
                ++applied;
                closed = row.closed_units;
            }
            CHECK(row.kind != PF_NATIVE_EVENT_MATCH_REJECTED);
        }
    }
    CHECK(applied == 1);
    CHECK(closed == 0.2);
    CHECK(strategy_native_open_lot_count_v1(h.host, std::numeric_limits<double>::quiet_NaN()) == 1);
    pf_native_open_lot_v1 lot;
    std::memset(&lot, 0, sizeof(lot));
    lot.struct_size = static_cast<std::uint32_t>(sizeof(lot));
    lot.version = PF_NATIVE_API_VERSION;
    CHECK(strategy_native_open_lot_get_v1(h.host, 0, &lot) == PF_NATIVE_OK);
    CHECK(lot.signed_units == 2.9);
    std::printf("c-host-snap-down: closed=%a, the book holds %a (s02 = %a closed whole)\n", closed,
                lot.signed_units, s02);
    strategy_native_host_free(h.host);
}

}  // namespace

int main() {
    settlement_cases();
    spec_cases();
    c_cases();
    std::printf("test_native_quantity_tolerance: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
