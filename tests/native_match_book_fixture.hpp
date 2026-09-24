// R5 lane PERF-K3: a randomized working book for the matcher's rows.
//
// One seeded host keeps a chosen number of requests working and churns them
// every bar: every intent (Transact, Reduce, Flatten, a host-sized cohort
// close), every trigger kind (market, limit with and without fill_through,
// stop, stop-limit, trail with and without an arm), OCA groups under both
// effects, per-point budgets, brackets whose legs wait for their entry under
// either arm scope and first-match rule, cohort rosters, replaces and
// cancels, and requests born inside the fill callback. The tape gaps through
// resting levels, so fills land at t=0 in the same points where the other
// requests' allowances are being refreshed.
//
// Every price is a whole number of quarter ticks (an exact binary fraction)
// and every choice comes from one xorshift stream, so a configuration replays
// bit for bit wherever it runs. What a run produced is summarized as values:
// the continuation hash read at every bar and at every applied fill, the
// broker hash, every trade, the event census. test_native_match_row_reuse
// compares two runs of one configuration through it (the matcher's row reuse
// on and off); test_native_match_hash_witness pins a few configurations'
// values harvested before the lane.
//
// Source-free: kernel-only builds register both rows.
#pragma once

#include <pineforge/native_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace k3_book {

using namespace pineforge;
namespace no = pineforge::native_order;

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed)
        : state(seed * 0x9E3779B97F4A7C15ull ^ 0xD1B54A32D192ED03ull) {
        if (state == 0) state = 1;
    }
    std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
    int between(int lo, int hi) { return lo + below(hi - lo + 1); }
    bool percent(int p) { return below(100) < p; }
};

// Every price is a whole number of quarter ticks.
inline double ticks(long count) { return static_cast<double>(count) * 0.25; }

enum class Path { None, Synthesized, Lower };

struct BookConfig {
    std::uint64_t seed = 1;
    int live = 10;    // the working requests the host tops the book up to
    int bars = 120;   // script bars (5 minutes each)
    Path path = Path::None;
    bool calc_on_fills = false;
    bool quantize = false;
};

inline const char* path_name(Path path) {
    switch (path) {
    case Path::None: return "none";
    case Path::Synthesized: return "synth";
    case Path::Lower: return "lower";
    }
    return "?";
}

// One-minute bars in quarter ticks with gaps, and the five-minute bars
// aggregated from them, so a lower-timeframe path agrees with the chart.
struct Tape {
    std::vector<Bar> minutes;
    std::vector<Bar> bars;
};

inline Tape make_tape(const BookConfig& config) {
    Rng rng(config.seed ^ 0x51ED270B27A9C3F1ull);
    Tape tape;
    const std::int64_t t0 = 1736121600000LL;
    long price = 400;  // 100.00
    const int minutes = config.bars * 5;
    tape.minutes.reserve(static_cast<std::size_t>(minutes));
    for (int index = 0; index < minutes; ++index) {
        long open = price;
        if (rng.percent(6)) open += rng.percent(50) ? rng.between(4, 24) : -rng.between(4, 24);
        long close = open + rng.between(-6, 6);
        if (open < 80) open = 80;
        if (close < 80) close = 80;
        const long high = (open > close ? open : close) + rng.between(0, 3);
        const long low = (open < close ? open : close) - rng.between(0, 3);
        Bar bar{};
        bar.open = ticks(open);
        bar.high = ticks(high);
        bar.low = ticks(low);
        bar.close = ticks(close);
        bar.volume = 1.0;
        bar.timestamp = t0 + static_cast<std::int64_t>(index) * 60000;
        tape.minutes.push_back(bar);
        price = close;
    }
    tape.bars.reserve(static_cast<std::size_t>(config.bars));
    for (int bucket = 0; bucket < config.bars; ++bucket) {
        Bar bar = tape.minutes[static_cast<std::size_t>(bucket * 5)];
        for (int k = 1; k < 5; ++k) {
            const Bar& minute = tape.minutes[static_cast<std::size_t>(bucket * 5 + k)];
            if (minute.high > bar.high) bar.high = minute.high;
            if (minute.low < bar.low) bar.low = minute.low;
            bar.close = minute.close;
            bar.volume += minute.volume;
        }
        tape.bars.push_back(bar);
    }
    return tape;
}

