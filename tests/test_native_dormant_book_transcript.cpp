// C3 / T3. The lab keeps stdout on the frozen base, then byte-compares the
// patched executable's stdout. All transcript code uses existing interfaces.
// T4/T5/T7 include this TU with PINEFORGE_DORMANT_BOOK_NO_MAIN to share the
// identical generator without adding a new fixture outside this lane's scope.
#include "../src/native_execution_consumer.hpp"
#include "native_match_book_fixture.hpp"
#include "native_order_full_fold.hpp"

#include <pineforge/native_host.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The C2-owned adapter also identifies a patched build. The frozen closure
// has neither adapter; patched nonvacuity assertions are automatic.
#if defined(PINEFORGE_TEST_DORMANT_JOIN_REACHABLE) && !defined(PINEFORGE_TEST_DORMANT_BOOK_PATCHED)
#define PINEFORGE_TEST_DORMANT_BOOK_PATCHED 1
#endif

namespace dormant_test {
using namespace pineforge;
namespace no = pineforge::native_order;
constexpr std::size_t resident = 4096;
constexpr std::int64_t start = 1736121600000LL;

inline void require(bool value, const char* text) {
    if (!value) throw std::runtime_error(text);
}

struct Trace {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> words;
    void add(std::uint64_t tag, std::uint64_t value) { words.emplace_back(tag, value); }
    void print(const char* name) const {
        for (std::size_t i = 0; i < words.size(); ++i)
            std::printf("%s %zu %llu %016llx\n", name, i,
                        static_cast<unsigned long long>(words[i].first),
                        static_cast<unsigned long long>(words[i].second));
    }
};

template <class Host>
void snapshot(Host& h, Trace& trace, std::uint64_t tag) {
    trace.add(tag, h.native_continuation_hash());
    full_fold::Fold f;
    const auto& core = h.consumer().request_core();
    f.u(core.live_size());
    for (const auto& row : core.live()) full_fold::fold(f, row);
    trace.add(tag + 1, f.h);
}

inline std::uint64_t trade_word(const Trade& t) {
    full_fold::Fold f;
    f.i(t.entry_time); f.i(t.exit_time);
    f.d(t.entry_price); f.d(t.exit_price); f.d(t.qty);
    f.d(t.pnl); f.d(t.pnl_pct); f.b(t.is_long);
    f.i(t.entry_bar_index); f.i(t.exit_bar_index);
    f.s(t.entry_id); f.s(t.entry_comment); f.s(t.exit_comment); f.s(t.exit_id);
    f.b(t.exit_from_bracket); f.d(t.max_runup); f.d(t.max_drawdown);
    f.d(t.commission); f.u(t.entry_incarnation); f.b(t.open_at_end); f.e(t.close_cause);
    return f.h;
}

template <class Host>
void finish(Host& h, Trace& trace) {
    trace.add(70, static_cast<std::uint64_t>(h.native_state().kind));
    trace.add(71, h.native_continuation_hash());
    trace.add(72, h.broker_state_hash());
    full_fold::Fold error;
    error.s(h.last_error());
    error.e(h.native_state().failure.code);
    error.e(h.native_state().failure.operation);
    trace.add(73, error.h);
    // Every retained market record, including every command payload member.
    full_fold::Fold record_prefix;
    const auto events = h.native_events(0);
    trace.add(74, events.size());
    for (const auto& row : events) {
        full_fold::Fold f;
        f.e(row.kind); f.u(row.ordinal);
        f.b(row.command.has_value());
        if (row.command) full_fold::fold(f, *row.command);
        f.b(row.driver.has_value());
        if (row.driver) {
            full_fold::fold(f, row.driver->coordinate);
            f.d(row.driver->raw_price);
            f.b(row.driver->sequence.has_value());
            if (row.driver->sequence) f.u(*row.driver->sequence);
            f.b(row.driver->matching); f.b(row.driver->excursion);
        }
        f.b(row.account.has_value());
        if (row.account) {
            f.u(row.account->ordinal); f.i(row.account->effective_time_ms);
            f.d(row.account->marked_equity); f.d(row.account->realized_balance);
            f.d(row.account->signed_units);
        }
        trace.add(75, f.h);
        record_prefix.u(f.h);
        trace.add(76, record_prefix.h);
    }
    const auto& core = h.consumer().request_core();
    trace.add(77, core.live_size());
    for (const auto& row : core.live()) {
        full_fold::Fold f;
        full_fold::fold(f, row);
        trace.add(78, f.h);
    }
    trace.add(79, static_cast<std::uint64_t>(h.trade_count()));
    for (int i = 0; i < h.trade_count(); ++i) trace.add(80, trade_word(h.get_trade(i)));
    trace.add(81, full_fold::core_value(core)); // Includes group/cohort receipts.
    for (const auto hash : h.recorded_hashes()) trace.add(82, hash);
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
    const auto& c = h.consumer();
    // Side diagnostics never enter the compared stdout transcript. The
    // class-guard counter is C2's bridge to C1's actual rejected guards.
    std::fprintf(stderr, "T3/T5 witnesses joined=%llu scanned=%llu fast=%llu eager=%llu "
                 "projected=%llu woken=%llu grid=%llu class_guards=%llu exits=%llu\n",
                 static_cast<unsigned long long>(c.dormant_rows()),
                 static_cast<unsigned long long>(c.scanned_rows()),
                 static_cast<unsigned long long>(c.fast_points()),
                 static_cast<unsigned long long>(c.eager_points()),
                 static_cast<unsigned long long>(c.projected_reads()),
                 static_cast<unsigned long long>(c.wakes_by_price()),
                 static_cast<unsigned long long>(c.grid_points()),
                 static_cast<unsigned long long>(c.class_guard_failures()),
                 static_cast<unsigned long long>(c.fast_path_exits()));
#endif
}

inline no::Request remote(std::size_t i) {
    const bool buy = (i & 1) == 0;
    const auto kind = (i / 2) % 3;
    no::Request r{no::Transact{buy ? 1.0 : -1.0}, "remote", ""};
    const double low = 20.0 + static_cast<double>(i % 16) * 0.25;
    const double high = 200.0 + static_cast<double>(i % 16) * 0.25;
    if (kind == 0) r.trigger = no::Limit{buy ? low : high};
    else if (kind == 1) r.trigger = no::Stop{buy ? high : low};
    else r.trigger = no::StopLimit{buy ? high : low, buy ? high + 1 : low - 1};
    if ((i % 7) == 0) r.capacity = no::PointBudget{0.5};
    return r;
}

inline NativeRunSpec specification(bool grid = false) {
    NativeRunSpec s;
    s.identity = {"c3-dormant-transcript", 1};
    s.input_tf = "5"; s.script_tf = "5";
    s.tickerid = "TEST:C3"; s.timezone = "UTC"; s.session = "24x7";
    s.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    s.initial_capital = 1.0e9; s.point_value = 1; s.account_fx = 1; s.price_tick = 0.25;
    s.event_retention = NativeEventRetention::Full;
    if (grid) s.price_grid = NativePriceGrid::QuantizeFillsAndTriggers;
    return s;
}

inline std::vector<Bar> tape() {
    const double prices[][4] = {
        {100, 101, 100, 100}, {100, 101, 100, 100}, {100, 101, 100, 100},
        {98, 104, 97, 101}, {101, 105, 96, 100}, {100, 103, 95, 101},
        {102, 106, 94, 100}, {100, 103, 97, 101}
    };
    std::vector<Bar> bars;
    for (std::size_t i = 0; i < std::size(prices); ++i)
        bars.push_back({prices[i][0], prices[i][1], prices[i][2], prices[i][3], 1,
                        start + static_cast<std::int64_t>(i) * 300000});
    return bars;
}

class Host : public NativeStrategyHost {
public:
    Host(std::uint64_t seed, bool prepared, bool enabled = true,
         bool callback_failure = false, bool cross_boundary = true)
        : rng_(seed), callback_failure_(callback_failure), cross_boundary_(cross_boundary) {
        set_broker_state_hash_recording(true);
        consumer().set_direct_mutation(!prepared);
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
        consumer().set_match_dormant(enabled);
#else
        (void)enabled;
#endif
    }
    Trace trace;
    bool crossed_up = false;
    bool crossed_down = false;
    bool ring_before = false;
    bool ring_after = false;
    std::size_t callback_commands = 0;

