// Fail-before target: 6cfcff0696. The old A4 containers (std::vector,
// std::map, std::unordered_map) have no resident_bytes()/resident_limit()
// observer and grow with these populations; this unit does not compile
// against the frozen previous-epoch header closure for the right reason
// (missing ordered-store API), and it FAILS there because every population
// below grows without a fixed resident bound. The supervisor compiles it
// against that closure first.
//
// Rows: P26 id-bound exits, P27 close logical units, P28 close ledger records
// + kept-copy peak, P29 pyramiding records, P30 exit tombstones, P31
// close-batch callsites. The reference is the base tree's literal booking and
// compaction arithmetic, restated here without regrouping any binary64 add.
#include <pineforge/source/pine_adapter.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// The adapter's explicit test-only accessor (a declared friend of
// PineExecutionAdapter): the inventory is reached without a compiler-specific
// private-access trick and without widening the public surface.
namespace pineforge::source {
struct A4InventoryFixture {
    template<class Adapter> static auto& bound_exits(Adapter& adapter) {
        return adapter.id_bound_exits_;
    }
    template<class Adapter> static auto& balances(Adapter& adapter) {
        return adapter.close_logical_units_;
    }
    template<class Adapter> static auto& ledger(Adapter& adapter) {
        return adapter.close_ledger_records_;
    }
    template<class Adapter> static auto& pyramids(Adapter& adapter) {
        return adapter.pyramiding_records_;
    }
    template<class Adapter> static auto& tombstones(Adapter& adapter) {
        return adapter.exit_tombstones_;
    }
    template<class Adapter> static auto& callsites(Adapter& adapter) {
        return adapter.close_batch_callsites_;
    }
    template<class Adapter> static void credit(Adapter& adapter, const SourceId& id, double units) {
        adapter.credit_close_ledger(id, units);
    }
    template<class Adapter> static void book(Adapter& adapter, const SourceId& id, double units) {
        adapter.book_close_ledger(id, units);
    }
    template<class Adapter> static void credit_pyramid(
            Adapter& adapter, const SourceId& id, std::uint64_t incarnation, double units) {
        adapter.credit_pyramiding_record(id, incarnation, units);
    }
    template<class Adapter> static void book_pyramid(
            Adapter& adapter, const SourceId& id, std::uint64_t armed_for, double units) {
        adapter.book_pyramiding_records(id, armed_for, units);
    }
    template<class Adapter> static bool& ledger_configured(Adapter& adapter) {
        return adapter.ledger_records_configured_;
    }
};
} // namespace pineforge::source

namespace {
using namespace pineforge;
using source::A4InventoryFixture;
using source::PineExecutionAdapter;
using source::SourceId;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool bits(double left, double right) {
    return std::memcmp(&left, &right, sizeof left) == 0;
}

// The row's named population is reached, AND the resident workspace is the
// fixed admitted configuration rather than a population-sized allocation.
template<class Store>
void bound(const Store& store, std::size_t population, const char* population_message) {
    require(store.size() == population, population_message);
    require(store.resident_bytes() <= store.resident_limit(), "A4 resident page limit holds");
    require(store.resident_bytes() == source::a4_store::kResidentBytes,
            "the admitted resident workspace is the fixed configuration");
}

// ---------------------------------------------------------------------------
// P27/P28: the base tree's credit and booking, restated on an ordered vector.
struct Record {
    std::string id;
    double units;
};
void reference_credit(std::vector<Record>& records, std::map<std::string, double>& balances,
                      const std::string& id, double units) {
    balances[id] += units;
    if (!records.empty() && records.back().id == id) records.back().units += units;
    else records.push_back({id, units});
}
void reference_book(std::vector<Record>& records, std::map<std::string, double>& balances,
                    const std::string& id, double units) {
    if (!(units > 0.0) || records.empty()) return;
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
    balances.clear();
    for (const auto& row : records) balances[row.id] += row.units;
}

void close_ledger() {
    PineExecutionAdapter adapter;
    std::vector<Record> expected;
    std::map<std::string, double> expected_balances;
    constexpr std::size_t count = 513;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string id = "entry-" + std::to_string(i % 17);
        const double units = i % 3 == 0 ? 0.125 : 1.0 + double(i % 7) / 16.0;
        reference_credit(expected, expected_balances, id, units);
        A4InventoryFixture::credit(adapter, id, units);
    }
    auto& ledger = A4InventoryFixture::ledger(adapter);
    auto& balances = A4InventoryFixture::balances(adapter);
    bound(ledger, count, "P28: a live, unbooked fill-order population");
    bound(balances, 17, "P27: one balance row per distinct id");

