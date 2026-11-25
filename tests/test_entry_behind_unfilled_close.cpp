#include <pineforge/pineforge.h>
#include <pineforge/source/pine_adapter.hpp>

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace no = native_order;
enum class End { Cancel, Risk, Flat, Fill, CurrentFill };
constexpr std::int64_t start = 1'736'683'200'000LL, hour = 3'600'000;
constexpr int hours = 5, per_hour = 6;

// A host lets the test set a loss limit at precommit. The normal Pine risk
// latches gate openings; a reducing close does not ask them for admission.
class Host final : public NativeStrategyHost {
public:
    explicit Host(End end) : end(end), adapter(*this) {}
    End end;
    source::PineExecutionAdapter adapter;
    no::RequestHandle parent{};
    int held = 0, callbacks = 0, sub_bars = 0;
    bool seed = false, clock_ok = true, visible = false, newborn = false;
    mutable bool loss_refused = false;

    void prepare_native_begin(const NativeBeginArgs& args) override {
        source::StagedConfiguration staged;
        staged.syminfo.timezone = "UTC";
        staged.syminfo.session = "24x7";
        staged.syminfo.mintick = 1;
        adapter.reset_for_run();
        adapter.set_configuration({});
        adapter.set_staged_configuration(staged);
        adapter.set_begin_mode(false, args.bar_magnifier);
        auto spec = adapter.project({}, staged, args);
        spec.event_retention = NativeEventRetention::Full;
        if (configure_native(spec).status != NativeSetupStatus::Applied)
            throw std::runtime_error("could not configure lower bars");
    }
    no::ExecutionTerms resolve_execution_terms(const NativeExecutionTermsFacts& f) const override {
        return adapter.resolve_terms(f);
    }
    NativePrecommitVerdict validate_execution_precommit(const NativePrecommitView& v) const override {
        // The short sells at 101; covering at 103 would realize a loss of 2.
        if (end == End::Risk && v.target == parent && v.closed_row_pnl.size() == 1
            && v.closed_row_pnl.front() < -1) {
            loss_refused = true;
            return NativePrecommitVerdict::Refuse;
        }
        return adapter.validate_precommit(v);
    }
    void on_native_applied(const no::ExecutionAppliedEvent& e,
                           const NativeDecisionContext& c) override { adapter.on_applied(e, c); }
    void on_native_bar_open(const Bar& b, const NativeDecisionContext& c) override { adapter.on_bar_open(b, c); }
    void on_native_sub_bar(const Bar&, const NativeDecisionContext&) override { ++sub_bars; }
    void on_native_bar(const Bar&, const NativeDecisionContext& c) override {
        const int i = c.coordinate.interval_index;
        clock_ok &= i == callbacks++ && c.script_bar_open_ms == start + i * hour;
        adapter.observe_terminal_receipts();
        adapter.begin_source_evaluation();
        if (i == 0) adapter.entry("seed", false);
        if (i == (end == End::Risk || end == End::Fill ? 2 : hours - 1)) {
            const auto lots = native_open_lots(101);
            seed = lots.size() == 1 && lots.front().entry_label == "seed"
                && lots.front().signed_units == -1 && lots.front().entry_price == 101;
            adapter.close("seed");
            adapter.entry("next", true);
            for (const auto& p : adapter.fixture_pending_snapshots()) {
                if (p.staged && p.snapshot.source_id == "next") {
                    parent = p.snapshot.paired_reversal_parent;
                    ++held;
                }
            }
            const auto& view = adapter.pending_intent_view();
            for (int j = 0; j < view.size(); ++j) {
                pf_pending_order_v1_t p{};
                if (view.copy_v1(j, &p) == 0 && std::string(p.id) == "next") visible = true;
            }
            if (end == End::Cancel) adapter.cancel("seed");
            if (end == End::Flat) {
                // Flatten now, then evaluate the already accepted close at
                // the same print, before the flatten's notification is drained.
                const auto first = submit({no::Flatten{}, "flatten", ""});
                if (first.handle) (void)execute_current({*first.handle});
            }
            if ((end == End::Flat || end == End::CurrentFill) && parent.incarnation) {
                const auto outcome = execute_current({parent});
                newborn = end == End::Flat ? std::holds_alternative<no::NoEffectEvent>(outcome)
                                           : std::holds_alternative<no::ExecutionAppliedEvent>(outcome);
            }
        }
        adapter.flush_pending_entries();
    }
};

