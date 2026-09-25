// R5 lane K-ULP5: an OCA-Reduce deduction binary64 cannot take off its
// recipient is absorbed, and the run goes on.
//
// A fill of one member of an OCA group whose effect is GroupEffect::Reduce is
// deducted from every live sibling (WorkingRequestCore::prepare_group_effect /
// apply_group_effect): from the sibling's remaining units, or -- while those
// units are not known yet -- into its pending total, which its terms
// (prepare_terms / apply_terms) or its owner's fill (prepare_owner_applied /
// apply_owner_applied) later takes off the units they bind. A deduction too
// small to move what it is taken from -- fl(units - d) is the units again, at
// most half an ulp of them -- and a delta too small to move a pending total
// could not be recorded: each of the four sites answered
// CoreFailure::UnrepresentableReservation and the run stopped after the
// member's fill was booked (lifecycle Failed, code 6, discriminator 7), on
// ordinary decimal quantities (K-ULP4's third-review probe p5b). Now such a
// deduction is ABSORBED: the recipient's units (or its pending total) stay as
// they are -- the exact binary64 result of taking a sub-ulp amount off them --
// the member's fill stands, and the event that records the deduction says it
// took nothing:
//
//   units          a ReservationReducedEvent with actual_deduction 0 and
//                  after == before (C: RESERVATION_REDUCED, closed_units 0)
//   pending        a DeferredGroupAdjustmentEvent with deferred_delta 0 whose
//                  pending_after is its pending_before, joined to no chain
//                  (C: DEFERRED_GROUP, closed_units 0)
//   terms, bound   a TermsResolvedEvent / QuantityBoundEvent whose
//                  pending_total is positive and effective_deduction 0: the
//                  resolved (or the owner's opened) units stand
//
// A pending total below half an ulp of a later delta rounds into it, as
// binary64 addition does (pending-rounds); it stopped the run too. A pending
// total that overflows (two fills of DBL_MAX) is no sub-ulp deduction and
// still stops the run with discriminator 7 (overflow, the control).
//
//   p5b            {0.5, fl(1000.1 - 1000)}: a resting Reduce 1000, a Reduce
//                  0.3 and a ScopeFraction 1 in one group; the fraction closes
//                  2.2759572004815709e-14, below half an ulp of the resting
//                  member's 999.7 -- absorbed; a later opening fills
//   units-*        a 2^60 recipient beside fills of 1 (absorbed) and 256
//                  (deducted), long and short; a hundred fills of 1, each
//                  absorbed on its own, then one of 100 (2^60 - 128)
//   pending        a bracket child waiting on its parent holds 2^60 pending;
//                  a fill of 1 is absorbed; the parent's fill binds the chain
//   pending-rounds a pending 2^-60 then a fill of 1: the total is 1
//   terms, -current a host-sized opening with 0.1 pending resolves 1e16: the
//                  1e16 stand; through execute_current, whose preview no
//                  longer throws
//   bound          a bracket child with 0.5 pending binds to an owner's 1e16
//   overflow       the control above
//   battery        seeded random OCA-Reduce groups beside decimal books and
//                  their dust: resting recipients of large and decimal units,
//                  deferred fractions, bracket children and host-sized
//                  openings; market reduces of the book's own lots, fractions,
//                  openings, host-sized openings and bracket owners. Every run
//                  completes, and every deduction is either taken (after is
//                  fl(before - d)) or absorbed exactly where binary64 cannot
//                  take it
// Every case runs on the request core's staged and direct paths, which must
// agree bit for bit, hashes included.
//
// Fail-before: compiled against f1af50dc (K-ULP4) the TU builds and fails 975
// of 177,837 checks: p5b, units-*, pending, pending-rounds, terms,
// terms-current and bound stop with code 6, discriminator 7 (terms-current's
// preview throws "native host-sized deduction is not representable" first),
// and the battery stops 321, 290 and 303 of its 1,000 runs per seed, each with
// code 6, discriminator 7. Source-free: the kernel-only profile registers the
// row.
#include "../src/engine_internal.hpp"
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <map>
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

constexpr double kP60 = 0x1p60;
constexpr double kNever = 1e6;   // a sell limit (and a buy stop) the run never reaches

// ---- A scripted host: each bar may run one callback ---------------------------

struct ScriptHost;
using Script = std::map<int, std::function<void(ScriptHost&)>>;

struct ScriptHost final : public NativeStrategyHost {
    explicit ScriptHost(bool direct) {
        as_native_consumer(execution_consumer()).set_direct_mutation(direct);
    }
    void on_native_run_begin() override { bar = 0; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const int b = bar++;
        const auto it = script.find(b);
        if (it != script.end()) it->second(*this);
    }
    // A HostSized request is answered the units its label names.
    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            const auto it = host_units.find(facts.definition->request.label);
            if (it != host_units.end()) {
                return {facts.default_resolved_price, it->second, no::OpeningShape::Transact};
            }
        }
        return {facts.default_resolved_price, std::nullopt, no::OpeningShape::Transact};
    }
    std::int64_t cycle() const { return position_cycle_seq_; }
    // Submits under `name`; answers whether the kernel accepted it.
    bool sub(const std::string& name, no::Request r) {
        r.label = name;
        const auto res = submit(r);
        if (res.status != no::SubmitStatus::Accepted || !res.handle) {
            ++refused;
            return false;
        }
        h[name] = *res.handle;
        return true;
    }
    std::vector<double> book() const {
        std::vector<double> out;
        for (const auto& lot : native_open_lots(std::numeric_limits<double>::quiet_NaN())) {
            out.push_back(lot.signed_units);
        }
        return out;
    }
    std::optional<no::RemainingProjection> remaining_of(const std::string& name) const {
        const auto it = h.find(name);
        if (it == h.end()) return std::nullopt;
        for (const auto& row : native_working_requests()) {
            if (row.definition && row.definition->handle == it->second) return row.remaining;
        }
        return std::nullopt;
    }

    Script script;
    std::map<std::string, double> host_units;
    std::map<std::string, no::RequestHandle> h;
    int bar = 0;
    int refused = 0;
    // execute_current's run, for the -current case.
    bool preview_threw = false;
    int current_kind = -1;
};

