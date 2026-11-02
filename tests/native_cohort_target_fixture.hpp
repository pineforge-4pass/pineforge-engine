#pragma once

// Counted rows run only when the observer macro is defined for both the
// library objects and tests in a proof build. In the default build these
// targets compare the indexed answer with the complete reader, and the cost
// target's spilled successor row guards against bypass.

#include "../src/native_execution_consumer.hpp"
#include "../src/native_cohort_target_work.hpp"
#include "native_order_full_fold.hpp"

#include <pineforge/native_host.hpp>
#include <pineforge/pineforge.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {
// Only observation and fault injection use friendship. All admitted orders,
// replacements, fills and closes enter through NativeStrategyHost.
struct NativeExecutionConsumerProbe {
    static native_order::TargetObservation read(
            NativeExecutionConsumer& c, const BacktestEngine& engine,
            const native_order::LiveRequest& live) {
        native_order::TargetObservation out;
        std::vector<native_order::RequestHandle> handles;
        c.read_target_into(engine, &live, out, handles);
        return out;
    }
    static void cold(NativeExecutionConsumer& c) { c.clear_cohort_target_cache(); }
    static native_order::TargetObservation cached(
            NativeExecutionConsumer& c, const BacktestEngine& engine,
            const native_order::LiveRequest& live) {
        const auto* target = c.cached_cohort_target(engine, live);
        if (!target) throw std::runtime_error("ordinary target did not enter the small target cache");
        return *target;
    }
    static void budget(NativeExecutionConsumer& c, std::size_t bytes) {
#ifdef PINEFORGE_HAS_COHORT_OPENING_INDEX
        c.cohort_opening_index_bytes_ = bytes;
#else
        // The base has only the complete reader. It has no optional index
        // whose budget can be selected; the same value tests still run.
        (void)bytes;
#endif
        cold(c);
    }
    static bool valid(const NativeExecutionConsumer& c) {
#ifdef PINEFORGE_HAS_COHORT_OPENING_INDEX
        return c.cohort_opening_index_current();
#else
        (void)c;
        return false;
#endif
    }
    static std::size_t capacity_bytes(const NativeExecutionConsumer& c) {
#ifdef PINEFORGE_HAS_COHORT_OPENING_INDEX
        return c.cohort_opening_index_.capacity()
            * sizeof(NativeExecutionConsumer::CohortOpeningIndexRow);
#else
        (void)c;
        return 0;
#endif
    }
    // Invalidate the real prepared token, or remove the real executing row,
    // AFTER its preview. The ordinary settlement and install then decide the
    // actual typed failure; no result or status is stubbed.
    static void interrupt_install(NativeExecutionConsumer& c,
                                  const native_order::RequestHandle& target,
                                  bool direct) {
        if (direct) {
            const auto cancelled = c.requests_.apply_cancel(target, c.next_timeline_ordinal_);
            if (cancelled.result.status != native_order::CancelStatus::Cancelled)
                throw std::runtime_error("fault did not cancel the executing request");
        } else {
            (void)c.requests_.cohort_open();
        }
    }
    static std::uint64_t settlements(const NativeExecutionConsumer& c) {
        return c.settlement_generation_;
    }
};
}  // inline namespace engine_script_run_v19
}  // namespace pineforge