    NativeExecutionConsumer& consumer() {
        return as_native_consumer(execution_consumer());
    }
    const std::vector<std::uint64_t>& recorded_hashes() const { return broker_state_hashes_; }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bar_;
        snapshot(*this, trace, 10);
        if (bar_ == 1) {
            // No spill before the first resident batch has a chance to join.
            for (std::size_t i = 0; i < resident; ++i) {
                auto r = remote(i);
                // Two real winners surround victim's priority. They first
                // become reachable at the fourth bar's opening gap.
                if (i == 3999 || i == 4002) {
                    r = no::Request{no::Transact{1}, i == 3999 ? "ring-a" : "ring-b", ""};
                    r.trigger = no::Limit{99};
                }
                const auto handle = put(r);
                original_.push_back(handle);
                if (i == 4000) victim_ = handle;
            }
        } else if (bar_ == 2) {
            for (std::size_t i = resident; i < resident + 512; ++i) put(remote(i));
            crossed_up = consumer().request_core().book_spilled();
        } else if (bar_ == 3) {
            no::Request skip{no::Transact{1}, "skip", ""};
            skip.trigger = no::Limit{99.5};
            put(skip); // A real t=0 MatchRejected/skip path; M-23.
            no::Request later{no::Transact{1}, "later", ""};
            later.trigger = no::Limit{97.5};
            put(later); // First nonzero-t winner completes the sweep.
        } else if (bar_ == 4 && cross_boundary_) {
            for (std::size_t i = 0; i < 1024; ++i) note(cancel(original_[i]));
            crossed_down = consumer().request_core().live_size() < resident;
        } else if (bar_ == 5 && cross_boundary_) {
            for (std::size_t i = 0; i < 1024; ++i) put(remote(resident + 512 + i));
            require(consumer().request_core().book_spilled(),
                    "T3 did not cross back above kBookMax");
        }
        if (bar_ >= 5 && ring_after) churn();
        observe_dormancy();
    }

    void on_native_applied(const no::ExecutionAppliedEvent& e,
                           const NativeDecisionContext&) override {
        const auto& label = e.definition->request.label;
        if (label == "ring-a") {
            require(e.cursor.t == 0, "T5 first callback was not at t=0");
            // Commands below and above W, then enough submissions to force
            // a spill of a not-yet-swept dormant resident row (M-2/M-8).
            note(cancel(original_[100]));
            auto changed = remote(4050);
            changed.trigger = no::Limit{99.25}; // A real wake/reprice into the remaining path.
            no::ReplaceOptions opts;
            opts.keep_handle = true;
            note(replace(original_[4050], changed, opts));
            for (std::size_t i = 0; i < 4; ++i) put(remote(9000 + i));
            callback_commands += 6;
            snapshot(*this, trace, 20);
            // Do every wide walk BEFORE priming the decoded-row ring.
            victim_rank_ = rank_of(victim_);
            read_all_interfaces(victim_rank_);
            const auto held = consumer().request_core().hold_live(victim_);
            require(static_cast<bool>(held), "T5 spilled victim disappeared");
            before_allowance_ = std::get<no::AllowanceUnits>(held->allowance);
            require(before_allowance_.point_ordinal != e.cursor.point.ordinal,
                    "T5 row above W was refreshed before the first winner");
            trace.add(30, before_allowance_.point_ordinal);
            ring_before = true;
            // No fold/walk after this read until ring-b's first hold_live.
        } else if (label == "ring-b") {
            require(ring_before && e.cursor.t == 0, "T5 second watermark callback missing");
            const auto& core = consumer().request_core();
            const auto lookup_before = core.book_lookup_reads().rows;
            const auto held = core.hold_live(victim_);
            require(static_cast<bool>(held), "T5 second read lost victim");
            const auto now = std::get<no::AllowanceUnits>(held->allowance);
            // Hand oracle: victim's priority is strictly between the real
            // winners, so the eager Evaluate refresh occurred between them.
            // Neither command touched victim: its initial/left are unchanged.
            require(now.point_ordinal == e.cursor.point.ordinal,
                    "T5 fresh decode did not project past the watermark");
            require(now.initial == before_allowance_.initial && now.left == before_allowance_.left,
                    "T5 projected refresh changed the row's units");
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
            if (consumer().eager_points() == 0)
                require(core.book_lookup_reads().rows > lookup_before,
                        "T5 ring victim was not spilled at its second read");
#else
            (void)lookup_before;
#endif
            trace.add(31, now.point_ordinal);
            read_all_interfaces(victim_rank_);
            ring_after = true;
            note(cancel(original_[200]));
            no::ReplaceOptions opts;
            opts.retain_trigger_state = true;
            note(replace(original_[4060], remote(4060), opts));
            callback_commands += 2;
            snapshot(*this, trace, 22);
            if (callback_failure_) throw std::runtime_error("C3 deliberate callback failure");
        } else {
            snapshot(*this, trace, 24);
        }
        observe_dormancy();
    }

    NativePrecommitVerdict validate_execution_precommit(const NativePrecommitView& view) const override {
        // Definitions are public facts; Refuse is an actual host verdict.
        return view.definition && view.definition->request.label == "skip"
            ? NativePrecommitVerdict::Refuse : NativePrecommitVerdict::Admit;
    }

