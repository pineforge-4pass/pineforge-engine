#include <pineforge/source/pine_adapter.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
using pineforge::source::PlacementSnapshot;
using pineforge::source::PlacementTable;

struct OldOnlyTag {
    using type = bool PlacementTable::*;
    friend type access(OldOnlyTag);
};
template<class Tag, typename Tag::type Member>
struct PrivateAccess {
    friend typename Tag::type access(Tag) { return Member; }
};
template struct PrivateAccess<OldOnlyTag, &PlacementTable::old_only_>;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

PlacementSnapshot snapshot(std::uint64_t serial) {
    PlacementSnapshot row;
    row.family = serial % 3 == 0 ? pineforge::source::PineOrderFamily::Entry
                               : pineforge::source::PineOrderFamily::Order;
    row.source_id = "book-" + std::to_string(serial % 5);
    row.from_entry = serial % 2 ? "" : "entry-" + std::to_string(serial % 7);
    row.comment = "row-" + std::to_string(serial);
    row.oca_name = "group-" + std::to_string(serial % 3);
    row.oca_type = static_cast<int>(serial % 3);
    row.requested_qty = static_cast<double>(serial + 1);
    row.is_long = serial % 2 == 0;
    row.opening = serial % 3 == 0;
    row.exit_levels.limit = static_cast<double>(serial + 2);
    row.command_sequence = serial + 11;
    return row;
}

auto contents(const PlacementSnapshot& row) {
    return std::tie(row.family, row.source_id, row.from_entry, row.comment,
                    row.oca_name, row.oca_type, row.requested_qty, row.is_long,
                    row.opening, row.exit_levels.limit, row.command_sequence);
}

// Recompute the erased identity stream from the model's retirement log.
// No production hash helper or table-derived expected value is used.
std::uint64_t erased_digest(const std::vector<PlacementSnapshot>& erased) {
    std::uint64_t result = 1469598103934665603ULL;
    const auto bytes = [&](const void* data, std::size_t count) {
        const auto* first = static_cast<const unsigned char*>(data);
        for (std::size_t index = 0; index != count; ++index)
            result = (result ^ first[index]) * 1099511628211ULL;
    };
    const auto text = [&](const std::string& value) {
        const std::uint64_t count = value.size();
        bytes(&count, sizeof count);
        bytes(value.data(), value.size());
    };
    for (const auto& row : erased) {
        const auto family = static_cast<std::uint64_t>(row.family);
        bytes(&family, sizeof family);
        text(row.source_id);
        text(row.from_entry);
        bytes(&row.command_sequence, sizeof row.command_sequence);
    }
    return result;
}

class Differential {
public:
    Differential() { table.fixture_use_memory_auxiliary_tables(); check(); }

    void put(std::uint64_t key) {
        const auto row = snapshot(++serial);
        const auto expected = model.emplace(key, row);
        const auto actual = table.try_emplace(key, row);
        require(actual.second == expected.second, "insert/duplicate result differs");
        require(actual.first->first == expected.first->first
                    && contents(actual.first->second) == contents(expected.first->second),
                "insert returns a different row");
        high_water = std::max(high_water, key);
        check();
    }

    void erase(std::uint64_t key) {
        const auto found = model.find(key);
        require(table.erase(key) == (found != model.end()), "erase result differs");
        if (found != model.end()) {
            erased.push_back(found->second);
            model.erase(found);
        }
        check();
    }

    void compact() { table.compact(); check(); }

    void query(std::uint64_t key) {
        const PlacementTable& rows = table;
        const auto expected = model.find(key);
        const auto found = rows.find(key);
        require((found == rows.end()) == (expected == model.end()), "const find differs");
        const auto mutable_found = table.find(key);
        require((mutable_found == table.end()) == (expected == model.end()),
                "mutable find differs");
        require(rows.contains(key) == (expected != model.end()), "contains differs");
        if (expected != model.end()) {
            require(contents(found->second) == contents(expected->second), "find content differs");
            require(contents(mutable_found->second) == contents(expected->second),
                    "mutable find content differs");
            require(contents(rows.at(key)) == contents(expected->second), "const at differs");
            require(contents(table.at(key)) == contents(expected->second), "mutable at differs");
        } else {
            bool const_threw = false, mutable_threw = false;
            try { (void)rows.at(key); } catch (const std::out_of_range&) { const_threw = true; }
            try { (void)table.at(key); } catch (const std::out_of_range&) { mutable_threw = true; }
            require(const_threw && mutable_threw, "missing at must throw");
        }
        const auto next = rows.upper_bound(key);
        const auto model_next = model.upper_bound(key);
        require((next == rows.end()) == (model_next == model.end()), "next-after end differs");
        if (model_next != model.end())
            require(next->first == model_next->first
                        && contents(next->second) == contents(model_next->second),
                    "next-after row differs");
    }