namespace cohort_target_test {
using namespace pineforge;
namespace no = pineforge::native_order;
using Probe = NativeExecutionConsumerProbe;
constexpr std::int64_t start = 1736121600000LL;
constexpr std::size_t payload_bytes = 64 * 1024; // Fixed policy, not a work budget.
inline void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
inline int failures = 0;
template<class F> void row(const char* name, F&& body) {
    try {
        body();
        std::printf("PASS %s\n", name);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", name, error.what());
    }
}

struct Host : NativeStrategyHost {
    std::function<void(Host&)> beginning, calculation, tick;
    std::function<void(Host&, const no::ExecutionAppliedEvent&)> notification;
    std::function<NativePrecommitVerdict(Host&, const NativePrecommitView&)> precommit;
    std::map<no::CohortHandle, double> cohort_close_units;
    std::size_t bars = 0, fills = 0;
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    void on_native_run_begin() override {
        bars = fills = 0;
        cohort_close_units.clear();
        if (beginning) beginning(*this);
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bars;
        if (calculation) calculation(*this);
    }
    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        ++fills;
        if (notification) notification(*this, event);
    }
    void on_native_tick(const Bar&, const NativeTickContext&) override {
        if (tick) tick(*this);
    }
    no::ExecutionTerms resolve_execution_terms(const NativeExecutionTermsFacts& facts) const override {
        auto terms = NativeStrategyHost::resolve_execution_terms(facts);
        if (const auto* cohort = std::get_if<no::BindCohort>(&facts.definition->request.owner)) {
            // Cohort closes are host-sized. The requested quantity is fixture
            // input; the engine supplies the currently selected exposure.
            terms.units = std::min(cohort_close_units.at(cohort->cohort),
                                   facts.scope_exposure_units);
        }
        return terms;
    }
    NativePrecommitVerdict validate_execution_precommit(const NativePrecommitView& view) const override {
        return precommit ? precommit(const_cast<Host&>(*this), view) : NativePrecommitVerdict::Admit;
    }
    // A fault in accounting between a real preview and real commit. The
    // financial implementation must return InvalidAccounting itself.
    void invalidate_accounting() { net_profit_sum_ = std::numeric_limits<double>::infinity(); }
    // A real protected-host write seam: the source host retires dust at its
    // applied notification in the same way. Keep all surviving book scalars
    // consistent, so only the derived opening view can be stale.
    void retire_lots_at_most(double units) {
        pyramid_entries_.erase(std::remove_if(pyramid_entries_.begin(), pyramid_entries_.end(),
            [units](const PyramidEntry& lot) { return lot.qty <= units; }), pyramid_entries_.end());
        if (pyramid_entries_.empty()) {
            reset_position_state_to_flat();
            return;
        }
        double total = 0, weighted = 0;
        for (const auto& lot : pyramid_entries_) { total += lot.qty; weighted += lot.qty * lot.price; }
        position_qty_ = total;
        position_entry_price_ = weighted / total;
        position_entry_count_ = static_cast<int>(pyramid_entries_.size());
    }
    const TradeLedger& trades() const { return trades_; }
};

inline NativeRunSpec spec(std::uint64_t run = 1) {
    NativeRunSpec s;
    s.identity = {"cohort-target", run};
    s.input_tf = s.script_tf = "1";
    s.tickerid = "TEST:COHORT";
    s.timezone = "UTC"; s.session = "24x7";
    s.initial_capital = 1.0e9;
    s.point_value = s.account_fx = 1;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerUnit;
    s.fee_value = 0.25;
    s.event_retention = NativeEventRetention::Full;
    s.report_policy = NativeReportPolicy::KernelRecorded;
    return s;
}
inline std::vector<Bar> tape(std::size_t n, double price = 100) {
    std::vector<Bar> bars(n);
    for (std::size_t i = 0; i < n; ++i)
        bars[i] = {price, price, price, price, 1, start + std::int64_t(i) * 60000};
    return bars;
}
inline void configure(Host& h, const NativeRunSpec& s) {
    require(h.configure_native(s).status == NativeSetupStatus::Applied, "configuration refused");
}
inline void run(Host& h, const NativeRunSpec& s, const std::vector<Bar>& bars) {
    configure(h, s);
    h.run(bars.data(), static_cast<int>(bars.size()));
    require(h.native_state().kind == NativeLifecycleKind::Completed, h.last_error().c_str());
    require(h.bars == bars.size(), "public run did not consume the declared tape");
}
inline no::RequestHandle put(Host& h, const no::Request& request) {
    const auto result = h.submit(request);
    if (result.status != no::SubmitStatus::Accepted || !result.handle)
        throw std::runtime_error("request rejected: " + request.label + " reason="
            + (result.reason ? std::to_string(static_cast<unsigned>(*result.reason)) : "absent"));
    return *result.handle;
}
inline no::ExecutionAppliedEvent apply(Host& h, const no::RequestHandle& handle) {
    auto result = h.execute_current({handle, NativeCurrentPriceRule::AsPresented});
    require(std::holds_alternative<no::ExecutionAppliedEvent>(result),
            "real current execution did not apply");
    return std::get<no::ExecutionAppliedEvent>(std::move(result));
}
inline no::LiveRequest live(Host& h, const no::RequestHandle& handle) {
    const auto* value = h.consumer().request_core().find_live(handle);
    require(value != nullptr, "fixture request is not working");
    return *value; // Own the definition; never hold a spilled-ring pointer.
}
inline no::Request close(Host& h, no::CohortHandle cohort, const std::string& label,
                         bool stop = false, double units = 1) {
    h.cohort_close_units[cohort] = units;
    no::Request r{no::HostSized{no::HostSizedKind::Close, std::nullopt}, label, ""};
    r.owner = no::BindCohort{cohort};
    if (stop) r.trigger = no::Stop{1};
    else r.trigger = no::Limit{1000};
    return r;
}
struct Population {
    std::vector<no::RequestHandle> openings, roots, closes;
    std::vector<no::CohortHandle> cohorts;
    std::size_t replacements = 0;
};
inline Population populate(Host& h, std::size_t n, bool successors, unsigned seed = 1) {
    Population p;
    for (std::size_t i = 0; i < n; ++i) {
        const std::string label = std::string(45, char('a' + (i + seed) % 26))
            + std::to_string(i * (2 * seed + 1));
        const no::Request entry{no::Transact{1}, label, "physical opening"};
        const auto root = put(h, entry);
        const auto cohort = h.cohort_open();
        h.cohort_add(cohort, root); // Enroll before fill, as an actual client must.
        auto opening = root;
        if (successors) {
            const auto result = h.replace(root, entry);
            require(result.status == no::ReplaceStatus::Replaced && result.successor,
                    "successor setup failed");
            opening = *result.successor;
            ++p.replacements;
            require(live(h, opening).definition->root == std::optional<no::RequestHandle>{root},
                    "replacement does not name its original root");
        }
        p.roots.push_back(root); p.openings.push_back(opening); p.cohorts.push_back(cohort);
        p.closes.push_back(put(h, close(h, cohort, label + "-take")));
        p.closes.push_back(put(h, close(h, cohort, label + "-stop", true)));
        require(apply(h, opening).opened_units == 1, "opening did not produce one real lot");
    }
    return p;
}

