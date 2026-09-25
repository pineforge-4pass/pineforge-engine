// R5 lane K-OCA-KEEP: a re-price that keeps its handle inside a group, then a
// sibling's fill -- the group's effect reaches every sibling it names, and the
// run goes on, recording what the plain replace records.
//
// A group member's fill reaches every live sibling born before it
// (WorkingRequestCore::group_recipients) in queue order: the order the book
// holds them, ascending LiveRequest::priority. A re-price that keeps its
// handle (ReplaceOptions::keep_handle, R5 lane V19-D) keeps the request's
// incarnation and takes the number a plain successor would have taken as its
// priority, so it moves to the back of the queue, and queue order and
// incarnation order part. The group-effect receipts were kept in commit order
// and bisected by (cause, recipient incarnation, effect), so each new receipt
// had to sort after the newest one: the re-priced request, drained behind a
// younger sibling, sorted before that sibling's receipt and was refused
// CoreFailure::InvalidCause, and the run stopped after the member's fill was
// booked (lifecycle Failed, code 2, discriminator 1) -- staged and direct,
// under GroupEffect::Cancel and ::Reduce alike (K-ULP5, finding 1). The plain
// replace records that sibling as the newest request, last in the drain, and
// V19-D's contract is that the two runs record one timeline, so the queue
// order is the drain's and the receipts follow it: they are ordered by cause
// alone, one cause's receipts stand in the order its drain committed them,
// a lookup bisects the cause and walks its receipts, and a receipt for a
// cause older than the newest one's is still InvalidCause.
//
//   probe-*        K-ULP5's probe_keep_handle_oca: A and B rest in one group,
//                  A is re-priced with keep_handle (handle 1, now behind B) and
//                  a market member fills; under Reduce each sibling is
//                  deducted, under Cancel each is cancelled, B then A -- the
//                  plain replace's journal, ordinal for ordinal, under every
//                  option set
//   trail-*        a trail close re-priced with keep_handle and its running
//                  best retained (retain_trigger_state), then a sibling's fill
//   bound-*        a close bound to the book re-priced with keep_handle and
//                  keep_binding, then a sibling's fill
//   grid-absorbed-* K-ULP5's finding 2: on a 0.1 quantity grid, a close of its
//                  scope's whole held total -- a ScopeFraction{1}, or a
//                  host-sized close answered with that total -- whose pending
//                  group deduction its units absorb settles them all in one
//                  fill, so it closes the scope whole: no floor, no dust lot, no
//                  InvalidTerms refusal; matched at a stop, or previewed and
//                  executed as the current execution
//   core-*         the order core itself: one cause's receipts in queue order
//                  and in reverse, each found again on a replay
//                  (AlreadyApplied) and read back in commit order; after a
//                  newer cause's receipts the older cause's are still found,
//                  and a recipient its drain never reached is refused; a
//                  receipt for a cause older than the newest one's is refused
//                  InvalidCause, and records nothing
//   battery        seeded groups of both effects -- entries, closes of every
//                  intent, bracket legs waiting for their entry, market
//                  members, shared cohorts -- re-priced in random subsets and
//                  random orders, from the bar and from the fill callback, and
//                  cancelled, under the four option sets (plain, keep_handle,
//                  keep_binding, both; a trail's best retained at random).
//                  Every run completes; the four record one run -- every fill,
//                  every journal event mapped to the host's slots with its
//                  ordinal (CloseBoundEvents aside, which keep_binding spares),
//                  every group-effect receipt, the queue order every bar, every
//                  answer, every trade but its entry incarnation; and the
//                  drains whose receipts came in other than incarnation order
//                  -- the ones refused before -- are counted, under each effect
//                  and for recipients whose units are deferred
// The consumer cases run on the request core's staged and direct paths, which
// must agree bit for bit, hashes included; the core rows drive each path on a
// core of its own. No C row: the C replace takes no options
// (strategy_native_replace_v1 / _ext_v1), so a C host re-prices with the plain
// replace only, whose successor is the newest request in both orders.
//
// Fail-before: compiled against 2a03c658 (K-ULP5) the TU builds and fails
// 490 of 3,436 checks: the 12 probe-*, trail-* and bound-* runs under a
// keep_handle option set stop with code 2, discriminator 1, cause the member's
// fill and recipient the re-priced request (the 12 plain and keep_binding runs
// complete); grid-absorbed-fraction floors F to 1.1000000000000001 and leaves
// a 2.2648549702353193e-14 dust lot, and both grid-absorbed-host rows refuse F
// InvalidTerms and leave the lot open (the current one's preview answers
// InvalidTerms first, and its execution is refused), staged and direct; core-*
// refuses the re-priced recipient InvalidCause in queue order, staged and
// direct (the reverse order passes, and the older-cause replay fails at its
// first drain); and of the battery's 360 runs, every one of the 180 under a
// keep_handle option set stops with code 2, discriminator 1, while no plain
// drain leaves incarnation order.
// Source-free: the kernel-only profile registers the row.
#include "../src/native_execution_consumer.hpp"
#include "native_match_book_fixture.hpp"
#include "native_order_full_fold.hpp"

#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace pineforge;
namespace no = pineforge::native_order;
using k3_book::ticks;

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

// ---- The option sets ------------------------------------------------------------

enum class Mode : int { Replace = 0, KeepHandle = 1, KeepBinding = 2, KeepBoth = 3 };
constexpr Mode kModes[] = {Mode::Replace, Mode::KeepHandle, Mode::KeepBinding, Mode::KeepBoth};
const char* mode_name(Mode mode) {
    switch (mode) {
    case Mode::Replace: return "replace";
    case Mode::KeepHandle: return "keep_handle";
    case Mode::KeepBinding: return "keep_binding";
    case Mode::KeepBoth: return "keep_handle+keep_binding";
    }
    return "?";
}
bool keeps_handle(Mode mode) { return mode == Mode::KeepHandle || mode == Mode::KeepBoth; }
bool keeps_binding(Mode mode) { return mode == Mode::KeepBinding || mode == Mode::KeepBoth; }
const char* effect_name(no::GroupEffect effect) {
    return effect == no::GroupEffect::Reduce ? "reduce" : "cancel";
}

// ---- A scripted host: each bar may run one callback -------------------------------

struct ScriptHost;
using Script = std::map<int, std::function<void(ScriptHost&)>>;

struct ScriptHost final : public NativeStrategyHost {
    ScriptHost(bool direct, Mode mode) : mode(mode) {
        as_native_consumer(execution_consumer()).set_direct_mutation(direct);
    }
    void on_native_run_begin() override { bar = 0; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const int b = bar++;
        const auto it = script.find(b);
        if (it != script.end()) it->second(*this);
    }
    const no::WorkingRequestCore& core() const {
        return as_native_consumer(execution_consumer()).request_core();
    }
    std::int64_t cycle() const { return position_cycle_seq_; }
    // A host-sized close is answered with its scope's held total.
    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        no::ExecutionTerms terms{facts.default_resolved_price, std::nullopt,
                                 no::OpeningShape::Transact};
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            terms.units = facts.scope_exposure_units;
        }
        return terms;
    }
    // Submits under `name`; answers whether the kernel accepted it.
    bool sub(const std::string& name, no::Request r) {
        r.label = name;
        const auto res = submit(r);
        if (res.status != no::SubmitStatus::Accepted || !res.handle) {
            ++refused;
            return false;
        }
        h[name] = *res.handle;
        names[res.handle->incarnation] = name;
        return true;
    }
    // Re-prices `name` under the run's option set; `retain` carries a trail's
    // running best.
    bool rep(const std::string& name, no::Request r, bool retain = false) {
        r.label = name;
        no::ReplaceOptions options;
        options.retain_trigger_state = retain;
        options.keep_handle = keeps_handle(mode);
        options.keep_binding = keeps_binding(mode);
        const auto res = replace(h[name], r, options);
        if (res.status != no::ReplaceStatus::Replaced || !res.successor) {
            ++refused;
            return false;
        }
        if (*res.successor == h[name]) ++kept;
        h[name] = *res.successor;
        names[res.successor->incarnation] = name;
        return true;
    }
    std::string name_of(const no::RequestHandle& handle) const {
        const auto it = names.find(handle.incarnation);
        return it == names.end() ? "#" + std::to_string(handle.incarnation) : it->second;
    }
    // The working book in queue order, by name.
    std::string queue() const {
        std::string out;
        for (const auto& live : core().live()) out += name_of(live.handle()) + " ";
        return out;
    }

    Mode mode;
    Script script;
    std::map<std::string, no::RequestHandle> h;
    std::map<std::uint64_t, std::string> names;
    std::vector<std::string> queues;
    std::optional<NativeCurrentExecutionPreview> preview;
    std::optional<NativeCurrentExecutionResult> executed;
    bool live_after = true;
    int bar = 0;
    int refused = 0;
    int kept = 0;
};

