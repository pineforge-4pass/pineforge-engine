// Reversal exit-owner gate: public reversing runs, with the pre-gate loop retained as the oracle.
// Like test_adapter_reversal_storage, this TU supplies the real adapter before
// the static archive is searched. The observation also works with the frozen
// base adapter: it counts that adapter's actual sorted-id visits, excluding the
// incoming market entry (the only preserved id in the growth fixture).
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace reversal_gate_fixture {
struct Probe {
    bool oracle = false;
    bool gate_seen = false;
    std::uint64_t calls = 0;
    std::string incoming;
    const std::string* owner = nullptr;
    const std::vector<std::string>* preserved = nullptr;
    std::uint64_t owner_calls = 0;
    bool bound_roster_diagnostic = false;
    int bound_diagnostic_bar = -1;
    std::uint64_t bound_parent_incarnation = 0;
    std::uint64_t bound_cancelled_legs = 0;
    bool bound_cancel_facts = false;
};
inline Probe* active = nullptr;

// One-shot diagnostic through the existing read-only hash observer. Unlike
// the pending facades, this observer can inspect retained placement rows after
// cancellation. It captures the receipt facts used by PendingBound assertions
// and changes no adapter state.
template<class Placements, class Handles, class Brackets, class Coof,
         class Delayed, class Shadows, class Stashed, class Relative>
void observe_bound_rosters(const Placements& placements, const Handles&,
                           const Brackets&, const Coof&, const Delayed&,
                           const Shadows&, const Stashed&, const Relative&) {
    if (!active || !active->bound_roster_diagnostic) return;
    active->bound_roster_diagnostic = false;
    const auto matches = [](const auto& snapshot) {
        return snapshot.source_id == "owner-exit" && snapshot.from_entry == "bound-parent";
    };
    std::uint64_t retained = 0, limit_legs = 0, stop_legs = 0;
    bool receipt_facts = true;
    const auto is_exit = [](const auto family) {
        return family == pineforge::source::PineOrderFamily::ExitLimit
            || family == pineforge::source::PineOrderFamily::ExitStop
            || family == pineforge::source::PineOrderFamily::ExitTrail;
    };
    const char* phase = active->oracle ? "oracle" : "candidate";
    for (const auto& row : placements) {
        if (!matches(row.second)) continue;
        ++retained;
        const auto& snapshot = row.second;
        if (!is_exit(snapshot.family)) receipt_facts = false;
        if (snapshot.family == pineforge::source::PineOrderFamily::ExitLimit)
            ++limit_legs;
        else if (snapshot.family == pineforge::source::PineOrderFamily::ExitStop)
            ++stop_legs;
        receipt_facts = receipt_facts && snapshot.pending_bound_exit
            && !snapshot.void_issue
            && snapshot.cancellation.cause
                == pineforge::source::PineCancellationCause::Explicit
            && snapshot.cancellation.source_incarnation == row.first
            && snapshot.cancellation.target_incarnation == row.first
            && snapshot.bracket_origin.incarnation == active->bound_parent_incarnation;
        std::fprintf(stderr,
            "reversal-gatebound-placement phase=%s bar=%d handle=%llu family=%d "
            "pending_bound=%d void=%d cancel_cause=%d cancel_source=%llu "
            "parent=%llu dormant=%d created_bar=%d\n",
            phase, active->bound_diagnostic_bar,
            static_cast<unsigned long long>(row.first), static_cast<int>(snapshot.family),
            snapshot.pending_bound_exit ? 1 : 0, snapshot.void_issue ? 1 : 0,
            static_cast<int>(snapshot.cancellation.cause),
            static_cast<unsigned long long>(snapshot.cancellation.source_incarnation),
            static_cast<unsigned long long>(snapshot.bracket_origin.incarnation),
            snapshot.legs.dormant() ? 1 : 0, snapshot.projection_created_bar);
    }
    active->bound_cancelled_legs = limit_legs + stop_legs;
    active->bound_cancel_facts = receipt_facts && retained == 2
        && limit_legs == 1 && stop_legs == 1;
}

inline void observe_call(const std::string& id) {
    if (!active) return;
    ++active->calls;
    if (active->owner && id == *active->owner) ++active->owner_calls;
}
template<class Visit> auto observe_old_walk(Visit visit) {
    return [visit = std::move(visit)](const std::string& id) mutable {
        // A fixed Debug build also walks skipped ids to assert the store
        // predicates. That verification is not a cancellation call. On the
        // frozen base no gate runs, so the growth fixture still excludes
        // its sole incoming id. With a gate, use its actual exemptions:
        // resting parents can fill without going through Host::enter().
        if (active && (!active->gate_seen || active->oracle)
            && (active->preserved
                ? !std::binary_search(active->preserved->begin(), active->preserved->end(), id)
                : id != active->incoming)) observe_call(id);
        visit(id);
    };
}
inline bool use_old_loop(const std::vector<std::string>& preserved) {
    if (!active) return false;
    active->gate_seen = true;
    active->preserved = &preserved;
    return active->oracle;
}
} // namespace reversal_gate_fixture

