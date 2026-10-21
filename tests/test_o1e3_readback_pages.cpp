#include "o1e3_allocation_probe.hpp"
#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <tuple>

namespace pineforge { inline namespace engine_script_run_v19 {
struct NativeExecutionConsumerProbe {
    static auto old_openings(const NativeExecutionConsumer& c, const BacktestEngine& engine,
                             const std::vector<native_order::RequestHandle>& handles, int64_t cycle) {
        return c.read_openings(engine, handles, cycle);
    }
    static std::size_t drain_population(const NativeExecutionConsumer& c) { return c.drain_seeds_.size(); }
    static std::size_t drain_workspace(const NativeExecutionConsumer& c) {
        return sizeof(c.drain_handles_) + sizeof(c.drain_seeds_)
            + c.drain_handles_.capacity() * sizeof(native_order::RequestHandle)
            + c.drain_seeds_.capacity() * sizeof(std::pair<native_order::EventId, native_order::RequestHandle>);
    }
};
}}

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
bool bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }
bool same(const NativeOpenLot& a, const NativeOpenLot& b) {
    return std::tie(a.ordinal, a.entry_incarnation, a.cycle, a.side, a.entry_label, a.entry_comment,
                    a.entry_time_ms, a.entry_bar_index)
        == std::tie(b.ordinal, b.entry_incarnation, b.cycle, b.side, b.entry_label, b.entry_comment,
                    b.entry_time_ms, b.entry_bar_index)
        && bits(a.entry_price, b.entry_price) && bits(a.signed_units, b.signed_units)
        && bits(a.entry_commission, b.entry_commission) && bits(a.mark, b.mark)
        && bits(a.unrealized_pnl, b.unrealized_pnl)
        && bits(a.favorable_excursion, b.favorable_excursion) && bits(a.adverse_excursion, b.adverse_excursion);
}
NativeRunSpec spec() {
    NativeRunSpec s;
    s.identity = {"o1e3-readback", 1}; s.input_tf = "1"; s.script_tf = "1";
    s.tickerid = "TEST:O1E3"; s.timezone = "UTC"; s.session = "24x7";
    s.initial_capital = 1e9; s.point_value = 1; s.account_fx = 1; s.price_tick = .01;
    s.close_execution = NativeCloseExecution::AfterCalculation;
    s.event_retention = NativeEventRetention::Full;
    return s;
}
struct Host : NativeStrategyHost {
    explicit Host(std::size_t n, bool drain) : population(n), drain_case(drain) {}
    std::size_t population;
    bool drain_case, reached = false;
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    no::RequestHandle put(no::Request request) {
        const auto result = submit(request);
        require(result.status == no::SubmitStatus::Accepted && result.handle.has_value(), "fixture submit refused");
        return *result.handle;
    }
    void apply(no::RequestHandle handle) {
        const auto result = execute_current({handle, NativeCurrentPriceRule::AsPresented});
        require(std::holds_alternative<no::ExecutionAppliedEvent>(result), "fixture execution refused");
    }
    // Frozen vector implementation of the lot projection, independent of
    // the new single-row projection used by the production owning wrapper.
    std::vector<NativeOpenLot> frozen_lots(double mark) const {
        std::vector<NativeOpenLot> out;
        const bool is_long = position_side_ != PositionSide::SHORT;
        const double fx = active_account_currency_fx();
        for (std::size_t i = 0; i < pyramid_entries_.size(); ++i) {
            const auto lot = pyramid_entries_[i];
            NativeOpenLot row;
            row.ordinal = i; row.entry_incarnation = lot.entry_incarnation; row.cycle = position_cycle_seq_;
            row.side = is_long ? no::Side::Long : no::Side::Short;
            row.entry_label = lot.entry_id; row.entry_comment = lot.entry_comment;
            row.entry_time_ms = lot.time; row.entry_bar_index = lot.entry_bar_index;
            row.entry_price = lot.price; row.signed_units = is_long ? lot.qty : -lot.qty;
            row.entry_commission = open_entry_commission(lot); row.mark = mark;
            double favorable = lot.max_runup, adverse = lot.max_drawdown;
            if (std::isfinite(mark)) {
                const double move = (is_long ? (mark - lot.price) : (lot.price - mark)) * lot.qty;
                row.unrealized_pnl = move * syminfo_.pointvalue * fx - row.entry_commission;
                favorable = std::max(favorable, move); adverse = std::max(adverse, -move);
            }
            row.favorable_excursion = favorable * syminfo_.pointvalue * fx;
            row.adverse_excursion = adverse * syminfo_.pointvalue * fx;
            out.push_back(std::move(row));
        }
        return out;
    }
    void readback() {
        std::vector<no::RequestHandle> handles;
        for (std::size_t i = 0; i < population; ++i) {
            const auto text = std::string(48, 'x') + std::to_string(i);
            auto opening = no::Request{no::Transact{1.0}, text, text};
            const auto handle = put(opening); apply(handle); handles.push_back(handle);
            auto working = no::Request{no::Transact{1.0}, text, text};
            working.trigger = no::Limit{1.0}; put(working);
        }
        auto& c = consumer();
        const auto expected_lots = frozen_lots(103.25);
        const auto expected_working = native_working_requests();
        require(expected_lots.size() == population && expected_working.size() == population,
                "E13 lot/request populations not reached");
        // Nonincarnation order, duplicate incarnations and a stale run with
        // the same incarnation exercise stable tie order and run filtering.
        std::reverse(handles.begin(), handles.end());
        // Copies first: push_back(handles[i]) would hold a reference into
        // the same vector across the reallocation it may perform.
        const auto duplicate = handles[population / 2];
        handles.push_back(duplicate);
        auto stale = handles.front(); ++stale.run.run_number; handles.push_back(stale);
        auto sorted = handles;
        std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
            return a.incarnation < b.incarnation;
        });
        o1e3_test::AllocationProbe memory;
        std::size_t lots_seen = 0, requests_seen = 0, openings_seen = 0;