no::Request tx(double units) { no::Request r; r.intent = no::Transact{units}; return r; }
no::Request red(double units) {
    no::Request r;
    r.intent = no::Reduce{no::ExplicitUnits{units}};
    return r;
}
no::Request in_group(no::Request r, std::uint64_t group, std::int64_t cohort,
                     no::GroupEffect effect) {
    r.group = no::Member{group, cohort, effect};
    return r;
}

std::string fmt(const char* format, double a, double b = 0.0, double c = 0.0) {
    char buffer[160];
    std::snprintf(buffer, sizeof buffer, format, a, b, c);
    return buffer;
}

// The journal, one line per command event: its ordinal, its kind and the
// request it concerns by name, and what a group effect wrote -- its cause's
// ordinal and its units. CloseBoundEvents are left out: keep_binding spares
// them and takes their ordinals (R5 lane V19-D).
std::string transcript(const ScriptHost& host) {
    std::string out;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        std::visit([&](const auto& event) {
            using E = std::decay_t<decltype(event)>;
            if constexpr (std::is_same_v<E, no::CloseBoundEvent>) {
                return;
            } else {
                out += std::to_string(event.ordinal) + " ";
                if constexpr (std::is_same_v<E, no::AcceptedEvent>) {
                    out += "Accepted " + host.name_of(event.handle());
                } else if constexpr (std::is_same_v<E, no::ReplacedEvent>) {
                    out += "Replaced " + host.name_of(event.successor());
                } else if constexpr (std::is_same_v<E, no::ExecutionAppliedEvent>) {
                    out += "Applied " + host.name_of(event.handle())
                        + fmt(" %.17g %.17g", event.closed_units, event.opened_units);
                } else if constexpr (std::is_same_v<E, no::ReservationReducedEvent>) {
                    const auto* after = std::get_if<no::RemainingProjectionUnits>(&event.after);
                    out += "Reduced " + host.name_of(event.recipient) + " cause "
                        + std::to_string(event.cause.ordinal)
                        + fmt(" %.17g %.17g -> %.17g", event.requested_delta, event.before.q,
                              after ? after->q : -1.0);
                } else if constexpr (std::is_same_v<E, no::DeferredGroupAdjustmentEvent>) {
                    out += "Deferred " + host.name_of(event.recipient) + " cause "
                        + std::to_string(event.cause.ordinal)
                        + fmt(" %.17g", event.pending_after.total);
                } else if constexpr (std::is_same_v<E, no::CancelledEvent>) {
                    out += "Cancelled " + host.name_of(event.handle()) + " reason "
                        + std::to_string(static_cast<int>(event.reason));
                    if (event.cause) out += " cause " + std::to_string(event.cause->ordinal);
                } else {
                    out += "kind " + std::to_string(row.command->index());
                }
                out += "\n";
            }
        }, *row.command);
    }
    return out;
}

struct Outcome {
    bool completed = false;
    unsigned code = 0, discriminator = 0;
    std::uint64_t cause = 0, recipient = 0;
    std::string transcript;
    std::vector<std::string> queues;
    std::uint64_t continuation = 0, broker = 0;
    int refused = 0, kept = 0;
    std::unique_ptr<ScriptHost> host;
};

NativeRunSpec spec_for(const std::string& name, std::optional<double> grid = std::nullopt) {
    NativeRunSpec s;
    s.event_retention = NativeEventRetention::Full;
    s.identity = {"k-oca-keep-" + name, 1};
    s.input_tf = "1";
    s.script_tf = "1";
    s.tickerid = "K-OCA-KEEP:PROBE";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 1e9;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    if (grid) s.quantity_grid = *grid;
    return s;
}