no::Request tx(double units) { no::Request r; r.intent = no::Transact{units}; return r; }
no::Request red(double units) {
    no::Request r;
    r.intent = no::Reduce{no::ExplicitUnits{units}};
    return r;
}
no::Request frac(double fraction) {
    no::Request r;
    r.intent = no::Reduce{no::ScopeFraction{fraction}};
    return r;
}
no::Request owner_opened(const no::RequestHandle& parent) {
    no::Request r;
    r.intent = no::Reduce{no::OwnerOpenedUnits{}};
    r.owner = no::WaitForApplied{parent};
    return r;
}
no::Request host_open() {
    no::Request r;
    r.intent = no::HostSized{no::HostSizedKind::Open, no::Side::Long};
    return r;
}
no::Request flatten() { no::Request r; r.intent = no::Flatten{}; return r; }
no::Request in_group(no::Request r, std::uint64_t group, std::int64_t cohort) {
    r.group = no::Member{group, cohort, no::GroupEffect::Reduce};
    return r;
}

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

// ---- One run and what it came to ----------------------------------------------

struct Counts {
    int reductions = 0, absorbed_units = 0;
    int deferred = 0, absorbed_pending = 0, rounded_pending = 0;
    int terms_with_pending = 0, absorbed_terms = 0;
    int bound_with_pending = 0, absorbed_bound = 0;
    void add(const Counts& o) {
        reductions += o.reductions; absorbed_units += o.absorbed_units;
        deferred += o.deferred; absorbed_pending += o.absorbed_pending;
        rounded_pending += o.rounded_pending;
        terms_with_pending += o.terms_with_pending; absorbed_terms += o.absorbed_terms;
        bound_with_pending += o.bound_with_pending; absorbed_bound += o.absorbed_bound;
    }
};

struct Outcome {
    bool completed = false;
    unsigned code = 0, discriminator = 0;
    std::string transcript;   // every command event, the fields a deduction writes
    std::uint64_t continuation = 0, broker = 0;
    std::vector<double> book;
    Counts counts;
    std::unique_ptr<ScriptHost> host;
};

double filled_of(const ScriptHost& host, const no::EventId& cause) {
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* a = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
            if (a->ordinal == cause.ordinal) return a->filled_working;
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

std::string fmt(const char* format, double a, double b = 0.0, double c = 0.0, double d = 0.0) {
    char buffer[256];
    std::snprintf(buffer, sizeof buffer, format, a, b, c, d);
    return buffer;
}

// Reads every event of a finished run; checks each deduction against binary64
// and counts the absorbed ones.
void read_events(Outcome& out) {
    const ScriptHost& host = *out.host;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        const auto& command = *row.command;
        out.transcript += std::to_string(command.index()) + ":";
        if (const auto* a = std::get_if<no::ExecutionAppliedEvent>(&command)) {
            out.transcript += fmt("A %a %a %a;", a->closed_units, a->opened_units, a->filled_working);
        } else if (const auto* r = std::get_if<no::ReservationReducedEvent>(&command)) {
            const double before = r->before.q;
            const auto* after = std::get_if<no::RemainingProjectionUnits>(&r->after);
            out.transcript += fmt("R %a %a %a %a;", r->requested_delta, r->actual_deduction, before,
                                  after ? after->q : -1.0);
            ++out.counts.reductions;
            CHECK(after != nullptr);
            if (!after) continue;
            const double d = std::min(r->requested_delta, before);
            CHECK(r->requested_delta == filled_of(host, r->cause));
            if (r->actual_deduction == 0.0) {
                // Absorbed: binary64 cannot take d off `before`.
                ++out.counts.absorbed_units;
                CHECK(d > 0.0 && d < before && before - d == before);
                CHECK(after->q == before);
            } else {
                CHECK(r->actual_deduction == d);
                CHECK(d == before ? after->q == 0.0 : after->q == before - d);
                CHECK(after->q < before);
            }
        } else if (const auto* g = std::get_if<no::DeferredGroupAdjustmentEvent>(&command)) {
            out.transcript += fmt("D %a %a;", g->deferred_delta, g->pending_after.total);
            ++out.counts.deferred;
            const double fill = filled_of(host, g->cause);
            const auto* before = std::get_if<no::PendingDeferred>(&g->pending_before);
            if (g->deferred_delta == 0.0) {
                // Absorbed: the pending total cannot move by this fill.
                ++out.counts.absorbed_pending;
                CHECK(before != nullptr);
                if (!before) continue;
                CHECK(before->total + fill == before->total);
                CHECK(g->pending_after.total == before->total);
                CHECK(g->pending_after.count == before->count);
                CHECK(g->pending_after.tail_receipt == before->tail_receipt);
                CHECK(!g->previous_pending_receipt);
            } else {
                CHECK(g->deferred_delta == fill);
                CHECK(g->pending_after.tail_receipt.ordinal == g->ordinal);
                if (before) {
                    CHECK(g->pending_after.total == before->total + fill);
                    CHECK(g->pending_after.total > before->total);
                    CHECK(g->pending_after.count == before->count + 1);
                    CHECK(g->previous_pending_receipt
                          && *g->previous_pending_receipt == before->tail_receipt);
                    if (g->pending_after.total == fill) ++out.counts.rounded_pending;
                } else {
                    CHECK(g->pending_after.total == fill && g->pending_after.count == 1);
                }
            }
        } else if (const auto* t = std::get_if<no::TermsResolvedEvent>(&command)) {
            const auto* after = std::get_if<no::RemainingProjectionUnits>(&t->remaining_after);
            out.transcript += fmt("T %a %a %a;", t->pending_total, t->effective_deduction,
                                  after ? after->q : -1.0);
            if (t->pending_total > 0.0 && t->input.terms.units && after) {
                ++out.counts.terms_with_pending;
                const double units = *t->input.terms.units;
                const double d = std::min(t->pending_total, units);
                if (t->effective_deduction == 0.0 && units > 0.0) {
                    ++out.counts.absorbed_terms;
                    CHECK(d < units && units - d == units);
                    CHECK(after->q == units);
                } else {
                    CHECK(t->effective_deduction == d);
                    CHECK(d == units ? after->q == 0.0 : after->q == units - d);
                }
            }
        } else if (const auto* q = std::get_if<no::QuantityBoundEvent>(&command)) {
            const auto* after = std::get_if<no::RemainingProjectionUnits>(&q->remaining);
            out.transcript += fmt("Q %a %a %a %a;", q->source_units, q->pending_total,
                                  q->effective_deduction, after ? after->q : -1.0);
            if (q->pending_total > 0.0 && after) {
                ++out.counts.bound_with_pending;
                const double d = std::min(q->pending_total, q->source_units);
                if (q->effective_deduction == 0.0) {
                    ++out.counts.absorbed_bound;
                    CHECK(d < q->source_units && q->source_units - d == q->source_units);
                    CHECK(after->q == q->source_units);
                } else {
                    CHECK(q->effective_deduction == d);
                    CHECK(d == q->source_units ? after->q == 0.0
                                               : after->q == q->source_units - d);
                }
            }
        } else if (const auto* m = std::get_if<no::MatchRejectedEvent>(&command)) {
            out.transcript += "M" + std::to_string(static_cast<unsigned>(m->reason)) + ";";
        } else if (const auto* c = std::get_if<no::CancelledEvent>(&command)) {
            out.transcript += "C" + std::to_string(static_cast<unsigned>(c->reason)) + ";";
        } else {
            out.transcript += ";";
        }
    }
}

