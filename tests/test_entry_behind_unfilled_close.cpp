#include <pineforge/pineforge.h>
#include <pineforge/source/pine_adapter.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
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

std::vector<Bar> prices() {
    std::vector<Bar> feed;
    for (int i = 0; i < hours * per_hour; ++i) {
        const double p = 100 + i / per_hour;
        feed.push_back({p, p, p, p, 1, start + i * (hour / per_hour)});
    }
    return feed;
}

int pending(const source::PendingIntentView& view, const char* id) {
    int count = 0;
    for (int i = 0; i < view.size(); ++i) {
        pf_pending_order_v1_t row{};
        if (view.copy_v1(i, &row) != 0) throw std::runtime_error("unreadable pending order");
        count += std::string(row.id) == id;
    }
    return count;
}

no::RequestHandle waiting_parent(const source::PineExecutionAdapter& adapter, const char* id) {
    no::RequestHandle parent{};
    for (const auto& row : adapter.fixture_pending_snapshots()) {
        if (!row.staged || row.snapshot.source_id != id) continue;
        if (parent.incarnation || !row.snapshot.paired_reversal_parent.incarnation)
            throw std::runtime_error("expected one entry waiting for an identified close");
        parent = row.snapshot.paired_reversal_parent;
    }
    if (!parent.incarnation || pending(adapter.pending_intent_view(), id) != 1)
        throw std::runtime_error("waiting entry missing from the public book");
    return parent;
}

// The native flatten is first in the next open's queue. The Pine close
// loses its position there, after the placing bar has finished its flush.
class PublishedBook final : public source::PineStrategyHost {
public:
    explicit PublishedBook(bool suppress) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(1);
        set_syminfo_metadata("qty_step", 0.25);
        set_probe_suppress_tail_logic(suppress);
        fixture_retain_all_events();
    }
    void on_source_bar(const Bar& b) override {
        const int i = pine_bar_index();
        if (i != bodies++ || b.timestamp != start + i * hour)
            throw std::runtime_error("unexpected script bar");
        if (i == 0) strategy_entry("seed", false);
        if (i == hours - 2) {
            const auto lots = native_open_lots(103);
            if (lots.size() != 1 || lots[0].entry_label != "seed" || lots[0].signed_units != -1
                || lots[0].entry_price != 101 || lots[0].entry_bar_index != 1)
                throw std::runtime_error("missing one-unit short");
            const auto placed = submit({no::Flatten{}, "flatten", ""});
            if (!placed.handle) throw std::runtime_error("flatten was not accepted");
            flatten = *placed.handle;
            strategy_close("seed");
            strategy_entry("next", true);
            parent = waiting_parent(adapter_, "next");
        }
        if (i == hours - 1) body_read = pending(pending_intent_view(), "next");
    }
    int bodies = 0, body_read = -1;
    no::RequestHandle parent{}, flatten{};
};

bool publication(bool suppress, const char* name) {
    const auto feed = prices();
    PublishedBook host(suppress);
    host.run(feed.data(), static_cast<int>(feed.size()), "10", "60", true);
    int close_ends = 0, close_fills = 0, flatten_fills = 0, child_fills = 0;
    std::uint64_t flatten_ordinal = 0, end_ordinal = 0;
    bool accepted = false;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* e = std::get_if<no::AcceptedEvent>(&*row.command))
            accepted |= e->handle() == host.parent && e->request().label == "__close__seed";
        if (const auto* e = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
            close_fills += e->handle() == host.parent;
            child_fills += e->request().label == "next";
            if (e->handle() == host.flatten && e->closed_units == 1
                && e->interval_index() == hours - 1 && e->resolved_price == 104) {
                ++flatten_fills;
                flatten_ordinal = e->ordinal;
            }
        }
        if (const auto* e = std::get_if<no::NoEffectEvent>(&*row.command); e && e->handle() == host.parent) {
            ++close_ends;
            end_ordinal = e->ordinal;
        }
        if (const auto* e = std::get_if<no::CancelledEvent>(&*row.command); e && e->handle() == host.parent) {
            ++close_ends;
            end_ordinal = e->ordinal;
        }
    }
    if (host.last_run_status() != 0 || !host.last_error().empty() || !accepted || close_ends != 1
        || close_fills || flatten_fills != 1 || end_ordinal <= flatten_ordinal || host.bodies != hours - int(suppress)) {
        std::cerr << "NOT COMPARED " << name << ": bodies=" << host.bodies << " close ends=" << close_ends
                  << " close fills=" << close_fills << " flatten fills=" << flatten_fills
                  << " error=" << host.last_error() << '\n';
        return false;
    }
    const int read = suppress ? pending(host.pending_intent_view(), "next") : host.body_read;
    if (read != 0 || child_fills != 0) {
        std::cerr << "FAIL " << name << ": expected pending=0 fills=0, got pending=" << read
                  << " fills=" << child_fills << '\n';
        return false;
    }
    std::cout << "PASS " << name << '\n';
    return true;
}