inline NativeRunSpec make_spec(const BookConfig& config, const Tape& tape) {
    NativeRunSpec spec;
    spec.identity = {"k3-book", 1};
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.tickerid = "TEST:K3BOOK";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    spec.initial_capital = 1.0e9;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.25;
    spec.fee_kind = NativeFeeKind::Percent;
    spec.fee_value = 0.05;
    // The books' readers take the whole event record once a run ends (V19-B).
    spec.event_retention = NativeEventRetention::Full;
    if (config.quantize) spec.price_grid = NativePriceGrid::QuantizeFillsAndTriggers;
    if (config.calc_on_fills) spec.calculation = NativeCalculationTrigger::BarCloseAndFills;
    if (config.path == Path::Synthesized) {
        IntrabarPath::synthesized path;
        path.samples = 4;
        spec.intrabar.value = path;
    } else if (config.path == Path::Lower) {
        IntrabarPath::lower_tf path;
        path.bars = tape.minutes;
        path.tf = "1";
        path.samples = 4;
        spec.intrabar.value = path;
    }
    return spec;
}

// What a run produced, as values.
struct Outcome {
    std::vector<std::uint64_t> trace;  // continuation at every bar and every fill
    std::uint64_t continuation = 0;
    std::uint64_t broker = 0;
    std::uint64_t trades_digest = 0;
    int trades = 0;
    std::size_t events = 0;
    std::uint64_t events_digest = 0;
    double position = 0.0;
    long accepted = 0;
    long rejected = 0;
    long replaced = 0;
    long cancelled = 0;
    long applied = 0;
    long unordered = 0;  // bars whose working book was not in incarnation order
    std::string error;
    bool completed = false;
};

inline std::uint64_t fnv(std::uint64_t h, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ull;
    }
    return h;
}
inline std::uint64_t fnv_u64(std::uint64_t h, std::uint64_t v) { return fnv(h, &v, sizeof v); }
inline std::uint64_t fnv_f64(std::uint64_t h, double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    return fnv_u64(h, bits);
}

class BookHost : public NativeStrategyHost {
public:
    explicit BookHost(const BookConfig& config) : config_(config), rng_(config.seed) {}

    Outcome outcome;

    void on_native_run_begin() override {
        rng_ = Rng(config_.seed);
        entries_.clear();
        cohort_.reset();
        bar_ = 0;
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ++bar_;
        observe_book();
        outcome.trace.push_back(native_continuation_hash());
        if (!cohort_) {
            cohort_ = cohort_open();
            after_command();
        }
        const long ref = static_cast<long>(bar.close * 4.0);
        churn(ref);
        top_up(ref);
        extras(ref);
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        ++outcome.applied;
        outcome.trace.push_back(native_continuation_hash());
        const long ref = static_cast<long>(event.resolved_price * 4.0);
        if (event.opened_units != 0.0 && rng_.percent(40)) entries_.push_back(event.handle());
        if (rng_.percent(35)) place(random_request(ref));
        if (rng_.percent(10)) churn(ref);
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
            digest = fnv_u64(digest, static_cast<std::uint64_t>(trade.entry_time));
            digest = fnv_u64(digest, static_cast<std::uint64_t>(trade.exit_time));
            digest = fnv_f64(digest, trade.entry_price);
            digest = fnv_f64(digest, trade.exit_price);
            digest = fnv_f64(digest, trade.qty);
            digest = fnv_f64(digest, trade.pnl);
            digest = fnv_f64(digest, trade.commission);
            digest = fnv_u64(digest, trade.entry_incarnation);
            digest = fnv_u64(digest, static_cast<std::uint64_t>(trade.entry_bar_index));
            digest = fnv_u64(digest, static_cast<std::uint64_t>(trade.exit_bar_index));
        }
        outcome.trades_digest = digest;
        const auto events = native_events(0);
        outcome.events = events.size();
        std::uint64_t census = 1469598103934665603ull;
        for (const auto& event : events) {
            census = fnv_u64(census, static_cast<std::uint64_t>(event.kind));
            census = fnv_u64(census, event.ordinal);
            if (event.command) census = fnv_u64(census, event.command->index());
        }
        outcome.events_digest = census;
    }

