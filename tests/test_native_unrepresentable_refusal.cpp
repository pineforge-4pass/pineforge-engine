// R5 lane K-ULP4 (audit X1): no ordinary quantity request stops a run.
//
// A request the settlement cannot book exactly on the book it meets -- the
// execution::Status::UnrepresentableQuantity of next_close_split, of
// order_action::plan and of an opening the surviving book absorbs
// (src/engine_execution.cpp) -- ends THAT request with a typed
// MatchRejected(MatchRejectReason::UnrepresentableQuantity), C word
// PF_NATIVE_MATCH_REJECT_UNREPRESENTABLE_QUANTITY; nothing moves and the run
// goes on (NativeExecutionConsumer::consume_matched_request). Until this lane
// the consumer turned the same inspection into a durable SettlementFailure:
// lifecycle Failed, code 6, discriminator 5, "native settlement inspection
// failed", on ordinary decimal quantities (AUDIT4-opus kulp NF1 / NF2).
//
// On a quantity grid, the book's own quantities are on the grid: a fraction
// whose product is its scope resolves to the scope, unfloored, and a Reduce of
// a FIFO boundary of its scope -- the binary64 sum of the scope's lots through
// one of them -- is admitted (NF3 / NF4).
//
//   survivor           open 1.5, Reduce 1.3, open 2.9: the book is
//                      {fl(1.5 - 1.3), 2.9}; Reduce 0.2 needs 2^-54 of the
//                      2.9 lot, below half its ulp
//   survivor-transact  the same close spelled Transact{-0.2}
//   plan               Reduce 2^-54 of a 2.9 lot: the position cannot move
//   stuck              a host-made 2^-55 lot at the head; Reduce 1.0 cannot
//                      take the dust lot off its rest
//   absorbed-opening   a 2^-55 lot, then open 200 on its side
//   absorbed-cross     a 2^-55 lot and 2.9, then Transact{-10}
//   current            the survivor close through execute_current: a
//                      MatchRejectedEvent, no throw; the preview answers
//                      UnrepresentableQuantity first
//   c-host-*           survivor and absorbed-opening through
//                      strategy_native_submit_v1, read back as MATCH_REJECTED
//                      rows of strategy_native_events_v1
//   grid-fraction-*    Reduce{ScopeFraction{1}} of books that settlement
//                      moved off a 0.1 grid: 0.09999999999999859 (found
//                      nothing to close), 0.19999999999999857 (closed one
//                      step), {0.10000000000000142, 0.2} (left 2^-50 dust);
//                      each now closes the book whole
//   grid-own-lot,      a Reduce of a FIFO boundary of its scope -- the head
//   grid-boundaries,   lot's own off-grid size, a prefix, the whole book, a
//   grid-bound-*       bound opening's lot -- was OffGrid at submit and now
//                      closes whole lots; a Book-scope Reduce of a later lot's
//                      size (it would split the head lot off-grid), any
//                      Transact and any other off-grid quantity are still
//                      OffGrid
//   scoped-dust        a bound scope of 2^-55 closed by Reduce 1.0: the
//                      request core cannot take 2^-55 off 1.0 -- the same
//                      typed refusal (code 6, discriminator 2 on 91d65ad6)
//   grid-after-dust    the AUDIT4 F5b/c/d continuations: open, an oversized
//                      Reduce and a reversal after the whole-scope close
//   controls           the lot's own size and the host's total, which close
// Every C++ case runs on the request core's staged and direct paths, which
// must agree bit for bit, hashes included, and every run completes.
//
// Fail-before: compiled against 91d65ad6 the TU stops at its first use of
// MatchRejectReason::UnrepresentableQuantity (no such enumerator). With that
// name spelled 10 it runs there and fails 100 of 350 checks: every refusal
// case stops its run with code 6, discriminator 5 (scoped-dust with
// discriminator 2), and every grid case keeps its residual, its dust lot or
// its OffGrid refusal; only the controls pass.
// Source-free: the kernel-only profile registers the row.
#include "../src/engine_internal.hpp"
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_c_api.h>
#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
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

