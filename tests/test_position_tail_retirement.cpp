#include "position_tail_retirement_fixture.hpp"

namespace o1w_tail {
// Same access technique as test_k1_origin_pin_bound. No source declarations
// or access specifiers are changed by the fixture.
template<class Tag, typename Tag::type Member>
struct Access {
    friend typename Tag::type member(Tag) { return Member; }
};
struct Rows {
    using type = ps::PlacementTable ps::PineExecutionAdapter::*;
    friend type member(Rows);
};
template struct Access<Rows, &ps::PineExecutionAdapter::placement_>;
struct Families {
    using type = std::unordered_map<std::uint64_t, ps::BracketRoster> ps::PineExecutionAdapter::*;
    friend type member(Families);
};
template struct Access<Families, &ps::PineExecutionAdapter::bracket_families_>;
struct Archive {
    using type = std::unordered_map<std::uint64_t, ps::BracketRoster::Settled>
        ps::PineExecutionAdapter::*;
    friend type member(Archive);
};
template struct Access<Archive, &ps::PineExecutionAdapter::settled_bracket_families_>;
struct Key {
    using type = std::uint64_t (ps::PineExecutionAdapter::*)(const ps::SourceId&,
                                                         const ps::SourceId&) const noexcept;
    friend type member(Key);
};
template struct Access<Key, &ps::PineExecutionAdapter::key_for>;
struct Hash {
    using type = void (ps::PineExecutionAdapter::*)(BrokerStateHashSink&) const;
    friend type member(Hash);
};
template struct Access<Hash, &ps::PineExecutionAdapter::hash_state>;

std::uint64_t adapter_hash(const ps::PineExecutionAdapter& adapter) {
    BrokerStateHashSink f;
    (adapter.*member(Hash{}))(f);
    return f.h;
}

void small_differential() {
    const auto bars = feed(40);
    for (const bool side : {true, false}) {
        ScaleOut kept(side), erased(side);
        Work old_work, new_work;
        {
            Retention reference(true);
            CountScope scope(old_work);
            kept.run(bars.data(), static_cast<int>(bars.size()));
        }
        {
            Retention reference(false);
            CountScope scope(new_work);
            erased.run(bars.data(), static_cast<int>(bars.size()));
        }
        check_work(old_work); check_work(new_work);
        equal_outcome(kept, erased);
        check(kept.scaled_while_open == erased.scaled_while_open
              && erased.scaled_while_open >= 40, "scale-out commands changed");
        check(new_work.placements < old_work.placements && new_work.peak_archived > 0,
              "small differential did not retire completed exits");
        // A second begin must discard the K3 memo and archived-family digest.
        const auto first = erased.broker_state_hash();
        {
            Retention reference(false);
            erased.run(bars.data(), static_cast<int>(bars.size()));
        }
        equal_outcome(kept, erased);
        check(first == erased.broker_state_hash(), "second run kept a retirement memo or archive");
    }
}

class BookHost : public ps::PineStrategyHost {
public:
    BookHost() {
        ps::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_value = 4.0;
        config.pyramiding = 2;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.01);
        set_broker_state_hash_recording(true);
    }

    std::vector<std::uint64_t> observations;
    std::size_t not_working = 0;
    void observe_book() {
        BrokerStateHashSink f;
        for (const auto& row : native_events(cursor_)) {
            cursor_ = std::max(cursor_, row.ordinal);
            if (!row.command) continue;
            f.u(row.ordinal); f.u(row.command->index());
            if (const auto* event = std::get_if<native_order::NotWorkingEvent>(&*row.command)) {
                f.u(event->target.incarnation);
                ++not_working;
            }
        }
        f.d(live_position_size()); f.d(position_avg_price()); f.d(current_equity());
        f.i(trade_count()); f.i(pending_order_count());
        for (int i = 0; i < pending_order_count(); ++i) {
            double stop = missing, limit = missing, trail = missing;
            f.i(pending_order_effective_levels(i, &stop, &limit, &trail));
            f.d(stop); f.d(limit); f.d(trail);
        }
        observations.push_back(f.h);
    }
protected:
    std::uint64_t cursor_ = 0;
};