std::vector<Bar> flat_bars(int n, double price = 100.0) {
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i) {
        bars.push_back(Bar{price, price, price, price, 10.0, 60'000LL * (i + 1)});
    }
    return bars;
}

Outcome run_one(const std::string& name, const Script& script, bool direct, Mode mode,
                const std::vector<Bar>& tape = flat_bars(8),
                std::optional<double> grid = std::nullopt) {
    Outcome out;
    out.host = std::make_unique<ScriptHost>(direct, mode);
    out.host->script = script;
    CHECK(out.host->configure_native(spec_for(name, grid)).status == NativeSetupStatus::Applied);
    out.host->run(tape.data(), static_cast<int>(tape.size()));
    const auto state = out.host->native_state();
    out.completed = state.kind == NativeLifecycleKind::Completed;
    out.code = static_cast<unsigned>(state.failure.code);
    out.discriminator = static_cast<unsigned>(state.failure.discriminator);
    out.cause = state.failure.context.cause.ordinal;
    out.recipient = state.failure.context.recipient.incarnation;
    out.transcript = transcript(*out.host);
    out.queues = out.host->queues;
    out.continuation = out.host->native_continuation_hash();
    out.broker = out.host->broker_state_hash();
    out.refused = out.host->refused;
    out.kept = out.host->kept;
    return out;
}

// Every option set on both paths: the paths agree bit for bit, every run
// completes, and every option set records the plain replace's run.
void run_directed(const std::string& name, const Script& script,
                  const std::function<void(const Outcome&, Mode)>& expect) {
    std::string reference;
    std::vector<std::string> reference_queues;
    for (const Mode mode : kModes) {
        scenario = name + "/" + mode_name(mode);
        const Outcome staged = run_one(name, script, false, mode);
        const Outcome direct = run_one(name, script, true, mode);
        CHECK(staged.completed == direct.completed);
        CHECK(staged.code == direct.code && staged.discriminator == direct.discriminator);
        CHECK(staged.transcript == direct.transcript);
        CHECK(staged.queues == direct.queues);
        CHECK(staged.continuation == direct.continuation);
        CHECK(staged.broker == direct.broker);
        std::printf("%-14s %-24s completed=%d code=%u disc=%u cause=%llu recipient=%llu kept=%d\n",
                    name.c_str(), mode_name(mode), staged.completed ? 1 : 0, staged.code,
                    staged.discriminator, static_cast<unsigned long long>(staged.cause),
                    static_cast<unsigned long long>(staged.recipient), staged.kept);
        CHECK(staged.completed);
        CHECK(staged.code == 0 && staged.discriminator == 0);
        CHECK(staged.refused == 0);
        CHECK(staged.kept == (keeps_handle(mode) ? 1 : 0));
        if (mode == Mode::Replace) {
            reference = staged.transcript;
            reference_queues = staged.queues;
        } else {
            CHECK(staged.transcript == reference);
            CHECK(staged.queues == reference_queues);
        }
        expect(staged, mode);
    }
}

// The drain's two events, B's then A's, after the member's fill: their lines.
std::vector<std::string> drain_after(const std::string& transcript, const std::string& filler) {
    std::vector<std::string> lines;
    std::size_t at = 0;
    bool seen = false;
    while (at < transcript.size()) {
        const std::size_t end = transcript.find('\n', at);
        const std::string line = transcript.substr(at, end - at);
        at = end + 1;
        if (!seen) {
            seen = line.find(" Applied " + filler + " ") != std::string::npos;
            continue;
        }
        if (line.find(" Reduced ") != std::string::npos
            || line.find(" Cancelled ") != std::string::npos
            || line.find(" Deferred ") != std::string::npos) {
            lines.push_back(line);
        } else {
            break;
        }
    }
    return lines;
}

std::uint64_t applied_ordinal(const std::string& transcript, const std::string& name) {
    const std::size_t at = transcript.find(" Applied " + name + " ");
    if (at == std::string::npos) return 0;
    const std::size_t start = transcript.rfind('\n', at);
    return std::stoull(transcript.substr(start == std::string::npos ? 0 : start + 1));
}

// ---- Directed cases -----------------------------------------------------------

// K-ULP5's probe: two resting buys of 5 in one group, A re-priced, a market
// buy of 1 in the group fills.
void probe(no::GroupEffect effect) {
    Script s;
    s[0] = [effect](ScriptHost& p) {
        auto a = in_group(tx(5.0), 9, 2, effect);
        a.trigger = no::Limit{1.0};
        p.sub("A", a);
        auto b = in_group(tx(5.0), 9, 3, effect);
        b.trigger = no::Limit{1.0};
        p.sub("B", b);
    };
    s[1] = [effect](ScriptHost& p) {
        auto a = in_group(tx(5.0), 9, 2, effect);
        a.trigger = no::Limit{2.0};
        p.rep("A", a);
    };
    s[2] = [effect](ScriptHost& p) {
        p.sub("filler", in_group(tx(1.0), 9, 1, effect));
        p.queues.push_back(p.queue());
    };
    run_directed(std::string("probe-") + effect_name(effect), s,
                 [effect](const Outcome& o, Mode) {
        // A re-price moves A behind B, whichever option set.
        CHECK(o.queues == std::vector<std::string>{"B A filler "});
        const auto cause = std::to_string(applied_ordinal(o.transcript, "filler"));
        const auto drain = drain_after(o.transcript, "filler");
        CHECK(drain.size() == 2);
        if (drain.size() != 2) return;
        if (effect == no::GroupEffect::Reduce) {
            CHECK(drain[0].find("Reduced B cause " + cause + " 1 5 -> 4") != std::string::npos);
            CHECK(drain[1].find("Reduced A cause " + cause + " 1 5 -> 4") != std::string::npos);
        } else {
            const auto group = std::to_string(static_cast<int>(no::CancelReason::Group));
            CHECK(drain[0].find("Cancelled B reason " + group + " cause " + cause)
                  != std::string::npos);
            CHECK(drain[1].find("Cancelled A reason " + group + " cause " + cause)
                  != std::string::npos);
        }
    });
}

// A long of 10; two closes in one group, A a trail, B a stop the run never
// reaches; A re-priced with its running best retained; a market close of 1 in
// the group.
void trail(no::GroupEffect effect) {
    Script s;
    s[0] = [](ScriptHost& p) { p.sub("long", tx(10.0)); };
    s[1] = [effect](ScriptHost& p) {
        auto a = in_group(red(2.0), 7, 2, effect);
        no::Trail t;
        t.offset = 1.0;
        a.trigger = t;
        p.sub("A", a);
        auto b = in_group(red(2.0), 7, 3, effect);
        b.trigger = no::Stop{50.0};
        p.sub("B", b);
    };
    s[3] = [effect](ScriptHost& p) {
        auto a = in_group(red(2.0), 7, 2, effect);
        no::Trail t;
        t.offset = 1.5;
        a.trigger = t;
        p.rep("A", a, /*retain=*/true);
    };
    s[4] = [effect](ScriptHost& p) {
        p.sub("filler", in_group(red(1.0), 7, 1, effect));
        p.queues.push_back(p.queue());
    };
    run_directed(std::string("trail-") + effect_name(effect), s,
                 [effect](const Outcome& o, Mode) {
        CHECK(o.queues == std::vector<std::string>{"B A filler "});
        const auto cause = std::to_string(applied_ordinal(o.transcript, "filler"));
        const auto drain = drain_after(o.transcript, "filler");
        CHECK(drain.size() == 2);
        if (drain.size() != 2) return;
        if (effect == no::GroupEffect::Reduce) {
            CHECK(drain[0].find("Reduced B cause " + cause + " 1 2 -> 1") != std::string::npos);
            CHECK(drain[1].find("Reduced A cause " + cause + " 1 2 -> 1") != std::string::npos);
        } else {
            CHECK(drain[0].find("Cancelled B") != std::string::npos);
            CHECK(drain[1].find("Cancelled A") != std::string::npos);
        }
    });
}

// A long of 10; two sell limits above the market in one group, each bound to
// the book at its first point; A re-priced (keep_binding carries its binding);
// a market close of 1 in the group.
void bound(no::GroupEffect effect) {
    Script s;
    s[0] = [](ScriptHost& p) { p.sub("long", tx(10.0)); };
    s[2] = [effect](ScriptHost& p) {
        auto a = in_group(red(3.0), 5, 2, effect);
        a.trigger = no::Limit{150.0};
        p.sub("A", a);
        auto b = in_group(red(3.0), 5, 3, effect);
        b.trigger = no::Limit{160.0};
        p.sub("B", b);
    };
    s[4] = [effect](ScriptHost& p) {
        auto a = in_group(red(3.0), 5, 2, effect);
        a.trigger = no::Limit{155.0};
        p.rep("A", a);
    };
    s[5] = [effect](ScriptHost& p) {
        p.sub("filler", in_group(red(1.0), 5, 1, effect));
        p.queues.push_back(p.queue());
    };
    run_directed(std::string("bound-") + effect_name(effect), s,
                 [effect](const Outcome& o, Mode) {
        CHECK(o.queues == std::vector<std::string>{"B A filler "});
        const auto cause = std::to_string(applied_ordinal(o.transcript, "filler"));
        const auto drain = drain_after(o.transcript, "filler");
        CHECK(drain.size() == 2);
        if (drain.size() != 2) return;
        if (effect == no::GroupEffect::Reduce) {
            CHECK(drain[0].find("Reduced B cause " + cause + " 1 3 -> 2") != std::string::npos);
            CHECK(drain[1].find("Reduced A cause " + cause + " 1 3 -> 2") != std::string::npos);
        } else {
            CHECK(drain[0].find("Cancelled B") != std::string::npos);
            CHECK(drain[1].find("Cancelled A") != std::string::npos);
        }
    });
}

// K-ULP5's finding 2 (item 5 of this lane): on a 0.1 quantity grid, a request
// whose pending group deduction its units absorb settles them all in one fill
// (settles_in_one_fill, src/native_execution_consumer.cpp). L1 =
// 0.30000000000000004 less a bound close of 0.3 leaves one ulp,
// 5.5511151231257827e-17; L2 = 1001.1 less a bound close of 1000 leaves
// 1.1000000000000227, off the grid. F, bound to L2, closes it all: a
// ScopeFraction{1}, or a host-sized close the host answers with its scope's
// total. D, a market Reduce of the dust lot bound to L1 in F's Reduce group,
// defers the dust into F's pending total, below half an ulp of the
// 1.1000000000000227 F settles. F rests behind a stop the price later falls
// through (the matcher's path), or -- the current path -- F and D are
// accepted in one callback, D is executed there, and F is previewed and
// executed after it. On 2a03c658 the fraction was floored to
// 1.1000000000000001 and left a dust lot of 2.2648549702353193e-14, and the
// host-sized close was refused InvalidTerms -- as it was before K-ULP5 -- and
// left the lot open; each now closes it whole.
enum class GridClose { Fraction, HostSized, HostSizedCurrent };

void grid_absorbed(GridClose kind) {
    const bool current = kind == GridClose::HostSizedCurrent;
    Script s;
    s[0] = [](ScriptHost& p) {
        p.sub("L1", tx(0.30000000000000004));
        p.sub("L2", tx(1001.1));
    };
    s[2] = [](ScriptHost& p) {
        auto cut1 = red(0.3);
        cut1.owner = no::BindOpening{p.h["L1"], p.cycle()};
        p.sub("cut1", cut1);
        auto cut2 = red(1000.0);
        cut2.owner = no::BindOpening{p.h["L2"], p.cycle()};
        p.sub("cut2", cut2);
    };
    s[4] = [kind, current](ScriptHost& p) {
        no::Request f;
        if (kind == GridClose::Fraction) {
            f.intent = no::Reduce{no::ScopeFraction{1.0}};
        } else {
            f.intent = no::HostSized{no::HostSizedKind::Close, std::nullopt};
        }
        f = in_group(f, 4, 1, no::GroupEffect::Reduce);
        if (!current) f.trigger = no::Stop{95.0};
        f.owner = no::BindOpening{p.h["L2"], p.cycle()};
        p.sub("F", f);
        const auto lots = p.native_open_lots(std::numeric_limits<double>::quiet_NaN());
        if (lots.empty()) return;
        auto d = in_group(red(lots.front().signed_units), 4, 2, no::GroupEffect::Reduce);
        d.owner = no::BindOpening{p.h["L1"], p.cycle()};
        p.sub("D", d);
        if (!current) return;
        NativeCurrentExecution run;
        run.target = p.h["D"];
        const auto d_result = p.execute_current(run);
        CHECK(std::holds_alternative<no::ExecutionAppliedEvent>(d_result));
        run.target = p.h["F"];
        p.preview = p.inspect_current_execution(run);
        p.executed = p.execute_current(run);
        p.live_after = p.core().find_live(p.h["F"]) != nullptr;
    };
    std::vector<Bar> tape;
    for (int i = 0; i < 12; ++i) {
        const double px = i < 8 ? 100.0 : 90.0;
        tape.push_back(Bar{px, px, px, px, 10.0, 60'000LL * (i + 1)});
    }
    const double dust = 0.30000000000000004 - 0.3;
    const double residue = 1001.1 - 1000.0;
    const std::string name = kind == GridClose::Fraction ? "grid-absorbed-fraction"
        : current ? "grid-absorbed-host-current" : "grid-absorbed-host";
    std::optional<Outcome> staged;
    for (const bool direct : {false, true}) {
        scenario = name + (direct ? "/direct" : "/staged");
        Outcome o = run_one(name, s, direct, Mode::Replace, tape, 0.1);
        CHECK(o.completed && o.code == 0 && o.discriminator == 0);
        CHECK(o.refused == 0);
        CHECK(!no::quantity_on_grid(residue, 0.1));
        CHECK(dust > 0.0 && residue - dust == residue);
        double d_closed = -1.0, f_closed = -1.0, pending = -1.0, deduction = -1.0;
        int f_rejected = -1;
        for (const auto& row : o.host->native_events(0)) {
            if (!row.command) continue;
            if (const auto* a = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
                if (o.host->name_of(a->handle()) == "D") d_closed = a->closed_units;
                if (o.host->name_of(a->handle()) == "F") f_closed = a->closed_units;
            } else if (const auto* t = std::get_if<no::TermsResolvedEvent>(&*row.command)) {
                if (o.host->name_of(t->handle()) == "F") {
                    pending = t->pending_total;
                    deduction = t->effective_deduction;
                }
            } else if (const auto* m = std::get_if<no::MatchRejectedEvent>(&*row.command)) {
                if (o.host->name_of(m->handle()) == "F") f_rejected = static_cast<int>(m->reason);
            }
        }
        // D closes the dust; F takes nothing off its units for it and closes
        // L2's residue whole, unfloored and admitted; the book is flat.
        CHECK(d_closed == dust);
        CHECK(pending == dust && deduction == 0.0);
        CHECK(f_rejected == -1);
        CHECK(f_closed == residue);
        const auto lots = o.host->native_open_lots(std::numeric_limits<double>::quiet_NaN());
        CHECK(lots.empty());
        // The preview, taken while D's dust stood pending on F, refuses nothing,
        // and the current execution closes the residue there and then.
        bool refused_terms = false;
        if (current) {
            CHECK(o.host->preview.has_value());
            if (o.host->preview) {
                CHECK(!o.host->preview->refusal);
                refused_terms = o.host->preview->terms_rejection.has_value();
            }
            CHECK(!refused_terms);
            const auto* applied = o.host->executed
                ? std::get_if<no::ExecutionAppliedEvent>(&*o.host->executed) : nullptr;
            CHECK(applied != nullptr);
            CHECK(applied && applied->closed_units == residue);
            CHECK(!o.host->live_after);
        }
        std::printf("%-34s completed=%d D=%.17g F=%.17g pending=%.17g deducted=%.17g "
                    "rejected=%d preview_terms_rejected=%d lots=%zu continuation=%016llx "
                    "broker=%016llx\n",
                    scenario.c_str(), o.completed ? 1 : 0, d_closed, f_closed, pending, deduction,
                    f_rejected, refused_terms ? 1 : 0, lots.size(),
                    static_cast<unsigned long long>(o.continuation),
                    static_cast<unsigned long long>(o.broker));
        if (!direct) {
            staged = std::move(o);
        } else if (staged) {
            CHECK(o.transcript == staged->transcript);
            CHECK(o.continuation == staged->continuation && o.broker == staged->broker);
        }
    }
}

// ---- The order core itself ------------------------------------------------------

const no::RunIdentity kCoreRun{"k-oca-keep-core", 1};

no::EvaluationContext print_ctx(std::uint64_t point) {
    no::EvaluationContext context;
    context.cursor.point.ordinal = point;
    context.cursor.point.effective_time_ms = 1000;
    context.cursor.point.provenance = NativePriceProvenance::ObservedPrint;
    context.driver_class = no::DriverEligibilityClass::ObservedPrint;
    context.existing_matching_bit = true;
    return context;
}

no::TargetObservation flat_book() {
    no::TargetObservation observation;
    observation.current_position = no::PositionFlat{};
    return observation;
}

no::TargetObservation long_book(std::int64_t cycle) {
    no::TargetObservation observation;
    observation.current_position = no::PositionNonflat{cycle, no::Side::Long};
    return observation;
}

void commit(no::WorkingRequestCore& core, no::Preparation<no::PreparedMutation> prep,
            std::uint64_t& ord) {
    CHECK(std::holds_alternative<no::PreparedMutation>(prep));
    if (!std::holds_alternative<no::PreparedMutation>(prep)) return;
    auto installed = core.install_mutation(std::get<no::PreparedMutation>(std::move(prep)));
    CHECK(std::holds_alternative<no::Installed>(installed));
    if (const auto* in = std::get_if<no::Installed>(&installed)) ord += in->events.count;
}

// Fills a market Transact of `units` at one print: its applied event's id.
no::EventId fill(no::WorkingRequestCore& core, const no::RequestHandle& handle, double units,
                 std::uint64_t& ord) {
    commit(core, core.prepare_evaluation(handle, print_ctx(ord + 1), flat_book(), ord), ord);
    const auto* live = core.find_live(handle);
    CHECK(live != nullptr);
    if (!live) return {};
    no::ExecutionProposal proposal;
    proposal.cursor.point.ordinal = std::get<no::AllowanceUnits>(live->allowance).point_ordinal;
    proposal.cursor.point.effective_time_ms = 1000;
    proposal.raw_price = 100;
    proposal.resolved_price = 100;
    proposal.physical_action = no::Transact{units};
    proposal.scope = execution::Book{};
    proposal.inspected_closed_units = 0.0;
    proposal.inspected_opened_units = units;
    proposal.inspected_current_ticket = 1;
    auto prep = core.prepare_execution(handle, proposal, ord);
    CHECK(std::holds_alternative<no::PreparedExecution>(prep));
    if (!std::holds_alternative<no::PreparedExecution>(prep)) return {};
    no::CommittedExecutionFacts facts;
    facts.result.status = execution::Status::Applied;
    facts.result.opened_units = units;
    facts.result.current_ticket = 1;
    facts.cycle_after = 1;
    facts.post_target = long_book(1);
    facts.committed_action = proposal.physical_action;
    auto installed =
            core.install_execution(std::get<no::PreparedExecution>(std::move(prep)), facts);
    CHECK(std::holds_alternative<no::Installed>(installed));
    if (const auto* in = std::get_if<no::Installed>(&installed)) ord += in->events.count;
    return no::EventId{kCoreRun, std::get<no::ExecutionAppliedEvent>(core.history().back()).ordinal};
}

// One recipient's group effect, staged (prepare, then install) or direct
// (apply): 0 installed, 1 no change, 2 refused.
struct Step {
    int kind = -1;
    no::NoChangeReason reason = no::NoChangeReason::NoTransition;
    no::CoreFailure code = no::CoreFailure::Invariant;
};

Step effect_on(no::WorkingRequestCore& core, const no::EventId& cause,
               const no::RequestHandle& recipient, std::uint64_t& ord, bool direct) {
    Step step;
    if (direct) {
        auto result = core.apply_group_effect(cause, recipient, ord);
        if (const auto* in = std::get_if<no::Installed>(&result)) {
            ord += in->events.count;
            step.kind = 0;
        } else if (const auto* none = std::get_if<no::NoChange>(&result)) {
            step.kind = 1;
            step.reason = none->reason;
        } else {
            step.kind = 2;
            step.code = std::get<no::PreparationError>(result).code;
        }
        return step;
    }
    auto prep = core.prepare_group_effect(cause, recipient, ord);
    if (std::holds_alternative<no::PreparedMutation>(prep)) {
        commit(core, std::move(prep), ord);
        step.kind = 0;
    } else if (const auto* none = std::get_if<no::NoChange>(&prep)) {
        step.kind = 1;
        step.reason = none->reason;
    } else {
        step.kind = 2;
        step.code = std::get<no::PreparationError>(prep).code;
    }
    return step;
}

no::RequestHandle member(no::WorkingRequestCore& core, double units, double limit,
                         std::int64_t cohort, no::GroupEffect effect, std::uint64_t& inc,
                         std::uint64_t& ord) {
    no::Request r = in_group(tx(units), 9, cohort, effect);
    if (limit > 0.0) r.trigger = no::Limit{limit};
    const auto res = core.submit(r, 1, inc, ord);
    CHECK(res.status == no::SubmitStatus::Accepted && res.handle);
    return res.handle ? *res.handle : no::RequestHandle{};
}

bool is_already_applied(const Step& step) {
    return step.kind == 1 && step.reason == no::NoChangeReason::AlreadyApplied;
}

void core_rows(no::GroupEffect effect, bool direct) {
    scenario = std::string("core-") + effect_name(effect) + (direct ? "-direct" : "-staged");
    // One cause, its recipients in queue order and in reverse.
    for (const bool reverse : {false, true}) {
        no::WorkingRequestCore core(kCoreRun);
        std::uint64_t inc = 1;
        std::uint64_t ord = 1;
        const auto a = member(core, 5.0, 1.0, 2, effect, inc, ord);
        const auto b = member(core, 5.0, 1.0, 3, effect, inc, ord);
        // A re-priced in place: it keeps its handle and moves behind B.
        no::ReplaceOptions keep;
        keep.keep_handle = true;
        no::Request moved = in_group(tx(5.0), 9, 2, effect);
        moved.trigger = no::Limit{2.0};
        const auto replaced = core.replace(a, moved, 1, inc, ord, std::nullopt, keep);
        CHECK(replaced.status == no::ReplaceStatus::Replaced);
        CHECK(replaced.successor && *replaced.successor == a);
        CHECK(core.live().size() == 2 && core.live()[0].handle() == b
              && core.live()[1].handle() == a);
        const auto filler = member(core, 1.0, 0.0, 1, effect, inc, ord);
        const no::EventId cause = fill(core, filler, 1.0, ord);
        // Queue order: B, then the re-priced A -- not incarnation order.
        CHECK(core.group_recipients(cause) == (std::vector<no::RequestHandle>{b, a}));
        const no::RequestHandle first = reverse ? a : b;
        const no::RequestHandle second = reverse ? b : a;
        const std::size_t receipts = core.group_effect_receipt_count();
        const Step one = effect_on(core, cause, first, ord, direct);
        CHECK(one.kind == 0);
        const Step two = effect_on(core, cause, second, ord, direct);
        CHECK(two.kind == 0);
        if (two.kind == 2) {
            std::printf("  [%s reverse=%d] second recipient refused code=%u\n", scenario.c_str(),
                        reverse ? 1 : 0, static_cast<unsigned>(two.code));
        }
        // Read back in commit order.
        CHECK(core.group_effect_receipt_count() == receipts + 2);
        if (core.group_effect_receipt_count() == receipts + 2) {
            const auto r0 = core.group_effect_receipt(receipts);
            const auto r1 = core.group_effect_receipt(receipts + 1);
            CHECK(r0.cause == cause && r0.recipient == first && r0.effect == effect);
            CHECK(r1.cause == cause && r1.recipient == second && r1.effect == effect);
            CHECK(r0.outcome_ordinal < r1.outcome_ordinal);
        }
        // A replay finds each receipt, on either path, and records nothing.
        const std::size_t history = core.history_end();
        for (const auto& recipient : {a, b}) {
            CHECK(is_already_applied(effect_on(core, cause, recipient, ord, false)));
            CHECK(is_already_applied(effect_on(core, cause, recipient, ord, true)));
        }
        CHECK(core.history_end() == history);
        if (effect == no::GroupEffect::Reduce) {
            for (const auto& recipient : {a, b}) {
                const auto* live = core.find_live(recipient);
                CHECK(live && std::holds_alternative<no::RemainingUnits>(live->remaining)
                      && std::get<no::RemainingUnits>(live->remaining).q == 4.0);
            }
        } else {
            CHECK(core.find_live(a) == nullptr && core.find_live(b) == nullptr);
        }
    }
    // One cause drained in queue order, a newer cause's receipts after it, and
    // the older cause again: its receipts are found -- the lookup's walk stops
    // at the cause's end -- and a recipient its drain never reached is refused.
    {
        no::WorkingRequestCore core(kCoreRun);
        std::uint64_t inc = 1;
        std::uint64_t ord = 1;
        const auto a = member(core, 5.0, 1.0, 2, effect, inc, ord);
        const auto b = member(core, 5.0, 1.0, 3, effect, inc, ord);
        const auto c = member(core, 5.0, 1.0, 5, effect, inc, ord);
        no::ReplaceOptions keep;
        keep.keep_handle = true;
        no::Request moved = in_group(tx(5.0), 9, 2, effect);
        moved.trigger = no::Limit{2.0};
        CHECK(core.replace(a, moved, 1, inc, ord, std::nullopt, keep).status
              == no::ReplaceStatus::Replaced);
        const auto f1 = member(core, 1.0, 0.0, 1, effect, inc, ord);
        const no::EventId older = fill(core, f1, 1.0, ord);
        CHECK(core.group_recipients(older) == (std::vector<no::RequestHandle>{b, c, a}));
        // c is left out of the older cause's drain.
        CHECK(effect_on(core, older, b, ord, direct).kind == 0);
        CHECK(effect_on(core, older, a, ord, direct).kind == 0);
        const auto f2 = member(core, 1.0, 0.0, 4, effect, inc, ord);
        const no::EventId newer = fill(core, f2, 1.0, ord);
        const auto later = core.group_recipients(newer);
        CHECK(!later.empty());
        for (const auto& recipient : later) {
            CHECK(effect_on(core, newer, recipient, ord, direct).kind == 0);
        }
        const std::size_t history = core.history_end();
        const std::size_t receipts = core.group_effect_receipt_count();
        for (const auto& recipient : {a, b}) {
            CHECK(is_already_applied(effect_on(core, older, recipient, ord, false)));
            CHECK(is_already_applied(effect_on(core, older, recipient, ord, true)));
        }
        const Step skipped_staged = effect_on(core, older, c, ord, false);
        const Step skipped_direct = effect_on(core, older, c, ord, true);
        CHECK(skipped_staged.kind == 2 && skipped_staged.code == no::CoreFailure::InvalidCause);
        CHECK(skipped_direct.kind == 2 && skipped_direct.code == no::CoreFailure::InvalidCause);
        CHECK(core.history_end() == history);
        CHECK(core.group_effect_receipt_count() == receipts);
    }
    // A receipt for a cause older than the newest one's is still refused.
    {
        no::WorkingRequestCore core(kCoreRun);
        std::uint64_t inc = 1;
        std::uint64_t ord = 1;
        const auto a = member(core, 5.0, 1.0, 2, effect, inc, ord);
        const auto b = member(core, 5.0, 1.0, 3, effect, inc, ord);
        const auto f1 = member(core, 1.0, 0.0, 1, effect, inc, ord);
        const no::EventId older = fill(core, f1, 1.0, ord);
        const auto f2 = member(core, 1.0, 0.0, 4, effect, inc, ord);
        const no::EventId newer = fill(core, f2, 1.0, ord);
        CHECK(older.ordinal < newer.ordinal);
        CHECK(effect_on(core, newer, b, ord, direct).kind == 0);
        const std::size_t history = core.history_end();
        const std::size_t receipts = core.group_effect_receipt_count();
        const Step late = effect_on(core, older, a, ord, direct);
        CHECK(late.kind == 2 && late.code == no::CoreFailure::InvalidCause);
        CHECK(core.history_end() == history);
        CHECK(core.group_effect_receipt_count() == receipts);
        CHECK(effect_on(core, newer, a, ord, direct).kind == 0);
        const Step late_b = effect_on(core, older, b, ord, direct);
        CHECK(late_b.kind == 2 && late_b.code == no::CoreFailure::InvalidCause);
        CHECK(is_already_applied(effect_on(core, newer, a, ord, direct)));
        CHECK(is_already_applied(effect_on(core, newer, b, ord, direct)));
    }
}

// ---- The seeded battery -------------------------------------------------------

enum class Kind : std::uint8_t { Entry, Exit, Child };

struct Slot {
    int id = 0;
    Kind kind = Kind::Exit;
    std::optional<no::RequestHandle> handle;
    int parent = -1;             // a child's entry
    int trigger = 0;             // 0 limit, 1 stop, 2 trail, 3 market
    int intent = 0;              // exits: 0 explicit, 1 flatten, 2 fraction, 3 host-sized
    bool buy = true;             // entries
    double units = 1.0;          // entries and explicit exits
    std::uint64_t group = 0;     // 0: none
    std::int64_t cohort = 0;
    no::GroupEffect effect = no::GroupEffect::Cancel;
    long level = 0;              // quarter ticks, the last one placed
};

struct Tally {
    long runs = 0, completed = 0, stopped = 0;
    long fills = 0, group_fills = 0;
    long reduced = 0, deferred = 0, group_cancels = 0;
    long replaces = 0, kept = 0, retained = 0, cancels = 0;
    long drains = 0;
    long misordered = 0;   // drains whose recipients came in other than incarnation order
    long misordered_cancel = 0, misordered_reduce = 0;
    long misordered_deferred = 0;   // ... with a recipient whose units were deferred
    void add(const Tally& o) {
        runs += o.runs; completed += o.completed; stopped += o.stopped;
        fills += o.fills; group_fills += o.group_fills;
        reduced += o.reduced; deferred += o.deferred; group_cancels += o.group_cancels;
        replaces += o.replaces; kept += o.kept; retained += o.retained; cancels += o.cancels;
        drains += o.drains; misordered += o.misordered;
        misordered_cancel += o.misordered_cancel; misordered_reduce += o.misordered_reduce;
        misordered_deferred += o.misordered_deferred;
    }
};

struct Record {
    std::vector<std::uint64_t> fills;     // one word per fill
    std::vector<std::uint64_t> events;    // one word per journal event
    std::vector<std::uint64_t> bars;      // queue order and position, every bar
    std::vector<std::uint64_t> answers;   // replace and cancel answers
    std::vector<std::uint64_t> receipts;  // group-effect receipts, in commit order
    std::vector<std::uint64_t> trades;
    std::vector<std::uint64_t> trades_no_incarnation;
    std::vector<std::uint64_t> hashes;    // continuation every bar (paths only)
    std::uint64_t broker = 0;
    std::string error;
    bool completed = false;
    unsigned code = 0, discriminator = 0;
    Tally tally;
};

class GroupHost final : public NativeStrategyHost {
public:
    GroupHost(std::uint64_t seed, Mode mode, bool direct)
        : seed_(seed), mode_(mode), rng_(seed) {
        as_native_consumer(execution_consumer()).set_direct_mutation(direct);
    }
    Record record;

    void on_native_run_begin() override {
        rng_ = k3_book::Rng(seed_);
        slots_.clear();
        by_incarnation_.clear();
        next_group_ = 1;
        bar_ = 0;
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ++bar_;
        ref_ = static_cast<long>(bar.close * 4.0);
        refresh();
        observe_bar();
        // A flat book is entered at the market now and then, so the closes
        // have a position to bind to.
        if (physical_position().signed_units == 0.0 && rng_.percent(40)) {
            Slot entry;
            entry.kind = Kind::Entry;
            entry.buy = rng_.percent(50);
            entry.units = static_cast<double>(rng_.between(2, 6));
            entry.trigger = 3;
            place(entry);
        }
        if (rng_.percent(45)) new_group();
        if (rng_.percent(30)) join_group();
        reprice_some(rng_.between(0, 4));
        if (rng_.percent(12)) cancel_one();
        // A bounded book.
        while (live_count() > 28) cancel_one();
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        const int slot = slot_of(event.handle());
        full_fold::Fold f;
        f.i(slot);
        f.u(event.ordinal);
        f.u(event.cursor.point.ordinal);
        f.i(event.cursor.point.interval_index);
        f.i(event.cursor.point.effective_time_ms);
        f.e(event.cursor.point.path_phase);
        f.d(event.cursor.t);
        f.d(event.raw_price);
        f.d(event.resolved_price);
        f.d(event.closed_units);
        f.d(event.opened_units);
        f.d(event.filled_working);
        f.b(event.terminal);
        f.i(event.cycle_before);
        f.i(event.cycle_after);
        record.fills.push_back(f.h);
        ++record.tally.fills;
        if (std::holds_alternative<no::Member>(event.request().group)) ++record.tally.group_fills;
        // Commands from the fill callback: a re-price on the rest of the path,
        // and now and then a new member.
        if (rng_.percent(30)) reprice_some(1);
        if (rng_.percent(10)) join_group();
    }

    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        no::ExecutionTerms terms{facts.default_resolved_price, std::nullopt,
                                 no::OpeningShape::Transact};
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            const double scope = facts.scope_exposure_units;
            terms.units = scope > 0.0 ? (scope < 2.0 ? scope : 2.0) : 1.0;
        }
        return terms;
    }

    const no::WorkingRequestCore& core() const {
        return as_native_consumer(execution_consumer()).request_core();
    }

    int slot_of(const no::RequestHandle& handle) const {
        const auto found = by_incarnation_.find(handle.incarnation);
        return found == by_incarnation_.end() ? -1 : found->second;
    }

private:
    void observe_bar() {
        full_fold::Fold f;
        f.i(bar_);
        for (const auto& live : core().live()) f.i(slot_of(live.handle()));
        f.d(physical_position().signed_units);
        f.u(physical_position().lot_count);
        record.bars.push_back(f.h);
        record.hashes.push_back(native_continuation_hash());
    }

    void refresh() {
        for (auto& slot : slots_) {
            if (slot.handle && !core().find_live(*slot.handle)) slot.handle.reset();
        }
    }

    long live_count() const {
        return static_cast<long>(std::count_if(slots_.begin(), slots_.end(),
                                               [](const Slot& s) { return s.handle.has_value(); }));
    }

    bool has_waiting_children(int parent) const {
        for (const auto& slot : slots_) {
            if (slot.kind != Kind::Child || slot.parent != parent || !slot.handle) continue;
            const auto* live = core().find_live(*slot.handle);
            if (live && std::holds_alternative<no::Wait>(live->authority)) return true;
        }
        return false;
    }

    no::Request request_for(const Slot& slot, bool retain) const {
        no::Request request;
        if (slot.kind == Kind::Entry) {
            request.intent = no::Transact{slot.buy ? slot.units : -slot.units};
            request.label = "e" + std::to_string(slot.id);
        } else {
            switch (slot.intent) {
            case 0: request.intent = no::Reduce{no::ExplicitUnits{slot.units}}; break;
            case 1: request.intent = no::Flatten{}; break;
            case 2: request.intent = no::Reduce{no::ScopeFraction{0.5}}; break;
            default: request.intent = no::HostSized{no::HostSizedKind::Close, std::nullopt}; break;
            }
            request.label = "x" + std::to_string(slot.id);
        }
        if (slot.kind == Kind::Child) {
            const auto& parent = slots_[static_cast<std::size_t>(slot.parent)];
            no::WaitForApplied wait;
            wait.parent = parent.handle ? *parent.handle : no::RequestHandle{};
            wait.scope = slot.intent == 1 ? no::NativeArmScope::Book : no::NativeArmScope::OwnerLot;
            request.owner = wait;
            if (slot.intent != 1) request.intent = no::Reduce{no::OwnerOpenedUnits{}};
        }
        switch (slot.trigger) {
        case 0: request.trigger = no::Limit{ticks(slot.level)}; break;
        case 1: request.trigger = no::Stop{ticks(slot.level)}; break;
        case 2: {
            no::Trail t;
            t.offset = ticks(2 + (slot.id % 5));
            if (!retain) t.arm_price = ticks(slot.level);
            request.trigger = t;
            break;
        }
        default: break;
        }
        if (slot.group != 0) request.group = no::Member{slot.group, slot.cohort, slot.effect};
        return request;
    }

    void place(Slot slot) {
        slot.id = static_cast<int>(slots_.size());
        if (slot.group != 0 && slot.cohort == 0) slot.cohort = slot.id + 1;
        const auto result = submit(request_for(slot, false));
        if (result.handle) {
            slot.handle = *result.handle;
            by_incarnation_[result.handle->incarnation] = slot.id;
        }
        full_fold::Fold f;
        f.i(slot.id);
        f.e(result.status);
        f.b(result.reason.has_value());
        if (result.reason) f.e(*result.reason);
        record.answers.push_back(f.h);
        slots_.push_back(slot);
    }

    // One member of `group`: an entry, a close, a bracket leg waiting for an
    // entry, or a market member whose fill drains the group at the next point.
    void add_member(std::uint64_t group, no::GroupEffect effect, std::int64_t cohort) {
        Slot slot;
        slot.group = group;
        slot.effect = effect;
        slot.cohort = cohort;
        const int pick = rng_.below(100);
        const long away = rng_.between(1, 14);
        if (pick < 30) {
            slot.kind = Kind::Entry;
            slot.buy = rng_.percent(50);
            slot.units = static_cast<double>(rng_.between(1, 3));
            slot.trigger = rng_.below(2);
            // A buy limit rests below the market, a buy stop above it.
            const bool below = slot.buy == (slot.trigger == 0);
            slot.level = ref_ + (below ? -away : away);
        } else if (pick < 62) {
            slot.kind = Kind::Exit;
            slot.intent = rng_.below(4);
            slot.units = static_cast<double>(rng_.between(1, 2));
            slot.trigger = rng_.below(3);
            slot.level = ref_ + (rng_.percent(50) ? away : -away);
        } else if (pick < 82) {
            // A leg waiting for a working entry, or a new entry of its own.
            int parent = -1;
            std::vector<int> entries;
            for (const auto& s : slots_) {
                if (s.kind == Kind::Entry && s.trigger != 3 && s.handle) entries.push_back(s.id);
            }
            if (!entries.empty() && rng_.percent(70)) {
                parent = entries[static_cast<std::size_t>(rng_.below(static_cast<int>(entries.size())))];
            } else {
                Slot entry;
                entry.kind = Kind::Entry;
                entry.buy = rng_.percent(50);
                entry.units = static_cast<double>(rng_.between(1, 3));
                entry.trigger = rng_.percent(50) ? 3 : 0;
                entry.level = ref_ + (entry.buy ? -away : away);
                place(entry);
                if (!slots_.back().handle) return;
                parent = slots_.back().id;
            }
            const Slot& owner = slots_[static_cast<std::size_t>(parent)];
            slot.kind = Kind::Child;
            slot.parent = parent;
            slot.intent = rng_.percent(25) ? 1 : 0;
            slot.trigger = rng_.below(3);
            slot.level = ref_ + ((owner.buy == (slot.trigger == 0)) ? away : -away);
        } else {
            // A market member: its fill drains the group.
            if (rng_.percent(50)) {
                slot.kind = Kind::Entry;
                slot.buy = rng_.percent(50);
                slot.units = static_cast<double>(rng_.between(1, 2));
            } else {
                slot.kind = Kind::Exit;
                slot.intent = rng_.percent(70) ? 0 : 2;
                slot.units = 1.0;
            }
            slot.trigger = 3;
        }
        place(slot);
    }

    void new_group() {
        const std::uint64_t group = next_group_++;
        const auto effect = rng_.percent(50) ? no::GroupEffect::Cancel : no::GroupEffect::Reduce;
        const int members = rng_.between(2, 5);
        std::int64_t shared = 0;
        for (int i = 0; i < members; ++i) {
            // Now and then a member shares its predecessor's cohort, which no
            // fill of the other reaches.
            const std::int64_t cohort = shared != 0 && rng_.percent(15) ? shared : 0;
            add_member(group, effect, cohort);
            shared = slots_.back().cohort;
        }
    }

    void join_group() {
        std::vector<int> members;
        for (const auto& slot : slots_) {
            if (slot.group != 0 && slot.handle) members.push_back(slot.id);
        }
        if (members.empty()) return;
        const Slot& other =
                slots_[static_cast<std::size_t>(members[static_cast<std::size_t>(rng_.below(static_cast<int>(members.size())))])];
        add_member(other.group, other.effect, 0);
    }

    // Re-prices up to `count` live requests, picked and ordered at random.
    void reprice_some(int count) {
        std::vector<int> live;
        for (const auto& slot : slots_) {
            if (slot.handle && slot.trigger != 3) live.push_back(slot.id);
        }
        for (std::size_t i = live.size(); i > 1; --i) {
            const std::size_t j = static_cast<std::size_t>(rng_.below(static_cast<int>(i)));
            std::swap(live[i - 1], live[j]);
        }
        for (int k = 0; k < count && k < static_cast<int>(live.size()); ++k) {
            if (slots_[static_cast<std::size_t>(live[static_cast<std::size_t>(k)])].handle) {
                reprice(live[static_cast<std::size_t>(k)]);
            }
        }
    }

    void reprice(int id) {
        Slot& slot = slots_[static_cast<std::size_t>(id)];
        slot.level = ref_ + (slot.level - ref_) / 2 + rng_.between(-3, 3);
        if (slot.level < 8) slot.level = 8;
        const bool retain = slot.trigger == 2 && rng_.percent(50);
        no::ReplaceOptions options;
        options.retain_trigger_state = retain;
        // A request whose children wait on it would end them under a plain
        // replace and keep them under keep_handle: the host keeps its handle
        // only where the two agree (R5 lane V19-D).
        if (keeps_handle(mode_) && !has_waiting_children(id)) options.keep_handle = true;
        if (keeps_binding(mode_)) options.keep_binding = true;
        // A leg whose entry is gone names no parent; its replace is refused.
        const auto result = replace(*slot.handle, request_for(slot, retain), options);
        ++record.tally.replaces;
        if (retain) ++record.tally.retained;
        full_fold::Fold f;
        f.i(slot.id);
        f.e(result.status);
        f.b(result.reason.has_value());
        if (result.reason) f.e(*result.reason);
        record.answers.push_back(f.h);
        if (result.status == no::ReplaceStatus::Replaced && result.successor) {
            if (*result.successor == *slot.handle) ++record.tally.kept;
            slot.handle = *result.successor;
            by_incarnation_[result.successor->incarnation] = slot.id;
        } else if (result.status != no::ReplaceStatus::ReplaceRejected) {
            slot.handle.reset();
        }
    }

    void cancel_one() {
        std::vector<int> live;
        for (const auto& slot : slots_) {
            if (slot.handle) live.push_back(slot.id);
        }
        if (live.empty()) return;
        Slot& slot = slots_[static_cast<std::size_t>(live[static_cast<std::size_t>(rng_.below(static_cast<int>(live.size())))])];
        const auto result = cancel(*slot.handle);
        ++record.tally.cancels;
        full_fold::Fold f;
        f.i(slot.id);
        f.e(result.status);
        record.answers.push_back(f.h);
        slot.handle.reset();
    }

    std::uint64_t seed_;
    Mode mode_;
    k3_book::Rng rng_;
    std::vector<Slot> slots_;
    std::map<std::uint64_t, int> by_incarnation_;
    std::uint64_t next_group_ = 1;
    long ref_ = 400;
    int bar_ = 0;
};

// The journal mapped to the host's slots, ordinal for ordinal, and what a
// group effect wrote, with its cause.
void read_journal(const GroupHost& host, Record& record) {
    auto cursor = [](full_fold::Fold& f, const no::MatchCursor& c) {
        f.u(c.point.ordinal);
        f.i(c.point.interval_index);
        f.i(c.point.effective_time_ms);
        f.e(c.point.path_phase);
        f.d(c.t);
    };
    for (const auto& row : host.native_events(0)) {
        if (row.kind != NativeEventKind::Command || !row.command) continue;
        full_fold::Fold f;
        bool keep = true;
        std::visit([&](const auto& event) {
            using E = std::decay_t<decltype(event)>;
            f.u(row.command->index());
            f.u(event.ordinal);
            if constexpr (std::is_same_v<E, no::CloseBoundEvent>) {
                keep = false;
            } else if constexpr (std::is_same_v<E, no::AcceptedEvent>) {
                f.i(host.slot_of(event.handle()));
            } else if constexpr (std::is_same_v<E, no::ReplacedEvent>) {
                f.i(host.slot_of(event.successor()));
            } else if constexpr (std::is_same_v<E, no::CancelledEvent>) {
                f.i(host.slot_of(event.handle()));
                f.e(event.reason);
                f.b(event.cause.has_value());
                if (event.cause) f.u(event.cause->ordinal);
                if (event.reason == no::CancelReason::Group) ++record.tally.group_cancels;
            } else if constexpr (std::is_same_v<E, no::NoEffectEvent>) {
                f.i(host.slot_of(event.handle()));
                cursor(f, event.cursor);
            } else if constexpr (std::is_same_v<E, no::MatchRejectedEvent>) {
                f.i(host.slot_of(event.handle()));
                f.e(event.reason);
                cursor(f, event.cursor);
            } else if constexpr (std::is_same_v<E, no::ActivatedEvent>) {
                f.i(host.slot_of(event.definition->handle));
                f.e(event.kind);
                f.d(event.reached_price);
                cursor(f, event.cursor);
            } else if constexpr (std::is_same_v<E, no::ArmedEvent>) {
                f.i(host.slot_of(event.definition->handle));
                f.u(event.after.index());
            } else if constexpr (std::is_same_v<E, no::ReservationReducedEvent>) {
                f.i(host.slot_of(event.recipient));
                f.u(event.cause.ordinal);
                f.d(event.requested_delta);
                f.d(event.actual_deduction);
                f.d(event.before.q);
                ++record.tally.reduced;
            } else if constexpr (std::is_same_v<E, no::DeferredGroupAdjustmentEvent>) {
                f.i(host.slot_of(event.recipient));
                f.u(event.cause.ordinal);
                f.d(event.deferred_delta);
                f.d(event.pending_after.total);
                f.u(event.pending_after.count);
                ++record.tally.deferred;
            } else if constexpr (std::is_same_v<E, no::ExecutionAppliedEvent>) {
                f.i(host.slot_of(event.handle()));
                f.d(event.resolved_price);
                f.d(event.filled_working);
                cursor(f, event.cursor);
            } else if constexpr (std::is_same_v<E, no::TermsResolvedEvent>) {
                f.i(host.slot_of(event.handle()));
                f.d(event.pending_total);
                f.d(event.effective_deduction);
                cursor(f, event.cursor);
            } else if constexpr (std::is_same_v<E, no::QuantityBoundEvent>) {
                f.i(host.slot_of(event.definition->handle));
                f.d(event.source_units);
                f.d(event.pending_total);
                f.d(event.effective_deduction);
            } else {
                // Rejections, NotWorking / InvalidHandle answers: the kind alone.
            }
        }, *row.command);
        if (keep) record.events.push_back(f.h);
    }
}

// The group-effect receipts the core committed, in commit order, mapped to the
// host's slots; and the drains they record: one cause's receipts, counted
// misordered when their recipients came in other than incarnation order --
// the drains the receipt check refused before this lane.
void read_receipts(const GroupHost& host, Record& record) {
    const auto& core = host.core();
    // The outcome events that deferred a deduction into a recipient's pending
    // total: their ordinals.
    std::vector<std::uint64_t> deferred;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* d = std::get_if<no::DeferredGroupAdjustmentEvent>(&*row.command)) {
            deferred.push_back(d->ordinal);
        }
    }
    std::uint64_t cause = 0;
    std::uint64_t last = 0;
    bool ordered = true;
    bool any_deferred = false;
    no::GroupEffect effect = no::GroupEffect::Cancel;
    auto close_drain = [&] {
        if (cause == 0) return;
        ++record.tally.drains;
        if (ordered) return;
        ++record.tally.misordered;
        ++(effect == no::GroupEffect::Cancel ? record.tally.misordered_cancel
                                             : record.tally.misordered_reduce);
        if (any_deferred) ++record.tally.misordered_deferred;
    };
    for (std::size_t index = 0; index < core.group_effect_receipt_count(); ++index) {
        const auto receipt = core.group_effect_receipt(index);
        full_fold::Fold f;
        f.u(receipt.cause.ordinal);
        f.i(host.slot_of(receipt.recipient));
        f.e(receipt.effect);
        f.u(receipt.outcome_ordinal);
        record.receipts.push_back(f.h);
        if (receipt.cause.ordinal != cause) {
            close_drain();
            cause = receipt.cause.ordinal;
            ordered = true;
            any_deferred = false;
            effect = receipt.effect;
        } else if (receipt.recipient.incarnation < last) {
            ordered = false;
        }
        last = receipt.recipient.incarnation;
        if (std::binary_search(deferred.begin(), deferred.end(), receipt.outcome_ordinal)) {
            any_deferred = true;
        }
    }
    close_drain();
}