std::vector<Bar> flat_bars(int n, double price = 100.0) {
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i) {
        bars.push_back(Bar{price, price, price, price, 10.0, 60'000LL * (i + 1)});
    }
    return bars;
}

NativeRunSpec spec_for(const std::string& name) {
    NativeRunSpec s;
    s.event_retention = NativeEventRetention::Full;
    s.identity = {"k-ulp5-" + name, 1};
    s.input_tf = "1";
    s.script_tf = "1";
    s.tickerid = "K-ULP5:ABSORB";
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

Outcome run_one(const std::string& name, const Script& script, const std::vector<Bar>& bars,
                bool direct, const std::map<std::string, double>& host_units = {}) {
    Outcome out;
    out.host = std::make_unique<ScriptHost>(direct);
    out.host->script = script;
    out.host->host_units = host_units;
    CHECK(out.host->configure_native(spec_for(name)).status == NativeSetupStatus::Applied);
    out.host->run(bars.data(), static_cast<int>(bars.size()));
    const auto state = out.host->native_state();
    out.completed = state.kind == NativeLifecycleKind::Completed;
    out.code = static_cast<unsigned>(state.failure.code);
    out.discriminator = static_cast<unsigned>(state.failure.discriminator);
    read_events(out);
    out.continuation = out.host->native_continuation_hash();
    out.broker = out.host->broker_state_hash();
    out.book = out.host->book();
    return out;
}

// Both paths, which must agree; answers the staged run.
Outcome run_both(const std::string& name, const Script& script, const std::vector<Bar>& bars,
                 const std::map<std::string, double>& host_units = {}) {
    scenario = name;
    Outcome staged = run_one(name, script, bars, false, host_units);
    const Outcome direct = run_one(name, script, bars, true, host_units);
    CHECK(staged.completed == direct.completed);
    CHECK(staged.code == direct.code && staged.discriminator == direct.discriminator);
    CHECK(staged.transcript == direct.transcript);
    CHECK(staged.continuation == direct.continuation);
    CHECK(staged.broker == direct.broker);
    CHECK(staged.book == direct.book);
    std::printf("%-16s completed=%d code=%u disc=%u  absorbed: units=%d pending=%d terms=%d "
                "bound=%d rounded=%d\n",
                name.c_str(), staged.completed ? 1 : 0, staged.code, staged.discriminator,
                staged.counts.absorbed_units, staged.counts.absorbed_pending,
                staged.counts.absorbed_terms, staged.counts.absorbed_bound,
                staged.counts.rounded_pending);
    return staged;
}

template <class Event>
std::vector<Event> events_of(const ScriptHost& host) {
    std::vector<Event> out;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* e = std::get_if<Event>(&*row.command)) out.push_back(*e);
    }
    return out;
}

std::vector<no::ExecutionAppliedEvent> fills_of(const ScriptHost& host, const std::string& name) {
    std::vector<no::ExecutionAppliedEvent> out;
    const auto it = host.h.find(name);
    if (it == host.h.end()) return out;
    for (const auto& a : events_of<no::ExecutionAppliedEvent>(host)) {
        if (a.handle() == it->second) out.push_back(a);
    }
    return out;
}

bool remaining_is(const ScriptHost& host, const std::string& name, double units) {
    const auto remaining = host.remaining_of(name);
    if (!remaining) return false;
    const auto* q = std::get_if<no::RemainingProjectionUnits>(&*remaining);
    return q && q->q == units;
}

// ---- Directed cases -----------------------------------------------------------