// Frozen base cohort branch + read_openings_into + refresh_openings. The
// independent input is the PUBLIC lot readback, not the new derived index.
inline no::TargetObservation reference(Host& h, const no::LiveRequest& request) {
    no::TargetObservation out;
    const auto lots = h.native_open_lots(100);
    if (!lots.empty()) {
        out.current_position = no::PositionNonflat{
            lots.front().cycle, lots.front().signed_units > 0 ? no::Side::Long : no::Side::Short};
    }
    const auto* cohort = std::get_if<no::CohortClose>(&request.authority);
    require(cohort != nullptr, "oracle was given a non-cohort request");
    const auto* position = std::get_if<no::PositionNonflat>(&out.current_position);
    if (!position) return out;
    std::vector<no::RequestHandle> handles;
    for (const auto& lot : lots) {
        no::RequestHandle handle{h.consumer().request_core().identity(), lot.entry_incarnation};
        if (h.consumer().request_core().cohort_contains(cohort->cohort, handle))
            handles.push_back(std::move(handle));
    }
    for (const auto& handle : handles) {
        no::OpeningObservation value;
        value.queried_opening = handle;
        value.queried_cycle = position->cycle;
        out.openings.push_back(std::move(value));
    }
    const auto by_incarnation = [](const auto& a, const auto& b) {
        return a.queried_opening.incarnation < b.queried_opening.incarnation;
    };
    if (out.openings.size() <= 32) {
        for (std::size_t i = 1; i < out.openings.size(); ++i) {
            if (!by_incarnation(out.openings[i], out.openings[i - 1])) continue;
            auto value = std::move(out.openings[i]);
            std::size_t j = i;
            for (; j > 0 && by_incarnation(value, out.openings[j - 1]); --j)
                out.openings[j] = std::move(out.openings[j - 1]);
            out.openings[j] = std::move(value);
        }
    } else {
        std::stable_sort(out.openings.begin(), out.openings.end(), by_incarnation);
    }
    for (auto& value : out.openings) {
        value.current_position = out.current_position;
        value.has_live_matching_lot = false;
    }
    for (const auto& lot : lots) {
        auto it = std::lower_bound(out.openings.begin(), out.openings.end(), lot.entry_incarnation,
            [](const auto& value, std::uint64_t incarnation) {
                return value.queried_opening.incarnation < incarnation;
            });
        for (; it != out.openings.end()
               && it->queried_opening.incarnation == lot.entry_incarnation; ++it) {
            if (it->queried_opening.run == h.consumer().request_core().identity()
                && it->queried_cycle == position->cycle)
                it->has_live_matching_lot = true;
        }
    }
    out.openings.erase(std::unique(out.openings.begin(), out.openings.end(),
        [](const auto& a, const auto& b) { return a.queried_opening == b.queried_opening; }),
        out.openings.end());
    return out;
}
inline bool same_position(const no::PositionIdentity& a, const no::PositionIdentity& b) {
    if (a.index() != b.index()) return false;
    const auto* x = std::get_if<no::PositionNonflat>(&a);
    const auto* y = std::get_if<no::PositionNonflat>(&b);
    return !x || (x->cycle == y->cycle && x->side == y->side);
}
inline bool same_opening(const no::OpeningObservation& a, const no::OpeningObservation& b) {
    return a.queried_opening == b.queried_opening && a.queried_cycle == b.queried_cycle
        && same_position(a.current_position, b.current_position)
        && a.has_live_matching_lot == b.has_live_matching_lot;
}
inline bool same(const no::TargetObservation& a, const no::TargetObservation& b) {
    if (!same_position(a.current_position, b.current_position)
        || a.opening.has_value() != b.opening.has_value()
        || a.openings.size() != b.openings.size() || a.external_openings != b.external_openings)
        return false;
    if (a.opening && !same_opening(*a.opening, *b.opening)) return false;
    for (std::size_t i = 0; i < a.openings.size(); ++i)
        if (!same_opening(a.openings[i], b.openings[i])) return false;
    return true;
}
inline void judge(Host& h, const no::LiveRequest& request) {
    require(same(Probe::read(h.consumer(), h, request), reference(h, request)),
            "cohort target differs from the frozen complete reader");
}