bool check(End end, const char* name) {
    std::vector<Bar> feed;
    for (int i = 0; i < hours * per_hour; ++i) {
        const double p = 100 + i / per_hour;
        feed.push_back({p, p, p, p, 1, start + i * (hour / per_hour)});
    }
    Host host(end);
    host.run(feed.data(), static_cast<int>(feed.size()), "10", "60", true);
    bool accepted = false, terminal = false, flattened = false;
    int parent_fills = 0, entries = 0;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        const auto& e = *row.command;
        if (const auto* a = std::get_if<no::AcceptedEvent>(&e))
            accepted |= a->handle() == host.parent && a->request().label == "__close__seed";
        if (const auto* a = std::get_if<no::ExecutionAppliedEvent>(&e)) {
            if (a->handle() == host.parent) ++parent_fills;
            if (a->request().label == "next" && a->opened_units == 1) ++entries;
            if (a->request().label == "flatten" && a->closed_units == 1) flattened = true;
        }
        if (const auto* a = std::get_if<no::CancelledEvent>(&e))
            terminal |= end == End::Cancel && a->handle() == host.parent
                && a->reason == no::CancelReason::User;
        if (const auto* a = std::get_if<no::MatchRejectedEvent>(&e))
            terminal |= end == End::Risk && host.loss_refused && a->handle() == host.parent
                && a->reason == no::MatchRejectReason::HostPrecommit;
        if (const auto* a = std::get_if<no::NoEffectEvent>(&e))
            terminal |= end == End::Flat && flattened && a->handle() == host.parent
                && std::holds_alternative<no::UnboundBookClose>(a->authority);
    }
    const bool fills = end == End::Fill || end == End::CurrentFill;
    if (!host.last_error().empty() || host.native_state().kind != NativeLifecycleKind::Completed
        || !host.clock_ok || host.callbacks != hours || host.sub_bars != hours * per_hour
        || !host.seed || host.held != 1 || !host.visible || !host.parent.incarnation || !accepted
        || ((end == End::CurrentFill || end == End::Flat) && !host.newborn)
        || (fills ? parent_fills != 1 : (!terminal || parent_fills != 0))) {
        std::cerr << "NOT COMPARED " << name << ": seed=" << host.seed << " held=" << host.held
                  << " visible=" << host.visible << " parent=" << host.parent.incarnation << " terminal=" << terminal
                  << " fills=" << parent_fills << " error=" << host.last_error() << '\n';
        return false;
    }
    const auto& view = host.adapter.pending_intent_view();
    for (int i = 0; i < view.size(); ++i) {
        pf_pending_order_v1_t p{};
        if (view.copy_v1(i, &p) != 0) {
            std::cerr << "NOT COMPARED " << name << ": unreadable pending row\n";
            return false;
        }
        if (std::string(p.id) == "next") {
            std::cerr << "FAIL " << name << ": next still pending at finish\n";
            return false;
        }
    }
    if (fills && (entries != 1 || host.physical_position().signed_units != 1)) {
        std::cerr << "FAIL " << name << ": filled close did not release its entry\n";
        return false;
    }
    std::cout << "PASS " << name << '\n';
    return true;
}
} // namespace

int main() {
    bool ok = true;
    for (const auto& [end, name] : {std::pair{End::Cancel, "script-cancel"},
                                   std::pair{End::Risk, "host-loss-limit"},
                                   std::pair{End::Flat, "flat-book"},
                                   std::pair{End::Fill, "filled-parent"},
                                   std::pair{End::CurrentFill, "undelivered-fill"}}) {
        try { ok &= check(end, name); }
        catch (const std::exception& e) {
            std::cerr << "NOT COMPARED " << name << ": " << e.what() << '\n';
            ok = false;
        }
    }
    return ok ? 0 : 1;
}
