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
// the pair parks X at 105, later bars reissue X at different stops, then a
// rally calls margin. The old stop must stay superseded in the retaining
// control and the erasing execution alike.
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
    bool saw_dormant = false, checked_supersession = false;
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 1) {
            strategy_entry("S", false, missing, missing, 80.0);
            strategy_exit("X", "S", missing, 105.0);
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
        }
        if (i >= 4 && i <= 7 && live_position_size() < 0.0)
            strategy_exit("X", "S", missing, 125.0 + i);
        if (i == 8) {
            for (const auto& row : adapter_.*member(Rows{}))
                if (row.second.family == ps::PineOrderFamily::ExitStop
                    && row.second.source_id == "X" && row.second.exit_levels.stop == 105.0)
                    check(!row.second.restored_after_margin,
                          "different-stop same-id reissue failed to supersede the old stop");
            checked_supersession = true;
        }
        observe_book();
    }
};

std::vector<Bar> margin_feed() {
    // Same directed price path as the established V19-D revival witness;
    // every command above runs against the real native consumer.
    std::vector<Bar> result;
    const auto add = [&](double open, double high, double low, double close) {
        result.push_back({open, high, low, close, 1.0,
                          start_ms + static_cast<std::int64_t>(result.size()) * 86400000});
    };
    add(100.0, 100.2, 99.8, 100.0);
    add(100.0, 100.2, 99.8, 100.0);
    add(100.0, 100.5, 99.7, 100.5);
    add(101.5, 103.0, 100.8, 102.0);
    for (int i = 0; i < 4; ++i) add(102.0, 102.5, 101.5, 102.0);
    add(102.0, 116.0, 101.8, 115.0);
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
        check(new_margin.saw_dormant && new_margin.checked_supersession
              && old_margin_work.witnesses > 0 && new_margin_work.witnesses > 0,
              "different-stop revival supersession was not exercised");
        bool margin_closed = false;
        for (int i = 0; i < new_margin.trade_count(); ++i) {
            const auto& trade = new_margin.get_trade(i);
            margin_closed = margin_closed || trade.exit_id == ps::kMarginCallLabel;
            check(trade.exit_id != "X", "superseded stop closed the position after margin");
        }
        check(margin_closed, "revival witness never reached a margin call");
        std::puts("test_position_tail_retirement: trades/reports, K3 oracle, archive/reissue/cancel/reset");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_position_tail_retirement: %s\n", error.what());
        return 1;
    }
}