// A real cancelled per-origin bracket remains a consumed-leg witness while
// its opening still holds units. A later dynamic limit of the SAME id has
// different stop bits, fills, and must remain as that earlier row's K3
// witness. A second, still later reissue must not replace the first witness.
class SameId final : public BookHost {
public:
    bool saw_bound_predecessor = false, saw_filled_witness = false, checked_hash = false;
    std::size_t cancelled_requests = 0;
    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        if (i == 0) strategy_entry("E", true, missing, missing, 8.0);
        if (i == 1)
            strategy_exit("X", "E", 150.0, 80.0, missing, missing, missing,
                          100.0, {}, 1.0);
        if (i == 2) {
            // Native cancellation retires the requests, without the source
            // strategy.cancel operation that withdraws every lifecycle leg.
            const auto working = native_working_requests();
            for (const auto& row : working)
                if (row.definition->request.label == "X") {
                    const auto result = cancel(row.definition->handle);
                    if (result.status == native_order::CancelStatus::Cancelled) ++cancelled_requests;
                }
        }
        if (i == 4)
            strategy_exit("X", "E", bar.close + 0.25, missing,
                          missing, missing, missing, 50.0);
        if (i == 6) inspect(true);
        if (i == 7)
            strategy_exit("X", "E", 150.0, 90.0, missing, missing, missing, 100.0);
        if (i == 8) inspect(false);
        observe_book();
    }
private:
    void inspect(bool hash_check) {
        auto& rows = adapter_.*member(Rows{});
        const ps::PlacementSnapshot* old = nullptr;
        ps::PlacementSnapshot* filled = nullptr;
        std::uint64_t oldest = 0, successor = 0;
        for (auto row : rows) {
            auto& q = row.second;
            if (q.family != ps::PineOrderFamily::ExitLimit || q.source_id != "X") continue;
            if (q.bracket_origin.incarnation != 0 && q.exit_levels.stop == 80.0) {
                old = &q; oldest = row.first;
            }
            if (q.filled_exit && q.bracket_origin.incarnation == 0 && std::isnan(q.exit_levels.stop)
                && q.projection_created_bar == 4) {
                filled = &q; successor = row.first;
            }
        }
        check(old && !old->legs.dormant(), "askable bound bracket was not retained");
        check(filled && successor > oldest
              && filled->placement_script_open_ms > old->placement_script_open_ms,
              "filled different-stop first supersessor was erased");
        check(filled->projection_predecessor == 0,
              "same-id witness used K2 instead of the different-stop K3 rule");
        saw_bound_predecessor = true;
        saw_filled_witness = true;
        if (hash_check) {
            const auto before = adapter_hash(adapter_);
            const auto saved = filled->comment;
            filled->comment += " hash mutation";
            check(adapter_hash(adapter_) != before, "mutable retained row used a stale hash");
            filled->comment = saved;
            check(adapter_hash(adapter_) == before, "restoring a retained row changed its hash");
            checked_hash = true;
        }
    }
};

// Close/reopen the position, then re-use the exit id: the archived family's
// prefix must be restored before append. Cancelling the bracket must still
// produce one non-working result per historical member, with the same
// ordinals as the every-row-retained reference.
class Recycle final : public BookHost {
public:
    bool saw_archive = false, saw_restored_prefix = false, cancelled_archive = false;
    std::size_t expected_cancel_results = 0;
    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        const auto key = (adapter_.*member(Key{}))("X", "E");
        auto& active = adapter_.*member(Families{});
        const auto& archived = adapter_.*member(Archive{});
        if (i == 0 || i == 10) strategy_entry("E", true, missing, missing, 8.0);
        if (i == 2 || i == 12) {
            strategy_exit("X", "E", bar.close + 0.25, missing,
                          missing, missing, missing, 50.0);
            const auto family = active.find(key);
            if (i == 12) {
                check(family != active.end() && family->second.settled_count() > 0,
                      "same-id append lost the settled prefix");
                saw_restored_prefix = true;
            }
        }
        if (i == 6) saw_archive = archived.find(key) != archived.end();
        if (i == 8) strategy_close_all();
        if (i == 18) {
            const auto cold = archived.find(key);
            const auto hot = active.find(key);
            expected_cancel_results = cold != archived.end() ? cold->second.count
                : hot != active.end() ? hot->second.size() : 0;
            check(expected_cancel_results > 0, "recycled bracket never settled a member");
            cancelled_archive = cold != archived.end();
            observe_book();
            const auto before = not_working;
            strategy_exit_cancel_bracket("X", "E");
            observe_book();
            check(not_working - before == expected_cancel_results,
                  "archived bracket cancellation lost a non-working outcome");
            check(archived.find(key) == archived.end() && active.find(key) == active.end(),
                  "bracket cancellation left a family summary");
        }
        if (i == 20) strategy_cancel_all();
        observe_book();
    }
};