void p5b() {
    Script s;
    s[0] = [](ScriptHost& p) { p.sub("open0.5", tx(0.5)); };
    s[2] = [](ScriptHost& p) { p.sub("open1000.1", tx(1000.1)); };
    s[4] = [](ScriptHost& p) {
        auto r = red(1000.0);
        r.owner = no::BindOpening{p.h["open1000.1"], p.cycle()};
        p.sub("bound-1000", r);
    };
    s[6] = [](ScriptHost& p) {
        auto resting = in_group(red(1000.0), 9, 3);
        resting.trigger = no::Limit{150.0};
        p.sub("oca-resting-1000", resting);
        p.sub("oca-reduce-0.3", in_group(red(0.3), 9, 1));
        p.sub("oca-fraction", in_group(frac(1.0), 9, 2));
    };
    s[8] = [](ScriptHost& p) { p.sub("after", tx(1.0)); };
    const Outcome o = run_both("p5b", s, flat_bars(12));
    const ScriptHost& host = *o.host;
    CHECK(o.completed);
    CHECK(o.code == 0 && o.discriminator == 0);
    CHECK(host.refused == 0);

    const double lot1 = 0.5 - 0.3;               // the 0.5 lot after the 0.3 close
    const double lot2 = 1000.1 - 1000.0;         // 0.10000000000002274
    const double scope = lot1 + lot2;            // what the fraction resolves
    const double dust = scope - 0.3;             // its units net of the 0.3 pending
    const double resting = 1000.0 - 0.3;         // 999.70000000000005
    CHECK(bits(dust) == bits(0x1.9ap-46));        // 2.2759572004815709e-14
    CHECK(dust < (std::nextafter(resting, 1e9) - resting) / 2);

    const auto reduced = events_of<no::ReservationReducedEvent>(host);
    CHECK(reduced.size() == 2);
    if (reduced.size() == 2) {
        CHECK(reduced[0].recipient == host.h.at("oca-resting-1000"));
        CHECK(reduced[0].requested_delta == 0.3 && reduced[0].actual_deduction == 0.3);
        CHECK(reduced[0].before.q == 1000.0);
        CHECK(std::get<no::RemainingProjectionUnits>(reduced[0].after).q == resting);
        // The absorbed one: requested the fraction's fill, took nothing.
        CHECK(reduced[1].recipient == host.h.at("oca-resting-1000"));
        CHECK(reduced[1].requested_delta == dust);
        CHECK(reduced[1].actual_deduction == 0.0);
        CHECK(reduced[1].before.q == resting);
        CHECK(std::get<no::RemainingProjectionUnits>(reduced[1].after).q == resting);
    }
    const auto deferred = events_of<no::DeferredGroupAdjustmentEvent>(host);
    CHECK(deferred.size() == 1);
    if (deferred.size() == 1) {
        CHECK(deferred[0].recipient == host.h.at("oca-fraction"));
        CHECK(deferred[0].deferred_delta == 0.3);
    }
    // The member's fill stands.
    const auto fraction = fills_of(host, "oca-fraction");
    CHECK(fraction.size() == 1);
    if (fraction.size() == 1) {
        CHECK(fraction[0].closed_units == dust && fraction[0].terminal);
    }
    // The resting member keeps its units, and the run goes on: the later
    // opening fills.
    CHECK(remaining_is(host, "oca-resting-1000", resting));
    CHECK(fills_of(host, "after").size() == 1);
    CHECK(o.book == (std::vector<double>{lot1 - dust, lot2, 1.0}));
    CHECK(o.counts.absorbed_units == 1);
}

void units(double sign) {
    Script s;
    s[0] = [sign](ScriptHost& p) {
        auto recipient = in_group(tx(sign * kP60), 7, 2);
        recipient.trigger = no::Limit{sign > 0 ? 1.0 : 200.0};
        p.sub("recipient", recipient);
        p.sub("fill-1", in_group(tx(sign * 1.0), 7, 1));
    };
    s[2] = [sign](ScriptHost& p) { p.sub("fill-256", in_group(tx(sign * 256.0), 7, 3)); };
    s[4] = [sign](ScriptHost& p) { p.sub("fill-1b", in_group(tx(sign * 1.0), 7, 4)); };
    s[6] = [sign](ScriptHost& p) { p.sub("after", tx(sign * 2.0)); };
    const Outcome o = run_both(sign > 0 ? "units-long" : "units-short", s, flat_bars(10));
    const ScriptHost& host = *o.host;
    CHECK(o.completed);
    const auto reduced = events_of<no::ReservationReducedEvent>(host);
    CHECK(reduced.size() == 3);
    if (reduced.size() == 3) {
        // fl(2^60 - 1) is 2^60: absorbed.
        CHECK(reduced[0].requested_delta == 1.0 && reduced[0].actual_deduction == 0.0);
        CHECK(reduced[0].before.q == kP60);
        CHECK(std::get<no::RemainingProjectionUnits>(reduced[0].after).q == kP60);
        // 256 is one ulp of 2^60: deducted exactly.
        CHECK(reduced[1].requested_delta == 256.0 && reduced[1].actual_deduction == 256.0);
        CHECK(std::get<no::RemainingProjectionUnits>(reduced[1].after).q == kP60 - 256.0);
        // fl(2^60 - 256 - 1) is 2^60 - 256: absorbed again.
        CHECK(reduced[2].requested_delta == 1.0 && reduced[2].actual_deduction == 0.0);
        CHECK(reduced[2].before.q == kP60 - 256.0);
        CHECK(std::get<no::RemainingProjectionUnits>(reduced[2].after).q == kP60 - 256.0);
    }
    CHECK(remaining_is(host, "recipient", kP60 - 256.0));
    CHECK(fills_of(host, "fill-1").size() == 1 && fills_of(host, "fill-1b").size() == 1);
    CHECK(fills_of(host, "after").size() == 1);
    CHECK(o.book == (std::vector<double>{sign * 1.0, sign * 256.0, sign * 1.0, sign * 2.0}));
    CHECK(events_of<no::CancelledEvent>(host).empty());
}