    void check() {
        ++checks;
        const PlacementTable& rows = table;
        require(rows.size() == model.size(), "size differs");
        require(rows.high_water() == high_water, "high-water differs");
        require(rows.erased_rows() == erased.size(), "erased count differs");
        require(rows.erased_digest() == erased_digest(erased), "erased digest differs");
        auto expected = model.begin();
        for (const auto& actual : rows) {
            require(expected != model.end(), "iteration has an extra row");
            require(actual.first == expected->first
                        && contents(actual.second) == contents(expected->second),
                    "iteration order/content differs");
            ++expected;
        }
        require(expected == model.end(), "iteration missed a row");
        expected = model.begin();
        rows.visit_rows([&](std::uint64_t key, const PlacementSnapshot& row) {
            require(expected != model.end(), "visit has an extra row");
            require(key == expected->first && contents(row) == contents(expected->second),
                    "visit order/content differs");
            ++expected;
            return false;
        });
        require(expected == model.end(), "visit missed a row");
        std::size_t stopped = 0;
        rows.visit_rows([&](std::uint64_t key, const PlacementSnapshot& row) {
            require(key == model.begin()->first
                        && contents(row) == contents(model.begin()->second),
                    "short visit differs");
            ++stopped;
            return true;
        });
        require(stopped == (model.empty() ? 0U : 1U), "visit did not stop");
        for (const auto& entry : model) {
            query(entry.first);
            query(entry.first - 1U);
        }
        query(0);
        query(high_water + 1U);
        query(std::numeric_limits<std::uint64_t>::max());
    }

    void drain() {
        while (!model.empty()) erase(model.begin()->first);
        compact();
        require(!old_only(), "empty compaction did not leave ordered storage");
    }

    bool old_only() const { return table.*access(OldOnlyTag{}); }

    void clear() {
        table.clear();
        model.clear();
        erased.clear();
        high_water = 0;
        check();
        require(!old_only(), "clear did not reset storage mode");
    }

    PlacementTable table;
    std::map<std::uint64_t, PlacementSnapshot> model;
    std::vector<PlacementSnapshot> erased;
    std::uint64_t high_water = 0, serial = 0, checks = 0;
};

void differential() {
    Differential run;
    // Dense inserts on both sides of the former 256-slot trigger.
    for (std::uint64_t key = 1; key <= 258; ++key) {
        run.put(key);
        require(!run.old_only(), "dense boundary insert converted");
    }
    run.erase(1);
    run.compact(); // A nonzero physical head must not be treated as a gap.
    run.put(259);
    require(!run.old_only(), "dense window with a retired prefix converted");
    for (std::uint64_t key = 3; key <= 259; ++key) run.erase(key);
    run.put(257);
    require(run.old_only(), "sparse 255-offset insert did not convert");
    run.put(258);
    run.put(256); // Non-monotone middle insertion in ordered storage.
    run.put(257); // Duplicate must not change the row or history.
    run.erase(257);
    run.drain();

    // Also retain a sorted prefix alongside a restarted dense window.
    for (std::uint64_t key = 1000; key != 1128; ++key) run.put(key);
    for (std::uint64_t key = 1001; key != 1127; ++key) run.erase(key);
    run.compact();
    run.put(999);
    require(!run.old_only(), "insertion below the dense window changed its mode");
    run.put(1400);
    require(run.old_only(), "sparse conversion with a retained prefix did not occur");
    run.drain();

    std::mt19937_64 random(0x5a17d3e4ULL);
    for (std::uint64_t cycle = 0; cycle != 3; ++cycle) {
        const std::uint64_t base = 10000U * (cycle + 1U);
        run.put(base);
        run.put(base + 257U);
        require(run.old_only(), "sparse 257-offset re-entry did not convert");
        run.put(base + 256U);
        run.put(base - 1U);
        for (unsigned step = 0; step != 240; ++step) {
            const auto key = base - 32U + random() % 640U;
            switch (random() % 6U) {
            case 0: case 1: run.put(key); break;
            case 2: run.erase(key); break;
            case 3:
                if (!run.model.empty()) {
                    auto found = run.model.begin();
                    std::advance(found, static_cast<std::ptrdiff_t>(random() % run.model.size()));
                    run.erase(found->first);
                } else run.erase(key);
                break;
            case 4: run.query(key); run.check(); break;
            default: run.compact(); break;
            }
        }
        run.drain();
    }
    run.put(90000);
    run.put(90256);
    require(run.old_only(), "sparse 256-offset insert did not convert");
    run.clear();
    run.put(7);
    require(!run.old_only(), "fresh dense window did not restart after clear");
    std::printf("placement-table differential seed=0x5a17d3e4 checks=%llu\n",
                static_cast<unsigned long long>(run.checks));
}
} // namespace

int main() {
    try {
        differential();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_placement_table_differential: %s\n", error.what());
        return 1;
    }
}