// Exercise the actual declined-pair/margin path as well as the table witness:
// the pair parks X at 105, a partial X at 101 fills, and another declined
// pair parks that retired row. A later X at 131 supersedes it through K3,
// without a live replacement link. The remaining 79 short units reach margin.
class ReissueRevival final : public BookHost {
public:
    ReissueRevival() {
        ps::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.pyramiding = 0;
        configure_pine_strategy(config);
        set_margin_call_enabled(true);
    }
    bool saw_dormant = false, saw_filled_candidate = false, checked_supersession = false;
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 1) {
            strategy_entry("S", false, missing, missing, 80.0);
            // Start partial too: percentage reissues preserve an existing
            // family's reservation, so a full first exit would retain 80.
            strategy_exit("X", "S", missing, 105.0, missing, missing, missing, 1.25);
        }
        if (i == 2) {
            strategy_entry("L", true);
            strategy_close("S", "Reverse to Long");
        }
        if (i == 4) {
            for (const auto& row : adapter_.*member(Rows{}))
                if (row.second.family == ps::PineOrderFamily::ExitStop
                    && row.second.source_id == "X" && row.second.exit_levels.stop == 105.0)
                    saw_dormant = saw_dormant || row.second.legs.dormant();
            check(saw_dormant, "declined pair did not park the original stop");
            // 80 * 1.25% = one unit. This buy stop is below the next 102
            // open, so it fills there without flattening the position cycle.
            strategy_exit("X", "S", missing, 101.0, missing, missing, missing, 1.25);
            filled_candidate_ = stop_at(4, 101.0);
            check(filled_candidate_ != 0, "partial stop was not placed");
        }
        if (i == 5) {
            observe_filled_candidate();
            strategy_entry("L", true);
            strategy_close("S", "Reverse to Long");
        }
        if (i == 7) inspect_supersession();
        if ((i == 6 || i == 7) && live_position_size() < 0.0) {
            if (i == 6)
                check((adapter_.*member(Rows{})).at(filled_candidate_).legs.dormant(),
                      "second declined pair did not park the filled candidate");
            strategy_exit("X", "S", missing, 125.0 + i);
            if (i == 6) {
                successor_ = stop_at(6, 131.0);
                const auto& rows = adapter_.*member(Rows{});
                check(successor_ > filled_candidate_
                      && rows.at(successor_).projection_predecessor == 0,
                      "filled stop reissue took the live-replacement K2 path");
            }
        }
        if (i == 8) {
            for (const auto& row : adapter_.*member(Rows{}))
                if (row.second.family == ps::PineOrderFamily::ExitStop
                    && row.second.source_id == "X"
                    && (row.second.exit_levels.stop == 105.0
                        || row.first == filled_candidate_))
                    check(!row.second.restored_after_margin,
                          "different-stop same-id reissue failed to supersede the old stop");
        }
        observe_book();
    }
