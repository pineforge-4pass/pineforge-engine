// Every refusal row uses APIs present before this patch. On the unchanged
// head it must compile and fail at the numeric-args assertion, not at setup.
// Expected quantities below are hand-derived from the input shapes.
#include <pineforge/detail/disk_resident_id_table.hpp>
#include <pineforge/detail/id_pair_store.hpp>
#include <pineforge/detail/o1_store.hpp>
#include <pineforge/native_order.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace pd = pineforge::detail;
namespace no = pineforge::native_order;
using pd::ScratchManifest;
using pd::ScratchResource;
using pd::ScratchSlab;

void require(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}

template<class Operation>
void refuses(const char* resource, std::uint64_t limit, std::uint64_t need,
             Operation operation) {
    try {
        operation();
    } catch (const pd::ScratchCapacityError& error) {
        const auto failure = pineforge::classify_run_failure(error);
        require(failure.code == pineforge::RunFailureCode::scratch_capacity_exceeded,
                "typed scratch capacity code");
        // Canonical key order also proves the quantities are JSON integers.
        const auto expected = "{\"limit\":" + std::to_string(limit)
            + ",\"need\":" + std::to_string(need) + ",\"resource\":\"" + resource + "\"}";
        require(failure.args && *failure.args == expected,
                "hand-derived resource, limit and need");
        return;
    }
    throw std::runtime_error("operation must reach its real capacity refusal");
}

void object_count() {
    pd::o1::Shape shape;
    shape.resident_pages = 4;
    shape.chunk_bytes = 16;
    shape.max_live_objects = 3;
    std::vector<std::byte> bytes(static_cast<std::size_t>(shape.page_bytes) * 128);
    pd::FixedMemoryScratchBacking backing({bytes.data(), bytes.size()}, shape.page_bytes);
    auto store = pd::o1::Store::open(shape, backing);
    for (int i = 0; i < 3; ++i)
        require(store->create(0, 1) != pd::o1::kNullId, "three objects admitted");
    refuses("store_objects", 3, 4, [&] { store->create(0, 1); });
    require(store->stats().objects_live == 3, "refused create consumes no live object");
    // A real successful retry at the reported need.
    shape.max_live_objects = 4;
    std::vector<std::byte> retry_bytes(static_cast<std::size_t>(shape.page_bytes) * 128);
    pd::FixedMemoryScratchBacking retry_backing(
        {retry_bytes.data(), retry_bytes.size()}, shape.page_bytes);
    auto retry = pd::o1::Store::open(shape, retry_backing);
    for (int i = 0; i < 4; ++i)
        require(retry->create(0, 1) != pd::o1::kNullId, "need four admits fourth create");
}

void id_pair() {
    using Store = pd::IdPairStore<std::uint64_t>;
    ScratchManifest manifest{};
    auto empty_slab = std::make_shared<ScratchSlab>(manifest);
    Store empty;
    empty.bind_scratch_tables(empty_slab,
        ScratchResource::erased_data, ScratchResource::erased_index);
    refuses("erased_index", 0, 1, [&] { empty.insert_or_assign("", 1); });
    manifest.K_e = 1;
    manifest.B_e = 0; // One 40-byte header; "a" requires 41 region bytes.
    auto slab = std::make_shared<ScratchSlab>(manifest);
    Store table;
    table.bind_scratch_tables(slab, ScratchResource::erased_data, ScratchResource::erased_index);
    refuses("erased_data", 40, 41, [&] { table.insert_or_assign("a", 1); });
    require(table.size() == 0, "byte refusal precedes publication");
    table.insert_or_assign("", 1);
    refuses("erased_index", 1, 2, [&] { table.insert_or_assign("b", 2); });

    manifest.K_e = 2;
    manifest.B_e = 1; // 81 bytes. Existing a=41; new pair b,c=42 -> 83.
    auto pair_slab = std::make_shared<ScratchSlab>(manifest);
    Store pair;
    pair.bind_scratch_tables(pair_slab, ScratchResource::erased_data, ScratchResource::erased_index);
    pair.insert_or_assign("a", 1);
    refuses("erased_data", 81, 83, [&] { pair.insert_or_assign("b", "c", 2); });
    // The first string append fails; need must still include the second.
    manifest.B_e = 3;
    auto retry_slab = std::make_shared<ScratchSlab>(manifest);
    Store retry;
    retry.bind_scratch_tables(retry_slab, ScratchResource::erased_data, ScratchResource::erased_index);
    retry.insert_or_assign("a", 1);
    require(retry.insert_or_assign("b", "c", 2), "83-byte retry admits whole pair");
    refuses("erased_data", 64, 65, [&] { retry.preflight_assign(std::string(65, 'x')); });
    refuses("erased_data", 64, 66,
            [&] { retry.preflight_assign("a", std::string(66, 'x')); });

    ScratchManifest consumed_manifest{};
    consumed_manifest.K_c = 1;
    consumed_manifest.B_c = 2; // 42 bytes; a + separator + b requires 43.
    auto consumed_slab = std::make_shared<ScratchSlab>(consumed_manifest);
    pd::IdPairStore<std::int64_t, true> consumed;
    consumed.bind_scratch_tables(consumed_slab,
        ScratchResource::consumed_data, ScratchResource::consumed_index);
    refuses("consumed_data", 42, 43,
            [&] { consumed.preflight_joined_single("a", '\x1f', "b"); });
    consumed.insert_or_assign("a", 1);
    refuses("consumed_index", 1, 2, [&] { consumed.insert_or_assign("b", 2); });
}

