// CR-150 TREE. Predicted first red on d91c96d47d: DiskIdTable has no
// bind_scratch member. The complete body must be compiled there on the lab;
// no fail-before or passing execution is claimed by this edit-only lane.
#include <pineforge/detail/disk_id_table.hpp>
#include <pineforge/detail/disk_resident_id_table.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using detail::ScratchManifest;
using detail::ScratchResource;
using detail::ScratchSlab;

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

struct Record {
    std::array<std::uint64_t, 5> words{};
    bool operator==(const Record& other) const { return words == other.words; }
};
static_assert(sizeof(Record) == 40, "the real cohort fixed-record width");
using Table = detail::DiskIdTable<Record>;

Record value(std::uint64_t n) { return {{{n, n + 1, n + 2, n + 3, n + 4}}}; }

struct Snapshot {
    std::array<std::uint64_t, 9> extents{};
    std::array<std::vector<std::uint8_t>, 9> bytes;
    bool operator==(const Snapshot& other) const {
        return extents == other.extents && bytes == other.bytes;
    }
};

void expose_zeroed_capacity(ScratchSlab& slab) {
    // A test-only readable prefix lets snapshots check the WHOLE exact region,
    // including unwritten bytes and neighbors, through the public copy API.
    for (const auto& region : slab.regions()) slab.extend(region.resource, region.capacity);
}
Snapshot snapshot(const ScratchSlab& slab) {
    Snapshot out;
    for (std::size_t i = 0; i < out.bytes.size(); ++i) {
        const auto& region = slab.regions()[i];
        out.extents[i] = region.logical_extent;
        out.bytes[i].resize(static_cast<std::size_t>(region.logical_extent));
        slab.read_at(region, 0, out.bytes[i].data(), out.bytes[i].size());
    }
    return out;
}
template<class F> void capacity_failure(const char* resource, F operation) {
    bool caught = false;
    try { operation(); }
    catch (const detail::ScratchCapacityError& error) {
        caught = true;
        const auto failure = classify_run_failure(error);
        require(failure.code == RunFailureCode::scratch_capacity_exceeded, "capacity code");
        require(run_failure_code_class(failure.code) == RunFailureClass::resource
                && !run_failure_code_retryable(failure.code), "nonretryable resource failure");
        require(failure.args
                && *failure.args == std::string("{\"resource\":\"") + resource + "\"}",
                "closed resource argument only");
    }
    require(caught, "real insertion must refuse capacity before mutation");
}

void packed_rotations_lookup_updates_and_orders() {
    ScratchManifest manifest;
    manifest.U = 7;
    manifest.B_U = 7;
    manifest.V = 1;
    manifest.B_V = 4;
    auto slab = std::make_shared<ScratchSlab>(manifest);
    Table table;
    table.set_directory("/dev/null/no-tree-fallback");
    table.bind_scratch(slab, ScratchResource::cohort);
    expose_zeroed_capacity(*slab);
    // Seven one-byte keys: Coh=1+96*7+7=680. The c,a,b prefix forces
    // a left/right rotation; the f,e,d suffix forces the opposite balancing.
    require(slab->region(ScratchResource::cohort).capacity == 680, "hand-derived Coh capacity");
    const std::array<std::string, 7> order{{"c", "a", "b", "f", "e", "d", "g"}};
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto before = snapshot(*slab);
        table.require_insert(order[i]);
        require(snapshot(*slab) == before, "successful ticket changes no bytes or extent");
        const auto row = table.insert(order[i], value(i + 10));
        require(row.second && row.first == 1 + 97 * i, "null byte then packed 56+40+1 nodes");
        require(table.find(order[i]) == row.first && table.get(row.first) == value(i + 10),
                "lookup and value after AVL rotations");
    }
    require(table.file_bytes() == 680 && !table.find("missing"), "exact fit and absent search");
    for (std::size_t i = 0; i < order.size(); ++i)
        require(table.get(*table.find(order[i])) == value(i + 10),
                "every value remains attached to its key after all rotations");
    std::vector<std::string> inserted, sorted;
    table.for_each_inserted([&](const auto& id, const auto&) { inserted.push_back(id); });
    table.for_each_sorted([&](const auto& id, const auto&) { sorted.push_back(id); });
    require(inserted == std::vector<std::string>(order.begin(), order.end()), "insertion walk");
    require(sorted == std::vector<std::string>({"a", "b", "c", "d", "e", "f", "g"}),
            "lexical walk and parent links after rotations");
    const auto before_duplicate = snapshot(*slab);
    table.require_insert("b");
    require(!table.insert("b", value(900)).second, "duplicate on full table reuses its node");
    require(snapshot(*slab) == before_duplicate, "duplicate preserves the entire physical tree");
    const auto b = *table.find("b");
    table.set(b, value(77));
    require(table.get(b) == value(77) && table.file_bytes() == 680, "overwrite never appends");
    const auto before_refusal = snapshot(*slab);
    capacity_failure("cohort", [&] { table.insert("h", value(8)); });
    require(snapshot(*slab) == before_refusal && table.size() == 7 && !table.find("h"),
            "overflow changes no region byte, link, cursor, count or neighbor");

    const std::weak_ptr<ScratchSlab> lifetime = slab;
    slab.reset();
    Table moved(std::move(table));
    require(!lifetime.expired() && table.size() == 0 && moved.get(b) == value(77),
            "move retains a copied slab owner, with no dangling byte view");
    moved.clear();
    require(moved.size() == 0 && moved.insert("fresh", value(1)).second,
            "clear keeps the bounded binding despite an impossible disk directory");
    moved.clear();
    moved.bind_scratch({}, ScratchResource::cohort);
    require(lifetime.expired(), "empty owner detaches the old slab after reset");
    bool disk_failed = false;
    try { moved.insert("disk", value(2)); }
    catch (const detail::AuxiliaryScratchIoError& error) {
        disk_failed = classify_run_failure(error).code == RunFailureCode::scratch_io_failed;
    }
    require(disk_failed, "empty owner really restores the ordinary disk path");
}