constexpr double kPrice = 100.0;
constexpr unsigned kUnrepresentable =
    static_cast<unsigned>(no::MatchRejectReason::UnrepresentableQuantity);

const double kSurvivor = 1.5 - 1.3;   // 0x1.9999999999998p-3

struct Step {
    enum Kind { Transact, Reduce, Fraction, Flatten, BoundReduce } kind = Transact;
    double value = 0.0;
    int bind = -1;   // BoundReduce: the step whose opening the Reduce binds to
};

struct Case {
    const char* name;
    std::vector<Step> steps;
    std::optional<double> grid;
    bool current = false;   // the last step through execute_current
};

// What one step came to: its submit answer, its terminal outcome, its fills
// and rows, and the book two bars later.
struct StepOutcome {
    int submit = -1;              // -1 accepted, else the RequestRejectReason
    int terminal = 0;             // 0 none, 1 applied, 2 MatchRejected, 3 NoEffect
    unsigned reason = 0;          // the MatchRejectReason
    std::vector<double> closed, opened, filled;
    std::vector<double> rows;
    std::vector<double> book;     // signed lot units after the step
    bool operator==(const StepOutcome& o) const {
        return submit == o.submit && terminal == o.terminal && reason == o.reason
            && closed == o.closed && opened == o.opened && filled == o.filled
            && rows == o.rows && book == o.book;
    }
};

struct Outcome {
    bool completed = false;
    unsigned code = 0, discriminator = 0;
    std::vector<StepOutcome> steps;
    std::uint64_t continuation = 0, broker = 0;
    // execute_current's answer for a `current` case, and the preview before it.
    int current_kind = -1;        // index of the NativeCurrentExecutionResult alternative
    unsigned current_reason = 0;
    std::optional<ex::Status> preview_readiness;
    bool current_threw = false;
    bool operator==(const Outcome& o) const {
        return completed == o.completed && code == o.code && discriminator == o.discriminator
            && steps == o.steps && continuation == o.continuation && broker == o.broker
            && current_kind == o.current_kind && current_reason == o.current_reason
            && preview_readiness == o.preview_readiness && current_threw == o.current_threw;
    }
};

NativeRunSpec spec_for(const Case& c) {
    NativeRunSpec s;
    s.event_retention = NativeEventRetention::Full;
    s.identity = {std::string("k-ulp4-") + c.name, 1};
    s.input_tf = "1";
    s.script_tf = "1";
    s.tickerid = "K-ULP4:REFUSAL";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 1e9;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    if (c.grid) s.quantity_grid = *c.grid;
    return s;
}

no::Request request_of(const Step& s) {
    no::Request r;
    switch (s.kind) {
    case Step::Transact: r.intent = no::Transact{s.value}; break;
    case Step::Reduce: r.intent = no::Reduce{no::ExplicitUnits{s.value}}; break;
    case Step::Fraction: r.intent = no::Reduce{no::ScopeFraction{s.value}}; break;
    case Step::Flatten: r.intent = no::Flatten{}; break;
    case Step::BoundReduce: r.intent = no::Reduce{no::ExplicitUnits{s.value}}; break;
    }
    r.label = "step";
    return r;
}