    void assert_fast_witness() {
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
        const auto& c = consumer();
        require(saw_dormant_, "T3/T5 vacuous: dormant_rows() never became positive");
        require(c.fast_points() > 0, "T3/T5 vacuous: fast_points() == 0");
        require(c.projected_reads() > 0, "T3/T5 vacuous: projected_reads() == 0");
        require(c.wakes_by_price() > 0, "T3/T5 vacuous: wakes_by_price() == 0");
        require(c.scanned_rows() > 0, "T3/T5 vacuous: scanned_rows() == 0");
        require(c.eager_points() == 0, "T3/T5 fast seed took an eager point");
        require(c.grid_points() == 0, "T3/T5 unquantized seed used a price grid");
#endif
    }

private:
    no::RequestHandle put(const no::Request& r) {
        const auto answer = submit(r);
        full_fold::Fold f;
        f.e(answer.status); f.u(answer.event_ordinal);
        full_fold::fold_optional(f, answer.handle);
        trace.add(40, f.h);
        require(answer.handle.has_value(), "T3 valid fixture submission refused");
        return *answer.handle;
    }
    void note(const no::CancelResult& r) {
        full_fold::Fold f;
        f.e(r.status); f.u(r.event_ordinal);
        trace.add(41, f.h);
    }
    void note(const no::ReplaceResult& r) {
        full_fold::Fold f;
        f.e(r.status); f.u(r.event_ordinal);
        full_fold::fold_optional(f, r.successor);
        trace.add(42, f.h);
    }
    std::size_t rank_of(const no::RequestHandle& handle) {
        std::size_t rank = 0;
        for (const auto& row : consumer().request_core().live()) {
            if (row.handle() == handle) return rank;
            ++rank;
        }
        throw std::runtime_error("T5 victim absent from live()");
    }
    void read_all_interfaces(std::size_t rank) {
        const auto& core = consumer().request_core();
        full_fold::Fold f;
        const auto* found = core.find_live(victim_);
        require(found != nullptr, "T5 find_live missed victim");
        full_fold::fold(f, *found);
        const auto held = core.hold_live(victim_);
        require(static_cast<bool>(held), "T5 hold_live missed victim");
        full_fold::fold(f, *held);
        const auto& ranked = core.live_at(rank);
        require(ranked.handle() == victim_, "T5 live_at returned another row");
        full_fold::fold(f, ranked);
        // LiveView::back is the public path through private live_back.
        full_fold::fold(f, core.live().back());
        trace.add(32, f.h);
    }
    void churn() {
        // Fixed xorshift choices, with all replacement options exercised
        // deterministically rather than hoping one seed chooses them.
        const auto& core = consumer().request_core();
        for (int choice = 0; choice < 4; ++choice) {
            const auto rank = static_cast<std::size_t>(rng_.next() % core.live_size());
            const auto held = core.hold_live(core.live_at(rank).handle());
            require(static_cast<bool>(held), "T3 churn read failed");
            auto r = held->request();
            if (auto* limit = std::get_if<no::Limit>(&r.trigger)) limit->price += 0.25;
            no::ReplaceOptions opts;
            opts.keep_handle = choice == 1;
            opts.retain_trigger_state = choice == 2;
            opts.keep_binding = choice == 3;
            note(replace(held->handle(), r, opts));
        }
    }
    void observe_dormancy() {
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
        saw_dormant_ = saw_dormant_ || consumer().dormant_rows() > 0;
#endif
    }
    k3_book::Rng rng_;
    bool callback_failure_;
    bool cross_boundary_;
    bool saw_dormant_ = false;
    int bar_ = 0;
    std::vector<no::RequestHandle> original_;
    no::RequestHandle victim_;
    std::size_t victim_rank_ = 0;
    no::AllowanceUnits before_allowance_{};
};

inline Trace run_fast(std::uint64_t seed, bool prepared, bool enabled = true,
                      bool fail_callback = false, bool grid = false) {
    Host h(seed, prepared, enabled, fail_callback);
    require(h.configure_native(specification(grid)).status == NativeSetupStatus::Applied,
            "T3 configure refused");
    const auto bars = tape();
    h.run(bars.data(), static_cast<int>(bars.size()));
    require(h.native_state().kind == (fail_callback ? NativeLifecycleKind::Failed
                                                  : NativeLifecycleKind::Completed),
            h.last_error().c_str());
    require(h.crossed_up && h.ring_before && h.ring_after && h.callback_commands > 0,
            "T3/T5 spill/watermark callback seed did not execute");
    if (!fail_callback) require(h.crossed_down, "T3 did not cross below kBookMax");
    if (enabled && !grid) h.assert_fast_witness();
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
    if (enabled && grid)
        require(h.consumer().grid_points() > 0 && h.consumer().eager_points() > 0,
                "T3 grid fallback had no grid/eager point witness");
#endif
    finish(h, h.trace);
    return std::move(h.trace);
}

// Existing xorshift generator supplies the mixed active paths: brackets,
// both OCO effects, trails, BookClose, rosters, gaps and callback submissions.
class MixedHost final : public k3_book::BookHost {
public:
    MixedHost(const k3_book::BookConfig& c, bool prepared, bool enabled)
        : k3_book::BookHost(c) {
        set_broker_state_hash_recording(true);
        consumer().set_direct_mutation(!prepared);
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
        consumer().set_match_dormant(enabled);
#else
        (void)enabled;
#endif
    }
    Trace trace;
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    const std::vector<std::uint64_t>& recorded_hashes() const { return broker_state_hashes_; }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& ctx) override {
        k3_book::BookHost::on_native_bar(bar, ctx);
        // Add real keep-binding/keep-handle/retained replacements to the
        // fixture's plain replaces. Observe a row before issuing its command.
        const auto& core = consumer().request_core();
        if (core.live_size() != 0) {
            const auto row = core.hold_live(core.live_at(0).handle());
            auto r = row->request();
            no::ReplaceOptions opts;
            opts.keep_handle = (++bars_ % 3) == 0;
            opts.keep_binding = (bars_ % 3) == 1;
            opts.retain_trigger_state = (bars_ % 3) == 2;
            const auto result = replace(row->handle(), r, opts);
            trace.add(43, static_cast<std::uint64_t>(result.status));
            after_command();
        }
        // ULP/tick edge levels on both sides of the next path's start.
        if (bars_ <= 2) {
            const double ulp = std::nextafter(bar.low, std::numeric_limits<double>::infinity())
                - bar.low;
            for (const int k : {-2, -1, 0, 1, 2}) {
                for (const double step : {ulp, 0.25}) {
                    no::Request r{no::Transact{k < 0 ? -1.0 : 1.0}, "edge", ""};
                    r.trigger = no::Limit{bar.low + k * step};
                    const auto result = submit(r);
                    trace.add(44, static_cast<std::uint64_t>(result.status));
                    after_command();
                }
            }
        }
        snapshot(*this, trace, 12);
    }
    void on_native_applied(const no::ExecutionAppliedEvent& e,
                           const NativeDecisionContext& ctx) override {
        k3_book::BookHost::on_native_applied(e, ctx);
        snapshot(*this, trace, 26);
    }
protected:
    void after_command() override { snapshot(*this, trace, 14); }
private:
    int bars_ = 0;
};

inline Trace run_mixed(std::uint64_t seed, bool grid, bool prepared, bool enabled = true) {
    k3_book::BookConfig c;
    c.seed = seed; c.live = 32; c.bars = 24; c.quantize = grid;
    c.path = (seed & 1) ? k3_book::Path::Synthesized : k3_book::Path::Lower;
    MixedHost h(c, prepared, enabled);
    const auto result = k3_book::run_book(h, c);
    require(result.completed, result.error.c_str());
    require(result.accepted > 0 && result.applied > 0, "T3 mixed seed did no work");
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
    require(grid ? h.consumer().grid_points() > 0 : h.consumer().grid_points() == 0,
            "T3 mixed seed grid counter disagrees with its specification");
#endif
    finish(h, h.trace);
    return std::move(h.trace);
}

