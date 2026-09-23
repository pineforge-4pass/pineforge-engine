// R5 lane PERF-L5: a working book aimed at the edges of the price band each
// driver point walks.
//
// The lane's band pre-check tests a priced trigger against the rest of the
// point's path before match_path reads the rest of the row, and passes a
// request over when the path cannot reach it. What such a test can get wrong is
// exactly an edge: a level the path touches without crossing, a level one tick
// outside the band, a gap that opens beyond a level, a cursor already inside
// the region, two requests at one level. This host therefore reads the tape
// ahead of the run and places its levels ON the next bars' open, high, low and
// close and on their one-minute bars' extremes, one quarter tick inside and
// outside them, and (under the quantizing grid) an eighth of a tick off them,
// next to levels no path of the run reaches. Every trigger kind the matcher
// prices is placed there: limit (with and without fill_through), stop,
// stop-limit, trail with and without an arm and with a zero offset, and market
// requests besides; every intent (Transact, Reduce, Flatten, a host-sized
// cohort close); OCA groups under both effects; per-point budgets; brackets
// whose legs wait for their entry under either arm scope and first-match rule;
// replaces that move a live request onto an edge; cancels; and requests born
// inside the fill callback, whose first match starts after the print that armed
// them.
//
// It reuses the randomized book's tape, spec and outcome
// (native_match_book_fixture.hpp): every price is a whole number of eighth
// ticks, an exact binary fraction, and every choice comes from one xorshift
// stream, so a configuration replays bit for bit on every host.
//
// Source-free: kernel-only builds register every row that uses it.
#pragma once

#include "native_match_book_fixture.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace l5_band {

using namespace pineforge;
namespace no = pineforge::native_order;
using k3_book::BookConfig;
using k3_book::Outcome;
using k3_book::Path;
using k3_book::Rng;
using k3_book::Tape;

// A price in eighth ticks (the tape is in quarter ticks, 0.25 each).
inline double eighths(long count) { return static_cast<double>(count) * 0.125; }
inline long to_eighths(double price) { return static_cast<long>(price * 8.0); }

class EdgeHost : public NativeStrategyHost {
public:
    EdgeHost(const BookConfig& config, const Tape& tape)
        : config_(config), tape_(tape), rng_(config.seed ^ 0xB5AD4ECEDA1CE2A9ull) {}

    Outcome outcome;

    void on_native_run_begin() override {
        rng_ = Rng(config_.seed ^ 0xB5AD4ECEDA1CE2A9ull);
        handles_.clear();
        entries_.clear();
        cohort_.reset();
        bar_ = 0;
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ++bar_;
        outcome.trace.push_back(native_continuation_hash());
        observe_book();
        if (!cohort_) cohort_ = cohort_open();
        const long ref = to_eighths(bar.close);
        move_onto_edges();
        top_up(ref);
        brackets();
        if (cohort_ && !entries_.empty() && rng_.percent(25)) {
            const std::size_t at =
                static_cast<std::size_t>(rng_.below(static_cast<int>(entries_.size())));
            cohort_add(*cohort_, entries_[at]);
        }
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        ++outcome.applied;
        outcome.trace.push_back(native_continuation_hash());
        if (cohort_member(event.request()) && event.opened_units != 0.0 && rng_.percent(40)) {
            entries_.push_back(event.handle());
        }
        // A request born on the rest of this segment: its first match starts
        // after the print that produced the callback.
        if (rng_.percent(45)) place(edge_request(to_eighths(event.resolved_price)));
        if (!handles_.empty() && rng_.percent(15)) {
            const std::size_t at =
                static_cast<std::size_t>(rng_.below(static_cast<int>(handles_.size())));
            const auto result = replace(handles_[at], edge_request(to_eighths(event.resolved_price)));
            if (result.successor) {
                ++outcome.replaced;
                handles_[at] = *result.successor;
            }
        }
    }