private:
    std::uint64_t filled_candidate_ = 0, successor_ = 0;

    std::uint64_t stop_at(int bar, double price) const {
        std::uint64_t found = 0;
        for (const auto& row : adapter_.*member(Rows{})) {
            const auto& q = row.second;
            if (q.family != ps::PineOrderFamily::ExitStop || q.source_id != "X"
                || q.from_entry != "S" || q.projection_created_bar != bar
                || q.exit_levels.stop != price) continue;
            check(found == 0, "partial-stop scenario placed ambiguous handles");
            found = row.first;
        }
        return found;
    }

    void observe_filled_candidate() {
        bool filled = false;
        for (const auto& row : native_events(cursor_)) {
            if (!row.command) continue;
            const auto* event = std::get_if<native_order::ExecutionAppliedEvent>(&*row.command);
            if (event && event->handle().incarnation == filled_candidate_)
                filled = event->terminal && event->closed_units == 1.0
                    && event->opened_units == 0.0 && event->interval_index() == 5
                    && event->cycle_before == event->cycle_after;
        }
        bool working = false;
        for (const auto& row : native_working_requests())
            working = working || row.definition->handle.incarnation == filled_candidate_;
        const auto& rows = adapter_.*member(Rows{});
        const auto candidate = rows.find(filled_candidate_);
        saw_filled_candidate = filled && !working && live_position_size() == -79.0
            && candidate != rows.end() && candidate->second.bracket_origin.incarnation == 0
            && candidate->second.legs.target().incarnation != 0;
        check(saw_filled_candidate, "K3 candidate was not a filled, retired partial stop");
        check(counting != nullptr, "filled-candidate oracle was not attached");
        counting->filled_candidate = filled_candidate_;
    }

    void inspect_supersession() {
        const auto& rows = adapter_.*member(Rows{});
        checked_supersession = saw_filled_candidate && counting != nullptr
            && counting->filled_candidate_witnesses > 0
            && counting->filled_candidate_first == successor_;
        check(checked_supersession, "full K3 oracle did not supersede the filled stop");
        check(rows.contains(successor_), "first different-stop witness was not retained");
        check(rows.contains(filled_candidate_) == ps::detail::retain_retired_rows(),
              "superseded filled candidate did not follow the retirement switch");
    }
};

// Preserve the reader's global-limit counterexample. The 25%-margin long
// remains leveraged after its half exit, so the later drop causes a real
// partial margin slice and leaves units for the restored bracket to flatten.
class GlobalLimitRevival final : public BookHost, public RevivalObserver {
public:
    GlobalLimitRevival() {
        ps::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.pyramiding = 0;
        config.margin_long = 25.0;
        config.margin_short = 100.0;
        configure_pine_strategy(config);
        set_margin_call_enabled(true);
    }