inline Trace run_carried(bool prepared, bool enabled = true) {
    Host h(0x31415926, prepared, enabled, false, false);
    require(h.configure_native(specification()).status == NativeSetupStatus::Applied,
            "T3 carried configure refused");
    const auto bars = tape();
    require(h.stream_begin(bars.data(), 3, "5", "5"), "T3 carried begin failed");
    require(h.stream_push_tick({start + 900000, 1, 100, 1}), "T3 initial stream tick failed");
    require(h.stream_advance_time(start + 1500000), "T3 carried advance failed");
    require(h.stream_push_tick({start + 1500001, 2, 98, 1}), "T3 gap stream tick failed");
    require(h.stream_end(false), "T3 carried end failed");
    std::size_t carried = 0;
    for (const auto& event : h.native_events(0))
        if (event.driver && event.driver->coordinate.provenance == NativePriceProvenance::CarriedOpen)
            ++carried;
    require(carried > 0 && h.ring_before && h.ring_after, "T3 carried seed did not execute");
    if (enabled) h.assert_fast_witness();
    finish(h, h.trace);
    return std::move(h.trace);
}

// A separate, deliberately eager seed: spilled BookClose rows under an open
// position, and a spilled winner at t=0 between their priorities. It witnesses
// the active_spilled per-scan guard (M-20) and the bulk close refresh (M-14).
class ActiveHost final : public NativeStrategyHost {
public:
    explicit ActiveHost(bool prepared, bool enabled) {
        set_broker_state_hash_recording(true);
        consumer().set_direct_mutation(!prepared);
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
        consumer().set_match_dormant(enabled);
#else
        (void)enabled;
#endif
    }
    Trace trace;
    bool saw_signal = false;
    bool spilled_closes = false;
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    const std::vector<std::uint64_t>& recorded_hashes() const { return broker_state_hashes_; }
    void on_native_run_begin() override {
        require(submit(no::Request{no::Transact{2}, "open", ""}).handle.has_value(),
                "T3 active seed opening refused");
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        snapshot(*this, trace, 50);
        if (++bar_ != 1) return;
        require(physical_position().signed_units == 2, "T3 active seed did not open");
        for (std::size_t i = 0; i < resident + 512; ++i) {
            if (i == resident + 128) {
                no::Request signal{no::Transact{1}, "active-signal", ""};
                signal.trigger = no::Limit{99};
                require(submit(signal).handle.has_value(), "T3 active signal refused");
            }
            no::Request close{no::Reduce{no::ExplicitUnits{1}}, "bound-close", ""};
            close.trigger = no::Limit{1000};
            require(submit(close).handle.has_value(), "T3 active close refused");
        }
        spilled_closes = consumer().request_core().book_spilled();
    }
    void on_native_applied(const no::ExecutionAppliedEvent& e,
                           const NativeDecisionContext&) override {
        if (e.definition->request.label == "active-signal") {
            require(e.cursor.t == 0, "T3 active signal did not fill at the gap");
            saw_signal = true;
        }
        snapshot(*this, trace, 52);
    }
private:
    int bar_ = 0;
};

inline Trace run_active(bool prepared, bool enabled = true) {
    ActiveHost h(prepared, enabled);
    require(h.configure_native(specification()).status == NativeSetupStatus::Applied,
            "T3 active configure refused");
    const auto bars = tape();
    h.run(bars.data(), 5);
    require(h.native_state().kind == NativeLifecycleKind::Completed, h.last_error().c_str());
    require(h.spilled_closes && h.saw_signal, "T3 active-spill seed did not execute");
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
    if (enabled)
        require(h.consumer().eager_points() > 0 && h.consumer().scanned_rows() > 0,
                "T3 active-spill seed did not exercise the eager guard");
#endif
    finish(h, h.trace);
    return std::move(h.trace);
}

// M-20 must change eligibility DURING a fast point: an initially oversized
// eager book alone would not test the per-scan recheck. A continuous-path
// fill callback consumes all dormant spill candidates, then spills active
// grouped rows, including one eligible on that very point's remaining suffix.
class OverflowHost final : public NativeStrategyHost {
public:
    OverflowHost(bool prepared, bool enabled) {
        set_broker_state_hash_recording(true);
        consumer().set_direct_mutation(!prepared);
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
        consumer().set_match_dormant(enabled);
#else
        (void)enabled;
#endif
    }
    Trace trace;
    bool overflowed = false;
    bool target_filled = false;
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    const std::vector<std::uint64_t>& recorded_hashes() const { return broker_state_hashes_; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        snapshot(*this, trace, 54);
        if (++bar_ != 1) return;
        for (std::size_t i = 0; i < 512; ++i) {
            no::Request r{no::Transact{1}, i == 256 ? "overflow-parent" : "rest", ""};
            r.trigger = no::Limit{i == 256 ? 99.0 : 50.0};
            require(submit(r).handle.has_value(), "T3 overflow parent population refused");
        }
    }
    void on_native_applied(const no::ExecutionAppliedEvent& e,
                           const NativeDecisionContext&) override {
        const auto& label = e.definition->request.label;
        if (label == "overflow-parent") {
            require(e.cursor.t > 0 && e.cursor.t < 1,
                    "T3 overflow callback did not run inside a continuous segment");
            parent_point_ = e.cursor.point.ordinal;
            parent_t_ = e.cursor.t;
            for (std::size_t i = 0; i < resident + 512; ++i) {
                const bool target = i == resident + 510;
                no::Request r{no::Transact{1}, target ? "overflow-target" : "grouped", ""};
                r.trigger = no::Limit{target ? 97.5 : 50.0};
                no::Member member;
                member.group = 100000 + i;
                r.group = member; // Never dormant, even though most cannot fill.
                require(submit(r).handle.has_value(), "T3 overflow group submit refused");
            }
            overflowed = consumer().request_core().book_spilled();
        } else if (label == "overflow-target") {
            // Hand oracle: 97.5 is on the remaining falling suffix after
            // the parent's 99 crossing, in the SAME point, later in t.
            require(e.cursor.point.ordinal == parent_point_ && e.cursor.t > parent_t_,
                    "T3 active-spilled target missed the callback's remaining suffix");
            require(e.resolved_price == 97.5, "T3 overflow target booked the wrong level");
            target_filled = true;
        }
        snapshot(*this, trace, 56);
    }
private:
    int bar_ = 0;
    std::uint64_t parent_point_ = 0;
    double parent_t_ = 0;
};

inline Trace run_overflow(bool prepared, bool enabled = true) {
    OverflowHost h(prepared, enabled);
    require(h.configure_native(specification()).status == NativeSetupStatus::Applied,
            "T3 overflow configure refused");
    const std::array<Bar, 3> bars{{
        {100, 101, 100, 100, 1, start},
        {100, 101, 100, 100, 1, start + 300000},
        {100, 101, 97, 100, 1, start + 600000}
    }};
    h.run(bars.data(), static_cast<int>(bars.size()));
    require(h.native_state().kind == NativeLifecycleKind::Completed, h.last_error().c_str());
    require(h.overflowed && h.target_filled, "T3 mid-point active-spill seed did not execute");
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
    if (enabled)
        require(h.consumer().fast_points() > 0 && h.consumer().eager_points() > 0
                    && h.consumer().fast_path_exits() > 0,
                "T3 overflow seed never left an established fast path");
#endif
    finish(h, h.trace);
    return std::move(h.trace);
}