// Each deduction is taken on its own: a hundred fills of 1 against 2^60 are a
// hundred absorbed deductions and leave it 2^60; one fill of 100 then takes it
// to fl(2^60 - 100), which is 2^60 - 128. The hundred share one cohort, so
// they reduce the recipient and not each other.
void units_repeated() {
    Script s;
    s[0] = [](ScriptHost& p) {
        auto recipient = in_group(tx(kP60), 7, 1);
        recipient.trigger = no::Limit{1.0};
        p.sub("recipient", recipient);
        for (int i = 0; i < 100; ++i) {
            p.sub("fill-" + std::to_string(i), in_group(tx(1.0), 7, 2));
        }
    };
    s[2] = [](ScriptHost& p) { p.sub("fill-100", in_group(tx(100.0), 7, 200)); };
    const Outcome o = run_both("units-repeated", s, flat_bars(6));
    const ScriptHost& host = *o.host;
    CHECK(o.completed);
    const auto reduced = events_of<no::ReservationReducedEvent>(host);
    CHECK(reduced.size() == 101);
    int absorbed = 0;
    for (std::size_t i = 0; i + 1 < reduced.size(); ++i) {
        if (reduced[i].requested_delta == 1.0 && reduced[i].actual_deduction == 0.0
            && reduced[i].before.q == kP60
            && std::get<no::RemainingProjectionUnits>(reduced[i].after).q == kP60) {
            ++absorbed;
        }
    }
    CHECK(absorbed == 100);
    if (reduced.size() == 101) {
        CHECK(reduced[100].requested_delta == 100.0 && reduced[100].actual_deduction == 100.0);
        CHECK(std::get<no::RemainingProjectionUnits>(reduced[100].after).q == kP60 - 128.0);
    }
    CHECK(remaining_is(host, "recipient", kP60 - 128.0));
    CHECK(o.counts.absorbed_units == 100);
}

// Bars at 100 but `trigger_bar`, where the price is 250: a buy stop at 200
// fills there.
std::vector<Bar> bars_with_rise(int n, int trigger_bar) {
    auto bars = flat_bars(n);
    const double p = 250.0;
    bars[trigger_bar] = Bar{p, p, p, p, 10.0, bars[trigger_bar].timestamp};
    return bars;
}

void pending() {
    Script s;
    s[0] = [](ScriptHost& p) {
        auto parent = tx(1.0);
        parent.trigger = no::Stop{200.0};
        p.sub("parent", parent);
        p.sub("child", in_group(owner_opened(p.h["parent"]), 7, 2));
        p.sub("fill-2^60", in_group(tx(kP60), 7, 1));
    };
    s[2] = [](ScriptHost& p) { p.sub("flat-a", flatten()); };
    s[4] = [](ScriptHost& p) { p.sub("fill-1", in_group(tx(1.0), 7, 3)); };
    s[6] = [](ScriptHost& p) { p.sub("flat-b", flatten()); };
    const Outcome o = run_both("pending", s, bars_with_rise(12, 9));
    const ScriptHost& host = *o.host;
    CHECK(o.completed);
    const auto deferred = events_of<no::DeferredGroupAdjustmentEvent>(host);
    CHECK(deferred.size() == 2);
    if (deferred.size() == 2) {
        CHECK(deferred[0].deferred_delta == kP60);
        CHECK(deferred[0].pending_after.total == kP60 && deferred[0].pending_after.count == 1);
        const no::EventId head{deferred[0].pending_after.tail_receipt};
        CHECK(head.ordinal == deferred[0].ordinal);
        // fl(2^60 + 1) is 2^60: absorbed, joined to no chain.
        CHECK(deferred[1].deferred_delta == 0.0);
        const auto* before = std::get_if<no::PendingDeferred>(&deferred[1].pending_before);
        CHECK(before && before->total == kP60 && before->count == 1 && before->tail_receipt == head);
        CHECK(deferred[1].pending_after.total == kP60 && deferred[1].pending_after.count == 1);
        CHECK(deferred[1].pending_after.tail_receipt == head);
        CHECK(!deferred[1].previous_pending_receipt);
    }
    CHECK(fills_of(host, "fill-1").size() == 1);
    // The parent's fill binds the chain it left: 2^60 pending exhausts the
    // child's 1 unit.
    CHECK(fills_of(host, "parent").size() == 1);
    const auto bound = events_of<no::QuantityBoundEvent>(host);
    CHECK(bound.size() == 1);
    if (bound.size() == 1) {
        CHECK(bound[0].source_units == 1.0 && bound[0].pending_total == kP60);
        CHECK(bound[0].prior_adjustment_ids.size() == 1);
        CHECK(bound[0].effective_deduction == 1.0);
    }
    const auto cancelled = events_of<no::CancelledEvent>(host);
    CHECK(cancelled.size() == 1 && cancelled[0].reason == no::CancelReason::Group);
    CHECK(o.counts.absorbed_pending == 1);
}

void pending_rounds() {
    Script s;
    s[0] = [](ScriptHost& p) {
        auto parent = tx(3.0);
        parent.trigger = no::Stop{200.0};
        p.sub("parent", parent);
        p.sub("child", in_group(owner_opened(p.h["parent"]), 7, 2));
        p.sub("fill-2^-60", in_group(tx(0x1p-60), 7, 1));
    };
    s[2] = [](ScriptHost& p) { p.sub("flat-a", flatten()); };
    s[4] = [](ScriptHost& p) { p.sub("fill-1", in_group(tx(1.0), 7, 3)); };
    s[6] = [](ScriptHost& p) { p.sub("flat-b", flatten()); };
    const Outcome o = run_both("pending-rounds", s, bars_with_rise(12, 9));
    const ScriptHost& host = *o.host;
    CHECK(o.completed);
    const auto deferred = events_of<no::DeferredGroupAdjustmentEvent>(host);
    CHECK(deferred.size() == 2);
    if (deferred.size() == 2) {
        CHECK(deferred[0].deferred_delta == 0x1p-60);
        // fl(2^-60 + 1) is 1: the earlier total rounds into the fill.
        CHECK(deferred[1].deferred_delta == 1.0);
        CHECK(deferred[1].pending_after.total == 1.0 && deferred[1].pending_after.count == 2);
    }
    const auto bound = events_of<no::QuantityBoundEvent>(host);
    CHECK(bound.size() == 1);
    if (bound.size() == 1) {
        CHECK(bound[0].source_units == 3.0 && bound[0].pending_total == 1.0);
        CHECK(bound[0].prior_adjustment_ids.size() == 2);
        CHECK(bound[0].effective_deduction == 1.0);
        CHECK(std::get<no::RemainingProjectionUnits>(bound[0].remaining).q == 2.0);
    }
    // The bound child is a market close of 2 of the parent's 3.
    CHECK(fills_of(host, "child").size() == 1);
    CHECK(o.book == (std::vector<double>{1.0}));
    CHECK(o.counts.rounded_pending == 1);
}