    for (const auto& id : {"entry-8", "absent", "entry-0", "entry-16", "entry-3"}) {
        reference_book(expected, expected_balances, id, 31.375);
        A4InventoryFixture::book(adapter, id, 31.375);
        require(ledger.size() == expected.size(),
                "P28 compaction loses no unbooked opening");
        std::size_t cursor = 0;
        for (const auto& row : ledger) {
            require(cursor < expected.size() && row.id == expected[cursor].id
                        && bits(row.units, expected[cursor].units),
                    "P28 named-first then FIFO booking and adjacent merge is bit exact");
            ++cursor;
        }
        require(cursor == expected.size(), "P28 iteration reaches every surviving record");
        require(balances.size() == expected_balances.size(),
                "P27 keeps exactly the surviving ids");
        for (const auto& row : expected_balances)
            require(bits(adapter.fixture_close_logical_units(row.first), row.second),
                    "P27 balances rebuilt in record order");
    }

    // rollback: a committed-looking edit inside an abandoned transaction
    // leaves the row exactly as it was.
    {
        auto& ledger_rows = A4InventoryFixture::ledger(adapter);
        require(!ledger_rows.empty(), "rollback fixture needs a live row");
        auto batch = ledger_rows.batch();
        source::a4_store::ScratchHandle handle = 0;
        require(ledger_rows.first(handle), "rollback fixture reads its first row");
        typename std::decay_t<decltype(ledger_rows)>::value_type row;
        require(ledger_rows.load(handle, row), "rollback fixture loads its first row");
        const auto before = row.units;
        row.units = before + 12345.0;
        ledger_rows.store(handle, row);
        // `batch` is abandoned: its destructor rolls the edit back.
    }
    {
        std::size_t cursor = 0;
        for (const auto& row : ledger) {
            require(cursor < expected.size() && row.id == expected[cursor].id
                        && bits(row.units, expected[cursor].units),
                    "rollback leaves the store exactly as it was");
            ++cursor;
        }
    }

    A4InventoryFixture::book(adapter, "entry-3", 1e9);
    require(ledger.empty() && balances.empty(), "a whole booking retires both stores");
}

// ---------------------------------------------------------------------------
// P29: pyramiding records keep physical fills separate and book by incarnation.
void pyramiding() {
    PineExecutionAdapter adapter;
    A4InventoryFixture::ledger_configured(adapter) = true;
    constexpr std::size_t count = 257;
    for (std::size_t i = 1; i <= count; ++i)
        A4InventoryFixture::credit_pyramid(adapter, i % 2 ? "A" : "B", i, 2.0);
    auto& records = A4InventoryFixture::pyramids(adapter);
    bound(records, count, "P29: one unmerged record per entry fill");
    A4InventoryFixture::book_pyramid(adapter, "A", 101, 3.0);
    require(records.size() == count - 1,
            "P29 the armed opening is booked before older named records");
    bool saw_oldest = false, saw_armed = false;
    double total = 0.0;
    for (const auto& row : records) {
        if (row.incarnation == 1) saw_oldest = bits(row.units, 1.0);
        if (row.incarnation == 101) saw_armed = true;
        total += row.units;
    }
    require(saw_oldest && !saw_armed,
            "P29 booking preserves the physical incarnation distinction");
    require(bits(total, double(count) * 2.0 - 3.0),
            "P29 booked units are exactly the consumed amount");
}