void one_excess_byte_count_tickets_binary_keys_and_rank() {
    ScratchManifest manifest;
    manifest.U = 2;
    manifest.B_U = 3;
    manifest.V = 2;
    manifest.B_V = 3;
    auto slab = std::make_shared<ScratchSlab>(manifest);
    Table table;
    table.bind_scratch(slab, ScratchResource::cohort);
    detail::DiskIdTable<std::uint32_t> ranks;
    ranks.bind_scratch(slab, ScratchResource::exit_rank);
    expose_zeroed_capacity(*slab);
    require(table.insert("a", value(1)).second, "first Coh row");
    const auto before = snapshot(*slab);
    // 1+96*2+3=196; a (1 byte) then bbb (3) needs 197: exactly
    // ONE excess byte, although the physical page still has room.
    capacity_failure("cohort", [&] { table.require_insert("bbb"); });
    capacity_failure("cohort", [&] { table.insert("bbb", value(2)); });
    require(snapshot(*slab) == before && table.size() == 1, "one-byte overrun is wholly read-only");
    const std::string binary("b\0", 2);
    const auto second = table.insert(binary, value(2));
    require(second.second && table.find(binary) == second.first && !table.find("b")
            && table.file_bytes() == 196, "length-based binary key fits exact packed extent");
    const auto rank0 = ranks.insert("a", 0);
    require(rank0.second && rank0.first == 1, "Rank reserves byte zero too");
    const auto rank_before = snapshot(*slab);
    capacity_failure("exit_rank", [&] { ranks.insert("bbb", 1); });
    require(snapshot(*slab) == rank_before, "Rank byte refusal preserves both trees");
    const auto rank1 = ranks.insert(binary, 1);
    // Rank=1+60*2+3=124; the second unaligned node starts at 62.
    require(rank1.second && rank1.first == 62 && ranks.file_bytes() == 124,
            "uint32 Rank values use the 60-byte packed fixed portion");
    ranks.set(rank1.first, 19);
    require(ranks.get(rank1.first) == 19, "unaligned uint32 value uses checked copies");

    ScratchManifest row_manifest;
    row_manifest.U = 1;
    row_manifest.B_U = 1024;
    auto rows = std::make_shared<ScratchSlab>(row_manifest);
    Table one_row;
    one_row.bind_scratch(rows, ScratchResource::cohort);
    expose_zeroed_capacity(*rows);
    one_row.insert("a", value(1));
    const auto row_before = snapshot(*rows);
    capacity_failure("cohort", [&] { one_row.insert("b", value(2)); });
    require(snapshot(*rows) == row_before, "U is enforced even when another header+key fits");

    auto empty = std::make_shared<ScratchSlab>(ScratchManifest{});
    Table zero;
    zero.bind_scratch(empty, ScratchResource::cohort);
    capacity_failure("cohort", [&] { zero.insert("", value(0)); });
    require(zero.size() == 0 && zero.file_bytes() == 0
            && empty->region(ScratchResource::cohort).logical_extent == 0,
            "U=0 cannot spend a reserved null byte or publish an empty key");
}