// Places step k at bar 2k and reads the book at bar 2k + 2.
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
        StepOutcome out;
        auto request = request_of(case_.steps[k]);
        const int bind = case_.steps[k].bind;
        if (case_.steps[k].kind == Step::BoundReduce && bind >= 0
            && static_cast<std::size_t>(bind) < handles.size() && handles[bind]) {
            request.owner = no::BindOpening{*handles[bind], position_cycle_seq_};
        }
        const auto res = submit(request);
        handles.push_back(res.handle);
        if (res.status != no::SubmitStatus::Accepted || !res.handle) {
            out.submit = res.reason ? static_cast<int>(*res.reason) : -2;
            incarnations.push_back(0);
        } else {
            incarnations.push_back(res.handle->incarnation);
            if (case_.current && k + 1 == case_.steps.size()) {
                const NativeCurrentExecution command{*res.handle};
                preview = inspect_current_execution(command).settlement_readiness;
                try {
                    const auto result = execute_current(command);
                    current_kind = static_cast<int>(result.index());
                    if (const auto* r = std::get_if<no::MatchRejectedEvent>(&result)) {
                        current_reason = static_cast<unsigned>(r->reason);
                    }
                } catch (const std::exception&) {
                    current_threw = true;
                }
            }
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
    std::vector<std::optional<no::RequestHandle>> handles;
    std::optional<ex::Status> preview;
    int current_kind = -1;
    unsigned current_reason = 0;
    bool current_threw = false;

private:
    Case case_;
    int bar_ = 0;
};