// ---------------------------------------------------------------------------
// P26/P30/P31: the auxiliary ordered populations.
void auxiliary_populations() {
    PineExecutionAdapter adapter;
    auto& exits = A4InventoryFixture::bound_exits(adapter);
    auto& tombstones = A4InventoryFixture::tombstones(adapter);
    auto& sites = A4InventoryFixture::callsites(adapter);
    constexpr std::size_t count = 521;
    for (std::size_t i = 1; i <= count; ++i) {
        typename std::decay_t<decltype(exits)>::value_type exit;
        exit.exit_id = "X" + std::to_string(i);
        exit.from_entry = "E" + std::to_string(i);
        exit.levels.limit = double(i);
        exits.push_back(std::move(exit));
        const SourceId key = "X" + std::to_string(i) + "\x1f" + "E" + std::to_string(i);
        tombstones.upsert(key, std::make_pair(key, std::uint64_t{i}));
        typename std::decay_t<decltype(sites)>::value_type site{};
        site.first = i;
        site.second.id = "last" + std::to_string(i);
        site.second.first_id = "first" + std::to_string(i);
        site.second.target = double(i) / 8.0;
        site.second.queue_sequence = count - i + 1;
        site.second.token = i;
        site.second.active = true;
        sites.upsert(i, site);
    }
    bound(exits, count, "P26: every retained exit/entry binding is present");
    bound(tombstones, count, "P30: one tombstone per partial-exit key");
    bound(sites, count, "P31: one current-bar callsite row per token");

    // P26 keeps fill order (the vector's order).
    std::size_t cursor = 0;
    for (const auto& row : exits)
        require(row.exit_id == "X" + std::to_string(++cursor),
                "P26 kept exits iterate in fill order");
    require(cursor == count, "P26 iteration reaches every kept binding");

    // P30 exact key/incarnation read, and key iteration is complete.
    require(tombstones.at("X31\x1f" "E31") == 31, "P30 exact tombstone key/incarnation read");
    std::set<std::string> seen;
    for (const auto& row : tombstones) seen.insert(row.first);
    require(seen.size() == count && seen.count("X17\x1f" "E17") == 1,
            "P30 iteration yields every tombstone key exactly once");

    // P31 iteration is ascending token, the std::map order the hash folds.
    std::uint64_t previous = 0;
    for (const auto& row : sites) {
        require(row.first > previous, "P31 callsites iterate in ascending token order");
        previous = row.first;
        require(row.second.token == row.first && row.second.active,
                "P31 the row carries its own token and state");
    }
}

// The P27 balance rebuild uses upserts inside one explicit transaction.
// Each upsert must join it without beginning or committing another one.
void keyed_batch_transactions() {
    PineExecutionAdapter adapter;
    auto& balances = A4InventoryFixture::balances(adapter);
    std::map<std::string, double> expected;
    const auto check = [&](const std::map<std::string, double>& rows) {
        require(balances.size() == rows.size(), "P27 batch keeps exactly the reference keys");
        auto reference = rows.begin();
        for (const auto& row : balances) {
            require(reference != rows.end() && row.first == reference->first
                        && bits(row.second, reference->second),
                    "P27 batch values and key order match the reference");
            ++reference;
        }
        require(reference == rows.end(), "P27 batch iteration reaches every reference row");
    };
    const auto upsert = [&](auto& rows, const std::string& id, double units) {
        rows[id] += units;
        balances.upsert(id, {id, rows.at(id)});
    };
    upsert(expected, "A", 0.125);
    upsert(expected, "B", 1.25);
    check(expected);

    {
        auto batch = balances.batch();
        upsert(expected, "A", 0.375);
        upsert(expected, "C", 2.5);
        upsert(expected, "A", 0.0625);
        check(expected);
        batch.commit();
    }
    check(expected);

    // Abandoning the batch must undo updates AND the newly inserted key.
    {
        auto pending = expected;
        auto batch = balances.batch();
        upsert(pending, "B", 8.0);
        upsert(pending, "D", 16.0);
        upsert(pending, "B", 0.5);
        check(pending);
    }
    check(expected);
    require(!balances.contains("D"), "P27 abandoned batch leaves no inserted key");

    // Explicitly nesting begin is still rejected as misuse, with no effect on
    // the outer transaction. It must never masquerade as a backing I/O error.
    {
        auto pending = expected;
        auto batch = balances.batch();
        upsert(pending, "A", 4.0);
        bool rejected = false;
        try {
            balances.begin_batch();
        } catch (const coded<std::logic_error>& error) {
            require(error.run_failure_code() == RunFailureCode::engine_invariant,
                    "P27 nested begin is an invariant failure, not scratch I/O");
            rejected = true;
        }
        require(rejected, "P27 explicit nested begin is refused");
        check(pending);
    }
    check(expected);

    // Both rollback paths leave the table ready for standalone updates.
    upsert(expected, "D", 0.875);
    check(expected);
}

} // namespace

int main() {
    try {
        close_ledger();
        pyramiding();
        auxiliary_populations();
        keyed_batch_transactions();
        std::puts("A4 ordered inventory: populations, booking and resident bounds passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A4 ordered inventory: %s\n", error.what());
        return 1;
    }
}