    struct Reading {
        std::uint64_t ordinal = 0, expected_selected = 0, selected = 0;
        std::uint64_t limit_witness = 0, cancel_after = 0, cancelled = 0;
        bool after = false;
        std::vector<std::uint64_t> expected_restored, restored;
        std::vector<std::pair<std::uint64_t, std::vector<std::uint64_t>>> lifecycles;
    };
    std::vector<Reading> readings;

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) strategy_entry("L", true, missing, missing, 250.0);
        if (i == 1) {
            strategy_exit("X", "", 110.0, 90.0);
            original_limit_ = exit_at(1, ps::PineOrderFamily::ExitLimit);
            original_stop_ = exit_at(1, ps::PineOrderFamily::ExitStop);
            check(original_limit_ != 0 && original_stop_ > original_limit_,
                  "global bracket did not place limit then stop");
        }
        if (i == 2) {
            strategy_entry("S", false);
            strategy_close("L", "Declined reversal");
        }
        if (i == 4) {
            const auto& rows = adapter_.*member(Rows{});
            const auto& limit = rows.at(original_limit_);
            const auto& stop = rows.at(original_stop_);
            saw_dormant_ = limit.legs.dormant() && stop.legs.dormant()
                && !oracle_withdrawn(limit.legs) && !oracle_withdrawn(stop.legs);
            check(saw_dormant_ && saw_decline_ && limit_rejected_ && stop_rejected_
                  && live_position_size() > 0.0,
                  "declined reversal did not suspend and retire both touched global requests");
            partial_units_ = live_position_size() * 0.5;
            strategy_exit("X", "", 105.0, missing, missing, missing, missing, 50.0);
            filled_limit_ = exit_at(4, ps::PineOrderFamily::ExitLimit);
            check(filled_limit_ > original_stop_
                  && rows.at(filled_limit_).projection_predecessor == 0,
                  "global partial reissue used a live predecessor instead of supersession");
        }
        if (i == 5) {
            for (const auto& row : native_events(cursor_)) {
                if (!row.command) continue;
                const auto* applied =
                    std::get_if<native_order::ExecutionAppliedEvent>(&*row.command);
                if (applied && applied->handle().incarnation == filled_limit_) {
                    check(applied->terminal && applied->closed_units == partial_units_
                          && applied->opened_units == 0.0
                          && applied->cycle_before == applied->cycle_after,
                          "global limit did not terminally fill half without ending its cycle");
                    ++partial_fills_;
                }
            }
            bool working = false;
            for (const auto& row : native_working_requests())
                working = working || row.definition->handle.incarnation == filled_limit_;
            check(partial_fills_ == 1 && !working && live_position_size() == partial_units_,
                  "global partial fill receipt or remaining long was missing");
        }
        if (i == 6) {
            const auto& rows = adapter_.*member(Rows{});
            const auto filled = rows.find(filled_limit_);
            // Record this before margin, but defer the assertion so the
            // unfixed run can expose the actual restored/cancelled-row delta.
            survived_sweep_ = filled != rows.end() && !filled->second.filled_exit;
        }
        observe_book();
    }

    void observe_book() {
        for (const auto& row : native_events(cursor_)) {
            if (!row.command) continue;
            if (const auto* rejected = std::get_if<native_order::MatchRejectedEvent>(&*row.command)) {
                if (rejected->cursor.point.interval_index == 3) {
                    saw_decline_ = saw_decline_ || (rejected->request().label == "S"
                        && (rejected->reason == native_order::MatchRejectReason::HostPrecommit
                            || rejected->reason == native_order::MatchRejectReason::InitialMargin));
                    if (rejected->reason == native_order::MatchRejectReason::HostPrecommit) {
                        limit_rejected_ = limit_rejected_
                            || rejected->handle().incarnation == original_limit_;
                        stop_rejected_ = stop_rejected_
                            || rejected->handle().incarnation == original_stop_;
                    }
                }
            }
            for (auto& reading : readings) {
                if (!reading.after || reading.selected == 0 || reading.cancelled != 0
                    || row.ordinal <= reading.cancel_after) continue;
                // The first command after the after-walk observer is the
                // revival's cancel. Both original requests are off the book.
                const auto* cancelled = std::get_if<native_order::NotWorkingEvent>(&*row.command);
                check(cancelled != nullptr, "global revival did not cancel its dormant target");
                reading.cancelled = cancelled->target.incarnation;
            }
        }
        BookHost::observe_book();
    }

    void observe_revival(bool before, const ps::PlacementTable& rows,
        const native_order::ExecutionAppliedEvent& event, const NativeDecisionContext& context,
        double units, std::int64_t cycle, std::uint64_t selected) override {
        check(units > 0.0 && event.closed_units > 0.0
              && event.cycle_before == event.cycle_after,
              "global revival oracle did not observe a partial long margin call");
        if (before) {
            check(!in_walk_, "overlapping global revival observations");
            in_walk_ = true;
            before_.clear();
            readings.emplace_back();
            auto& reading = readings.back();
            reading.ordinal = event.ordinal;
            std::uint64_t selected_row = 0;
            for (const auto& row : rows) {
                const auto& q = row.second;
                if (!oracle_exit(q.family) || q.source_id != "X"
                    || !q.from_entry.empty() || q.placement_cycle != cycle) continue;
                before_.emplace_back(row.first, q);
                if (!q.legs.dormant() || q.legs.target().incarnation == 0
                    || oracle_withdrawn(q.legs)) continue;
                const auto witness = walk_supersessor(rows, row.first, q);
                if (row.first == original_limit_) reading.limit_witness = witness;
                if (witness != 0) continue;
                reading.expected_restored.push_back(row.first);
                double stop = q.exit_levels.stop;
                if (!(std::isfinite(stop) && stop > 0.0)) {
                    const auto& suspension = q.legs.suspension();
                    const double original = suspension && suspension->revival_definition
                        ? suspension->revival_definition->prices().stop_price : missing;
                    stop = std::isfinite(original) ? original : q.legs.prices().stop_price;
                }
                const auto& bounds = q.leg_activation.bounds();
                const bool ready = !bounds || (bounds->position_cycle == cycle
                    && context.coordinate.interval_index >= bounds->stop_first_bar);
                const bool full = std::isfinite(q.requested_qty)
                    ? q.requested_qty >= units - 1e-9
                    : (!std::isfinite(q.qty_percent) || q.qty_percent >= 100.0 - 1e-5);
                if (full && ready && std::isfinite(stop) && stop >= event.resolved_price
                    && (selected_row == 0 || row.first < selected_row)) {
                    selected_row = row.first;
                    reading.expected_selected = q.legs.target().incarnation;
                }
            }
            std::sort(reading.expected_restored.begin(), reading.expected_restored.end());
            return;
        }
        check(in_walk_ && !readings.empty() && readings.back().ordinal == event.ordinal,
              "global revival after-observer lost its independent before-walk");
        auto& reading = readings.back();
        reading.selected = selected;
        for (const auto& prior : before_) {
            const auto& q = rows.at(prior.first);
            const auto& action = q.legs.last_action();
            const bool restored = action && action->cause.event == event.ordinal
                && action->cause.phase == exit_legs::Phase::AfterMargin
                && std::holds_alternative<exit_legs::Restore>(action->operation);
            if (restored) {
                reading.restored.push_back(prior.first);
                check(q.restored_after_margin && !q.legs.dormant()
                      && q.legs.revision() == prior.second.legs.revision() + 1
                      && q.legs.target().incarnation == prior.second.legs.target().incarnation
                      && q.legs.target().owner == prior.second.legs.target().owner
                      && action->expected_revision == prior.second.legs.revision()
                      && action->cause.bar == context.coordinate.interval_index,
                      "global revival did not preserve identity and advance the restored lifecycle");
                for (const auto leg : {exit_legs::Leg::Stop, exit_legs::Leg::Limit,
                                       exit_legs::Leg::Trail})
                    check(!q.legs.retired(leg)
                          && q.legs.generation(leg) == prior.second.legs.generation(leg) + 1,
                          "global revival did not restore all three leg generations");
            } else {
                check(lifecycle(q) == lifecycle(prior.second),
                      "global revival changed a row the independent walk did not restore");
            }
            reading.lifecycles.emplace_back(prior.first, lifecycle(q));
        }
        std::sort(reading.restored.begin(), reading.restored.end());
        check(reading.expected_restored == reading.restored
              && reading.expected_selected == reading.selected,
              "global revival disagrees with independent full-table walk");
        reading.cancel_after = event.ordinal;
        for (const auto& row : native_events(cursor_))
            reading.cancel_after = std::max(reading.cancel_after, row.ordinal);
        reading.after = true;
        in_walk_ = false;
    }

    void check_path() const {
        check(saw_dormant_ && saw_decline_ && limit_rejected_ && stop_rejected_
              && partial_fills_ == 1 && survived_sweep_,
              "global filled limit was not retained unmarked through the pre-margin sweep");
        check(!in_walk_ && !readings.empty(), "global limit scenario skipped its real revival oracle");
        for (const auto& reading : readings) {
            check(reading.after && reading.limit_witness == filled_limit_,
                  "independent margin walk did not read the filled global limit as evidence");
            check(std::find(reading.expected_restored.begin(), reading.expected_restored.end(),
                            original_stop_) != reading.expected_restored.end()
                  && std::find(reading.expected_restored.begin(), reading.expected_restored.end(),
                               original_limit_) == reading.expected_restored.end(),
                  "control walk did not restore the global stop and skip its superseded limit");
            check(reading.expected_selected == original_stop_
                  && reading.cancelled == reading.expected_selected,
                  "global revival cancelled a different handle from the independently selected stop");
        }
    }
