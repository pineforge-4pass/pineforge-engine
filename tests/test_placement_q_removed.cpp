// CR150 q removal. Predicted red on d91c96d47d: PlacementTable has no
// bind_auxiliary_scratch_tables member. The disk-only query witness also
// fails there: its retained lookup tries to open q in a missing directory.
#include <pineforge/source/pine_adapter.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using source::PlacementSnapshot;
using source::PlacementTable;
using detail::ScratchManifest;
using detail::ScratchResource;
using detail::ScratchSlab;
constexpr auto kAbsent = std::numeric_limits<std::uint64_t>::max();

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

// The independent nested minima/reference algorithm from
// test_adapter_auxiliary_disk.cpp: retained scan plus indexed erased minima.
// The query materializes a test-only index, never the production store.
class Reference {
    struct Minima {
        std::uint64_t any = kAbsent;
        std::map<std::string, std::uint64_t> pairs;
    };
    using Index = std::map<std::string, Minima>;
    static void fold(Index& index, const PlacementSnapshot& row) {
        auto& minimum = index[row.source_id];
        minimum.any = std::min(minimum.any, row.command_sequence);
        const auto inserted = minimum.pairs.emplace(row.from_entry, row.command_sequence);
        if (!inserted.second) inserted.first->second = std::min(inserted.first->second, row.command_sequence);
    }
public:
    void put(std::uint64_t incarnation, const PlacementSnapshot& row) { retained.emplace(incarnation, row); }
    void erase(std::uint64_t incarnation) { fold(erased_, retained.at(incarnation)); retained.erase(incarnation); }
    void replace(std::uint64_t incarnation, const PlacementSnapshot& row) { retained.at(incarnation) = row; }
    std::uint64_t query(const std::string& id, const std::string& from) const {
        auto index = erased_;
        for (const auto& row : retained) fold(index, row.second);
        const auto found = index.find(id);
        if (found == index.end()) return kAbsent;
        const auto pair = found->second.pairs.find(from);
        if (pair != found->second.pairs.end() && pair->second != kAbsent) return pair->second;
        return found->second.any;
    }
    std::map<std::uint64_t, PlacementSnapshot> retained;
private:
    Index erased_;
};

PlacementSnapshot row(std::string id, std::string from, std::uint64_t sequence) {
    PlacementSnapshot result;
    result.source_id = std::move(id);
    result.from_entry = std::move(from);
    result.command_sequence = sequence;
    return result;
}

void scan_reference(bool bounded) {
    PlacementTable table;
    auto slab = std::shared_ptr<ScratchSlab>{};
    if (bounded) {
        ScratchManifest manifest;
        // Three rows retired below need no more than six keys and their
        // single+pair bytes. q is deliberately absent from the manifest.
        manifest.K_e = 6;
        manifest.B_e = 128;
        slab = std::make_shared<ScratchSlab>(manifest);
        table.bind_auxiliary_scratch_tables(slab);
    } else {
        // No disk access is permissible for a retained-only query. This is
        // an invalid descendant of a file, independent of TMPDIR contents.
        table.set_auxiliary_tables_directory("/dev/null/pineforge-q-must-not-open");
    }
    Reference reference;
    const auto insert = [&](std::uint64_t incarnation, const PlacementSnapshot& value) {
        table.try_emplace(incarnation, value);
        reference.put(incarnation, value);
    };
    const auto query = [&](const std::string& id, const std::string& from) {
        require(table.command_sequence_for(id, from) == reference.query(id, from),
                "scan/e union equals the independent nested indexed answer");
    };
    insert(1, row("early", "pair", 77));
    insert(2, row("early", "other", 3));
    insert(3, row("early", "max", kAbsent));
    insert(4, row("x", "", kAbsent));
    query("early", "pair"); // Exact pair wins even though any=3.
    for (std::uint64_t incarnation = 5; incarnation < 405; ++incarnation)
        insert(incarnation, row("id-" + std::to_string(incarnation), "from", incarnation));
    query("early", "pair"); // Early pair survives many unrelated ids.
    query("early", "max"); // Explicit MAX pair falls back to any=3.
    query("early", "unknown");
    query("x", "");
    query("unknown", "");
    require(table.auxiliary_disk_bytes() == 0, "queries retain no q data/index files");
    if (slab) {
        require(slab->region(ScratchResource::sequence_data).capacity == 0
                && slab->region(ScratchResource::sequence_index).capacity == 0
                && slab->region(ScratchResource::erased_data).logical_extent == 0,
                "q-empty layout remains untouched by scans of retained placements");
    }
    auto replacement = row("new-id", "new-from", 1);
    reference.replace(1, replacement);
    table.replace(1, std::move(replacement));
    query("early", "pair");
    query("new-id", "new-from");
    query("early", "max");
    require(table.auxiliary_disk_bytes() == 0, "replace has no q reset or lazy refold writes");
    if (!bounded) table.set_auxiliary_tables_directory("");
    for (const auto incarnation : {1ULL, 3ULL, 4ULL}) {
        require(table.erase(incarnation), "erase real row into e");
        reference.erase(incarnation);
    }
    table.compact();
    query("new-id", "new-from");
    query("early", "max");
    query("x", "");
    insert(405, row("x", "other", 9));
    query("x", ""); // Erased MAX exact pair still falls back to retained 9.
    require(table.erased_rows() == 3 && (bounded || table.auxiliary_disk_bytes() > 0),
            "e retains erased history and disk mode still writes e");
}

using Bytes = std::array<std::vector<std::uint8_t>, detail::kScratchRegionCount>;
Bytes image(const ScratchSlab& slab) {
    Bytes result;
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto& region = slab.regions()[i];
        result[i].resize(static_cast<std::size_t>(region.logical_extent));
        slab.read_at(region.resource, 0, result[i].data(), result[i].size());
    }
    return result;
}