#if defined(PINEFORGE_HAS_NATIVE_ROW_READBACK_V1)
        require(c.working_request_count() == population, "E13 current-book count");
        for (std::size_t i = 0; i < population; ++i) {
            const auto lot = c.open_lot_at(*this, 103.25, i);
            const auto working = c.working_request_at(i);
            (void)c.open_lot_at(*this, 0.0, population - i - 1);
            (void)c.working_request_at(population - i - 1);
            require(same(lot, expected_lots[i]), "E13 lot projection differs from frozen implementation");
            require(working.definition->handle == expected_working[i].definition->handle
                    && working.definition->request.label == expected_working[i].definition->request.label
                    && working.remaining.index() == expected_working[i].remaining.index()
                    && working.trigger_state.index() == expected_working[i].trigger_state.index(),
                    "E13 working projection differs");
            ++lots_seen; ++requests_seen;
        }
        const auto openings = c.opening_rows(*this, handles, position_cycle_seq_);
#else
        // Frozen header closure still compiles; RED is the actual owning
        // snapshot allocation after reaching the same lot/request counts.
        const auto lots = native_open_lots(103.25);
        const auto working = native_working_requests();
        lots_seen = lots.size(); requests_seen = working.size();
        const auto openings = NativeExecutionConsumerProbe::old_openings(c, *this, handles, position_cycle_seq_);
#endif
        require(openings.size() == sorted.size(), "E13 opening population not reached");
        for (const auto& row : openings) {
            const auto saved = row;
            (void)openings[openings.size() - openings_seen - 1];
            require(row.queried_opening == sorted[openings_seen]
                    && row.queried_cycle == position_cycle_seq_
                    && row.current_position.index() == no::PositionIdentity{no::PositionNonflat{}}.index()
                    && row.has_live_matching_lot == (row.queried_opening.run == handles.front().run),
                    "E13 sorted opening leaves differ");
            require(row.queried_opening == saved.queried_opening, "E13 nested read evicted opening row");
            ++openings_seen;
        }
        const auto peak = memory.stop();
        require(lots_seen == population && requests_seen == population && openings_seen == sorted.size(),
                "E13 full-consumption witness");
        require(peak <= 16384, "E13 readback workspace grew with book population");
        reached = true;
    }
    void drain() {
        std::vector<no::RequestHandle> parents, children;
        for (std::size_t i = 0; i < population; ++i) {
            no::Request parent{no::Transact{1.0}, "parent", ""};
            parent.trigger = no::Limit{1.0};
            parent.group = no::Member{17, static_cast<std::int64_t>(i + 1), no::GroupEffect::Cancel};
            const auto p = put(parent); parents.push_back(p);
            no::Request child{no::Reduce{no::ExplicitUnits{1.0}}, "child", ""};
            child.owner = no::WaitForApplied{p}; children.push_back(put(child));
        }
        require(native_working_requests().size() == population * 2, "E13 dependency population not reached");
        no::Request trigger{no::Transact{1.0}, "trigger", ""};
        trigger.group = no::Member{17, 0, no::GroupEffect::Cancel};
        apply(put(trigger));
        std::size_t cancelled = 0;
        for (const auto& event : native_events(0)) {
            if (!event.command) continue;
            const auto* row = std::get_if<no::CancelledEvent>(&*event.command);
            if (!row) continue;
            const auto& expected = cancelled < population ? parents[cancelled] : children.at(cancelled - population);
            require(row->handle() == expected, "E13 dependency dispatch order changed");
            ++cancelled;
        }
        require(cancelled == population * 2 && native_working_requests().empty(),
                "E13 drain did not deliver every terminal row");
        require(NativeExecutionConsumerProbe::drain_population(consumer()) == population + 1,
                "E13 drain seed population not reached");
        require(NativeExecutionConsumerProbe::drain_workspace(consumer()) <= 4096,
                "E13 drain retained population-sized workspace");
        reached = true;
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (drain_case) drain(); else readback();
    }
};
void run(std::size_t population, bool drain) {
    Host host(population, drain);
    require(host.configure_native(spec()).status == NativeSetupStatus::Applied, "E13 fixture configure");
    const Bar bar{100, 100, 100, 100, 1, 1736121600000LL};
    host.run(&bar, 1);
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.reached, "E13 fixture callback did not finish");
}
}
int main() {
    try { run(256, false); run(1024, false); run(256, true); run(1024, true); }
    catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
    return 0;
}