// P2-6 keepalive control. Exactly ONE row spills. Unlike the large churn
// seed, neither callback issues a command or walks a large spilled tier after
// priming. Between the callbacks base does one full scan of that one row and
// one hold_scanned before promoting it into the vacancy left by ring-first.
// Thus fewer than kSide=8 later keeps occur. The patched fast scan decodes
// none. A weak owner detects an illegally cleared ring BEFORE raw dereference.
class RingWindowHost final : public NativeStrategyHost {
public:
    RingWindowHost(bool prepared, bool enabled) {
        set_broker_state_hash_recording(true);
        consumer().set_direct_mutation(!prepared);
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
        consumer().set_match_dormant(enabled);
#else
        (void)enabled;
#endif
    }
    Trace trace;
    bool before = false;
    bool after = false;
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    const std::vector<std::uint64_t>& recorded_hashes() const { return broker_state_hashes_; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        snapshot(*this, trace, 90);
        if (++bar_ == 1) {
            for (std::size_t i = 0; i < resident; ++i) {
                no::Request r{no::Transact{1}, i + 2 == resident ? "ring-first" : "ring-rest", ""};
                r.trigger = no::Limit{i + 2 == resident ? 99.0 : 50.0};
                const auto result = submit(r);
                require(result.handle.has_value(), "T5 ring population refused");
                if (i + 1 == resident) victim_ = *result.handle;
            }
        } else if (bar_ == 2) {
            no::Request r{no::Transact{1}, "ring-second", ""};
            r.trigger = no::Limit{99};
            require(submit(r).handle.has_value(), "T5 ring second winner refused");
            require(consumer().request_core().book_spilled(), "T5 ring victim never spilled");
        }
    }
    void on_native_applied(const no::ExecutionAppliedEvent& e,
                           const NativeDecisionContext&) override {
        const auto& core = consumer().request_core();
        if (e.request().label == "ring-first") {
            require(e.cursor.t == 0, "T5 ring first winner was not at t=0");
            snapshot(*this, trace, 92); // All whole-book reads precede priming.
            std::size_t rank = 0;
            for (const auto& row : core.live()) {
                if (row.handle() == victim_) break;
                ++rank;
            }
            require(rank < core.live_size(), "T5 ring victim absent");
            const auto reads = core.book_lookup_reads().rows;
            raw_find_ = core.find_live(victim_);
            require(raw_find_ != nullptr, "T5 ring find_live missed");
            raw_rank_ = &core.live_at(rank);
            require(raw_rank_->handle() == victim_, "T5 ring live_at missed");
            held_ = core.hold_live(victim_);
            require(static_cast<bool>(held_), "T5 ring hold_live missed");
            // Each API MUST decode the spilled row, not accidentally read
            // an already resident row. The hand count is one read per API.
            require(core.book_lookup_reads().rows - reads == 3,
                    "T5 ring APIs did not read a spilled victim");
            saved_ = std::get<no::AllowanceUnits>(held_->allowance);
            require(saved_.point_ordinal != e.cursor.point.ordinal,
                    "T5 ring victim above W was already refreshed");
            {
                const auto ring_only = core.hold_live(victim_);
                lifetime_ = ring_only;
            }
            trace.add(94, saved_.point_ordinal);
            before = true;
        } else if (e.request().label == "ring-second") {
            require(before && e.cursor.t == 0, "T5 ring second winner was not at t=0");
            require(!lifetime_.expired(), "T5 watermark update cleared the decoded keepalive ring");
            for (const auto* copy : {held_.get(), raw_find_, raw_rank_}) {
                const auto units = std::get<no::AllowanceUnits>(copy->allowance);
                require(units.point_ordinal == saved_.point_ordinal
                            && units.initial == saved_.initial && units.left == saved_.left,
                        "T5 watermark update re-projected an already held copy");
                trace.add(95, units.point_ordinal);
            }
            const auto fresh = core.hold_live(victim_);
            require(static_cast<bool>(fresh), "T5 ring fresh read missed");
            const auto units = std::get<no::AllowanceUnits>(fresh->allowance);
            require(units.point_ordinal == e.cursor.point.ordinal
                        && units.initial == saved_.initial && units.left == saved_.left,
                    "T5 ring fresh read did not match the eager refresh");
            trace.add(96, units.point_ordinal);
            after = true;
            snapshot(*this, trace, 98);
        }
    }
private:
    int bar_ = 0;
    no::RequestHandle victim_{};
    no::AllowanceUnits saved_{};
    std::shared_ptr<const no::LiveRequest> held_;
    std::weak_ptr<const no::LiveRequest> lifetime_;
    const no::LiveRequest* raw_find_ = nullptr;
    const no::LiveRequest* raw_rank_ = nullptr;
};

inline Trace run_ring_window(bool prepared, bool enabled = true) {
    RingWindowHost h(prepared, enabled);
    require(h.configure_native(specification()).status == NativeSetupStatus::Applied,
            "T5 ring configure refused");
    const auto bars = tape();
    h.run(bars.data(), 4);
    require(h.native_state().kind == NativeLifecycleKind::Completed, h.last_error().c_str());
    require(h.before && h.after, "T5 ring window never advanced past the victim");
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
    if (enabled)
        require(h.consumer().dormant_rows() > 0 && h.consumer().fast_points() > 0
                    && h.consumer().projected_reads() > 0 && h.consumer().wakes_by_price() > 0
                    && h.consumer().scanned_rows() > 0 && h.consumer().eager_points() == 0,
                "T5 ring window was vacuously eager");
#endif
    finish(h, h.trace);
    return std::move(h.trace);
}

