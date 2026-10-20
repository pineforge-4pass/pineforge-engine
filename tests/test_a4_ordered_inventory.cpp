// Fail-before target: 6cfcff0696. The old A4 containers have no resident_bytes
// observer and grow with these populations. The supervisor must compile this
// unit against the frozen header closure before compiling the implementation.
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/source/pine_intent_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using source::PineExecutionAdapter;
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
#define MEMBER(Tag, field) \
    struct Tag { friend auto access(Tag); }; \
    template struct Access<Tag, &PineExecutionAdapter::field>
MEMBER(BoundExits, id_bound_exits_);
MEMBER(Balances, close_logical_units_);
MEMBER(Ledger, close_ledger_records_);
MEMBER(Pyramids, pyramiding_records_);
MEMBER(Tombstones, exit_tombstones_);
MEMBER(Callsites, close_batch_callsites_);
MEMBER(AddMarks, market_pyramid_adds_);
MEMBER(Trails, trail_state_at_open_);
MEMBER(Retunes, carried_trail_retunes_);
MEMBER(Unrepeated, unrepeated_margin_calls_);
MEMBER(Sweep, retired_row_scratch_);
MEMBER(Credit, credit_close_ledger);
MEMBER(Book, book_close_ledger);
MEMBER(CreditPyramid, credit_pyramiding_record);
MEMBER(BookPyramid, book_pyramiding_records);
MEMBER(LedgerConfigured, ledger_records_configured_);
#undef MEMBER
struct Frozen { friend auto access(Frozen); };
template struct Access<Frozen, &source::PineLanguageState::pos_view_frozen_entry_qty_>;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool bits(double a, double b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

template<class Store> void bound(const Store& store, std::size_t population) {
    require(store.size() == population, "reach the named population before measuring its bound");
    require(store.resident_bytes() <= store.resident_limit(), "A4 resident page limit holds");
}

// The reference is the base's literal two-pass booking and adjacent-id
// compaction. It deliberately does not regroup floating point additions.
struct Record { std::string id; double units; };
void reference_book(std::vector<Record>& records, const std::string& id, double units) {
    double left = units;
    const auto take = [&](Record& row) {
        if (!(left > 1e-10) || !(row.units > 0.0)) return;
        const double amount = std::min(row.units, left);
        row.units -= amount;
        left -= amount;
    };
    for (auto& row : records) if (row.id == id) take(row);
    for (auto& row : records) take(row);
    std::vector<Record> kept;
    for (const auto& row : records) {
        if (!(row.units > 1e-10)) continue;
        if (!kept.empty() && kept.back().id == row.id) kept.back().units += row.units;
        else kept.push_back(row);
    }
    records = std::move(kept);
}
void close_ledger() {
    PineExecutionAdapter adapter;
    std::vector<Record> expected;
    constexpr std::size_t count = 513;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string id = "entry-" + std::to_string(i % 17);
        const double units = i % 3 == 0 ? 0.125 : 1.0 + double(i % 7) / 16.0;
        (adapter.*access(Credit{}))(id, units);
        expected.push_back({id, units});
    }
    auto& ledger = adapter.*access(Ledger{});
    auto& balances = adapter.*access(Balances{});
    bound(ledger, count); // P28: a live, unbooked population, not a retired count.
    bound(balances, 17); // P27.
    for (const auto& id : {"entry-8", "absent", "entry-0", "entry-16"}) {
        reference_book(expected, id, 31.375);
        (adapter.*access(Book{}))(id, 31.375);
        require(ledger.size() == expected.size(), "compaction loses no unbooked opening");
        std::map<std::string, double> sums;
        std::size_t cursor = 0;
        for (const auto& row : ledger) {
            require(row.id == expected[cursor].id && bits(row.units, expected[cursor].units),
                    "named-first then FIFO booking/adjacent merge is bit exact");
            sums[row.id] += row.units;
            ++cursor;
        }
        require(balances.size() == sums.size(), "same retained logical id population");
        for (const auto& row : sums)
            require(bits(adapter.fixture_close_logical_units(row.first), row.second),
                    "balances rebuild in record order");
    }
    (adapter.*access(Book{}))("entry-3", 1e9);
    require(ledger.empty() && balances.empty(), "whole booking retires both stores");
}

void pyramid_records() {
    PineExecutionAdapter adapter;
    adapter.*access(LedgerConfigured{}) = true;
    constexpr std::size_t count = 257;
    for (std::size_t i = 1; i <= count; ++i)
        (adapter.*access(CreditPyramid{}))(i % 2 ? "A" : "B", i, 2.0);
    auto& records = adapter.*access(Pyramids{});
    bound(records, count); // P29: physical fills remain separate.
    (adapter.*access(BookPyramid{}))("A", 101, 3.0);
    require(records.size() == count - 1, "armed opening is booked before older named records");
    bool saw_oldest = false, saw_armed = false;
    for (const auto& row : records) {
        if (row.incarnation == 1) saw_oldest = bits(row.units, 1.0);
        if (row.incarnation == 101) saw_armed = true;
    }
    require(saw_oldest && !saw_armed, "pyramiding booking preserves incarnation distinction");
}