#define for_each_sorted_id(visit) \
    for_each_sorted_id(::reversal_gate_fixture::observe_old_walk(visit))
#define PINEFORGE_FULL_CLOSE_EXIT_ORACLE() ::reversal_gate_fixture::use_old_loop(preserved)
#define PINEFORGE_FULL_CLOSE_EXIT_CALL() ::reversal_gate_fixture::observe_call(id)
#include "../src/source/pine_adapter.cpp"
#undef PINEFORGE_FULL_CLOSE_EXIT_CALL
#undef PINEFORGE_FULL_CLOSE_EXIT_ORACLE
#undef for_each_sorted_id
#define PINEFORGE_BROKER_HASH_OBSERVE(rows, families, archived, digest) \
    ::reversal_gate_fixture::observe_bound_rosters(rows, live_handles_, pending_bracket_legs_, \
        pending_coof_requests_, delayed_market_orders_, source_shadow_pending_, \
        id_bound_exits_, pending_relative_exits_)
#include "../src/source/pine_state_hash.cpp"
#undef PINEFORGE_BROKER_HASH_OBSERVE

namespace reversal_gate_fixture {
using namespace pineforge;
namespace no = pineforge::native_order;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr double kCapital = 1000000.0;
constexpr std::int64_t kStart = 1736121600000LL;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> void bits(std::string& out, const T& value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof value);
}
void text(std::string& out, const std::string& value) {
    bits(out, value.size());
    out += value;
}

// Serialize values, never addresses or the padding of a Trade/string.
// This is the report_image contract used by test_id_adapter_storage.
std::string report_image(const BacktestEngine& host) {
    ReportC report{};
    host.fill_report(&report);
    struct Release {
        ReportC& report;
        ~Release() { BacktestEngine::free_report(&report); }
    } release{report};
    std::string out;
    bits(out, report.total_trades); bits(out, report.trades_len);
    bits(out, report.net_profit);
    bits(out, report.input_bars_processed); bits(out, report.script_bars_processed);
    bits(out, report.security_feeds_total); bits(out, report.security_eval_complete_total);
    bits(out, report.security_eval_partial_total); bits(out, report.magnifier_sub_bars_total);
    bits(out, report.magnifier_sample_ticks_total); bits(out, report.input_tf_seconds);
    bits(out, report.script_tf_seconds); bits(out, report.script_tf_ratio);
    bits(out, report.needs_aggregation); bits(out, report.bar_magnifier_enabled);
    bits(out, report.metrics);
    for (int i = 0; i < report.trades_len; ++i) {
        const auto& t = report.trades[i];
        bits(out, t.entry_time); bits(out, t.exit_time);
        bits(out, t.entry_price); bits(out, t.exit_price); bits(out, t.pnl); bits(out, t.pnl_pct);
        bits(out, t.is_long); bits(out, t.max_runup); bits(out, t.max_drawdown);
        bits(out, t.qty); bits(out, t.commission);
        bits(out, t.entry_bar_index); bits(out, t.exit_bar_index); bits(out, t.open_at_end);
    }
    bits(out, report.security_diag_len);
    for (int i = 0; i < report.security_diag_len; ++i) {
        const auto& row = report.security_diag[i];
        bits(out, row.sec_id); bits(out, row.feed_count);
        bits(out, row.eval_complete_count); bits(out, row.eval_partial_count);
    }
    bits(out, report.trace_names_len);
    for (int i = 0; i < report.trace_names_len; ++i) text(out, report.trace_names[i]);
    bits(out, report.trace_len);
    for (int i = 0; i < report.trace_len; ++i) {
        const auto& row = report.trace[i];
        bits(out, row.timestamp); bits(out, row.bar_index);
        bits(out, row.name_id); bits(out, row.value);
    }
    bits(out, report.equity_curve_len);
    for (std::int64_t i = 0; i < report.equity_curve_len; ++i) bits(out, report.equity_curve[i]);
    bits(out, report.broker_state_hash_len);
    for (std::int64_t i = 0; i < report.broker_state_hash_len; ++i)
        bits(out, report.broker_state_hash[i]);
    bits(out, host.report_trade_count());
    for (int i = 0; i < host.report_trade_count(); ++i) {
        const auto row = host.get_report_trade(i);
        text(out, row.entry_id); text(out, row.exit_id);
        text(out, row.entry_comment); text(out, row.exit_comment);
        bits(out, row.entry_incarnation); bits(out, row.exit_from_bracket); bits(out, row.close_cause);
    }
    return out;
}