protected:
    // Called after every command this host issues (submit, replace, cancel and
    // the roster commands), inside the callback that issued it. A no-op here;
    // test_native_direct_mutation reads the core and the hashes there, so its
    // two runs are compared command by command (R5 lane L3).
    virtual void after_command() {}

private:
    // The working book, as the host sees it; its order is the kernel's.
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

    no::Request random_request(long ref) {
        no::Request request;
        request.label = "k3";
        const bool buy = rng_.percent(50);
        const int units = rng_.between(1, 3);
        const int intent = rng_.below(100);
        if (intent < 55) {
            request.intent = no::Transact{buy ? double(units) : -double(units)};
        } else if (intent < 75) {
            request.intent = no::Reduce{no::ExplicitUnits{double(units)}};
        } else if (intent < 85) {
            request.intent = no::Flatten{};
        } else {
            request.intent = no::Reduce{no::ScopeFraction{0.5}};
        }
        // A resting level sits on the side a buy (sell) waits on; a few are
        // already through the market and fill at the next point's t=0.
        const long away = rng_.percent(15) ? -rng_.between(1, 6) : rng_.between(1, 40);
        const long below = ref - away;
        const long above = ref + away;
        const int trigger = rng_.below(100);
        if (trigger < 12) {
            request.trigger = no::Market{};
        } else if (trigger < 45) {
            request.trigger = no::Limit{ticks(buy ? below : above), rng_.percent(10)};
        } else if (trigger < 65) {
            request.trigger = no::Stop{ticks(buy ? above : below)};
        } else if (trigger < 77) {
            const long stop = buy ? above : below;
            request.trigger = no::StopLimit{ticks(stop), ticks(buy ? stop + 2 : stop - 2)};
        } else {
            no::Trail trail;
            trail.offset = ticks(rng_.between(0, 8));
            if (rng_.percent(60)) trail.arm_price = ticks(buy ? below : above);
            request.trigger = trail;
        }
        if (rng_.percent(30)) {
            no::Member member;
            member.group = static_cast<std::uint64_t>(rng_.between(1, 4));
            member.effect = rng_.percent(70) ? no::GroupEffect::Cancel : no::GroupEffect::Reduce;
            request.group = member;
        }
        if (rng_.percent(10)) request.capacity = no::PointBudget{1.0};
        return request;
    }

    void place(const no::Request& request) {
        const auto result = submit(request);
        after_command();
        if (result.handle) {
            ++outcome.accepted;
            handles_.push_back(*result.handle);
            if (std::holds_alternative<no::Transact>(request.intent) && rng_.percent(30)) {
                entries_.push_back(*result.handle);
            }
        } else {
            ++outcome.rejected;
        }
    }

    // Replace or cancel a few working requests.
    void churn(long ref) {
        if (handles_.empty()) return;
        const int touches = rng_.between(0, 3);
        for (int touch = 0; touch < touches && !handles_.empty(); ++touch) {
            const std::size_t at = static_cast<std::size_t>(rng_.below(static_cast<int>(handles_.size())));
            const no::RequestHandle target = handles_[at];
            if (rng_.percent(70)) {
                const auto result = replace(target, random_request(ref));
                after_command();
                if (result.successor) {
                    ++outcome.replaced;
                    handles_[at] = *result.successor;
                } else {
                    handles_.erase(handles_.begin() + static_cast<std::ptrdiff_t>(at));
                }
            } else {
                if (cancel(target).status == no::CancelStatus::Cancelled) ++outcome.cancelled;
                after_command();
                handles_.erase(handles_.begin() + static_cast<std::ptrdiff_t>(at));
            }
        }
    }

    void top_up(long ref) {
        int budget = config_.live;
        while (static_cast<int>(native_working_requests().size()) < config_.live && budget-- > 0) {
            place(random_request(ref));
        }
    }

    void extras(long ref) {
        // A market transaction: it fills at the next point's t=0, among that
        // point's allowance refreshes.
        if (rng_.percent(10)) {
            no::Request market{no::Transact{rng_.percent(50) ? 1.0 : -1.0}, "k3-m", ""};
            place(market);
        }
        if (rng_.percent(4)) {
            no::Request flat{no::Flatten{}, "k3-f", ""};
            place(flat);
        }
        // A bracket: an entry and two legs armed by its fill, one OCA group.
        if (rng_.percent(12)) {
            const bool buy = rng_.percent(50);
            no::Request entry{no::Transact{buy ? 1.0 : -1.0}, "k3-e", ""};
            if (rng_.percent(50)) entry.trigger = no::Limit{ticks(buy ? ref - 2 : ref + 2)};
            const auto parent = submit(entry);
            after_command();
            if (parent.handle) {
                ++outcome.accepted;
                no::WaitForApplied wait;
                wait.parent = *parent.handle;
                wait.first_match = rng_.percent(50) ? no::NativeArmFirstMatch::AtArmPrint
                                                    : no::NativeArmFirstMatch::AfterArmPrint;
                wait.scope = rng_.percent(50) ? no::NativeArmScope::OwnerLot
                                              : no::NativeArmScope::Book;
                no::Member member;
                member.group = static_cast<std::uint64_t>(100 + bar_);
                no::Request take{no::Reduce{no::ExplicitUnits{1.0}}, "k3-tp", ""};
                take.trigger = no::Limit{ticks(buy ? ref + 6 : ref - 6)};
                take.owner = wait;
                take.group = member;
                no::Request stop{no::Reduce{no::ExplicitUnits{1.0}}, "k3-sl", ""};
                stop.trigger = no::Stop{ticks(buy ? ref - 6 : ref + 6)};
                stop.owner = wait;
                stop.group = member;
                place(take);
                place(stop);
            } else {
                ++outcome.rejected;
            }
        }
        // A roster and a host-sized close bound to it.
        if (cohort_ && !entries_.empty() && rng_.percent(20)) {
            const std::size_t at = static_cast<std::size_t>(rng_.below(static_cast<int>(entries_.size())));
            cohort_add(*cohort_, entries_[at]);
            after_command();
        }
        if (cohort_ && rng_.percent(8)) {
            no::Request close{no::HostSized{no::HostSizedKind::Close, std::nullopt}, "k3-c", ""};
            close.owner = no::BindCohort{*cohort_};
            if (rng_.percent(50)) close.trigger = no::Limit{ticks(ref + rng_.between(-4, 12))};
            place(close);
        }
    }

    BookConfig config_;
    Rng rng_;
    std::vector<no::RequestHandle> handles_;
    std::vector<no::RequestHandle> entries_;
    std::optional<no::CohortHandle> cohort_;
    int bar_ = 0;
};

// Runs one configuration on a host the caller built (a subclass may turn a
// consumer switch before the run) and returns what it produced.
template <class Host>
Outcome run_book(Host& host, const BookConfig& config) {
    const Tape tape = make_tape(config);
    const auto setup = host.configure_native(make_spec(config, tape));
    if (setup.status != NativeSetupStatus::Applied) {
        host.outcome.error = "configure_native refused the spec";
        return host.outcome;
    }
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    host.finish();
    return host.outcome;
}

}  // namespace k3_book