private:
    std::uint64_t original_limit_ = 0, original_stop_ = 0, filled_limit_ = 0;
    double partial_units_ = 0.0;
    int partial_fills_ = 0;
    bool saw_dormant_ = false, survived_sweep_ = false, in_walk_ = false;
    bool saw_decline_ = false, limit_rejected_ = false, stop_rejected_ = false;
    std::vector<std::pair<std::uint64_t, ps::PlacementSnapshot>> before_;

    // Compare every lifecycle word, including replay payload and suspension,
    // without a hash collision or an expected state made by Lifecycle::apply.
    struct LifecycleWords {
        std::vector<std::uint64_t> words;
        void u(std::uint64_t value) { words.push_back(value); }
        void i(std::int64_t value) { u(static_cast<std::uint64_t>(value)); }
        void b(bool value) { u(value ? 1 : 0); }
        void d(double value) {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof bits);
            u(bits);
        }
    };
    static std::vector<std::uint64_t> lifecycle(const ps::PlacementSnapshot& q) {
        LifecycleWords sink;
        q.legs.visit(sink);
        sink.b(q.restored_after_margin);
        return sink.words;
    }
    std::uint64_t exit_at(int bar, ps::PineOrderFamily family) const {
        std::uint64_t found = 0;
        for (const auto& row : adapter_.*member(Rows{})) {
            const auto& q = row.second;
            if (q.source_id != "X" || !q.from_entry.empty() || q.family != family
                || q.projection_created_bar != bar) continue;
            check(found == 0 && q.legs.target().incarnation == row.first
                  && q.bracket_origin.incarnation == 0,
                  "global exit has ambiguous rows or a different lifecycle target");
            found = row.first;
        }
        return found;
    }
};