    no::ExecutionTerms resolve_execution_terms(const NativeExecutionTermsFacts& facts) const override {
        no::ExecutionTerms terms{facts.default_resolved_price, std::nullopt,
                                 no::OpeningShape::Transact};
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)
            && facts.scope_exposure_units > 0.0) {
            terms.units = facts.scope_exposure_units < 1.0 ? facts.scope_exposure_units : 1.0;
        }
        return terms;
    }

    void finish() {
        outcome.completed = native_state().kind == NativeLifecycleKind::Completed;
        outcome.error = last_error();
        outcome.continuation = native_continuation_hash();
        outcome.broker = broker_state_hash();
        outcome.position = physical_position().signed_units;
        outcome.trades = trade_count();
        std::uint64_t digest = 1469598103934665603ull;
        for (int index = 0; index < outcome.trades; ++index) {
            const Trade& trade = get_trade(index);
            digest = k3_book::fnv_u64(digest, static_cast<std::uint64_t>(trade.entry_time));
            digest = k3_book::fnv_u64(digest, static_cast<std::uint64_t>(trade.exit_time));
            digest = k3_book::fnv_f64(digest, trade.entry_price);
            digest = k3_book::fnv_f64(digest, trade.exit_price);
            digest = k3_book::fnv_f64(digest, trade.qty);
            digest = k3_book::fnv_f64(digest, trade.pnl);
            digest = k3_book::fnv_f64(digest, trade.commission);
            digest = k3_book::fnv_u64(digest, trade.entry_incarnation);
            digest = k3_book::fnv_u64(digest, static_cast<std::uint64_t>(trade.entry_bar_index));
            digest = k3_book::fnv_u64(digest, static_cast<std::uint64_t>(trade.exit_bar_index));
        }
        outcome.trades_digest = digest;
        // Every event of the run, field by field where the matcher decides
        // it: the kind and ordinal of each row, the command's alternative, and
        // for the rows a trigger or a fill writes, the cursor and prices.
        const auto events = native_events(0);
        outcome.events = events.size();
        std::uint64_t census = 1469598103934665603ull;
        for (const auto& event : events) {
            census = k3_book::fnv_u64(census, static_cast<std::uint64_t>(event.kind));
            census = k3_book::fnv_u64(census, event.ordinal);
            if (!event.command) continue;
            census = k3_book::fnv_u64(census, event.command->index());
            if (const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&*event.command)) {
                census = k3_book::fnv_f64(census, applied->cursor.t);
                census = k3_book::fnv_u64(census, applied->cursor.point.ordinal);
                census = k3_book::fnv_f64(census, applied->raw_price);
                census = k3_book::fnv_f64(census, applied->resolved_price);
                census = k3_book::fnv_f64(census, applied->filled_working);
            } else if (const auto* activated = std::get_if<no::ActivatedEvent>(&*event.command)) {
                census = k3_book::fnv_f64(census, activated->cursor.t);
                census = k3_book::fnv_u64(census, activated->cursor.point.ordinal);
                census = k3_book::fnv_f64(census, activated->reached_price);
                census = k3_book::fnv_u64(census, static_cast<std::uint64_t>(activated->kind));
            }
        }
        outcome.events_digest = census;
    }