void read_trades(const GroupHost& host, Record& record) {
    for (int index = 0; index < host.trade_count(); ++index) {
        const Trade& trade = host.get_trade(index);
        full_fold::Fold f;
        f.i(trade.entry_time);
        f.i(trade.exit_time);
        f.d(trade.entry_price);
        f.d(trade.exit_price);
        f.d(trade.qty);
        f.d(trade.pnl);
        f.b(trade.is_long);
        f.i(trade.entry_bar_index);
        f.i(trade.exit_bar_index);
        f.s(trade.entry_id);
        f.s(trade.exit_id);
        f.d(trade.commission);
        f.b(trade.open_at_end);
        record.trades_no_incarnation.push_back(f.h);
        f.u(trade.entry_incarnation);
        record.trades.push_back(f.h);
    }
}

Record run_battery_one(std::uint64_t seed, Mode mode, bool direct) {
    k3_book::BookConfig config;
    config.seed = seed;
    config.bars = 60;
    config.path = static_cast<k3_book::Path>(seed % 3);
    config.calc_on_fills = seed % 4 == 1;
    const k3_book::Tape tape = k3_book::make_tape(config);
    NativeRunSpec spec = k3_book::make_spec(config, tape);
    spec.identity = {"k-oca-keep-battery", 1};
    auto host = std::make_unique<GroupHost>(seed, mode, direct);
    CHECK(host->configure_native(spec).status == NativeSetupStatus::Applied);
    host->run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    Record record = std::move(host->record);
    const auto state = host->native_state();
    record.completed = state.kind == NativeLifecycleKind::Completed;
    record.code = static_cast<unsigned>(state.failure.code);
    record.discriminator = static_cast<unsigned>(state.failure.discriminator);
    record.error = host->last_error();
    record.broker = host->broker_state_hash();
    read_journal(*host, record);
    read_receipts(*host, record);
    read_trades(*host, record);
    record.tally.runs = 1;
    record.tally.completed = record.completed ? 1 : 0;
    record.tally.stopped = record.completed ? 0 : 1;
    return record;
}