std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1000.0, kStart + i * 60000LL});
    return bars;
}
std::string id_for(int i) { return "reversal-owner-" + std::to_string(i); }

class Host : public source::PineStrategyHost {
public:
    explicit Host(bool coof = false) {
        source::PineStrategyConfig config;
        config.initial_capital = kCapital;
        config.process_orders_on_close = true;
        config.calc_on_order_fills = coof;
        config.margin_long = config.margin_short = 0.0;
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        set_broker_state_hash_recording(true);
    }
    // The existing recording-hash witness's portability seam: compare the
    // native broker and source extension while fixing the tzdata execution hash.
    std::uint64_t broker_state_hash_projection() const override {
        return broker_state_hash_from_execution_hash(0x5eed1234abcd0398ULL);
    }
    void enter(const std::string& id, bool is_long) {
        require(active != nullptr, "the public-run observer is installed");
        active->incoming = id;
        strategy_entry(id, is_long, kNa, kNa, 1.0);
        reads.push_back(broker_state_hash_projection());
    }
    bool pending(const char* id, pf_pending_order_v1_t* copied = nullptr) const {
        const auto& view = pending_intent_view();
        for (int i = 0; i < view.size(); ++i) {
            pf_pending_order_v1_t row{};
            require(view.copy_v1(i, &row) == 0, "copy the public pending projection");
            if (std::string(row.id) == id) {
                if (copied) *copied = row;
                return true;
            }
        }
        return false;
    }
    bool working(const char* id) const {
        for (const auto& row : native_working_requests())
            if (row.definition->request.label == id) return true;
        return false;
    }
    std::vector<std::uint64_t> reads;
    std::uint64_t reversals = 0;
protected:
    void on_native_applied_event(const no::ExecutionAppliedEvent& event,
                                const NativeDecisionContext&) override {
        if (event.closed_units > 0.0 && event.opened_units != 0.0) ++reversals;
        reads.push_back(broker_state_hash_projection());
    }
};

class DenseHost final : public Host {
public:
    explicit DenseHost(bool fixed_ids) : fixed_ids_(fixed_ids) {}
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        enter(id_for(fixed_ids_ ? i % 2 : i), i % 2 == 0);
    }
private:
    bool fixed_ids_;
};

struct Result {
    std::string report;
    std::vector<std::uint64_t> reads;
    std::uint64_t final_hash = 0, calls = 0, reversals = 0;
    std::uint64_t owner_calls = 0;
};

template<class H> Result run(H& host, const std::vector<Bar>& bars, bool oracle,
                            const std::string* owner = nullptr) {
    Probe probe;
    probe.oracle = oracle;
    probe.owner = owner;
    struct Restore {
        Probe* previous = active;
        ~Restore() { active = previous; }
    } restore;
    active = &probe;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(host.last_run_status() == 0 && host.last_error().empty(), host.last_error().c_str());
    return {report_image(host), host.reads, host.broker_state_hash_projection(),
            probe.calls, host.reversals, probe.owner_calls};
}

void same_result(const Result& actual, const Result& old) {
    require(actual.report == old.report && actual.reads == old.reads
            && actual.final_hash == old.final_hash && actual.reversals == old.reversals,
            "reversal exit owners: every trade, report, equity and broker hash equals the old loop");
}

void check_dense_trades(const DenseHost& host, int n, bool fixed_ids) {
    // One unit opens at each 100 close. Each following opposite unit closes
    // its predecessor at 100 and opens one new unit: N-1 closed trades,
    // zero realized P&L, zero fees, and unchanged equity.
    require(host.trade_count() == n - 1 && host.reversals == static_cast<std::uint64_t>(n - 1),
            "the public market tape reverses once per bar after the first");
    for (int i = 0; i < host.trade_count(); ++i) {
        const auto row = host.get_trade(i);
        require(row.entry_id == id_for(fixed_ids ? i % 2 : i)
                && row.exit_id == id_for(fixed_ids ? (i + 1) % 2 : i + 1)
                && row.entry_bar_index == i && row.exit_bar_index == i + 1
                && row.entry_time == kStart + i * 60000LL
                && row.exit_time == kStart + (i + 1) * 60000LL
                && row.entry_price == 100.0 && row.exit_price == 100.0 && row.qty == 1.0
                && row.pnl == 0.0 && row.commission == 0.0 && !row.open_at_end
                && row.is_long == (i % 2 == 0) && !row.exit_from_bracket,
                "the hand-worked reversing trade rows are exact");
    }
    ReportC report{};
    host.fill_report(&report);
    struct Release {
        ReportC& report;
        ~Release() { BacktestEngine::free_report(&report); }
    } release{report};
    require(report.net_profit == 0.0 && report.equity_curve_len > 0
            && report.broker_state_hash_len > 0, "record equity and hashes through the public report");
    for (std::int64_t i = 0; i < report.equity_curve_len; ++i)
        require(report.equity_curve[i].equity == kCapital
                && report.equity_curve[i].open_profit == 0.0,
                "zero-P&L tape keeps every equity sample unchanged");
}