void auxiliary_populations() {
    PineExecutionAdapter adapter;
    auto& exits = adapter.*access(BoundExits{});
    auto& tombstones = adapter.*access(Tombstones{});
    auto& sites = adapter.*access(Callsites{});
    auto& marks = adapter.*access(AddMarks{});
    auto& trails = adapter.*access(Trails{});
    auto& retunes = adapter.*access(Retunes{});
    auto& calls = adapter.*access(Unrepeated{});
    constexpr std::size_t count = 521;
    for (std::size_t i = 1; i <= count; ++i) {
        typename std::decay_t<decltype(exits)>::value_type exit;
        exit.exit_id = "X" + std::to_string(i);
        exit.from_entry = "E" + std::to_string(i);
        exits.push_back(std::move(exit));
        tombstones["X" + std::to_string(i) + "\x1f" + "E" + std::to_string(i)] = i;
        auto& site = sites[i];
        site.id = "last" + std::to_string(i);
        site.first_id = "first" + std::to_string(i);
        site.target = double(i) / 8.0;
        site.queue_sequence = count - i + 1;
        marks.insert(i);
        trails.emplace(i, NativeTrailState{});
        native_order::RequestHandle handle;
        handle.incarnation = i;
        retunes.push_back({handle, double(i) / 16.0});
        calls.push_back(i);
    }
    bound(exits, count); bound(tombstones, count); bound(sites, count); // P26, P30, P31.
    bound(marks, count); bound(trails, count); bound(retunes, count); bound(calls, count); // P32-P35.
    std::size_t cursor = 0;
    for (const auto& row : retunes)
        require(row.first.incarnation == ++cursor && bits(row.second, double(cursor) / 16.0),
                "carried trail retunes retain insertion order");
    require(tombstones.at("X31\x1f" "E31") == 31, "exact tombstone key/incarnation read");
    calls.erase(std::find(calls.begin(), calls.end(), 37));
    require(calls.size() == count - 1 && std::find(calls.begin(), calls.end(), 37) == calls.end(),
            "unrepeated request consumed once without disturbing peers");
    auto& sweep = adapter.*access(Sweep{});
    for (std::size_t i = 1; i <= count; ++i) {
        sweep.roots.push_back(i);
        sweep.askable_origins.push_back(i);
        sweep.unsuperseded.push_back({i, count});
        sweep.held.push_back({i, nullptr, count, false});
        sweep.doomed.push_back(i);
        sweep.working_roots.push_back(i);
        sweep.lot_openings.push_back(i);
    }
    bound(sweep.roots, count); bound(sweep.askable_origins, count);
    bound(sweep.unsuperseded, count); bound(sweep.held, count); bound(sweep.doomed, count);
    bound(sweep.working_roots, count); bound(sweep.lot_openings, count); // P36.
    sweep.clear();
    require(sweep.roots.empty() && sweep.held.empty() && sweep.unsuperseded.size() == count,
            "sweep resets workspaces while preserving the high-water memo");
}

void frozen_and_intents() {
    source::PineLanguageState language;
    auto& frozen = language.*access(Frozen{});
    std::map<std::string, double> expected;
    constexpr std::size_t count = 263;
    for (std::size_t lot = 0; lot < count * 3; ++lot) {
        const std::string id = "lot-" + std::to_string(lot % count);
        const double qty = lot % 2 == 0 ? 0.125 : 1.0 / 3.0;
        frozen[id] += qty;
        expected[id] += qty;
    }
    bound(frozen, count); // P39: sum per lot in the original visit order.
    for (const auto& row : expected) require(bits(frozen.at(row.first), row.second), "frozen per-id bits");
    source::detail::PineIntentState state(nullptr, 1);
    for (std::size_t i = 0; i < count; ++i) {
        source::detail::IntentCancel payload{"cancel-" + std::to_string(i)};
        state.rows.push_back({std::move(payload), {}, {}});
    }
    bound(state.rows, count); // P41: one wide evaluation still spills.
    std::size_t i = 0;
    for (const auto& row : state.rows)
        require(std::get<source::detail::IntentCancel>(row.payload).id == "cancel-" + std::to_string(i++),
                "intent replay retains the full ordered payload");
    source::detail::reset_intent_evaluation(state, 7, {}, {}, {});
    require(state.rows.empty() && state.has_evaluation && state.label_ms == 7,
            "empty last evaluation replaces the previous signal");
}
} // namespace

int main() {
    try {
        close_ledger(); pyramid_records(); auxiliary_populations(); frozen_and_intents();
        std::puts("A4 ordered inventory: populations and differential booking passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A4 ordered inventory: %s\n", error.what());
        return 1;
    }
}