bool same_run(const Record& a, const Record& b, bool incarnations) {
    return a.fills == b.fills && a.events == b.events && a.bars == b.bars
        && a.answers == b.answers && a.receipts == b.receipts && a.error == b.error
        && a.completed == b.completed
        && (incarnations ? a.trades == b.trades
                         : a.trades_no_incarnation == b.trades_no_incarnation);
}

Tally battery(std::uint64_t seed, int scripts) {
    Tally total;
    long mismatched = 0;
    long keep_stops = 0;
    for (int i = 0; i < scripts; ++i) {
        const std::uint64_t script = seed * 1000003ULL + static_cast<std::uint64_t>(i);
        Record reference;
        for (const Mode mode : kModes) {
            scenario = "battery-" + std::to_string(seed) + "-" + std::to_string(i) + "/"
                + mode_name(mode);
            const Record staged = run_battery_one(script, mode, false);
            const Record direct = run_battery_one(script, mode, true);
            // The two core paths of one option set: bit for bit.
            CHECK(same_run(staged, direct, true));
            CHECK(staged.hashes == direct.hashes && staged.broker == direct.broker);
            CHECK(staged.code == direct.code && staged.discriminator == direct.discriminator);
            if (!staged.completed) {
                if (keeps_handle(mode)) ++keep_stops;
                std::printf("  %s STOPPED code=%u disc=%u\n", scenario.c_str(), staged.code,
                            staged.discriminator);
            }
            CHECK(staged.completed);
            if (mode == Mode::Replace) {
                reference = staged;
                // The plain replace's drains always come in incarnation order.
                CHECK(staged.tally.misordered == 0);
            } else {
                // Every option set records the plain replace's run.
                const bool same = same_run(staged, reference, false);
                CHECK(same);
                if (!same) ++mismatched;
            }
            total.add(staged.tally);
        }
    }
    scenario = "battery-" + std::to_string(seed);
    std::printf("battery seed %llu: scripts=%d runs=%ld completed=%ld STOPPED=%ld (keep_handle %ld)  "
                "mismatched=%ld  fills=%ld group=%ld  reduced=%ld deferred=%ld cancelled=%ld  "
                "replaces=%ld kept=%ld retained=%ld cancels=%ld  drains=%ld misordered=%ld "
                "(cancel %ld, reduce %ld, deferred %ld)\n",
                static_cast<unsigned long long>(seed), scripts, total.runs, total.completed,
                total.stopped, keep_stops, mismatched, total.fills, total.group_fills,
                total.reduced, total.deferred, total.group_cancels, total.replaces, total.kept,
                total.retained, total.cancels, total.drains, total.misordered,
                total.misordered_cancel, total.misordered_reduce, total.misordered_deferred);
    CHECK(total.stopped == 0);
    CHECK(mismatched == 0);
    // Every seed reaches the shape this row is about, under both effects and
    // for a recipient whose units are deferred.
    CHECK(total.misordered_cancel > 0 && total.misordered_reduce > 0);
    CHECK(total.misordered_deferred > 0);
    CHECK(total.reduced > 0 && total.deferred > 0 && total.group_cancels > 0);
    return total;
}

}  // namespace

int main() {
    for (const auto effect : {no::GroupEffect::Reduce, no::GroupEffect::Cancel}) {
        probe(effect);
        trail(effect);
        bound(effect);
        core_rows(effect, false);
        core_rows(effect, true);
    }
    grid_absorbed(GridClose::Fraction);
    grid_absorbed(GridClose::HostSized);
    grid_absorbed(GridClose::HostSizedCurrent);
    Tally all;
    for (std::uint64_t seed : {11ULL, 22ULL, 33ULL}) all.add(battery(seed, 30));
    scenario = "battery";
    CHECK(all.kept > 0 && all.retained > 0);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