private:
    void observe_book() {
        const auto working = native_working_requests();
        std::uint64_t previous = 0;
        for (const auto& row : working) {
            if (row.definition->handle.incarnation <= previous) {
                ++outcome.unordered;
                break;
            }
            previous = row.definition->handle.incarnation;
        }
        handles_.clear();
        for (const auto& row : working) handles_.push_back(row.definition->handle);
    }

    // The bar the run delivers after the one being calculated, or the last.
    const Bar& ahead(int bars) const {
        std::size_t index = static_cast<std::size_t>(bar_ - 1 + bars);
        if (index >= tape_.bars.size()) index = tape_.bars.size() - 1;
        return tape_.bars[index];
    }
    const Bar& minute_ahead(int bars) const {
        std::size_t bucket = static_cast<std::size_t>(bar_ - 1 + bars);
        if (bucket >= tape_.bars.size()) bucket = tape_.bars.size() - 1;
        const std::size_t minute = bucket * 5 + static_cast<std::size_t>(rng_.below(5));
        return tape_.minutes[minute < tape_.minutes.size() ? minute : tape_.minutes.size() - 1];
    }

    // An edge of the path ahead: an open, high, low or close of the next bar
    // or of one of its minutes, one quarter tick inside or outside it, and
    // under the quantizing grid an eighth of a tick off it; now and then a
    // level no path of the run reaches.
    long edge(bool upper) {
        if (rng_.percent(12)) return upper ? 400L * 8 : 20L * 8;
        const Bar& bar = rng_.percent(60) ? ahead(1) : (rng_.percent(50) ? minute_ahead(1) : ahead(2));
        double price = 0.0;
        switch (rng_.below(5)) {
        case 0: price = bar.open; break;
        case 1: price = bar.close; break;
        case 2: price = upper ? bar.high : bar.low; break;
        case 3: price = upper ? bar.high : bar.low; break;
        default: price = upper ? bar.low : bar.high; break;
        }
        long level = to_eighths(price);
        const int nudge = rng_.below(100);
        if (nudge < 20) level += 2;
        else if (nudge < 40) level -= 2;
        else if (config_.quantize && nudge < 50) level += rng_.percent(50) ? 1 : -1;
        return level;
    }

    no::Request edge_request(long ref) {
        no::Request request;
        request.label = "l5";
        const bool buy = rng_.percent(50);
        const int units = rng_.between(1, 3);
        const int intent = rng_.below(100);
        if (intent < 50) {
            request.intent = no::Transact{buy ? double(units) : -double(units)};
        } else if (intent < 72) {
            request.intent = no::Reduce{no::ExplicitUnits{double(units)}};
        } else if (intent < 82) {
            request.intent = no::Flatten{};
        } else if (intent < 92) {
            request.intent = no::Reduce{no::ScopeFraction{0.5}};
        } else {
            request.intent = no::HostSized{no::HostSizedKind::Close, std::nullopt};
            if (cohort_) request.owner = no::BindCohort{*cohort_};
        }
        // A buy waits below (a limit) or above (a stop); a sell the reverse.
        const long lower = rng_.percent(85) ? edge(false) : ref - rng_.between(0, 4) * 2;
        const long upper = rng_.percent(85) ? edge(true) : ref + rng_.between(0, 4) * 2;
        const int trigger = rng_.below(100);
        if (trigger < 8) {
            request.trigger = no::Market{};
        } else if (trigger < 40) {
            request.trigger = no::Limit{eighths(buy ? lower : upper), rng_.percent(15)};
        } else if (trigger < 62) {
            request.trigger = no::Stop{eighths(buy ? upper : lower)};
        } else if (trigger < 76) {
            const long stop = buy ? upper : lower;
            const long offset = rng_.between(-1, 2) * 2;
            request.trigger = no::StopLimit{eighths(stop), eighths(buy ? stop + offset : stop - offset)};
        } else {
            no::Trail trail;
            trail.offset = eighths(rng_.between(0, 3) * 2);
            if (rng_.percent(70)) trail.arm_price = eighths(buy ? lower : upper);
            request.trigger = trail;
        }
        if (rng_.percent(25)) {
            no::Member member;
            member.group = static_cast<std::uint64_t>(rng_.between(1, 3));
            member.effect = rng_.percent(70) ? no::GroupEffect::Cancel : no::GroupEffect::Reduce;
            request.group = member;
        }
        if (rng_.percent(10)) request.capacity = no::PointBudget{1.0};
        return request;
    }

    // A cohort holds only one-unit entries: an entry that fills in several
    // slices holds several lots under one incarnation, a cohort close's
    // target then names that incarnation once per lot, and the settlement
    // refuses the duplicate selection and fails the run (a kernel limit
    // outside this lane; its report names it).
    static bool cohort_member(const no::Request& request) {
        const auto* transact = std::get_if<no::Transact>(&request.intent);
        return transact && (transact->signed_units == 1.0 || transact->signed_units == -1.0)
            && std::holds_alternative<no::ImmediateRemaining>(request.capacity);
    }

    void place(const no::Request& request) {
        const auto result = submit(request);
        if (result.handle) {
            ++outcome.accepted;
            handles_.push_back(*result.handle);
            if (cohort_member(request) && rng_.percent(30)) {
                entries_.push_back(*result.handle);
            }
        } else {
            ++outcome.rejected;
        }
    }

    // Replace a few live requests onto the next bar's edges; cancel one now
    // and then. Two replaced onto one level make a tie.
    void move_onto_edges() {
        if (handles_.empty()) return;
        const int touches = rng_.between(0, 3);
        std::optional<no::Request> twin;
        for (int touch = 0; touch < touches && !handles_.empty(); ++touch) {
            const std::size_t at =
                static_cast<std::size_t>(rng_.below(static_cast<int>(handles_.size())));
            const no::RequestHandle target = handles_[at];
            if (rng_.percent(80)) {
                no::Request request = twin && rng_.percent(50)
                    ? *twin : edge_request(to_eighths(ahead(0).close));
                twin = request;
                const auto result = replace(target, request);
                if (result.successor) {
                    ++outcome.replaced;
                    handles_[at] = *result.successor;
                } else {
                    handles_.erase(handles_.begin() + static_cast<std::ptrdiff_t>(at));
                }
            } else {
                if (cancel(target).status == no::CancelStatus::Cancelled) ++outcome.cancelled;
                handles_.erase(handles_.begin() + static_cast<std::ptrdiff_t>(at));
            }
        }
    }

    void top_up(long ref) {
        int budget = config_.live;
        while (static_cast<int>(native_working_requests().size()) < config_.live && budget-- > 0) {
            place(edge_request(ref));
        }
        // A pair at one level: a tie between two requests of one bar.
        if (rng_.percent(20)) {
            const no::Request request = edge_request(ref);
            place(request);
            place(request);
        }
    }

    // An entry with two legs armed by its fill, one OCA group, the legs on
    // the edges of the bar after the entry's.
    void brackets() {
        if (!rng_.percent(14)) return;
        const bool buy = rng_.percent(50);
        no::Request entry{no::Transact{buy ? 1.0 : -1.0}, "l5-e", ""};
        if (rng_.percent(60)) entry.trigger = no::Limit{eighths(buy ? edge(false) : edge(true))};
        const auto parent = submit(entry);
        if (!parent.handle) {
            ++outcome.rejected;
            return;
        }
        ++outcome.accepted;
        no::WaitForApplied wait;
        wait.parent = *parent.handle;
        wait.first_match = rng_.percent(50) ? no::NativeArmFirstMatch::AtArmPrint
                                            : no::NativeArmFirstMatch::AfterArmPrint;
        wait.scope = rng_.percent(50) ? no::NativeArmScope::OwnerLot : no::NativeArmScope::Book;
        no::Member member;
        member.group = static_cast<std::uint64_t>(1000 + bar_);
        no::Request take{no::Reduce{no::ExplicitUnits{1.0}}, "l5-tp", ""};
        take.trigger = no::Limit{eighths(buy ? edge(true) : edge(false))};
        take.owner = wait;
        take.group = member;
        no::Request stop{no::Reduce{no::ExplicitUnits{1.0}}, "l5-sl", ""};
        if (rng_.percent(30)) {
            no::Trail trail;
            trail.offset = eighths(rng_.between(0, 2) * 2);
            stop.trigger = trail;
        } else {
            stop.trigger = no::Stop{eighths(buy ? edge(false) : edge(true))};
        }
        stop.owner = wait;
        stop.group = member;
        place(take);
        place(stop);
    }

    BookConfig config_;
    const Tape& tape_;
    mutable Rng rng_;
    std::vector<no::RequestHandle> handles_;
    std::vector<no::RequestHandle> entries_;
    std::optional<no::CohortHandle> cohort_;
    int bar_ = 0;
};

// Runs one configuration on a host the caller built over `tape` (a subclass
// may turn a consumer switch before the run) and returns what it produced.
template <class Host>
Outcome run_edges(Host& host, const BookConfig& config, const Tape& tape) {
    const auto setup = host.configure_native(k3_book::make_spec(config, tape));
    if (setup.status != NativeSetupStatus::Applied) {
        host.outcome.error = "configure_native refused the spec";
        return host.outcome;
    }
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    host.finish();
    return host.outcome;
}

}  // namespace l5_band