std::vector<Bar> global_limit_margin_feed() {
    std::vector<Bar> result;
    const auto add = [&](double open, double high, double low, double close) {
        result.push_back({open, high, low, close, 1.0,
                          start_ms + static_cast<std::int64_t>(result.size()) * 86400000});
    };
    add(100.0, 100.2, 99.8, 100.0); // 0: submit the leveraged long
    add(100.0, 100.2, 99.8, 100.0); // 1: arm global X limit 110 / stop 90
    add(100.0, 100.2, 99.8, 100.0); // 2: submit all-in short / paired long close
    // The gap declines the frozen all-in short. Both dormant prices then
    // touch and receive terminal precommit refusals, giving the later call
    // fresh requests with no projection link. The 89 low stays above margin.
    add(102.0, 111.0, 89.0, 102.0); // 3: decline, then retire both touched requests
    add(102.0, 102.2, 101.8, 102.0); // 4: dormant predecessor; reissue X limit 105 at 50%
    add(102.0, 106.0, 101.8, 105.0); // 5: terminal partial fill, same position cycle
    add(105.0, 105.2, 104.8, 105.0); // 6: receipt-safe sweep before margin
    add(105.0, 105.2, 19.0, 19.0);   // 7: partial margin slice, then real global revival
    add(19.0, 19.2, 18.8, 19.0);
    add(19.0, 19.2, 18.8, 19.0);
    return result;
}

std::vector<Bar> margin_feed() {
    // The V19-D directed path, with a second upward gap at bar 6 to decline
    // the second all-in reversal after the one-unit stop has filled.
    std::vector<Bar> result;
    const auto add = [&](double open, double high, double low, double close) {
        result.push_back({open, high, low, close, 1.0,
                          start_ms + static_cast<std::int64_t>(result.size()) * 86400000});
    };
    add(100.0, 100.2, 99.8, 100.0);
    add(100.0, 100.2, 99.8, 100.0);
    add(100.0, 100.5, 99.7, 100.5);
    add(101.5, 103.0, 100.8, 102.0);
    for (int i = 0; i < 2; ++i) add(102.0, 102.5, 101.5, 102.0);
    add(103.0, 103.5, 102.5, 103.0);
    add(103.0, 103.5, 102.5, 103.0);
    add(103.0, 116.0, 102.8, 115.0);
    add(115.0, 115.5, 114.5, 115.0);
    add(115.0, 115.5, 114.5, 115.0);
    return result;
}

template<class Host>
void paired_books(const std::vector<Bar>& bars, Host& kept, Host& erased,
                  Work& old_work, Work& new_work) {
    {
        Retention reference(true);
        CountScope scope(old_work);
        kept.run(bars.data(), static_cast<int>(bars.size()));
        kept.observe_book();
    }
    {
        Retention reference(false);
        CountScope scope(new_work);
        erased.run(bars.data(), static_cast<int>(bars.size()));
        erased.observe_book();
    }
    equal_outcome(kept, erased);
    check(kept.observations == erased.observations, "book or command outcomes changed");
    check(kept.not_working == erased.not_working, "non-working command population changed");
    check_work(old_work); check_work(new_work);
}