// REV-CX K1/M24: the back two rows are real later candidates, hence awake
// before the first t=0 callback. Its two submissions use the fill's vacancy,
// then force make_book_room while the highest dormant row is in the MIDDLE.
// The core may conservatively spill the back active row (documented fallback)
// or spill the middle only at a safe boundary. All three original fills must
// still identify the correct row, in order, at the same point and price.
class MiddleSpillHost final : public NativeStrategyHost {
public:
    MiddleSpillHost(bool prepared, bool enabled, bool force_eager)
        : enabled_(enabled), force_eager_(force_eager) {
        set_broker_state_hash_recording(true);
        consumer().set_direct_mutation(!prepared);
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
        consumer().set_match_dormant(enabled);
#else
        (void)enabled;
#endif
    }
    Trace trace;
    std::size_t filled = 0;
    bool spilled = false;
    bool middle_spilled = false;
    bool forced_eager = false;
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    const std::vector<std::uint64_t>& recorded_hashes() const { return broker_state_hashes_; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        snapshot(*this, trace, 100);
        ++bar_;
        if (bar_ == 4 && force_eager_) {
            // The notification's two new remote rows have now joined on
            // later points. Two grouped rows occupy the vacancies from the
            // later fills, leaving those newer dormant rows in the middle.
            // An eager next point must promote the earlier spilled victim
            // while these higher-priority dormant rows still remain.
            for (std::size_t i = 0; i < 2; ++i) {
                no::Request r{no::Transact{1}, "middle-active-tail", ""};
                r.trigger = no::Limit{50};
                no::Member member;
                member.group = 900000 + i;
                r.group = member;
                require(submit(r).handle.has_value(), "T3 middle active-tail placement refused");
            }
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
            consumer().set_match_dormant(false);
#endif
            forced_eager = true;
        }
        if (bar_ != 1) return;
        for (std::size_t i = 0; i < resident; ++i) {
            const bool winner = i == resident - 6 || i >= resident - 2;
            no::Request r{no::Transact{winner ? static_cast<double>(i - (resident - 7)) : 1},
                          winner ? "middle-winner" : "middle-rest", ""};
            r.trigger = no::Limit{winner ? 99.0 : 50.0};
            const auto result = submit(r);
            require(result.handle.has_value(), "T3 middle-spill population refused");
            if (winner) winners_[next_++] = *result.handle;
            if (i == resident - 3) middle_ = *result.handle;
        }
    }
    void on_native_applied(const no::ExecutionAppliedEvent& e,
                           const NativeDecisionContext&) override {
        require(e.request().label == "middle-winner" && filled < winners_.size(),
                "T3 middle-spill unexpected fill");
        require(e.handle() == winners_[filled] && e.cursor.t == 0 && e.resolved_price == 98,
                "T3 middle-spill changed a later candidate's identity, cursor or price");
        // Hand arithmetic from population: indices 4090,4094,4095 carry
        // quantities 1,5,6 respectively; derive it from each accepted request.
        require(e.opened_units == std::get<no::Transact>(e.request().intent).signed_units,
                "T3 middle-spill read another resident row's quantity");
        if (filled++ == 0) {
            point_ = e.cursor.point.ordinal;
            const auto& core = consumer().request_core();
            require(e.terminal && core.find_live(e.handle()) == nullptr,
                    "T3 notification ran before its winner was consumed");
            const auto before = core.book_lookup_reads().rows;
            require(static_cast<bool>(core.hold_live(middle_)),
                    "T3 notification middle row was absent before submissions");
            require(core.book_lookup_reads().rows == before,
                    "T3 notification middle row was already spilled");
            for (std::size_t i = 0; i < 2; ++i) {
                no::Request r{no::Transact{1}, "middle-new", ""};
                r.trigger = no::Limit{50};
                require(submit(r).handle.has_value(), "T3 middle callback submit refused");
            }
            spilled = core.book_spilled();
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
            if (enabled_) {
                const auto reads = core.book_lookup_reads().rows;
                require(static_cast<bool>(core.hold_live(middle_)),
                        "T3 notification lost the middle dormant row");
                require(core.book_lookup_reads().rows == reads + 1,
                        "T3 notification did not spill the middle dormant row");
                middle_spilled = true;
            }
#endif
        }
        require(e.cursor.point.ordinal == point_, "T3 middle-spill deferred a later candidate");
        snapshot(*this, trace, 102);
    }
private:
    int bar_ = 0;
    bool enabled_;
    bool force_eager_;
    std::size_t next_ = 0;
    std::uint64_t point_ = 0;
    std::array<no::RequestHandle, 3> winners_{};
    no::RequestHandle middle_{};
};

inline Trace run_middle_spill(bool prepared, bool enabled = true, bool force_eager = false) {
    MiddleSpillHost h(prepared, enabled, force_eager);
    require(h.configure_native(specification()).status == NativeSetupStatus::Applied,
            "T3 middle-spill configure refused");
    const auto bars = tape();
    h.run(bars.data(), force_eager ? 5 : 4);
    require(h.native_state().kind == NativeLifecycleKind::Completed, h.last_error().c_str());
    require(h.spilled && h.filled == 3, "T3 middle-spill callback/candidates did not execute");
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
    if (enabled) {
        require(h.middle_spilled, "T3 notification middle-spill witness was vacuous");
        require(h.consumer().dormant_rows() > 0 && h.consumer().fast_points() > 0
                    && h.consumer().scanned_rows() > 0,
                "T3 middle-spill never established the fast path");
        if (force_eager)
            require(h.forced_eager && h.consumer().eager_points() > 0,
                    "T3 middle-spill followup did not exercise retained-candidate promotion");
    }
#endif
    finish(h, h.trace);
    return std::move(h.trace);
}

// M9/M23: the first nonzero-t winner has the LOWEST priority. Completing
// only to that priority leaves every higher-priority dormant allowance old.
// Observe inside its callback, before a later point can repair the defect.
class CompleteHost final : public NativeStrategyHost {
public:
    CompleteHost(bool prepared, bool enabled) {
        set_broker_state_hash_recording(true);
        consumer().set_direct_mutation(!prepared);
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
        consumer().set_match_dormant(enabled);
#else
        (void)enabled;
#endif
    }
    Trace trace;
    bool checked = false;
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    const std::vector<std::uint64_t>& recorded_hashes() const { return broker_state_hashes_; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        snapshot(*this, trace, 170);
        if (++bar_ != 1) return;
        for (std::size_t i = 0; i < 64; ++i) {
            no::Request r{no::Transact{1}, i == 0 ? "complete-first" : "complete-rest", ""};
            r.trigger = no::Limit{i == 0 ? 99.0 : 50.0};
            require(submit(r).handle.has_value(), "T3 complete population refused");
        }
    }
    void on_native_applied(const no::ExecutionAppliedEvent& e,
                           const NativeDecisionContext&) override {
        require(e.request().label == "complete-first" && e.cursor.t > 0 && e.cursor.t < 1,
                "T3 complete seed missed its interior winner");
        std::size_t inspected = 0;
        for (const auto& row : consumer().request_core().live()) {
            const auto units = std::get<no::AllowanceUnits>(row.allowance);
            // Hand arithmetic: all 63 remote rows were Evaluate winners at
            // t=0, before the first interior fill; their units stay one.
            require(units.point_ordinal == e.cursor.point.ordinal
                        && units.initial == 1 && units.left == 1,
                    "T3 complete left the higher-priority dormant suffix unswept");
            ++inspected;
        }
        require(inspected == 63, "T3 complete seed lost a remote row");
        checked = true;
        snapshot(*this, trace, 172);
    }
private:
    int bar_ = 0;
};

inline Trace run_complete(bool prepared, bool enabled = true) {
    CompleteHost h(prepared, enabled);
    require(h.configure_native(specification()).status == NativeSetupStatus::Applied,
            "T3 complete configure refused");
    const std::array<Bar, 3> bars{{
        {100, 101, 100, 100, 1, start},
        {100, 101, 100, 100, 1, start + 300000},
        {100, 101, 98, 100, 1, start + 600000}
    }};
    h.run(bars.data(), static_cast<int>(bars.size()));
    require(h.native_state().kind == NativeLifecycleKind::Completed, h.last_error().c_str());
    require(h.checked, "T3 complete callback never ran");
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
    if (enabled)
        require(h.consumer().dormant_rows() > 0 && h.consumer().fast_points() > 0
                    && h.consumer().wakes_by_price() > 0 && h.consumer().eager_points() == 0,
                "T3 complete seed was vacuously eager");
#endif
    finish(h, h.trace);
    return std::move(h.trace);
}