void terms() {
    Script s;
    s[0] = [](ScriptHost& p) { p.sub("lot", tx(0.1)); };
    s[2] = [](ScriptHost& p) {
        p.sub("close-0.1", in_group(red(0.1), 41, 1));
        p.sub("host-open", in_group(host_open(), 41, 2));
    };
    const Outcome o = run_both("terms", s, flat_bars(6), {{"host-open", 1e16}});
    const ScriptHost& host = *o.host;
    CHECK(o.completed);
    const auto resolved = events_of<no::TermsResolvedEvent>(host);
    CHECK(resolved.size() == 1);
    if (resolved.size() == 1) {
        // fl(1e16 - 0.1) is 1e16: absorbed; the host's units stand.
        CHECK(resolved[0].pending_total == 0.1);
        CHECK(resolved[0].effective_deduction == 0.0);
        CHECK(resolved[0].prior_adjustment_ids.size() == 1);
        CHECK(std::get<no::RemainingProjectionUnits>(resolved[0].remaining_after).q == 1e16);
    }
    const auto opened = fills_of(host, "host-open");
    CHECK(opened.size() == 1 && opened[0].opened_units == 1e16);
    CHECK(o.book == (std::vector<double>{1e16}));
    CHECK(o.counts.absorbed_terms == 1);
}

void terms_current() {
    Script s;
    s[0] = [](ScriptHost& p) { p.sub("lot", tx(0.1)); };
    s[2] = [](ScriptHost& p) {
        p.sub("close-0.1", in_group(red(0.1), 42, 1));
        p.sub("host-open", in_group(host_open(), 42, 2));
        (void)p.execute_current(NativeCurrentExecution{p.h["close-0.1"]});
        try {
            (void)p.inspect_current_execution(NativeCurrentExecution{p.h["host-open"]});
        } catch (const std::exception&) {
            p.preview_threw = true;
        }
        try {
            const auto result = p.execute_current(NativeCurrentExecution{p.h["host-open"]});
            p.current_kind = std::holds_alternative<no::ExecutionAppliedEvent>(result) ? 1 : 0;
        } catch (const std::exception&) {
            p.current_kind = -2;
        }
    };
    const Outcome o = run_both("terms-current", s, flat_bars(6), {{"host-open", 1e16}});
    const ScriptHost& host = *o.host;
    CHECK(o.completed);
    CHECK(!host.preview_threw);
    CHECK(host.current_kind == 1);   // execute_current answered the host-sized fill
    const auto resolved = events_of<no::TermsResolvedEvent>(host);
    CHECK(resolved.size() == 1);
    if (resolved.size() == 1) {
        CHECK(resolved[0].pending_total == 0.1 && resolved[0].effective_deduction == 0.0);
    }
    CHECK(fills_of(host, "host-open").size() == 1);
    CHECK(o.book == (std::vector<double>{1e16}));
}

void bound() {
    Script s;
    s[0] = [](ScriptHost& p) { p.sub("lot", tx(0.5)); };
    s[2] = [](ScriptHost& p) {
        auto parent = tx(1e16);
        parent.trigger = no::Stop{200.0};
        p.sub("parent", parent);
        auto child = in_group(owner_opened(p.h["parent"]), 8, 2);
        child.trigger = no::Limit{kNever};
        p.sub("child", child);
        p.sub("close-0.5", in_group(red(0.5), 8, 1));
    };
    const Outcome o = run_both("bound", s, bars_with_rise(12, 9));
    const ScriptHost& host = *o.host;
    CHECK(o.completed);
    const auto bound_events = events_of<no::QuantityBoundEvent>(host);
    CHECK(bound_events.size() == 1);
    if (bound_events.size() == 1) {
        // fl(1e16 - 0.5) is 1e16: absorbed; the owner's units stand.
        CHECK(bound_events[0].source_units == 1e16);
        CHECK(bound_events[0].pending_total == 0.5);
        CHECK(bound_events[0].effective_deduction == 0.0);
        CHECK(std::get<no::RemainingProjectionUnits>(bound_events[0].remaining).q == 1e16);
    }
    CHECK(remaining_is(host, "child", 1e16));
    CHECK(o.book == (std::vector<double>{1e16}));
    CHECK(o.counts.absorbed_bound == 1);
}

// Control: DBL_MAX pending plus DBL_MAX overflows binary64. That is no
// sub-ulp deduction, and the run still stops with discriminator 7.
void overflow() {
    const double maxv = std::numeric_limits<double>::max();
    Script s;
    s[0] = [](ScriptHost& p) {
        auto parent = tx(1.0);
        parent.trigger = no::Stop{0.02};
        p.sub("parent", parent);
        p.sub("child", in_group(owner_opened(p.h["parent"]), 7, 2));
    };
    s[1] = [maxv](ScriptHost& p) { p.sub("fill-max-a", in_group(tx(maxv), 7, 1)); };
    s[3] = [](ScriptHost& p) { p.sub("flat", flatten()); };
    s[5] = [maxv](ScriptHost& p) { p.sub("fill-max-b", in_group(tx(maxv), 7, 3)); };
    const Outcome o = run_both("overflow", s, flat_bars(10, 0.01));
    CHECK(!o.completed);
    CHECK(o.code == static_cast<unsigned>(NativeFailureCode::SettlementFailure));
    CHECK(o.discriminator
          == static_cast<unsigned>(no::CoreFailure::UnrepresentableReservation));
    CHECK(fills_of(*o.host, "fill-max-b").size() == 1);
}