void growth() {
    std::uint64_t calls[2]{}, old_calls[2]{};
    for (int sample = 0; sample < 2; ++sample) {
        const int n = sample == 0 ? 128 : 256;
        const auto bars = tape(n);
        for (bool fixed_ids : {false, true}) {
            DenseHost candidate(fixed_ids), oracle(fixed_ids);
            const auto actual = run(candidate, bars, false);
            const auto old = run(oracle, bars, true);
            check_dense_trades(candidate, n, fixed_ids);
            check_dense_trades(oracle, n, fixed_ids);
            same_result(actual, old);
            // Count the base's real visits, not a guessed charge per bar.
            // The only exemption is the incoming id; every other visit calls
            // the old full-close canceler. Unique ids cost 1+...+(N-1);
            // the two fixed ids cost one call per reversal.
            const auto expected = fixed_ids ? static_cast<std::uint64_t>(n - 1)
                : static_cast<std::uint64_t>(n) * static_cast<std::uint64_t>(n - 1) / 2;
            require(old.calls == expected, "the retained oracle counts actual full-close calls");
            std::printf("reversal-gateN=%d fixed_ids=%d calls=%llu old_calls=%llu reversals=%llu\n",
                n, fixed_ids, static_cast<unsigned long long>(actual.calls),
                static_cast<unsigned long long>(old.calls),
                static_cast<unsigned long long>(actual.reversals));
            if (!fixed_ids) { calls[sample] = actual.calls; old_calls[sample] = old.calls; }
        }
    }
    std::fflush(stdout);
    require(2 * calls[1] <= 5 * calls[0],
            "reversal exit owners: cancellation calls grow faster than bars");
    require(old_calls[1] > 3 * old_calls[0], "the old-loop control retains the quadratic defect");
}

enum class OwnerCase { Live, Empty, Coof, Anchored, PendingBound };
constexpr int kPrefix = 64;

const char* owner_case_name(OwnerCase which) {
    switch (which) {
    case OwnerCase::Live: return "Live";
    case OwnerCase::Empty: return "Empty";
    case OwnerCase::Coof: return "Coof";
    case OwnerCase::Anchored: return "Anchored";
    case OwnerCase::PendingBound: return "PendingBound";
    }
    return "unknown";
}

std::string owner_case_id(OwnerCase which) {
    if (which == OwnerCase::Empty) return {};
    if (which == OwnerCase::Anchored) return "anchored-parent";
    if (which == OwnerCase::PendingBound) return "bound-parent";
    return id_for(kPrefix - 1);
}