// A host lets the test set a loss limit at precommit. The normal Pine risk
// latches gate openings; a reducing close does not ask them for admission.
class Host : public NativeStrategyHost {
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

enum class Change { Script, BeforeFlush, AfterFlush, CancelSuccessor, KeepHandle };
class ReplacedClose final : public Host {
public:
    explicit ReplacedClose(Change change) : Host(End::Fill), change(change) {}
    void on_native_bar(const Bar&, const NativeDecisionContext& c) override {
        const int i = c.coordinate.interval_index;
        clock_ok &= i == callbacks++ && c.script_bar_open_ms == start + i * hour;
        adapter.observe_terminal_receipts();
        adapter.begin_source_evaluation();
        if (i == 0) adapter.entry("seed", false);
        if (i == 2) {
            if (physical_position().signed_units != -1) throw std::runtime_error("missing seed short");
            const double na = std::numeric_limits<double>::quiet_NaN();
            // The explicit quantity lets a script reissue its close at the
            // same call site without the repeated default-close suppression.
            const double qty = change == Change::Script ? 1.0 : na;
            adapter.close("seed", "", qty, na, false, 17);
            adapter.entry("next", true);
            parent = waiting_parent(adapter, "next");
            if (change == Change::AfterFlush || change == Change::CancelSuccessor)
                adapter.flush_pending_entries();
            if (change == Change::Script) {
                adapter.close("seed", "resized", qty, na, false, 17);
                for (const auto& w : native_working_requests())
                    if (w.definition->request.comment == "resized") successor = w.definition->handle;
            } else {
                const auto w = native_working_request(parent);
                if (!w) throw std::runtime_error("original close is not working");
                auto request = w->definition->request;
                request.comment = "resized";
                no::ReplaceOptions options;
                options.keep_handle = change == Change::KeepHandle;
                const auto result = replace(parent, request, options);
                if (!result.successor || result.status != no::ReplaceStatus::Replaced)
                    throw std::runtime_error("native close replacement was not accepted");
                successor = *result.successor;
                if (change == Change::CancelSuccessor && cancel(successor).status != no::CancelStatus::Cancelled)
                    throw std::runtime_error("successor cancellation was not accepted");
            }
            if (!successor.incarnation || (successor == parent) != (change == Change::KeepHandle))
                throw std::runtime_error("unexpected replacement identity");
            if (change == Change::AfterFlush || change == Change::CancelSuccessor) return;
        }
        if (i == 3) {
            read_after = pending(adapter.pending_intent_view(), "next");
            close_after = pending(adapter.pending_intent_view(), "__close__seed");
        }
        adapter.flush_pending_entries();
        if (i == 2) read_at_flush = pending(adapter.pending_intent_view(), "next");
    }
    const Change change;
    no::RequestHandle successor{};
    int read_at_flush = -1, read_after = -1, close_after = -1;
};

bool replacement(Change change, const char* name) {
    const auto feed = prices();
    ReplacedClose host(change);
    host.run(feed.data(), static_cast<int>(feed.size()), "10", "60", true);
    int replacements = 0, successor_fills = 0, successor_cancels = 0, children = 0, opened = 0;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* e = std::get_if<no::ReplacedEvent>(&*row.command))
            replacements += e->predecessor() == host.parent && e->successor() == host.successor
                && e->kept_handle() == (change == Change::KeepHandle);
        if (const auto* e = std::get_if<no::CancelledEvent>(&*row.command))
            successor_cancels += e->handle() == host.successor && e->reason == no::CancelReason::User;
        if (const auto* e = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
            successor_fills += e->handle() == host.successor && e->closed_units == 1
                && e->interval_index() == 3 && e->resolved_price == 103;
            if (e->request().label == "next") {
                ++children;
                opened += e->opened_units == 1 && e->interval_index() == 3 && e->resolved_price == 103;
            }
        }
    }
    const bool kept = change == Change::KeepHandle, cancelled = change == Change::CancelSuccessor;
    if (!host.last_error().empty() || host.native_state().kind != NativeLifecycleKind::Completed
        || !host.clock_ok || host.callbacks != hours || host.sub_bars != hours * per_hour
        || replacements != 1 || successor_fills != int(!cancelled) || successor_cancels != int(cancelled)) {
        std::cerr << "NOT COMPARED " << name << ": replacements=" << replacements << " successor fills="
                  << successor_fills << " cancellations=" << successor_cancels << " error=" << host.last_error() << '\n';
        return false;
    }
    const bool read_at_flush = change != Change::AfterFlush && !cancelled;
    if ((read_at_flush && host.read_at_flush != int(kept)) || host.read_after != 0 || host.close_after != 0
        || pending(host.adapter.pending_intent_view(), "next") != 0 || children != int(kept)
        || opened != int(kept) || host.physical_position().signed_units != (cancelled ? -1 : int(kept))) {
        std::cerr << "FAIL " << name << ": pending after flush=" << host.read_at_flush
                  << " at next read=" << host.read_after << " old closes=" << host.close_after
                  << " child fills=" << children << '\n';
        return false;
    }
    std::cout << "PASS " << name << '\n';
    return true;
}

bool check(End end, const char* name) {
    const auto feed = prices();
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
    for (const auto& [change, name] : {std::pair{Change::Script, "script replacement"},
                                      std::pair{Change::BeforeFlush, "native replacement before flush"},
                                      std::pair{Change::AfterFlush, "native replacement after flush"},
                                      std::pair{Change::CancelSuccessor, "cancelled successor"},
                                      std::pair{Change::KeepHandle, "retained close handle"}}) {
        try { ok &= replacement(change, name); }
        catch (const std::exception& e) {
            std::cerr << "NOT COMPARED " << name << ": " << e.what() << '\n';
            ok = false;
        }
    }
    for (bool suppress : {false, true}) {
        const char* name = suppress ? "suppressed final bar" : "next script read";
        try { ok &= publication(suppress, name); }
        catch (const std::exception& e) {
            std::cerr << "NOT COMPARED " << name << ": " << e.what() << '\n';
            ok = false;
        }
    }
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