Outcome run_cpp(const Case& c, bool direct) {
    auto host = std::make_unique<Host>(c, direct);
    Outcome out;
    CHECK(host->configure_native(spec_for(c)).status == NativeSetupStatus::Applied);
    std::vector<Bar> bars;
    const int n = static_cast<int>(2 * c.steps.size() + 3);
    for (int i = 0; i < n; ++i) {
        bars.push_back(Bar{kPrice, kPrice, kPrice, kPrice, 10.0, 60'000LL * (i + 1)});
    }
    host->run(bars.data(), n);
    const auto state = host->native_state();
    out.completed = state.kind == NativeLifecycleKind::Completed;
    out.code = static_cast<unsigned>(state.failure.code);
    out.discriminator = static_cast<unsigned>(state.failure.discriminator);
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
    if (!out.steps.empty() && out.steps.back().book.empty() && !host->book().empty()) {
        out.steps.back().book = host->book();
    }
    out.continuation = host->native_continuation_hash();
    out.broker = host->broker_state_hash();
    out.preview_readiness = host->preview;
    out.current_kind = host->current_kind;
    out.current_reason = host->current_reason;
    out.current_threw = host->current_threw;
    return out;
}

void print_outcome(const char* path, const Outcome& o) {
    std::printf("  %-6s completed=%d code=%u discriminator=%u\n", path, o.completed ? 1 : 0,
                o.code, o.discriminator);
    for (std::size_t k = 0; k < o.steps.size(); ++k) {
        const auto& s = o.steps[k];
        std::printf("    step %zu submit=%d terminal=%d reason=%u fills=%zu", k, s.submit,
                    s.terminal, s.reason, s.filled.size());
        for (std::size_t i = 0; i < s.filled.size(); ++i) {
            std::printf(" [closed=%a opened=%a filled=%a]", s.closed[i], s.opened[i], s.filled[i]);
        }
        std::printf(" book:");
        for (double lot : s.book) std::printf(" %a", lot);
        std::printf("\n");
    }
}

// Runs both twins, prints them, requires them equal and complete.
Outcome run_case(const Case& c) {
    scenario = c.name;
    const Outcome staged = run_cpp(c, false);
    const Outcome direct = run_cpp(c, true);
    std::printf("%s\n", c.name);
    print_outcome("staged", staged);
    print_outcome("direct", direct);
    CHECK(staged == direct);
    CHECK(staged.completed);
    CHECK(staged.code == 0 && staged.discriminator == 0);
    CHECK(staged.steps.size() == c.steps.size());
    return staged;
}

// Step k was refused with the typed reason, moved nothing, booked no fill.
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

// Step k filled once, terminally, and left `book`.
void filled(const Outcome& o, std::size_t k, double closed, double opened, double units,
            const std::vector<double>& book) {
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
    CHECK(s.book == book);
}

// ---- A C host ------------------------------------------------------------------

struct CHost {
    const Case* c = nullptr;
    pf_strategy_t host = nullptr;
    int bar = 0;
    std::vector<std::uint64_t> incarnations;
    std::vector<int> submits;
    std::vector<std::vector<double>> books;
};

std::vector<double> c_book(pf_strategy_t h) {
    std::vector<double> out;
    const int n = strategy_native_open_lot_count_v1(h, std::numeric_limits<double>::quiet_NaN());
    for (int i = 0; i < n; ++i) {
        pf_native_open_lot_v1 row;
        std::memset(&row, 0, sizeof(row));
        row.struct_size = static_cast<std::uint32_t>(sizeof(row));
        row.version = PF_NATIVE_API_VERSION;
        if (strategy_native_open_lot_get_v1(h, i, &row) == PF_NATIVE_OK) {
            out.push_back(row.signed_units);
        }
    }
    return out;
}

int c_on_run_begin(void* user) {
    static_cast<CHost*>(user)->bar = 0;
    return 0;
}

int c_on_bar(void* user, const pf_bar_t*, const pf_native_decision_v1*) {
    auto& s = *static_cast<CHost*>(user);
    const int b = s.bar++;
    if (b % 2 != 0) return 0;
    const std::size_t k = static_cast<std::size_t>(b / 2);
    if (k > 0 && s.books.size() == k - 1) s.books.push_back(c_book(s.host));
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
    case Step::Fraction:
        r.intent = PF_NATIVE_INTENT_REDUCE;
        r.reduce_size = PF_NATIVE_REDUCE_SCOPE_FRACTION;
        r.intent_value = step.value;
        break;
    case Step::Flatten: r.intent = PF_NATIVE_INTENT_FLATTEN; break;
    case Step::BoundReduce: return 1;   // the C cases bind nothing
    }
    std::uint64_t inc = 0;
    std::uint32_t reject = 0;
    const int rc = strategy_native_submit_v1(s.host, &r, &inc, &reject);
    s.incarnations.push_back(rc == PF_NATIVE_OK ? inc : 0);
    s.submits.push_back(rc == PF_NATIVE_OK ? -1 : static_cast<int>(reject));
    return 0;
}

// The C host's terminal outcome per step, read back from the event record.
struct CStep {
    int submit = -1;
    int terminal = 0;
    unsigned reason = 0;
    std::vector<double> filled;
    std::vector<double> book;
};

struct COutcome {
    int rc = 0;
    std::uint32_t lifecycle = 0, code = 0;
    std::vector<CStep> steps;
};

COutcome run_c(const Case& c) {
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
    COutcome out;
    if (!h.host) return out;
    const std::string key = std::string("k-ulp4-c-") + c.name;
    pf_native_run_spec_v1 spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.struct_size = static_cast<std::uint32_t>(sizeof(spec));
    spec.session_key = key.c_str();
    spec.run_number = 1;
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "REFUSAL";
    spec.tickerid = "K-ULP4:REFUSAL";
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
    if (c.grid) {
        spec.optional_mask |= PF_NATIVE_SPEC_OPTIONAL_QUANTITY_GRID;
        spec.quantity_grid = *c.grid;
    }
    CHECK(strategy_configure_native_v1(h.host, &spec) == 0);
    const int n = static_cast<int>(2 * c.steps.size() + 3);
    std::vector<pf_bar_t> bars(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        auto& bar = bars[static_cast<std::size_t>(i)];
        bar.open = bar.high = bar.low = bar.close = kPrice;
        bar.volume = 10.0;
        bar.timestamp = 60'000LL * (i + 1);
    }
    out.rc = strategy_native_run_v1(h.host, bars.data(), n, nullptr);
    pf_native_state_v1 state;
    std::memset(&state, 0, sizeof(state));
    state.struct_size = static_cast<std::uint32_t>(sizeof(state));
    state.version = PF_NATIVE_API_VERSION;
    CHECK(strategy_native_state_v1(h.host, &state) == PF_NATIVE_OK);
    out.lifecycle = state.lifecycle;
    out.code = state.failure_code;
    std::vector<pf_native_event_v1> rows(4096);
    for (auto& row : rows) {
        std::memset(&row, 0, sizeof(row));
        row.struct_size = static_cast<std::uint32_t>(sizeof(row));
        row.version = PF_NATIVE_API_VERSION;
    }
    const int got = strategy_native_events_v1(h.host, 0, rows.data(), static_cast<int>(rows.size()));
    CHECK(got >= 0);
    for (std::size_t k = 0; k < h.incarnations.size(); ++k) {
        CStep step;
        step.submit = h.submits[k];
        if (k < h.books.size()) step.book = h.books[k];
        for (int i = 0; i < got; ++i) {
            const auto& row = rows[static_cast<std::size_t>(i)];
            if (h.incarnations[k] == 0 || row.incarnation != h.incarnations[k]) continue;
            if (row.kind == PF_NATIVE_EVENT_APPLIED) {
                step.filled.push_back(row.closed_units);
                if (row.terminal) step.terminal = 1;
            } else if (row.kind == PF_NATIVE_EVENT_MATCH_REJECTED) {
                step.terminal = 2;
                step.reason = row.reason;
            } else if (row.kind == PF_NATIVE_EVENT_NO_EFFECT) {
                step.terminal = 3;
            }
        }
        out.steps.push_back(step);
    }
    if (!out.steps.empty() && out.steps.back().book.empty()) out.steps.back().book = c_book(h.host);
    strategy_native_host_free(h.host);
    return out;
}

// The C host's run completes and its step k is the typed MATCH_REJECTED row.
void c_refused(const Case& c, std::size_t k, const std::vector<double>& book) {
    scenario = c.name;
    const COutcome o = run_c(c);
    std::printf("%s: rc=%d lifecycle=%u code=%u\n", c.name, o.rc, o.lifecycle, o.code);
    CHECK(o.rc == PF_NATIVE_OK);
    CHECK(o.lifecycle == PF_NATIVE_LIFECYCLE_COMPLETED);
    CHECK(o.code == 0);
    CHECK(k < o.steps.size());
    if (k >= o.steps.size()) return;
    const auto& s = o.steps[k];
    std::printf("  step %zu submit=%d terminal=%d reason=%u fills=%zu\n", k, s.submit, s.terminal,
                s.reason, s.filled.size());
    CHECK(s.submit == -1);
    CHECK(s.terminal == 2);
    CHECK(s.reason == PF_NATIVE_MATCH_REJECT_UNREPRESENTABLE_QUANTITY);
    CHECK(s.reason == kUnrepresentable);
    CHECK(s.filled.empty());
    CHECK(s.book == book);
}

// ---- Cases -----------------------------------------------------------------------

void refusal_cases() {
    const double s02 = kSurvivor;
    const std::vector<Step> survivor_book = {
        {Step::Transact, 1.5}, {Step::Reduce, 1.3}, {Step::Transact, 2.9}};
    auto with = [&](std::vector<Step> steps, std::vector<Step> more) {
        steps.insert(steps.end(), more.begin(), more.end());
        return steps;
    };

    // Reduce 0.2: the rest 2^-54 is below half an ulp of the 2.9 lot.
    {
        const auto o = run_case({"survivor", with(survivor_book,
            {{Step::Reduce, 0.2}, {Step::Flatten, 0.0}})});
        CHECK(0.2 - s02 == 0x1p-54);
        refused(o, 3, {s02, 2.9});
        // The run went on: the next command settles.
        filled(o, 4, s02 + 2.9, 0.0, s02 + 2.9, {});
    }
    {
        const auto o = run_case({"survivor-transact", with(survivor_book, {{Step::Transact, -0.2}})});
        refused(o, 3, {s02, 2.9});
    }
    // The position cannot move by 2^-54.
    {
        const auto o = run_case({"plan", {{Step::Transact, 2.9}, {Step::Reduce, 0x1p-54}}});
        refused(o, 1, {2.9});
    }
    // A host-made dust lot at the head: the whole-lot close of 2^-55 cannot
    // decrement the rest 1.0.
    {
        const auto o = run_case({"stuck", with(survivor_book,
            {{Step::Reduce, std::nextafter(s02, 0.0)}, {Step::Reduce, 1.0}, {Step::Flatten, 0.0}})});
        filled(o, 3, std::nextafter(s02, 0.0), 0.0, std::nextafter(s02, 0.0), {0x1p-55, 2.9});
        refused(o, 4, {0x1p-55, 2.9});
        filled(o, 5, 0x1p-55 + 2.9, 0.0, 0x1p-55 + 2.9, {});
    }
    // The dust lot's side absorbs 200 into the book's total: fl(2^-55 + 200) is
    // 200, so the survivor would vanish from it.
    {
        const auto o = run_case({"absorbed-opening",
            {{Step::Transact, 0x1p-55}, {Step::Transact, 200.0}, {Step::Transact, 2.0}}});
        refused(o, 1, {0x1p-55});
        // fl(2^-55 + 2) is 2 too: every opening beside the dust lot is refused;
        // the run is not.
        refused(o, 2, {0x1p-55});
    }
    {
        const auto o = run_case({"absorbed-cross",
            {{Step::Transact, 0x1p-55}, {Step::Transact, -10.0}, {Step::Flatten, 0.0}}});
        // A crossing whose closing leg is the dust lot and whose opening the
        // rest absorbs -- fl(10 - 2^-55) is 10, the lot does not move it.
        refused(o, 1, {0x1p-55});
        filled(o, 2, 0x1p-55, 0.0, 0x1p-55, {});
    }
    // A scope of dust closed by a far larger request: the stage caps the close
    // to the scope's 2^-55, which settles, but the request's 1.0 cannot be
    // moved by 2^-55 -- the request core's check, answered the same way
    // (on 91d65ad6: code 6, discriminator 2, NonrepresentableQuantity).
    {
        const double dust = 0.2 - (0.3 - 0.1);
        const auto o = run_case({"scoped-dust",
            {{Step::Transact, 0.1}, {Step::Transact, 0.2}, {Step::Transact, 5.0},
             {Step::Reduce, 0.3}, {Step::BoundReduce, 1.0, 1}, {Step::BoundReduce, dust, 1}}});
        CHECK(dust == 0x1p-55);
        filled(o, 3, 0.3, 0.0, 0.3, {dust, 5.0});
        refused(o, 4, {dust, 5.0});
        // The dust lot's own size closes it.
        filled(o, 5, dust, 0.0, dust, {5.0});
    }
    // execute_current answers the refusal as a MatchRejectedEvent (alternative 3
    // of NativeCurrentExecutionResult) and does not throw; the preview shows the
    // readiness first.
    {
        Case c{"current", with(survivor_book, {{Step::Reduce, 0.2}}), std::nullopt, true};
        const auto o = run_case(c);
        CHECK(!o.current_threw);
        CHECK(o.current_kind == 3);
        CHECK(o.current_reason == kUnrepresentable);
        CHECK(o.preview_readiness && *o.preview_readiness == ex::Status::UnrepresentableQuantity);
        refused(o, 3, {s02, 2.9});
    }
    // Controls: the lot's own size and the host's total close.
    {
        const auto o = run_case({"control-lot", with(survivor_book, {{Step::Reduce, s02}})});
        filled(o, 3, s02, 0.0, s02, {2.9});
    }
    // 3.1 is one ulp above the book's held total: an oversized close takes
    // the book whole and is charged what it held.
    {
        const auto o = run_case({"control-total", with(survivor_book, {{Step::Reduce, 3.1}})});
        CHECK(s02 + 2.9 == std::nextafter(3.1, 0.0));
        filled(o, 3, s02 + 2.9, 0.0, s02 + 2.9, {});
    }
    c_refused({"c-host-survivor", with(survivor_book, {{Step::Reduce, 0.2}})}, 3, {s02, 2.9});
    c_refused({"c-host-absorbed-opening", {{Step::Transact, 0x1p-55}, {Step::Transact, 200.0}}}, 1,
              {0x1p-55});
}

void grid_cases() {
    const double left_a = 0.2 - (39.1 - 39.0);   // 0x1.9999999999934p-4
    const double left_b = 0.3 - (39.1 - 39.0);   // 0x1.9999999999966p-3
    const double left_c = 39.0 - 38.9;           // 0x1.9999999999ap-4
    CHECK(!no::quantity_on_grid(left_a, 0.1));
    CHECK(!no::quantity_on_grid(left_b, 0.1));
    CHECK(!no::quantity_on_grid(left_c, 0.1));
    // Reduce{ScopeFraction{1}} of a book the settlement moved off its grid
    // resolves to the book's own held total and closes it whole.
    {
        const auto o = run_case({"grid-fraction-no-fill",
            {{Step::Transact, 39.0}, {Step::Transact, 0.2}, {Step::Transact, -39.1},
             {Step::Fraction, 1.0}}, 0.1});
        filled(o, 2, 39.1, -0.0, 39.1, {left_a});
        filled(o, 3, left_a, 0.0, left_a, {});
    }
    {
        const auto o = run_case({"grid-fraction-step",
            {{Step::Transact, 39.0}, {Step::Transact, 0.3}, {Step::Transact, -39.1},
             {Step::Fraction, 1.0}}, 0.1});
        filled(o, 3, left_b, 0.0, left_b, {});
    }
    {
        const auto o = run_case({"grid-fraction-dust",
            {{Step::Transact, 39.0}, {Step::Transact, 0.2}, {Step::Reduce, 38.9},
             {Step::Fraction, 1.0}}, 0.1});
        filled(o, 3, left_c + 0.2, 0.0, left_c + 0.2, {});
        CHECK(o.steps.size() == 4 && o.steps[3].rows == std::vector<double>({left_c, 0.2}));
    }
    // A fraction below one is still floored onto the grid.
    {
        const auto o = run_case({"grid-fraction-part",
            {{Step::Transact, 39.0}, {Step::Transact, 0.3}, {Step::Transact, -39.1},
             {Step::Fraction, 0.75}}, 0.1});
        filled(o, 3, 0.1, 0.0, 0.1, {left_b - 0.1});
    }
    // A Reduce whose units are a FIFO boundary of its scope -- the head lot's
    // own size, a prefix, the whole book -- is admitted on the grid and closes
    // whole lots.
    {
        const auto o = run_case({"grid-own-lot",
            {{Step::Transact, 39.0}, {Step::Transact, 0.2}, {Step::Transact, -39.1},
             {Step::Reduce, left_a}}, 0.1});
        filled(o, 3, left_a, 0.0, left_a, {});
    }
    // {fl(39 - 38.9), 0.2, 0.3}: the head lot's own size, then the prefix of
    // the first two lots, each off the 0.1 grid.
    {
        const auto o = run_case({"grid-boundaries-head",
            {{Step::Transact, 39.0}, {Step::Transact, 0.2}, {Step::Reduce, 38.9},
             {Step::Transact, 0.3}, {Step::Reduce, left_c}}, 0.1});
        filled(o, 4, left_c, 0.0, left_c, {0.2, 0.3});
        CHECK(o.steps.size() == 5 && o.steps[4].rows == std::vector<double>({left_c}));
    }
    {
        const double prefix = left_c + 0.2;
        CHECK(!no::quantity_on_grid(prefix, 0.1));
        const auto o = run_case({"grid-boundaries-prefix",
            {{Step::Transact, 39.0}, {Step::Transact, 0.2}, {Step::Reduce, 38.9},
             {Step::Transact, 0.3}, {Step::Reduce, prefix}}, 0.1});
        filled(o, 4, prefix, 0.0, prefix, {0.3});
        CHECK(o.steps.size() == 5 && o.steps[4].rows == std::vector<double>({left_c, 0.2}));
    }
    // A bound scope's own boundary: its opening's lot.
    {
        const double big = 1000.1 - 1000.0;   // 0x1.99999999a3p-4 on a 0.1 grid
        CHECK(!no::quantity_on_grid(big, 0.1));
        const auto o = run_case({"grid-bound-boundary",
            {{Step::Transact, 0.5}, {Step::Transact, 1000.1}, {Step::BoundReduce, 1000.0, 1},
             {Step::BoundReduce, big, 1}}, 0.1});
        filled(o, 2, 1000.0, 0.0, 1000.0, {0.5, big});
        filled(o, 3, big, 0.0, big, {0.5});
    }
    // Not a boundary: a Book-scope Reduce of the second lot's size would take
    // it FIFO from the first, on-grid lot and leave an off-grid split, so it
    // is still OffGrid; so is any Transact (it could open), and any other
    // off-grid quantity.
    {
        const double big = 1000.1 - 1000.0;
        const auto o = run_case({"grid-off-grid-still-refused",
            {{Step::Transact, 0.5}, {Step::Transact, 1000.1}, {Step::BoundReduce, 1000.0, 1},
             {Step::Reduce, big}, {Step::Transact, -0.5 - big},
             {Step::Reduce, std::nextafter(0.5 + big, 0.0)}}, 0.1});
        CHECK(o.steps.size() == 6);
        if (o.steps.size() == 6) {
            CHECK(o.steps[3].submit == static_cast<int>(no::RequestRejectReason::OffGrid));
            CHECK(o.steps[4].submit == static_cast<int>(no::RequestRejectReason::OffGrid));
            CHECK(o.steps[5].submit == static_cast<int>(no::RequestRejectReason::OffGrid));
            CHECK(o.steps[5].book == std::vector<double>({0.5, big}));
        }
    }
    // AUDIT4 F5b/c/d: after the whole-scope close nothing is left to trip on.
    const std::vector<Step> dust_book = {
        {Step::Transact, 39.0}, {Step::Transact, 0.2}, {Step::Reduce, 38.9}, {Step::Fraction, 1.0}};
    {
        auto steps = dust_book;
        steps.push_back({Step::Transact, 200.0});
        const auto o = run_case({"grid-after-dust-open", steps, 0.1});
        filled(o, 4, 0.0, 200.0, 200.0, {200.0});
    }
    {
        auto steps = dust_book;
        steps.push_back({Step::Reduce, 100.0});
        const auto o = run_case({"grid-after-dust-reduce", steps, 0.1});
        CHECK(o.steps.size() == 5 && o.steps[4].terminal == 3);
    }
    {
        auto steps = dust_book;
        steps.push_back({Step::Transact, -100.0});
        const auto o = run_case({"grid-after-dust-cross", steps, 0.1});
        filled(o, 4, 0.0, -100.0, 100.0, {-100.0});
    }
    // The C host resolves the same whole scope.
    {
        scenario = "c-host-grid-fraction";
        const Case c{"c-host-grid-fraction",
                     {{Step::Transact, 39.0}, {Step::Transact, 0.3}, {Step::Transact, -39.1},
                      {Step::Fraction, 1.0}}, 0.1};
        const COutcome o = run_c(c);
        CHECK(o.rc == PF_NATIVE_OK && o.lifecycle == PF_NATIVE_LIFECYCLE_COMPLETED);
        CHECK(o.steps.size() == 4);
        if (o.steps.size() == 4) {
            CHECK(o.steps[3].terminal == 1);
            CHECK(o.steps[3].filled == std::vector<double>({left_b}));
            CHECK(o.steps[3].book.empty());
        }
    }
}

}  // namespace

int main() {
    refusal_cases();
    grid_cases();
    std::printf("test_native_unrepresentable_refusal: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