class OwnerHost final : public Host {
public:
    explicit OwnerHost(OwnerCase which) : Host(which == OwnerCase::Coof), which_(which) {}
    void observe_owner_projection() {
        projection_bar = pine_bar_index();
        observed_exit = {};
        observed_pending = pending("owner-exit", &observed_exit);
        observed_working = working("owner-exit");
        const auto& stats = adapter_.anchored_relative_stats();
        observed_anchored = stats.anchored;
        observed_adopted = stats.adopted;
        observed_withdrawn = stats.withdrawn;
        require(!observed_pending || std::string(observed_exit.from_entry) == owner_case_id(which_),
                "the projected exit belongs to this case's owner");
    }
    bool exit_binding_is(bool bound) const {
        bool seen = false;
        for (const auto& row : adapter_.fixture_pending_snapshots()) {
            if (row.snapshot.source_id != "owner-exit"
                || row.snapshot.from_entry != "bound-parent") continue;
            seen = true;
            if (row.snapshot.void_issue || row.snapshot.pending_bound_exit != bound) return false;
        }
        return seen;
    }
    std::uint64_t bound_parent_handle() const {
        for (const auto& row : adapter_.fixture_pending_snapshots()) {
            if (row.staged || !row.snapshot.opening
                || row.snapshot.family != pineforge::source::PineOrderFamily::Entry
                || row.snapshot.source_id != "bound-parent") {
                continue;
            }
            return row.incarnation;
        }
        return 0;
    }
    void reverse_after_owner() {
        owner_seen = true;
        owner_calls_before = active->owner_calls;
        enter("after-owner", true);
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        last_source_bar = i;
        // Layout estimate from source, not measured: a resting stop-limit
        // fills on the path's Close, before the ordinary close calculation.
        // Unlike a POOC fill AFTER that calculation, this fill recalculates.
        if (which_ == OwnerCase::Coof && i == kPrefix) {
            previous_ = i;
            ++coof_callbacks;
            if (!coof_issued) {
                stage = "coof-producer";
                require(coof_parent_filled, "the COOF producer follows the resting parent's fill");
                strategy_exit("owner-exit", id_for(kPrefix - 1), kNa, 150.0);
                coof_issued = true;
            } else if (!owner_seen) {
                stage = "coof-queued";
                observe_owner_projection();
                // exit() marks this Close-born leg next_open=true, so both
                // flush_coof_tail() calls leave it queued. copy_v1 includes
                // the queue and reports no native handle for this row.
                coof_queued = observed_pending && !observed_working
                    && observed_exit.incarnation == 0
                    && observed_exit.created_during_coof_recalc
                    && observed_exit.coof_born_at_close_recalc
                    && observed_exit.created_bar == kPrefix
                    && observed_exit.stop_price == 150.0;
                require(coof_queued, "a real Close-born COOF exit is publicly queued before reversal");
                reverse_after_owner();
            }
            reads.push_back(broker_state_hash_projection());
            return;
        }
        if (i == previous_) return;
        previous_ = i;
        if (i < kPrefix) {
            const auto id = which_ == OwnerCase::Empty && i == kPrefix - 1
                ? std::string{} : id_for(i);
            if (which_ == OwnerCase::Coof && i == kPrefix - 1) {
                // On the next high-first bar: O100 -> H100.5 -> L99
                // activates the stop, then C100 reaches its sell limit.
                // The preceding long has already filled, so this explicit
                // priced entry is sized as a reversal when it is placed.
                strategy_entry(id_for(kPrefix - 1), false, 100.0, 99.0, 1.0);
            } else enter(id, i % 2 == 0);
            if (i == kPrefix - 1 && which_ == OwnerCase::Anchored) {
                // A short STOP parent at 50 never fills on the 99..101 tape.
                // Its unresolved loss is anchored to that future fill.
                strategy_entry("anchored-parent", false, kNa, 50.0, 1.0);
                strategy_exit("owner-exit", "anchored-parent", kNa, kNa, kNa, kNa, kNa,
                              100.0, {}, kNa, {}, kNa, 400.0);
            }
            if (i == kPrefix - 1 && which_ == OwnerCase::PendingBound) {
                // The short limit at 150 waits through a reversal to long.
                // Whole absolute exits bind to this pending id. The reversal
                // owner walk selects them, then the reversal purge cancels
                // their live legs while the parent remains unfilled.
                strategy_entry("bound-parent", false, 150.0, kNa, 1.0);
                strategy_exit("owner-exit", "bound-parent", 50.0, 200.0);
            }
        } else if (i == kPrefix) {
            stage = "owner-before-reversal";
            if (which_ == OwnerCase::Live || which_ == OwnerCase::Empty)
                strategy_exit("owner-exit",
                    which_ == OwnerCase::Empty ? std::string{} : id_for(kPrefix - 1),
                    kNa, 150.0);
            if (which_ == OwnerCase::Anchored) {
                observe_owner_projection();
                anchored_before = observed_anchored;
                // FromOwnerFill children are PendingUntilArmed: the public
                // working enumeration hides this child. One parent and one
                // loss operand produce one anchor; that parent never filled.
                require(working("anchored-parent") && !anchored_filled
                        && anchored_before == 1 && observed_adopted == 0
                        && observed_withdrawn == 0 && !observed_working,
                        "a real anchored child waits without a working projection");
            } else if (which_ == OwnerCase::PendingBound) {
                observe_owner_projection();
                pending_bound_seen = exit_binding_is(true);
                bound_parent_incarnation = bound_parent_handle();
                // The 64 prefix entries consume handles 1..64; the pending
                // parent is 65 and its two exit legs are 66 and 67.
                require(working("bound-parent") && !bound_filled && pending_bound_seen
                        && (observed_pending || observed_working)
                        && bound_parent_incarnation == 65,
                        "a real nonvoid pending-bound exit exists before the reversal");
            }
            if (which_ == OwnerCase::Anchored || which_ == OwnerCase::PendingBound)
                reverse_after_owner();
        } else if (i == kPrefix + 1
                   && (which_ == OwnerCase::Live || which_ == OwnerCase::Empty)) {
            // The prior bar staged the exit; this callback is the first
            // observation point before the opposite entry is issued.
            stage = "owner-before-reversal";
            observe_owner_projection();
            require(observed_pending || observed_working,
                    "a real public exit exists before the reversal");
            reverse_after_owner();
        } else if (i == kPrefix + 1 && which_ == OwnerCase::PendingBound) {
            // The first reversal's drain has removed the public exit rows.
            // The parent still works; its two cancelled placements remain in
            // the retained table until the next source read.
            stage = "bound-cancelled-source";
            observe_owner_projection();
            survival_pending = observed_pending;
            survival_working = observed_working;
            survival_binding = exit_binding_is(true);
            survival_parent_working = working("bound-parent");
            cancelled = !observed_pending && !observed_working;
            active->bound_parent_incarnation = bound_parent_incarnation;
            active->bound_diagnostic_bar = i;
            active->bound_roster_diagnostic = true;
            (void)broker_state_hash_projection();
            bound_cancelled_legs = active->bound_cancelled_legs;
            bound_cancel_facts = active->bound_cancel_facts;
            require(first_close_calls == 1
                    && active->owner_calls == owner_calls_before + first_close_calls,
                    "the first full close selected the pending-bound owner exactly once");
            require(cancelled && !survival_pending && !survival_working
                    && !survival_binding && survival_parent_working && !bound_filled
                    && bound_cancel_facts && bound_cancelled_legs == 2,
                    "the first reversal cancels both bound legs and retains explicit receipts");
        } else if (i == kPrefix + 2 && which_ == OwnerCase::PendingBound) {
            // Bar 65's high fills bound-parent after its source callback. The
            // gate sees no remaining exit owner at that reversal, then this
            // bar's source callback issues the second reversal.
            stage = "bound-before-reversal";
            observe_owner_projection();
            bound_gone_seen = !observed_pending && !observed_working;
            bound_cancel_facts = active->bound_cancel_facts;
            require(cancelled && bound_cancel_facts && bound_filled
                    && bound_fill_calls == 0 && !working("bound-parent")
                    && bound_gone_seen && !exit_binding_is(true),
                    "the parent fill leaves no bound exit owner for the second reversal");
            owner_calls_before = active->owner_calls;
            enter("after-bound-fill", true);
        } else if (i == kPrefix + 3 && which_ == OwnerCase::PendingBound) {
            // The second Applied callback proves that the candidate owner set
            // was empty; this source boundary proves the exits remain gone.
            stage = "bound-gone-source";
            observe_owner_projection();
            cancelled = !observed_pending && !observed_working;
            const std::uint64_t expected_second_owner_calls = active->oracle ? 1U : 0U;
            require(bound_gone_seen
                    && bound_close_calls == expected_second_owner_calls && cancelled
                    && bound_cancel_facts && !exit_binding_is(true),
                    "the second reversal has no remaining bound exit owner to cancel");
        }
        reads.push_back(broker_state_hash_projection());
    }
    bool owner_seen = false, cancelled = false;
    bool coof_parent_filled = false, coof_issued = false, coof_queued = false;
    bool anchored_filled = false, bound_filled = false;
    bool pending_bound_seen = false, bound_gone_seen = false;
    bool bound_cancel_facts = false;
    bool applied_pending = false, applied_working = false, applied_binding = false;
    bool survival_pending = false, survival_working = false, survival_binding = false;
    bool survival_parent_working = false;
    std::uint64_t anchored_before = 0, owner_calls_before = 0;
    std::uint64_t bound_parent_incarnation = 0, bound_cancelled_legs = 0;
    std::uint64_t first_close_calls = 0, bound_fill_calls = 0, bound_close_calls = 0;
    const char* stage = "prefix";
    int last_source_bar = -1, projection_bar = -1, applied_bar = -1, coof_callbacks = 0;
    pf_pending_order_v1_t observed_exit{};
    bool observed_pending = false, observed_working = false;
    std::uint64_t observed_anchored = 0, observed_adopted = 0, observed_withdrawn = 0;
protected:
    void on_native_applied_event(const no::ExecutionAppliedEvent& event,
                                const NativeDecisionContext& context) override {
        Host::on_native_applied_event(event, context);
        if (which_ == OwnerCase::Coof && event.request().label == id_for(kPrefix - 1)
            && event.opened_units != 0.0) {
            stage = "coof-parent-applied";
            coof_parent_filled = true;
            require(context.script_bar_open_ms == kStart + kPrefix * 60000LL
                    && context.coordinate.path_phase == NativePathPhase::Close
                    && context.coordinate.provenance != NativePriceProvenance::Calculation
                    && context.coordinate.provenance != NativePriceProvenance::AfterCalculationClose
                    && event.opened_units == -1.0 && event.closed_units == 1.0
                    && event.resolved_price == 100.0,
                    "the COOF parent reverses at the path Close before POOC calculation");
        }
        if (event.request().label == "anchored-parent" && event.opened_units != 0.0)
            anchored_filled = true;
        if (event.request().label == "bound-parent" && event.opened_units != 0.0) {
            stage = "bound-parent-applied";
            bound_filled = true;
            require(context.script_bar_open_ms == kStart + (kPrefix + 2) * 60000LL
                    && context.coordinate.path_phase == NativePathPhase::High
                    && event.opened_units == -1.0 && event.closed_units == 1.0
                    && event.resolved_price == 150.0,
                    "the pending-bound parent really reverses at the 150 gap");
            // The parent fill has no exit owner left to select.
            bound_fill_calls = active->owner_calls - owner_calls_before - first_close_calls;
            require(cancelled && bound_fill_calls == 0,
                    "the binding fill makes no owner cancellation call");
        }
        if (event.request().label == "after-owner" && event.closed_units > 0.0) {
            stage = "after-owner-applied";
            require(owner_seen, "the reversal follows the observed exit");
            first_close_calls = active->owner_calls - owner_calls_before;
            require(first_close_calls == 1,
                    "this reversal calls the observed owner's full-close canceler exactly once");
            if (which_ == OwnerCase::PendingBound) {
                // Retain each operand at the reversal point for diagnostics;
                // the next source callback checks the retained cancellation.
                applied_bar = context.coordinate.interval_index;
                applied_pending = pending("owner-exit");
                applied_working = working("owner-exit");
                applied_binding = exit_binding_is(true);
                require(context.script_bar_open_ms == kStart + kPrefix * 60000LL
                        && event.opened_units == 1.0 && event.closed_units == 1.0
                        && event.resolved_price == 100.0,
                        "the first full close precedes the pending parent's gap");
            } else {
                cancelled = !pending("owner-exit") && !working("owner-exit");
                require(cancelled, "the reversal cancels the old owner's queued exit");
                if (which_ == OwnerCase::Anchored) {
                    observed_withdrawn = adapter_.anchored_relative_stats().withdrawn;
                    require(!anchored_filled
                            && adapter_.anchored_relative_stats().anchored == anchored_before
                            && adapter_.anchored_relative_stats().adopted == 0
                            && adapter_.anchored_relative_stats().withdrawn == anchored_before,
                            "the anchored child is withdrawn at full close");
                }
            }
        }
        if (event.request().label == "after-bound-fill" && event.closed_units > 0.0) {
            stage = "after-bound-applied";
            bound_close_calls = active->owner_calls - owner_calls_before;
            const std::uint64_t expected_second_owner_calls = active->oracle ? 1U : 0U;
            require(bound_gone_seen && bound_close_calls == expected_second_owner_calls,
                    "the candidate has no bound owner; the old logical walk may still visit it");
            require(context.script_bar_open_ms == kStart + (kPrefix + 2) * 60000LL
                    && event.opened_units == 1.0 && event.closed_units == 1.0
                    && event.resolved_price == 150.0,
                    "the second full close follows the observed binding fill");
        }
    }
private:
    OwnerCase which_;
    int previous_ = -1;
};