// ---- The seeded battery -------------------------------------------------------

// splitmix64: the same stream on every platform.
struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
    template <class T> T pick(const std::vector<T>& v) { return v[static_cast<std::size_t>(below(static_cast<int>(v.size())))]; }
};

// Recipient sizes: large (far below which a decimal fill is sub-ulp) and
// ordinary.
double big_units(Rng& r) {
    switch (r.below(4)) {
    case 0: return std::ldexp(1.0, 20 + r.below(43));
    case 1: {   // 10^k by exact multiplication (5^k fits the significand)
        double p = 1000.0;
        for (int k = r.below(15); k > 0; --k) p *= 10.0;
        return p;
    }
    case 2: return r.pick(std::vector<double>{1000.0, 999.7, 12345.678, 1e15 + 0.5, 4503599627370497.0});
    default: return r.pick(std::vector<double>{3.7, 12.5, 0.9, 250.25});
    }
}

double decimal_units(Rng& r) { return (1 + r.below(40)) / 10.0; }

// One scenario: a decimal book, its residues, and one OCA-Reduce group.
Script battery_script(std::uint64_t seed, std::map<std::string, double>* host_units) {
    Rng r{seed};
    Script s;
    // The book: one to three decimal lots, and a big lot a bound close cuts
    // to its decimal residue (p5b's fl(1000.1 - 1000)).
    std::vector<double> opens;
    const int lots = 1 + r.below(3);
    for (int i = 0; i < lots; ++i) opens.push_back(decimal_units(r));
    const bool residue = r.below(2) == 0;
    const double big_lot = r.pick(std::vector<double>{1000.1, 39.1, 1.3, 106.0 / 84.5});
    s[0] = [opens, residue, big_lot](ScriptHost& p) {
        for (std::size_t i = 0; i < opens.size(); ++i) p.sub("open" + std::to_string(i), tx(opens[i]));
        if (residue) p.sub("big", tx(big_lot));
    };
    if (residue) {
        s[2] = [big_lot](ScriptHost& p) {
            if (!p.h.count("big")) return;
            auto cut = red(std::floor(big_lot));
            cut.owner = no::BindOpening{p.h["big"], p.cycle()};
            p.sub("cut", cut);
        };
    }
    // The group at bar 4: recipients first (they rest), then fillers, in a
    // shuffled order.
    struct Member { int kind; double value; };
    std::vector<Member> members;
    const int recipients = 1 + r.below(3);
    for (int i = 0; i < recipients; ++i) members.push_back({r.below(5), big_units(r)});
    const int fillers = 1 + r.below(3);
    for (int i = 0; i < fillers; ++i) members.push_back({10 + r.below(6), 0.0});
    for (std::size_t i = members.size(); i > 1; --i) {
        const std::size_t j = static_cast<std::size_t>(r.below(static_cast<int>(i)));
        std::swap(members[i - 1], members[j]);
    }
    const std::uint64_t group = 1 + r.below(1000);
    std::vector<double> fractions{1.0, 0.5, 0.3, 0.9999999999999999, 0.25};
    std::vector<double> fracs;
    std::vector<int> picks;
    std::vector<double> host_answers;
    for (std::size_t i = 0; i < members.size(); ++i) {
        fracs.push_back(r.pick(fractions));
        picks.push_back(r.below(8));
        host_answers.push_back(big_units(r));
    }
    for (std::size_t i = 0; i < members.size(); ++i) {
        (*host_units)["m" + std::to_string(i)] = host_answers[i];
    }
    s[4] = [members, group, fracs, picks](ScriptHost& p) {
        const auto book = p.book();
        for (std::size_t i = 0; i < members.size(); ++i) {
            const std::string name = "m" + std::to_string(i);
            const std::int64_t cohort = static_cast<std::int64_t>(i) + 1;
            const auto& m = members[i];
            switch (m.kind) {
            case 0: {   // resting Reduce of large or decimal units
                auto q = in_group(red(m.value), group, cohort);
                q.trigger = no::Limit{kNever};
                p.sub(name, q);
                break;
            }
            case 1: {   // resting Transact that never fills
                auto q = in_group(tx(m.value), group, cohort);
                q.trigger = no::Limit{1.0};
                p.sub(name, q);
                break;
            }
            case 2: {   // resting fraction: deferred
                auto q = in_group(frac(fracs[i]), group, cohort);
                q.trigger = no::Limit{kNever};
                p.sub(name, q);
                break;
            }
            case 3: {   // bracket child of an owner that never fills: unbound
                auto parent = tx(m.value);
                parent.trigger = no::Stop{kNever};
                if (p.sub(name + "-owner", parent)) {
                    p.sub(name, in_group(owner_opened(p.h[name + "-owner"]), group, cohort));
                }
                break;
            }
            case 4: {   // resting host-sized opening
                auto q = in_group(host_open(), group, cohort);
                q.trigger = no::Limit{1.0};
                p.sub(name, q);
                break;
            }
            case 10: {  // market Reduce of one of the book's own lots, or a decimal
                double q = picks[i] % 2 == 0 ? 0.1 : 0.3;
                if (!book.empty() && picks[i] < 6) {
                    q = std::abs(book[static_cast<std::size_t>(picks[i]) % book.size()]);
                }
                p.sub(name, in_group(red(q), group, cohort));
                break;
            }
            case 11:    // market fraction
                p.sub(name, in_group(frac(fracs[i]), group, cohort));
                break;
            case 12:    // market opening of a decimal or a dust quantity
                p.sub(name, in_group(tx(picks[i] < 4 ? 0.1 * (1 + picks[i])
                                                     : std::ldexp(1.0, -40 - 3 * picks[i])),
                                     group, cohort));
                break;
            case 13:    // market host-sized opening
                p.sub(name, in_group(host_open(), group, cohort));
                break;
            case 14: {  // a market owner and its bracket child, the child a member;
                        // an owner far larger than the dust deferred into its
                        // child absorbs it when it binds
                const double owner_units = picks[i] < 4
                    ? 0.1 * (1 + picks[i])
                    : std::vector<double>{1e6, 0x1p30, 12345.678, 1e9}[picks[i] - 4];
                if (p.sub(name + "-owner", tx(owner_units))) {
                    auto child = in_group(owner_opened(p.h[name + "-owner"]), group, cohort);
                    child.trigger = no::Limit{kNever};
                    p.sub(name, child);
                }
                break;
            }
            default: {  // a bound close of the residue lot, when there is one
                if (p.h.count("big")) {
                    auto q = in_group(frac(1.0), group, cohort);
                    q.owner = no::BindOpening{p.h["big"], p.cycle()};
                    p.sub(name, q);
                } else {
                    p.sub(name, in_group(frac(1.0), group, cohort));
                }
                break;
            }
            }
        }
    };
    // Later fillers of the same group: the dust the book now holds, fractions,
    // the smallest lot ahead of a large owner whose bracket child it defers
    // into, and -- after a Flatten -- a dust opening beside the survivors.
    const int later = r.below(4);
    std::vector<int> later_kinds;
    for (int i = 0; i < later; ++i) later_kinds.push_back(r.below(5));
    s[6] = [later_kinds, group](ScriptHost& p) {
        for (std::size_t i = 0; i < later_kinds.size(); ++i) {
            const std::string name = "late" + std::to_string(i);
            const std::int64_t cohort = 100 + static_cast<std::int64_t>(i);
            const auto book = p.book();
            switch (later_kinds[i]) {
            case 0:
                if (!book.empty()) p.sub(name, in_group(red(book.front()), group, cohort));
                break;
            case 1:
                if (!book.empty()) p.sub(name, in_group(red(book.back()), group, cohort));
                break;
            case 2: p.sub(name, in_group(frac(1.0), group, cohort)); break;
            case 3: p.sub(name, in_group(frac(0.5), group, cohort)); break;
            default: {
                if (book.empty()) break;
                double smallest = std::abs(book.front());
                for (double lot : book) smallest = std::min(smallest, std::abs(lot));
                p.sub(name, in_group(red(smallest), group, cohort));
                if (p.sub(name + "-owner", tx(1e6))) {
                    auto child = in_group(owner_opened(p.h[name + "-owner"]), group, cohort + 50);
                    child.trigger = no::Limit{kNever};
                    p.sub(name + "-child", child);
                }
                break;
            }
            }
        }
    };
    if (r.below(2) == 0) {
        const int e = 30 + r.below(30);
        s[8] = [](ScriptHost& p) { p.sub("flat", flatten()); };
        s[10] = [e, group](ScriptHost& p) { p.sub("dust", in_group(tx(std::ldexp(1.0, -e)), group, 200)); };
    }
    return s;
}