// M19 has a deterministic active-trail witness in addition to mixed seeds.
// Hand oracle: sell trail arms at 100, initial stop is 100-2=98. The third
// bar reaches 106 without touching 98, raises best to 106, then crosses
// 106-2=104. Skipping active trail observation leaves stop 98 and misses it.
class TrailHost final : public NativeStrategyHost {
public:
    TrailHost(bool prepared, bool enabled) {
        set_broker_state_hash_recording(true);
        consumer().set_direct_mutation(!prepared);
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
        consumer().set_match_dormant(enabled);
#else
        (void)enabled;
#endif
    }
    Trace trace;
    bool tracked = false;
    bool filled = false;
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    const std::vector<std::uint64_t>& recorded_hashes() const { return broker_state_hashes_; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        snapshot(*this, trace, 180);
        if (++bar_ == 1) {
            for (std::size_t i = 0; i < 8; ++i)
                require(submit(remote(i)).handle.has_value(), "T3 trail remote placement refused");
            no::Request r{no::Transact{-1}, "active-trail", ""};
            no::Trail trail;
            trail.offset = 2;
            trail.arm_price = 100;
            r.trigger = trail;
            const auto result = submit(r);
            require(result.handle.has_value(), "T3 active trail placement refused");
            trail_ = *result.handle;
        } else if (bar_ == 2) {
            const auto row = consumer().request_core().hold_live(trail_);
            require(static_cast<bool>(row), "T3 trail disappeared before its excursion");
            const auto* state = std::get_if<no::TrailTrack>(&row->trigger_state);
            require(state && state->best == 100, "T3 trail never entered real tracking state");
            tracked = true;
        }
    }
    void on_native_applied(const no::ExecutionAppliedEvent& e,
                           const NativeDecisionContext&) override {
        require(tracked && !filled && e.handle() == trail_,
                "T3 trail seed executed another or repeated request");
        require(e.resolved_price == 104,
                "T3 active trail did not execute its hand-derived 106-2 stop");
        // BASE inspect_candidate keeps Transact's sign; both execution paths
        // copy inspected_opened_units into the event. Flat -> sell one opens
        // -1 signed unit, while the stop remains 106-2=104.
        require(e.opened_units == std::get<no::Transact>(e.request().intent).signed_units
                    && e.closed_units == 0 && e.terminal,
                "T3 active trail did not open its signed sell units terminally");
        filled = true;
        snapshot(*this, trace, 182);
    }
private:
    int bar_ = 0;
    no::RequestHandle trail_{};
};

inline Trace run_trail(bool prepared, bool enabled = true) {
    TrailHost h(prepared, enabled);
    require(h.configure_native(specification()).status == NativeSetupStatus::Applied,
            "T3 trail configure refused");
    const std::array<Bar, 3> bars{{
        {100, 100, 100, 100, 1, start},
        {100, 100, 100, 100, 1, start + 300000},
        {100, 106, 99, 100, 1, start + 600000}
    }};
    h.run(bars.data(), static_cast<int>(bars.size()));
    require(h.native_state().kind == NativeLifecycleKind::Completed, h.last_error().c_str());
    require(h.tracked && h.filled && h.physical_position().signed_units == -1,
            "T3 active trail seed missed its tracked excursion/fill");
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
    if (enabled)
        require(h.consumer().dormant_rows() > 0 && h.consumer().fast_points() > 0
                    && h.consumer().scanned_rows() > 0 && h.consumer().eager_points() == 0,
                "T3 active trail seed was vacuously eager");
#endif
    finish(h, h.trace);
    return std::move(h.trace);
}

using DriverCounts = std::array<std::array<std::uint64_t, 2>, 7>;

// Independent policy oracle transcribed from BASE trigger_permits_driver,
// engine 3ea5a0d60:4253-4281. It is a contract table, not the new class guard:
// Limit-only on carried opens, all priced triggers on observed/tick/open/
// excursion points, no priced trigger after a confirmed calculation or at a
// target-only current execution. StopLimitLive remains an OTHER trigger.
inline bool base_priced_permission(no::DriverEligibilityClass driver, bool limit) {
    switch (driver) {
    case no::DriverEligibilityClass::ObservedPrint:
    case no::DriverEligibilityClass::TickAfterCalculation:
    case no::DriverEligibilityClass::ConfirmedOpen:
    case no::DriverEligibilityClass::ConfirmedExcursion:
        return true;
    case no::DriverEligibilityClass::CarriedOpen:
        return limit;
    case no::DriverEligibilityClass::ConfirmedAfterCalculationClose:
    case no::DriverEligibilityClass::CurrentExecution:
        return false;
    }
    throw std::runtime_error("T3 unknown driver class");
}

inline no::DriverEligibilityClass recorded_driver_class(const NativeDriverPoint& point,
                                                        bool observed_mode) {
    using C = no::DriverEligibilityClass;
    switch (point.coordinate.provenance) {
    case NativePriceProvenance::CurrentExecution: return C::CurrentExecution;
    case NativePriceProvenance::ObservedPrint: return C::ObservedPrint;
    case NativePriceProvenance::CarriedOpen: return C::CarriedOpen;
    case NativePriceProvenance::AfterCalculationClose:
        return observed_mode ? C::TickAfterCalculation : C::ConfirmedAfterCalculationClose;
    case NativePriceProvenance::ModeledOHLCOpen: return C::ConfirmedOpen;
    case NativePriceProvenance::Confirmed:
        return point.coordinate.path_phase == NativePathPhase::Open
            ? C::ConfirmedOpen : C::ConfirmedExcursion;
    case NativePriceProvenance::ModeledOHLCClose:
        return point.excursion ? C::ConfirmedExcursion : C::ConfirmedAfterCalculationClose;
    case NativePriceProvenance::Calculation:
    case NativePriceProvenance::PartialFinalized:
        return C::ConfirmedAfterCalculationClose;
    }
    throw std::runtime_error("T3 unknown recorded price provenance");
}