void global_limit_differential() {
    const auto bars = global_limit_margin_feed();
    GlobalLimitRevival kept, erased;
    Work old_work, new_work;
    const auto run = [&](GlobalLimitRevival& host, Work& work, bool retain) {
        Retention reference(retain);
        CountScope count(work);
        RevivalScope observe(host);
        host.run(bars.data(), static_cast<int>(bars.size()));
        host.observe_book();
    };
    run(kept, old_work, true);
    run(erased, new_work, false);
    equal_outcome(kept, erased);
    check(kept.observations == erased.observations && kept.not_working == erased.not_working,
          "global limit retirement changed book or command outcomes");
    check_work(old_work); check_work(new_work);
    kept.check_path(); erased.check_path();
    check(kept.readings.size() == erased.readings.size(), "global margin revival population changed");
    for (std::size_t i = 0; i < kept.readings.size(); ++i) {
        const auto& control = kept.readings[i];
        const auto& actual = erased.readings[i];
        check(control.ordinal == actual.ordinal
              && control.expected_restored == actual.restored
              && control.expected_selected == actual.selected
              && control.expected_selected == actual.cancelled
              && control.lifecycles == actual.lifecycles,
              "global retirement changed restored rows, cancelled handle or full lifecycle state");
    }
    bool margin = false, revived = false;
    for (int i = 0; i < erased.trade_count(); ++i) {
        const auto& trade = erased.get_trade(i);
        margin = margin || trade.exit_id == ps::kMarginCallLabel;
        revived = revived || (trade.exit_id == "X" && trade.exit_bar_index > 5
                              && !trade.open_at_end);
    }
    check(margin && revived, "global limit scenario did not book both margin and revived-exit trades");
}
} // namespace o1w_tail

int main() {
    try {
        using namespace o1w_tail;
        small_differential();
        const auto bars = feed(6);
        SameId kept, erased;
        Work old_work, new_work;
        paired_books(bars, kept, erased, old_work, new_work);
        check(erased.saw_bound_predecessor && erased.saw_filled_witness && erased.checked_hash
              && erased.cancelled_requests > 0,
              "same-id supersession path was not exercised");
        check(new_work.filled_witnesses > 0, "full K3 oracle never read a filled witness");
        Recycle old_cycle, new_cycle;
        Work old_cycle_work, new_cycle_work;
        paired_books(bars, old_cycle, new_cycle, old_cycle_work, new_cycle_work);
        check(new_cycle.saw_archive && new_cycle.saw_restored_prefix && new_cycle.cancelled_archive,
              "archive/reissue/cancel path was not exercised");
        check(old_cycle.expected_cancel_results == new_cycle.expected_cancel_results,
              "settled prefix member count changed");
        ReissueRevival old_margin, new_margin;
        Work old_margin_work, new_margin_work;
        paired_books(margin_feed(), old_margin, new_margin, old_margin_work, new_margin_work);
        check(new_margin.saw_dormant && new_margin.saw_filled_candidate
              && new_margin.checked_supersession && old_margin.checked_supersession
              && old_margin_work.witnesses > 0 && new_margin_work.witnesses > 0
              && old_margin_work.filled_candidate_witnesses > 0
              && new_margin_work.filled_candidate_witnesses > 0,
              "different-stop revival supersession was not exercised");
        bool margin_closed = false;
        int partial_closes = 0;
        for (int i = 0; i < new_margin.trade_count(); ++i) {
            const auto& trade = new_margin.get_trade(i);
            margin_closed = margin_closed || trade.exit_id == ps::kMarginCallLabel;
            if (trade.exit_id == "X") {
                check(trade.exit_bar_index == 5 && trade.qty == 1.0 && !trade.open_at_end,
                      "superseded stop closed the position after its one-unit fill");
                ++partial_closes;
            }
        }
        check(partial_closes == 1, "partial-stop scenario did not book exactly one X fill");
        check(margin_closed, "revival witness never reached a margin call");
        global_limit_differential();
        std::puts("test_position_tail_retirement: trades/reports, K3 oracle, "
                  "global limit/margin revival, archive/reissue/cancel/reset");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_position_tail_retirement: %s\n", error.what());
        return 1;
    }
}