Counts battery(std::uint64_t seed, int runs) {
    Counts total;
    int completed = 0;
    int stopped = 0;
    for (int i = 0; i < runs; ++i) {
        std::map<std::string, double> host_units;
        const Script s = battery_script(seed * 1000003ULL + static_cast<std::uint64_t>(i), &host_units);
        const std::string name = "battery-" + std::to_string(seed) + "-" + std::to_string(i);
        scenario = name;
        const Outcome staged = run_one(name, s, flat_bars(14), false, host_units);
        const Outcome direct = run_one(name, s, flat_bars(14), true, host_units);
        CHECK(staged.completed == direct.completed);
        CHECK(staged.transcript == direct.transcript);
        CHECK(staged.continuation == direct.continuation && staged.broker == direct.broker);
        if (staged.completed) {
            ++completed;
        } else {
            ++stopped;
            std::printf("  %s STOPPED code=%u disc=%u\n", name.c_str(), staged.code,
                        staged.discriminator);
        }
        CHECK(staged.completed);
        total.add(staged.counts);
    }
    scenario = "battery-" + std::to_string(seed);
    std::printf("battery seed %llu: runs=%d completed=%d STOPPED=%d  reductions=%d absorbed=%d  "
                "deferred=%d absorbed=%d rounded=%d  terms=%d absorbed=%d  bound=%d absorbed=%d\n",
                static_cast<unsigned long long>(seed), runs, completed, stopped,
                total.reductions, total.absorbed_units, total.deferred, total.absorbed_pending,
                total.rounded_pending, total.terms_with_pending, total.absorbed_terms,
                total.bound_with_pending, total.absorbed_bound);
    CHECK(stopped == 0);
    // Every seed reaches the two absorptions a resting sibling meets.
    CHECK(total.absorbed_units > 0);
    CHECK(total.absorbed_pending > 0);
    return total;
}

}  // namespace

int main() {
    p5b();
    units(1.0);
    units(-1.0);
    units_repeated();
    pending();
    pending_rounds();
    terms();
    terms_current();
    bound();
    overflow();
    Counts all;
    for (std::uint64_t seed : {11ULL, 22ULL, 33ULL}) all.add(battery(seed, 1000));
    // Across the seeds it reaches every site: the terms and the bind too.
    scenario = "battery";
    CHECK(all.absorbed_terms > 0);
    CHECK(all.absorbed_bound > 0);
    CHECK(all.rounded_pending > 0);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