class DriverMatrixHost final : public NativeStrategyHost {
public:
    DriverMatrixHost(bool prepared, bool enabled) : enabled_(enabled) {
        set_broker_state_hash_recording(true);
        consumer().set_direct_mutation(!prepared);
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
        consumer().set_match_dormant(enabled);
#else
        (void)enabled;
#endif
    }
    Trace trace;
    bool current = false;
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    const std::vector<std::uint64_t>& recorded_hashes() const { return broker_state_hashes_; }
    void on_native_run_begin() override {
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            no::Request r{no::Transact{1}, "matrix-priced", ""};
            if (i == 0) r.trigger = no::Limit{20};
            else if (i == 1) r.trigger = no::Stop{200};
            else if (i == 2) r.trigger = no::StopLimit{200, 201};
            // Stop already through the print, limit well below every print:
            // this real activation yields StopLimitLive, never an execution.
            else r.trigger = no::StopLimit{90, 50};
            const auto result = submit(r);
            require(result.handle.has_value(), "T3 driver matrix population refused");
            rows_[i] = *result.handle;
        }
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        snapshot(*this, trace, 110);
        if (++bar_ != 2) return;
        std::array<no::AllowanceUnits, 4> before{};
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const auto row = consumer().request_core().hold_live(rows_[i]);
            require(static_cast<bool>(row), "T3 driver matrix priced row disappeared");
            before[i] = std::get<no::AllowanceUnits>(row->allowance);
        }
        const auto result = submit(no::Request{no::Transact{0.125}, "matrix-current", ""});
        require(result.handle.has_value(), "T3 current-execution target refused");
        const auto executed = execute_current(NativeCurrentExecution{*result.handle});
        const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&executed);
        require(applied && applied->cursor.point.provenance == NativePriceProvenance::CurrentExecution,
                "T3 driver matrix did not execute a real CurrentExecution point");
        current = true;
        // Hand oracle: CurrentExecution consumes ONLY its named market target.
        // Unrelated priced rows retain all three allowance words.
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const auto row = consumer().request_core().hold_live(rows_[i]);
            require(static_cast<bool>(row), "T3 current execution consumed an unrelated priced row");
            const auto now = std::get<no::AllowanceUnits>(row->allowance);
            require(now.point_ordinal == before[i].point_ordinal
                        && now.initial == before[i].initial && now.left == before[i].left,
                    "T3 CurrentExecution refreshed an unrelated priced row");
        }
        snapshot(*this, trace, 112);
    }
    void on_native_tick(const Bar&, const NativeTickContext&) override {
        // Captures carried-open/after-calculation changes before the next
        // observed point can overwrite a wrong allowance.
        snapshot(*this, trace, 114);
    }
    void on_native_applied(const no::ExecutionAppliedEvent&,
                           const NativeDecisionContext&) override {
        snapshot(*this, trace, 116);
    }
    void inspect_drivers(bool forward, std::uint64_t confirmed_through,
                         DriverCounts& visits, DriverCounts& refused) {
        const auto& core = consumer().request_core();
        std::array<std::shared_ptr<const no::LiveRequest>, 4> rows;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            rows[i] = core.hold_live(rows_[i]);
            require(static_cast<bool>(rows[i]), "T3 driver matrix lost a priced witness");
        }
        require(std::holds_alternative<no::StopLimitLive>(rows[3]->trigger_state),
                "T3 driver matrix never activated its live stop-limit witness");
        for (const auto& event : native_events(0)) {
            if (!event.driver) continue;
            const auto& point = *event.driver;
            // The final warmup close and the first tick can share a time.
            // The recorded event boundary, not its timestamp, distinguishes
            // ConfirmedAfterCalculationClose from TickAfterCalculation.
            const bool observed = forward && event.ordinal > confirmed_through;
            const auto driver = recorded_driver_class(point, observed);
            no::EvaluationContext context;
            context.cursor.point = point.coordinate;
            context.driver_class = driver;
            context.existing_matching_bit = point.matching;
            for (const auto& row : rows) {
                const bool limit = std::holds_alternative<no::Limit>(row->request().trigger);
                const auto cls = static_cast<std::size_t>(driver);
                const auto kind = limit ? 0U : 1U;
                const auto facts = core.eligibility_facts(*row, context);
                const bool expected = base_priced_permission(driver, limit);
                require(facts.driver_ok == expected,
                        "T3 real driver disagrees with the base Limit/Other permission matrix");
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
                // The base policy above is independent of the derived class
                // guard. Check that guard on every recorded cell as well:
                // suppressing a permitted class can otherwise hide behind
                // successful joins in the other class.
                require(consumer().dormant_driver_permitted(context, limit) == expected,
                        "T3 dormant driver disagrees with the base Limit/Other permission matrix");
#endif
                ++visits[cls][kind];
                if (!facts.driver_ok) ++refused[cls][kind];
                trace.add(118 + 2 * cls + kind, facts.driver_ok);
            }
        }
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
        if (enabled_)
            require(consumer().dormant_rows() > 0 && consumer().fast_points() > 0
                        && consumer().scanned_rows() > 0 && consumer().eager_points() == 0,
                    "T3 driver matrix never exercised fast dormant matching");
#endif
    }
private:
    int bar_ = 0;
    bool enabled_;
    std::array<no::RequestHandle, 4> rows_{};
};

inline Trace run_driver_matrix(bool prepared, bool enabled = true) {
    Trace result;
    DriverCounts visits{}, refused{};
    for (const bool forward : {false, true}) {
        DriverMatrixHost h(prepared, enabled);
        auto spec = specification();
        spec.close_execution = NativeCloseExecution::AfterCalculation;
        require(h.configure_native(spec).status == NativeSetupStatus::Applied,
                "T3 driver matrix configure refused");
        const auto bars = tape();
        std::uint64_t confirmed_through = 0;
        if (forward) {
            require(h.stream_begin(bars.data(), 3, "5", "5"), "T3 driver matrix begin failed");
            for (const auto& event : h.native_events(0))
                confirmed_through = std::max(confirmed_through, event.ordinal);
            require(h.stream_push_tick({start + 900000, 1, 100, 1}), "T3 matrix first tick failed");
            require(h.stream_advance_time(start + 1500000), "T3 matrix quiet carry failed");
            require(h.stream_push_tick({start + 1500001, 2, 100, 1}), "T3 matrix later tick failed");
            require(h.stream_end(true), "T3 driver matrix end failed");
        } else {
            h.run(bars.data(), 4);
        }
        require(h.native_state().kind == NativeLifecycleKind::Completed, h.last_error().c_str());
        require(h.current, "T3 matrix CurrentExecution was absent");
        h.inspect_drivers(forward, confirmed_through, visits, refused);
        finish(h, h.trace);
        result.words.insert(result.words.end(), h.trace.words.begin(), h.trace.words.end());
    }
    for (std::size_t c = 0; c < visits.size(); ++c) {
        for (std::size_t kind = 0; kind < 2; ++kind) {
            require(visits[c][kind] > 0, "T3 did not visit every driver-class/trigger cell");
            const bool allowed = base_priced_permission(
                static_cast<no::DriverEligibilityClass>(c), kind == 0);
            require(allowed ? refused[c][kind] == 0 : refused[c][kind] == visits[c][kind],
                    "T3 driver permission guard witness was vacuous");
            result.add(140 + 2 * c + kind, visits[c][kind]);
            result.add(160 + 2 * c + kind, refused[c][kind]);
        }
    }
    return result;
}
} // namespace dormant_test

#ifndef PINEFORGE_DORMANT_BOOK_NO_MAIN
int main(int argc, char** argv) {
    try {
        using namespace dormant_test;
        if (argc == 2 && std::strcmp(argv[1], "--notification-middle-spill") == 0) {
            for (const bool prepared : {false, true}) {
                const auto off = run_middle_spill(prepared, false, true);
                const auto on = run_middle_spill(prepared, true, true);
                require(off.words == on.words,
                        "T3 notification middle-spill eager/dormant transcript bytes differ");
                on.print(prepared ? "notification-middle-prepared" : "notification-middle-direct");
            }
            return 0;
        }
        require(argc == 1, "T3 unknown transcript row selector");
        run_fast(0x12345678, false).print("fast-a");
        run_fast(0x9abcdef0, false).print("fast-b");
        run_fast(0x12345678, false, true, false, true).print("spilled-grid");
        run_fast(0x12345678, false, true, true).print("partial-failure");
        for (const std::uint64_t seed : {7, 18, 43, 70}) {
            run_mixed(seed, false, false).print("mixed-raw");
            run_mixed(seed, true, false).print("mixed-grid");
        }
        run_carried(false).print("carried");
        run_active(false).print("active-spill");
        run_overflow(false).print("mid-point-active-spill");
        run_ring_window(false).print("ring-keepalive");
        run_middle_spill(false).print("middle-spill");
        run_driver_matrix(false).print("driver-matrix");
        run_complete(false).print("complete-before-interior");
        run_trail(false).print("active-trail");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "test_native_dormant_book_transcript: %s\n", e.what());
        return 1;
    }
}
#endif