void retirement_ticket(std::uint64_t keys, std::uint64_t bytes, const char* expected_failure_args) {
    ScratchManifest manifest;
    manifest.K_e = keys;
    manifest.B_e = bytes;
    auto slab = std::make_shared<ScratchSlab>(manifest);
    PlacementTable table, disk;
    table.bind_auxiliary_scratch_tables(slab);
    for (std::uint64_t incarnation = 1; incarnation <= 3; ++incarnation) {
        const auto value = row("x", incarnation == 3 ? "b" : "a", 100 - incarnation);
        table.try_emplace(incarnation, value);
        disk.try_emplace(incarnation, value);
    }
    for (const auto& region : slab->regions()) slab->extend(region.resource, region.capacity);
    const auto before = image(*slab);
    const auto digest = table.erased_digest();
    const auto reserve_all = [&] {
        table.preflight_erased_rows([&](const auto& emit) {
            table.visit_rows([&](std::uint64_t, const PlacementSnapshot& value) {
                emit(value);
                return false;
            });
        });
    };
    if (expected_failure_args) {
        bool caught = false;
        try { reserve_all(); }
        catch (const detail::ScratchCapacityError& error) {
            const auto failure = classify_run_failure(error);
            // K=3,B=4: limit 124=40*3+4, need 125=41+42+42; K=2: limit 2, need 3 distinct keys.
            caught = failure.code == RunFailureCode::scratch_capacity_exceeded && failure.args
                && *failure.args == expected_failure_args;
        }
        require(caught && image(*slab) == before && table.size() == 3
                && table.erased_rows() == 0 && table.erased_digest() == digest,
                "whole retirement ticket refuses before its first single/pair fold or erasure");
        return;
    }
    reserve_all();
    require(image(*slab) == before, "successful whole-workset ticket is read-only");
    for (std::uint64_t incarnation = 1; incarnation <= 3; ++incarnation) {
        require(table.erase(incarnation) && disk.erase(incarnation), "erase admitted rows");
        require(table.erased_digest() == disk.erased_digest(), "erasure-point digest bytes stay identical");
    }
    require(table.command_sequence_for("x", "a") == disk.command_sequence_for("x", "a")
            && table.command_sequence_for("x", "b") == disk.command_sequence_for("x", "b"),
            "deduplicated keys retain both exact minima");
}

void individual_erase_and_replace() {
    {
        auto slab = std::make_shared<ScratchSlab>(ScratchManifest{});
        PlacementTable table;
        table.bind_auxiliary_scratch_tables(slab);
        table.try_emplace(1, row("before", "", 5));
        require(table.command_sequence_for("before", "") == 5, "retained query needs no history budget");
        table.replace(1, row("after", "new", 7));
        require(table.command_sequence_for("before", "") == kAbsent
                && table.command_sequence_for("after", "new") == 7
                && table.erased_rows() == 0 && table.auxiliary_disk_bytes() == 0,
                "replace has an empty refold workset even with all scratch regions empty");
    }
    for (const bool index_failure : {true, false}) {
        ScratchManifest manifest;
        manifest.K_e = index_failure ? 1 : 2;
        manifest.B_e = index_failure ? 2 : 1;
        // For row (x,""): two distinct domains, bytes=1+1. K=1 refuses
        // I_e; K=2,B=1 gives D=81 instead of the necessary 82, refusing D_e.
        auto slab = std::make_shared<ScratchSlab>(manifest);
        PlacementTable table;
        table.bind_auxiliary_scratch_tables(slab);
        table.try_emplace(1, row("old", "", 8));
        table.replace(1, row("x", "", kAbsent));
        for (const auto& region : slab->regions()) slab->extend(region.resource, region.capacity);
        const auto before = image(*slab);
        const auto digest = table.erased_digest();
        bool caught = false;
        try { table.erase(1); }
        catch (const detail::ScratchCapacityError& error) {
            const auto failure = classify_run_failure(error);
            // Index: limit 1, need 2 domains; data: limit 81=40*2+1, need 82=(40+1)*2 for x and (x,"").
            caught = failure.args && *failure.args == (index_failure
                ? "{\"limit\":1,\"need\":2,\"resource\":\"erased_index\"}"
                : "{\"limit\":81,\"need\":82,\"resource\":\"erased_data\"}");
        }
        require(caught && image(*slab) == before && table.size() == 1 && table.contains(1)
                && table.at(1).source_id == "x" && table.erased_rows() == 0
                && table.erased_digest() == digest && table.command_sequence_for("x", "") == kAbsent,
                "both minima are reserved before taking a replaced row, even an explicit MAX row");
    }
}
} // namespace

int main() {
    try {
        scan_reference(false);
        scan_reference(true);
        individual_erase_and_replace();
        // Unique workset keys: x, (x,a), (x,b). Duplicate (x,a) rows share
        // storage. K=3,B=5 gives D=120+5=125 exactly; B=4 is one byte short.
        retirement_ticket(3, 5, nullptr);
        // limit 124=40*3+4, need 125=(40+1)+(40+1+1)+(40+1+1): x, (x,a), (x,b).
        retirement_ticket(3, 4, "{\"limit\":124,\"need\":125,\"resource\":\"erased_data\"}");
        // limit 2=K_e, need 3: x and (x,a) fit, then the distinct (x,b) needs a third slot.
        retirement_ticket(2, 5, "{\"limit\":2,\"need\":3,\"resource\":\"erased_index\"}");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_placement_q_removed: %s\n", error.what());
        return 1;
    }
}