using CohortRecord = std::array<std::uint64_t, 5>; // 40 bytes, fixed node=96.

void disk_id() {
    ScratchManifest manifest{};
    auto empty_slab = std::make_shared<ScratchSlab>(manifest);
    pd::DiskIdTable<CohortRecord> empty;
    empty.bind_scratch(empty_slab, ScratchResource::cohort);
    refuses("cohort", 0, 1, [&] { empty.insert("", {}); });
    manifest.U = 1;
    manifest.B_U = 0;
    manifest.V = 1;
    manifest.B_V = 0;
    auto slab = std::make_shared<ScratchSlab>(manifest);
    pd::DiskIdTable<CohortRecord> cohorts;
    cohorts.bind_scratch(slab, ScratchResource::cohort);
    refuses("cohort", 0, 1, [&] { cohorts.insert("a", {}); });
    cohorts.insert("", {});
    refuses("cohort", 1, 2, [&] { cohorts.insert("b", {}); });
    refuses("cohort", 64, 65, [&] { cohorts.require_insert(std::string(65, 'x')); });
    pd::DiskIdTable<std::uint32_t> ranks;
    ranks.bind_scratch(slab, ScratchResource::exit_rank);
    refuses("exit_rank", 0, 1, [&] { ranks.insert("a", 0); });
    ranks.insert("", 0);
    refuses("exit_rank", 1, 2, [&] { ranks.insert("b", 0); });

    // Exercise physical-region guards independently of the declared key-byte
    // budget: DiskIdTable accepts other trivially copyable record widths.
    pd::DiskIdTable<std::array<std::uint64_t, 6>> wide;
    auto wide_slab = std::make_shared<ScratchSlab>(manifest);
    wide.bind_scratch(wide_slab, ScratchResource::cohort);
    // Region=1+96=97; actual node=56+48=104 plus null byte=105.
    refuses("cohort", 97, 105, [&] { wide.insert("", {}); });
    manifest.B_U = 8;
    pd::DiskIdTable<std::array<std::uint32_t, 11>> narrow;
    auto narrow_slab = std::make_shared<ScratchSlab>(manifest);
    narrow.bind_scratch(narrow_slab, ScratchResource::cohort);
    // Region=1+96+8=105; node=56+44=100, key=8 -> 109.
    refuses("cohort", 105, 109, [&] { narrow.insert("abcdefgh", {}); });

    ScratchManifest batch_manifest{};
    batch_manifest.U = 1;
    batch_manifest.B_U = 8;
    auto batch_slab = std::make_shared<ScratchSlab>(batch_manifest);
    pd::DiskIdTable<CohortRecord> batch;
    batch.bind_scratch(batch_slab, ScratchResource::cohort);
    refuses("cohort", 1, 2, [&] {
        batch.require_inserts([](const auto& emit) { emit("a"); emit("b"); });
    });
    batch_manifest.U = 2;
    batch_manifest.B_U = 1;
    auto bytes_slab = std::make_shared<ScratchSlab>(batch_manifest);
    pd::DiskIdTable<CohortRecord> batch_bytes;
    batch_bytes.bind_scratch(bytes_slab, ScratchResource::cohort);
    refuses("cohort", 1, 2, [&] {
        batch_bytes.require_inserts([](const auto& emit) { emit("a"); emit("b"); });
    });
}

struct Facts {
    using Record = CohortRecord;
    Record retired_record() const { return {}; }
    static Facts restore_record(const Record&) { return {}; }
    bool spillable() const { return false; }
};

void resident_cohort() {
    ScratchManifest manifest{};
    manifest.U = 4097;
    manifest.B_U = 32776; // More than the bytes in all 4097 "k<number>" keys.
    auto slab = std::make_shared<ScratchSlab>(manifest);
    pd::DiskResidentIdTable<Facts> table;
    table.bind_scratch(slab, ScratchResource::cohort);
    std::vector<std::string> keys;
    for (int i = 0; i < 4097; ++i) keys.push_back("k" + std::to_string(i));
    // A declared batch of 4097 live rows on an empty table exceeds 4096.
    refuses("cohort", 4096, 4097, [&] {
        table.require_inserts([&](const auto& emit) {
            for (const auto& key : keys) emit(key);
        });
    });
    require(table.size() == 0, "declared-count refusal is read-only");
    for (int i = 0; i < 4096; ++i)
        require(table.emplace(keys[i], Facts{}).second, "resident setup admits 4096 live rows");
    refuses("cohort", 4096, 4097, [&] { table.emplace(keys.back(), Facts{}); });
    require(table.size() == 4096, "attempted-count refusal preserves live residents");
}