struct OwnerDiagnostic {
    const char* name;
    const OwnerHost* host;
    const Result& actual;
    const Result& old;
    const char* phase = "candidate";
    ~OwnerDiagnostic() {
        // Print once even if a callback throws, so the first failing case
        // retains its projection, callback count and producer state.
        std::fprintf(stderr,
            "reversal-gateowners case=%s phase=%s stage=%s bar=%d projection_bar=%d "
            "pending=%d working=%d anchored=%llu adopted=%llu withdrawn=%llu "
            "coof_filled=%d issued=%d queued=%d callbacks=%d "
            "row_handle=%llu row_bar=%d row_coof=%d row_close=%d row_stop=%.17g "
            "pending_bound=%d bound_filled=%d bound_gone=%d bound_receipts=%llu "
            "applied_bar=%d applied_pending=%d applied_working=%d applied_binding=%d "
            "survival_pending=%d survival_working=%d survival_binding=%d survival_parent=%d "
            "first_calls=%llu binding_calls=%llu bound_calls=%llu "
            "calls=%llu old_calls=%llu owner_calls=%llu\n",
            name, phase, host->stage, host->last_source_bar, host->projection_bar,
            host->observed_pending ? 1 : 0, host->observed_working ? 1 : 0,
            static_cast<unsigned long long>(host->observed_anchored),
            static_cast<unsigned long long>(host->observed_adopted),
            static_cast<unsigned long long>(host->observed_withdrawn),
            host->coof_parent_filled ? 1 : 0, host->coof_issued ? 1 : 0,
            host->coof_queued ? 1 : 0, host->coof_callbacks,
            static_cast<unsigned long long>(host->observed_exit.incarnation),
            host->observed_exit.created_bar,
            host->observed_exit.created_during_coof_recalc ? 1 : 0,
            host->observed_exit.coof_born_at_close_recalc ? 1 : 0,
            host->observed_exit.stop_price,
            host->pending_bound_seen ? 1 : 0, host->bound_filled ? 1 : 0,
            host->bound_gone_seen ? 1 : 0,
            static_cast<unsigned long long>(host->bound_cancelled_legs),
            host->applied_bar, host->applied_pending ? 1 : 0,
            host->applied_working ? 1 : 0, host->applied_binding ? 1 : 0,
            host->survival_pending ? 1 : 0, host->survival_working ? 1 : 0,
            host->survival_binding ? 1 : 0, host->survival_parent_working ? 1 : 0,
            static_cast<unsigned long long>(host->first_close_calls),
            static_cast<unsigned long long>(host->bound_fill_calls),
            static_cast<unsigned long long>(host->bound_close_calls),
            static_cast<unsigned long long>(actual.calls),
            static_cast<unsigned long long>(old.calls),
            static_cast<unsigned long long>(actual.owner_calls));
    }
};