struct Facts {
    using Record = ::Record;
    Record row{};
    Record retired_record() const noexcept { return row; }
    static Facts restore_record(const Record& record) { return {record}; }
    bool spillable() const noexcept { return true; }
};

void resident_refusal_precedes_writeback_and_eviction() {
    ScratchManifest manifest;
    manifest.U = 2;
    manifest.B_U = 2; // 1+96*2+2=195; only a and b together fit.
    auto slab = std::make_shared<ScratchSlab>(manifest);
    detail::DiskResidentIdTable<Facts> table;
    table.set_directory("/dev/null/no-resident-fallback");
    table.bind_scratch(slab, ScratchResource::cohort);
    expose_zeroed_capacity(*slab);
    {
        const auto first = table.emplace("a", Facts{value(1)}).first;
        first->second.row = value(9); // Deliberately dirty, not written back yet.
        const auto nested = table.find("a");
        require(&first->second == &nested->second, "nested lookup keeps shared-row pin identity");
        table.spill();
        require(table.resident_size() == 1, "a pinned row cannot be evicted");
    }
    const auto before = snapshot(*slab);
    capacity_failure("cohort", [&] { table.emplace("bb", Facts{value(2)}); });
    require(snapshot(*slab) == before && table.size() == 1 && table.resident_size() == 1,
            "emplace ticket precedes dirty writeback, eviction and all index changes");
    require(table.find_resident("a")->second.row == value(9), "refusal retains the physical live row");
    table.emplace("b", Facts{value(2)});
    table.spill();
    require(table.resident_size() == 0, "successful insertion retains ordinary spill policy");
    require(table.find("a")->second.row == value(9), "spilled facts restore their latest record");
    table.spill();
    {
        const auto a = table.find("a");
        const auto b = table.find("b");
        require(a != table.end() && b != table.end(), "pin both restored resident rows");
        std::vector<std::string> newest, insertion;
        for (const auto& row : table) newest.push_back(row.first);
        for (const auto& id : table.resident_ids()) insertion.push_back(id);
        require(newest == std::vector<std::string>({"b", "a"})
                && insertion == std::vector<std::string>({"a", "b"}),
                "resident walk is newest first, resident_ids is first insertion order");
    }
    table.spill();
    std::vector<std::string> ids;
    table.for_each_sorted_id([&](const auto& id) { ids.push_back(id); });
    require(ids == std::vector<std::string>({"a", "b"}) && table.resident_size() == 0,
            "every logical id, including spilled ids, walks without restoring rows");
}

void complete_borrowed_workset_is_one_read_only_ticket() {
    ScratchManifest manifest;
    manifest.U = 3;
    manifest.B_U = 4;
    auto slab = std::make_shared<ScratchSlab>(manifest);
    Table table;
    table.bind_scratch(slab, ScratchResource::cohort);
    expose_zeroed_capacity(*slab);
    table.insert("a", value(1));
    const auto before = snapshot(*slab);
    const std::array<std::string, 4> refused{{"a", "b", "b", "ccc"}};
    capacity_failure("cohort", [&] {
        table.require_inserts([&](auto&& visit) {
            for (const auto& key : refused) visit(key);
        });
    });
    // a already exists; b is repeated. The whole workset needs a+b+ccc =
    // FIVE key bytes, one more than B_U=4. Checking only b would succeed.
    require(snapshot(*slab) == before && table.size() == 1 && !table.find("b"),
            "later-key failure publishes no prefix of the borrowed workset");
    const std::array<std::string, 4> fits{{"a", "b", "b", "cc"}};
    table.require_inserts([&](auto&& visit) {
        for (const auto& key : fits) visit(key);
    });
    require(snapshot(*slab) == before, "successful multi-key ticket is read-only too");
    table.insert("b", value(2));
    table.insert("cc", value(3));
    // Three headers and 1+1+2 bytes: Coh=1+96*3+4=293 exactly.
    require(table.size() == 3 && table.file_bytes() == 293,
            "workset deduplicates existing and repeated ids without extra rows or bytes");
}
} // namespace

int main() {
    try {
        packed_rotations_lookup_updates_and_orders();
        one_excess_byte_count_tickets_binary_keys_and_rank();
        resident_refusal_precedes_writeback_and_eviction();
        complete_borrowed_workset_is_one_read_only_ticket();
        std::puts("test_disk_id_table_ram: packed trees, orders and capacity tickets");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_disk_id_table_ram: %s\n", error.what());
        return 1;
    }
}