void roots() {
    ScratchManifest manifest{};
    manifest.R = 1;
    auto slab = std::make_shared<ScratchSlab>(manifest);
    no::WorkingRequestCore core{{"capacity-args", 1}};
    core.bind_scratch_slab(slab);
    no::Request request{no::Transact{1.0}, "moving", ""};
    request.trigger = no::Limit{90.0};
    no::CommandContext context{};
    std::uint64_t incarnation = 1, ordinal = 1;
    const auto submitted = core.apply_submit(request, context, incarnation, ordinal).result;
    require(submitted.status == no::SubmitStatus::Accepted && submitted.handle,
            "root setup submit accepted");
    ++incarnation;
    ++ordinal;
    const auto replaced = core.apply_replace(
        *submitted.handle, request, context, incarnation, ordinal).result;
    require(replaced.status == no::ReplaceStatus::Replaced && replaced.successor,
            "first replacement consumes exactly one root");
    ++incarnation;
    ++ordinal;
    refuses("successor_roots", 1, 2, [&] {
        core.apply_replace(*replaced.successor, request, context, incarnation, ordinal);
    });
}

void slab_bytes() {
    ScratchManifest manifest{};
    manifest.R = 1; // Root region is exactly 16 bytes, not its 4096-byte page.
    ScratchSlab slab(manifest);
    refuses("successor_roots", 16, 17,
            [&] { slab.require_extend(ScratchResource::successor_roots, 17); });
    refuses("successor_roots", 16, 19,
            [&] { slab.extend(ScratchResource::successor_roots, 19); });
    const std::array<char, 2> bytes{{'a', 'b'}};
    refuses("successor_roots", 16, 17,
            [&] { slab.write_at(ScratchResource::successor_roots, 15, bytes.data(), 2); });
    refuses("successor_roots", 16, 20,
            [&] { slab.write_at(ScratchResource::successor_roots, 18, bytes.data(), 2); });
    // Exact UINT64_MAX, with no allocation or floating-point conversion.
    refuses("successor_roots", 16, UINT64_C(18446744073709551615), [&] {
        slab.write_at(ScratchResource::successor_roots,
                      UINT64_C(18446744073709551615), nullptr, 0);
    });
}

void layout_limits() {
    ScratchManifest manifest{};
    manifest.V = 28;
    refuses("layout", 27, 28, [&] { pd::compute_layout(manifest); });
    // bars is limited to floor(UINT64_MAX/2) before its reserve is doubled.
    refuses("layout", UINT64_C(9223372036854775807), UINT64_C(9223372036854775808),
            [&] { pd::default_scratch_manifest(UINT64_C(9223372036854775808)); });
    if constexpr (std::numeric_limits<std::size_t>::digits == 64
                  && std::numeric_limits<off_t>::digits == 63) {
        manifest = {};
        manifest.R = UINT64_C(576460752303423488); // 2^59 rows * 16 = 2^63 bytes.
        refuses("layout", UINT64_C(9223372036854775807), UINT64_C(9223372036854775808),
                [&] { pd::compute_layout(manifest); });
    }
}

void legacy() {
    const auto resource_only = [](const auto& operation) {
        try {
            operation();
        } catch (const pd::ScratchCapacityError& error) {
            const auto value = pineforge::classify_run_failure(error);
            require(value.code == pineforge::RunFailureCode::scratch_capacity_exceeded,
                    "old-style code unchanged");
            require(value.args && *value.args == "{\"resource\":\"layout\"}",
                    "old-style site retains exactly resource");
            return;
        }
        throw std::runtime_error("old-style site must refuse");
    };
    ScratchManifest manifest{};
    manifest.K_q = 1; // Removed region, no meaningful capacity to provision.
    resource_only([&] { pd::compute_layout(manifest); });
    resource_only([] { pd::scratch_layout_detail::add(UINT64_MAX, 1); });
    resource_only([] { pd::scratch_layout_detail::multiply(UINT64_MAX, 2); });
    // Both pre-existing message overloads remain callable and resource-only.
    resource_only([] { throw pd::ScratchCapacityError(ScratchResource::layout, "legacy"); });
    resource_only([] {
        throw pd::ScratchCapacityError(ScratchResource::layout, std::string("legacy"));
    });
}
} // namespace

int main(int argc, char** argv) {
    struct Row { const char* name; void (*run)(); };
    const Row rows[] = {
        {"objects", object_count}, {"id_pair", id_pair}, {"disk_id", disk_id},
        {"resident", resident_cohort}, {"roots", roots}, {"slab", slab_bytes},
        {"layout", layout_limits}, {"legacy", legacy},
    };
    try {
        bool ran = false;
        for (const auto& row : rows) {
            if (argc == 1 || (argc == 2 && std::string(argv[1]) == row.name)) {
                row.run();
                ran = true;
            }
        }
        require(ran, "unknown capacity test row");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_cr403_capacity_args: %s\n", error.what());
        return 1;
    }
}