#ifdef PINEFORGE_COHORT_TARGET_OBSERVER
namespace work = no::cohort_target_work;
struct Counts {
    std::array<std::uint64_t, std::size_t(work::Event::Count)> values{};
    static void charge(void* context, work::Event event) noexcept {
        ++static_cast<Counts*>(context)->values[std::size_t(event)];
    }
    std::uint64_t get(work::Event event) const { return values[std::size_t(event)]; }
};
inline void print(const Counts& c, std::size_t lots, std::size_t targets, const char* window) {
    std::printf("window=%s L=%zu targets=%zu canonical=%llu contains=%llu live=%llu"
                " issued=%llu successor=%llu target_lots=%llu refresh_lots=%llu disk_roots=%llu\n",
        window, lots, targets,
        static_cast<unsigned long long>(c.get(work::Event::Canonical)),
        static_cast<unsigned long long>(c.get(work::Event::Contains)),
        static_cast<unsigned long long>(c.get(work::Event::FindLive)),
        static_cast<unsigned long long>(c.get(work::Event::Issued)),
        static_cast<unsigned long long>(c.get(work::Event::Successor)),
        static_cast<unsigned long long>(c.get(work::Event::TargetLot)),
        static_cast<unsigned long long>(c.get(work::Event::RefreshLot)),
        static_cast<unsigned long long>(c.get(work::Event::RootDiskRead)));
    std::printf("target_calls=%llu refresh_calls=%llu resident_steps=%llu issued_comparisons=%llu"
                " root_tail_comparisons=%llu roster_comparisons=%llu\n",
        static_cast<unsigned long long>(c.get(work::Event::Target)),
        static_cast<unsigned long long>(c.get(work::Event::Refresh)),
        static_cast<unsigned long long>(c.get(work::Event::ResidentStep)),
        static_cast<unsigned long long>(c.get(work::Event::IssuedComparison)),
        static_cast<unsigned long long>(c.get(work::Event::RootTailComparison)),
        static_cast<unsigned long long>(c.get(work::Event::HandleCompare)));
}
#endif

inline std::vector<std::uint64_t> event_values(Host& h) {
    std::vector<std::uint64_t> out;
    for (const auto& event : h.consumer().request_core().history()) {
        full_fold::Fold f; full_fold::fold(f, event); out.push_back(f.h);
    }
    return out;
}
inline std::vector<std::vector<std::uint64_t>> event_words(Host& h) {
    std::vector<std::vector<std::uint64_t>> out;
    for (const auto& event : h.consumer().request_core().history()) {
        out.emplace_back();
        full_fold::WordTrace trace(out.back());
        full_fold::Fold f; full_fold::fold(f, event);
    }
    return out;
}
inline std::vector<std::vector<std::uint64_t>> trade_words(Host& h) {
    std::vector<std::vector<std::uint64_t>> out;
    for (std::size_t i = h.trades().base(); i < h.trades().size(); ++i) {
        const auto& t = h.trades()[i];
        out.emplace_back();
        full_fold::WordTrace trace(out.back());
        full_fold::Fold f;
        f.i(t.entry_time); f.i(t.exit_time);
        f.d(t.entry_price); f.d(t.exit_price); f.d(t.qty); f.d(t.pnl); f.d(t.pnl_pct);
        f.b(t.is_long); f.i(t.entry_bar_index); f.i(t.exit_bar_index);
        f.s(t.entry_id); f.s(t.entry_comment); f.s(t.exit_comment); f.s(t.exit_id);
        f.b(t.exit_from_bracket); f.d(t.max_runup); f.d(t.max_drawdown); f.d(t.commission);
        f.u(t.entry_incarnation); f.b(t.open_at_end); f.e(t.close_cause);
    }
    return out;
}
}  // namespace cohort_target_test