void owners() {
    for (const auto which : {OwnerCase::Live, OwnerCase::Empty, OwnerCase::Coof,
                            OwnerCase::Anchored, OwnerCase::PendingBound}) {
        auto bars = tape(kPrefix + 5);
        if (which == OwnerCase::Coof)
            bars[kPrefix].high = 100.5; // High first; the stop-limit fills on Low -> Close.
        if (which == OwnerCase::PendingBound) {
            // Bar 65's source callback (after bar 65's own path) proves the
            // retained cancellation facts while the parent still works. Bar
            // 66's high then fills the parent before callback 66, which issues
            // the second reversal that fills at bar 67's open.
            bars[kPrefix + 2].high = 151.0;
            bars[kPrefix + 2].low = 99.0;
            bars[kPrefix + 2].close = 150.0; // the market reversal issued in callback 66 fills at this close
            for (int i = kPrefix + 3; i < static_cast<int>(bars.size()); ++i) {
                bars[i].open = bars[i].close = 150.0;
                bars[i].high = 151.0; bars[i].low = 149.0;
            }
        }
        OwnerHost candidate(which), oracle(which);
        const auto owner = owner_case_id(which);
        Result actual, old;
        OwnerDiagnostic diagnostic{owner_case_name(which), &oracle, actual, old, "oracle"};
        old = run(oracle, bars, true, &owner);
        diagnostic.phase = "candidate";
        diagnostic.host = &candidate;
        actual = run(candidate, bars, false, &owner);
        diagnostic.phase = "checks";
        diagnostic.host = &candidate;
        same_result(actual, old);
        require(actual.calls >= 1,
                "the gate selected the owner and called its full-close canceler");
        require(actual.owner_calls == 1U,
                "the gate calls this owner only for the first reversal; no second exit owner remains");
        for (const auto* host : {&candidate, &oracle}) {
            require(host->owner_seen && host->cancelled, "the owner cancellation witness completed");
            require(which != OwnerCase::Coof
                    || (host->coof_parent_filled && host->coof_issued
                        && host->coof_queued && host->coof_callbacks == 2),
                    "the COOF callback really issued the queued exit");
            require(which != OwnerCase::PendingBound
                    || (host->pending_bound_seen && host->bound_cancel_facts
                        && host->bound_cancelled_legs == 2
                        && host->bound_filled && host->bound_gone_seen
                        && host->bound_fill_calls == 0
                        && host->bound_close_calls == (host == &candidate ? 0U : 1U)
                        && !host->survival_pending && !host->survival_working
                        && !host->survival_binding && host->survival_parent_working),
                    "the pending-bound cancellation and owner transitions both ran");
            require(host->first_close_calls == 1,
                    "the first reversal's cancel call was for the observed owner");
            // 64 prefix entries give 63 closes; after-owner gives the 64th.
            // PendingBound adds two reversals (the 150 fill, then its close).
            require(host->trade_count() == kPrefix + (which == OwnerCase::PendingBound ? 2 : 0),
                    "owner fixtures keep the hand-worked public closed-trade count");
            for (int i = 0; i < host->trade_count(); ++i)
                require(!host->get_trade(i).exit_from_bracket,
                        "no cancelled far exit becomes a later bracket trade");
        }
        diagnostic.phase = "complete";
    }
}
} // namespace reversal_gate_fixture

int main(int argc, char** argv) {
    try {
        const bool all = argc == 1;
        const bool only_growth = argc == 2 && std::string(argv[1]) == "--growth";
        const bool only_owners = argc == 2 && std::string(argv[1]) == "--owners";
        if (!all && !only_growth && !only_owners) {
            std::fputs("expected --growth, --owners, or no arguments\n", stderr);
            return 2;
        }
        if (all || only_growth) reversal_gate_fixture::growth();
        if (all || only_owners) reversal_gate_fixture::owners();
        std::puts("reversal exit owner gate: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
