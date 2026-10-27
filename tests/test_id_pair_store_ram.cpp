// CR150 RAM ID-pair lane. Predicted red on d91c96d47d: IdPairStore has no
// bind_scratch_tables member. No executed fail-before claim; lab owns that run.
#include <pineforge/detail/id_pair_store.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using detail::ScratchManifest;
using detail::ScratchResource;
using detail::ScratchSlab;
using Store = detail::IdPairStore<std::uint64_t>;
using Key = std::pair<std::string, std::optional<std::string>>;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool put(Store& store, const Key& key, std::uint64_t value) {
    return key.second ? store.insert_or_assign(key.first, *key.second, value)
                      : store.insert_or_assign(key.first, value);
}
bool remove(Store& store, const Key& key) {
    return key.second ? store.erase(key.first, *key.second) : store.erase(key.first);
}
std::optional<std::uint64_t> get(const Store& store, const Key& key) {
    return key.second ? store.lookup(key.first, *key.second) : store.lookup(key.first);
}

// Publish all zero padding within the exact capacities for byte inspection.
// This fixture step happens before the operation boundary, never in a ticket.
void expose_bytes(ScratchSlab& slab) {
    for (const auto& region : slab.regions()) slab.extend(region.resource, region.capacity);
}
struct Image {
    std::array<std::vector<std::uint8_t>, detail::kScratchRegionCount> bytes;
    std::array<std::uint64_t, detail::kScratchRegionCount> extents{};
    explicit Image(const ScratchSlab& slab) {
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            const auto& region = slab.regions()[i];
            extents[i] = region.logical_extent;
            bytes[i].resize(static_cast<std::size_t>(region.logical_extent));
            slab.read_at(region.resource, 0, bytes[i].data(), bytes[i].size());
        }
    }
    bool operator==(const Image& other) const { return bytes == other.bytes && extents == other.extents; }
};
template<class Operation>
void refuses_unchanged(const std::shared_ptr<ScratchSlab>& slab, ScratchResource resource,
                       std::uint64_t limit, std::uint64_t need, const Store& store, Operation operation) {
    const Image before(*slab);
    const auto size = store.size();
    bool caught = false;
    try { operation(); }
    catch (const detail::ScratchCapacityError& error) {
        caught = true;
        const auto failure = classify_run_failure(error);
        require(failure.code == RunFailureCode::scratch_capacity_exceeded
                && run_failure_code_class(failure.code) == RunFailureClass::resource
                && !run_failure_code_retryable(failure.code), "coded nonretryable capacity failure");
        // Expected limit/need are hand-derived at each call below.
        require(failure.args && *failure.args == "{\"limit\":" + std::to_string(limit)
                + ",\"need\":" + std::to_string(need) + ",\"resource\":\""
                + detail::scratch_resource_name(resource) + "\"}", "exact capacity arguments");
    }
    require(caught, "one additional record must refuse before mutation");
    require(Image(*slab) == before && store.size() == size,
            "all region bytes, extents and live count survive the refusal");
}

void compare(const Store& ram, const Store& disk, const Store& oracle,
             const std::vector<Key>& allocation_order) {
    require(ram.size() == oracle.size() && ram.size() == disk.size(), "live sizes match oracle");
    std::map<Key, std::uint64_t> observed, expected;
    for (const auto& row : oracle) expected.emplace(Key{row.first, row.from_entry}, row.second);
    auto ram_at = ram.begin();
    auto disk_at = disk.begin();
    // First-record order is independent of the map oracle's lexical order.
    for (const auto& key : allocation_order) {
        const auto value = get(oracle, key);
        require(get(ram, key) == value && get(disk, key) == value, "full-key lookup under collisions");
        if (!value) continue;
        require(ram_at != ram.end() && disk_at != disk.end(), "record scan includes live allocation");
        require(Key{ram_at->first, ram_at->from_entry} == key
                && Key{disk_at->first, disk_at->from_entry} == key
                && ram_at->second == *value && disk_at->second == *value,
                "RAM and disk scan allocation order, skipping dead rows");
        observed.emplace(key, ram_at->second);
        ++ram_at;
        ++disk_at;
    }
    require(ram_at == ram.end() && disk_at == disk.end() && observed == expected,
            "iteration neither duplicates nor omits a live key");
}

void seeded_sequences(bool collide) {
    std::vector<Key> keys{{"", std::nullopt}, {"x", std::nullopt}, {"x", std::string{}},
        {std::string("a\0b", 3), std::string("c\0d", 3)},
        {std::string(64, 'a'), std::string(64, 'b')}};
    for (int i = 0; i < 59; ++i)
        keys.emplace_back("exit-" + std::to_string(i), "entry-" + std::to_string(i % 7));
    ScratchManifest manifest;
    manifest.K_e = keys.size();
    for (const auto& key : keys)
        manifest.B_e += key.first.size() + (key.second ? key.second->size() : 0);
    auto slab = std::make_shared<ScratchSlab>(manifest);
    Store ram, disk, oracle;
    ram.bind_scratch_tables(slab, ScratchResource::erased_data, ScratchResource::erased_index);
    oracle.fixture_use_memory_reference();
    if (collide) {
        // A per-instance fixture forces equal FULL hashes, not just equal
        // bucket bits. The real probe/key comparison still handles each row.
        ram.fixture_force_hash_collisions(7);
        disk.fixture_force_hash_collisions(7);
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto value = i == 2 ? std::numeric_limits<std::uint64_t>::max() : i;
        require(put(ram, keys[i], value) == put(oracle, keys[i], value)
                && put(disk, keys[i], value), "initial key insertion");
    }
    require(ram.lookup("x") == 1 && ram.lookup("x", "") == std::numeric_limits<std::uint64_t>::max(),
            "single x and empty-pair x are distinct, including explicit UINT64_MAX");
    const auto capacity = slab->region(ScratchResource::erased_index).capacity;
    const auto extent = slab->region(ScratchResource::erased_data).logical_extent;
    std::mt19937_64 random(0x150221);
    for (int step = 0; step < 1500; ++step) {
        const auto& key = keys[static_cast<std::size_t>(random() % keys.size())];
        if (random() % 3 == 0) {
            const auto expected = remove(oracle, key);
            require(remove(ram, key) == expected && remove(disk, key) == expected,
                    "seeded erase/live-flag result");
        } else {
            const auto value = random();
            const auto expected = put(oracle, key, value);
            require(put(ram, key, value) == expected && put(disk, key, value) == expected,
                    "seeded overwrite/resurrection result");
        }
        compare(ram, disk, oracle, keys);
    }
    require(ram.disk_bytes() == 0 && disk.disk_bytes() > 0
            && slab->region(ScratchResource::erased_index).capacity == capacity
            && slab->region(ScratchResource::erased_data).logical_extent == extent,
            "fixed form never grows, allocates a replacement index or opens a file");
    Store moved(std::move(ram));
    require(ram.empty(), "move leaves no live keys behind");
    compare(moved, disk, oracle, keys);
    std::weak_ptr<ScratchSlab> lifetime = slab;
    slab.reset();
    require(!lifetime.expired(), "store retains its own slab owner");
    moved.clear();
    require(moved.insert_or_assign("x", 8) && moved.lookup("x") == 8
            && moved.disk_bytes() == 0, "clear preserves the binding for a new run");
}

void boundaries() {
    ScratchManifest manifest;
    manifest.K_e = 2;
    manifest.B_e = 2;
    manifest.U = manifest.V = manifest.R = 1; // neighboring regions are checked too
    auto slab = std::make_shared<ScratchSlab>(manifest);
    Store store;
    store.bind_scratch_tables(slab, ScratchResource::erased_data, ScratchResource::erased_index);
    expose_bytes(*slab);
    // D=40*2+2=82. One 42-byte string plus its 40-byte header exactly fills
    // D while one ever-key slot remains. Page rounding grants no extra byte.
    const std::string exact(42, 'L');
    require(store.insert_or_assign(exact, 1), "exact data fit");
    // limit 82=40*2+2, need 122=82+40: append an empty key after the exact-fit record.
    refuses_unchanged(slab, ScratchResource::erased_data, 82, 122, store,
                      [&] { store.insert_or_assign("", 2); });
    require(store.erase(exact) && store.insert_or_assign(exact, 9),
            "resurrection at exact capacity reuses old bytes");
    require(!store.insert_or_assign(exact, 10), "overwrite at exact capacity uses no ticket bytes");
    store.clear();
    expose_bytes(*slab);
    const auto before = Image(*slab);
    store.preflight_assign("x", std::string_view(""));
    require(Image(*slab) == before, "successful admission is read-only");
    // Header and first fit, but 40+1+42=83 exceeds D=82. No header may land.
    // limit 82=40*2+2, need 83=40+1+42: the whole first pair is reserved.
    refuses_unchanged(slab, ScratchResource::erased_data, 82, 83, store,
                      [&] { store.insert_or_assign("x", std::string(42, 'R'), 3); });

    ScratchManifest load;
    load.K_e = load.B_e = 32; // Q(32)=64, D=32*(40+1)=1312, I=64*16=1024.
    auto loaded = std::make_shared<ScratchSlab>(load);
    Store half;
    half.bind_scratch_tables(loaded, ScratchResource::erased_data, ScratchResource::erased_index);
    expose_bytes(*loaded);
    for (int i = 0; i < 32; ++i) half.insert_or_assign(std::string(1, static_cast<char>(i)), i);
    require(loaded->region(ScratchResource::erased_index).capacity == 64 * 16,
            "hand-derived Q/2 load ceiling uses exactly one 64-slot index");
    // limit 32=min(K=32,Q/2=32), need 33: a new key after 32 distinct inserts.
    refuses_unchanged(loaded, ScratchResource::erased_index, 32, 33, half,
                      [&] { half.insert_or_assign(std::string(1, 'Z'), 33); });
    require(half.erase(std::string(1, '\0')), "erase keeps an ever-key slot");
    // limit 32, need 33: erasure leaves all 32 ever-key slots occupied.
    refuses_unchanged(loaded, ScratchResource::erased_index, 32, 33, half,
                      [&] { half.insert_or_assign(std::string(1, 'Z'), 33); });
    require(half.insert_or_assign(std::string(1, '\0'), 77), "resurrect at Q/2");

    auto zero = std::make_shared<ScratchSlab>(ScratchManifest{});
    Store empty;
    empty.bind_scratch_tables(zero, ScratchResource::erased_data, ScratchResource::erased_index);
    require(!empty.lookup("x") && !empty.erase("x"), "Q=0 reads are empty");
    // limit 0=min(K=0,Q/2=0), need 1: the first key in an empty manifest.
    refuses_unchanged(zero, ScratchResource::erased_index, 0, 1, empty,
                      [&] { empty.insert_or_assign("x", 1); });
}

void consumed_single_string() {
    ScratchManifest manifest;
    manifest.K_c = 1;
    manifest.B_c = 3; // Single "X" + separator + "E": D=40+3=43.
    auto slab = std::make_shared<ScratchSlab>(manifest);
    detail::IdPairStore<std::int64_t> store;
    store.bind_scratch_tables(slab, ScratchResource::consumed_data, ScratchResource::consumed_index);
    const auto key = std::string("X") + '\x1f' + "E";
    store.preflight_joined_single("X", '\x1f', "E");
    store.insert_or_assign(key, -17);
    require(store.at(key) == -17 && !store.lookup("X", "E"), "consumed domain is one signed string key");
    require(store.erase(key), "erase consumed fixture row");
    const Image before(*slab);
    store.preflight_joined_single("X", '\x1f', "E");
    require(Image(*slab) == before, "joined dead-key ticket needs no bytes");
    store.insert_or_assign(key, std::numeric_limits<std::int64_t>::min());
    require(store.at(key) == std::numeric_limits<std::int64_t>::min(), "signed resurrection keeps all bits");
    bool refused_control = false;
    try {
        store.preflight_joined_single(std::string("bad") + '\x1f', '\x1f', "E");
    } catch (const detail::ScratchCapacityError& error) {
        refused_control = std::string(error.what()) == "order ids cannot contain the control character 0x1f"
            && classify_run_failure(error).args
            && *classify_run_failure(error).args == "{\"resource\":\"consumed_data\"}";
    }
    require(refused_control, "consumed ids reject the separator before joined splitting");
    bool refused = false;
    try { store.preflight_joined_single("Y", '\x1f', "E"); }
    catch (const detail::ScratchCapacityError& error) {
        const auto value = classify_run_failure(error);
        // limit 1=K_c, need 2: X<sep>E already occupies the sole ever-key slot.
        refused = value.args && *value.args == "{\"limit\":1,\"need\":2,\"resource\":\"consumed_index\"}";
    }
    require(refused, "new joined key uses consumed_index capacity");

    manifest.K_c = 2;
    manifest.B_c = 0; // D=80, joined key of 41 bytes requires 81: +1 byte.
    auto short_slab = std::make_shared<ScratchSlab>(manifest);
    detail::IdPairStore<std::int64_t> short_store;
    short_store.bind_scratch_tables(short_slab, ScratchResource::consumed_data, ScratchResource::consumed_index);
    expose_bytes(*short_slab);
    const Image short_before(*short_slab);
    refused = false;
    try { short_store.preflight_joined_single(std::string(39, 'x'), '\x1f', "E"); }
    catch (const detail::ScratchCapacityError& error) {
        const auto value = classify_run_failure(error);
        // limit 80=40*2+0, need 81=40+39+1+1: header, first id, separator, second id.
        refused = value.args && *value.args == "{\"limit\":80,\"need\":81,\"resource\":\"consumed_data\"}";
    }
    require(refused && Image(*short_slab) == short_before && short_store.empty(),
            "joined precommit extent includes separator and whole record before mutation");
}

template<class F>
void text_refusal(ScratchResource resource, std::uint64_t limit, std::uint64_t need,
                  const std::string& expected, F&& operation) {
    bool caught = false;
    try { operation(); }
    catch (const detail::ScratchCapacityError& error) {
        caught = true;
        require(error.what() == expected, "exact limit and workaround from the live manifest");
        const auto failure = classify_run_failure(error);
        // Expected limit/need are hand-derived at each call below; English checks remain exact.
        require(failure.code == RunFailureCode::scratch_capacity_exceeded && failure.args
                && *failure.args == "{\"limit\":" + std::to_string(limit)
                    + ",\"need\":" + std::to_string(need) + ",\"resource\":\""
                    + detail::scratch_resource_name(resource) + "\"}",
                "message change preserves code and exact capacity args");
    }
    require(caught, "message test must trigger the real ticket");
}

void limit_messages_and_oversized_components() {
    for (const bool consumed : {false, true}) {
        ScratchManifest m;
        m.K_e = m.K_c = 1;
        m.B_e = m.B_c = 3;
        const auto data = consumed ? ScratchResource::consumed_data : ScratchResource::erased_data;
        const auto index = consumed ? ScratchResource::consumed_index : ScratchResource::erased_index;
        auto slab = std::make_shared<ScratchSlab>(m);
        Store store;
        store.bind_scratch_tables(slab, data, index);
        store.insert_or_assign("a", 1);
        // limit 1=K, need 2: b is the second distinct key after a.
        text_refusal(index, 1, 2, consumed
            ? "more than 1 keys for partially filled exits in one run; reuse ids, e.g. a modulo of bar_index"
            : "more than 1 retired order-id keys in one run (each retired order uses 2); reuse ids, e.g. a modulo of bar_index",
            [&] { store.preflight_assign("b"); });
        m.K_e = m.K_c = 2;
        m.B_e = m.B_c = 0; // 80 exact bytes: one 40-byte key + its header fills it.
        slab = std::make_shared<ScratchSlab>(m);
        Store bytes;
        bytes.bind_scratch_tables(slab, data, index);
        bytes.insert_or_assign(std::string(40, 'x'), 2);
        // limit 80=40*2, need 120=80+40: an empty key still needs a header after the full record.
        text_refusal(data, 80, 120, consumed
            ? "partially filled exits use more than 0 bytes in one run; use shorter ids or reuse ids"
            : "retired order ids use more than 0 bytes in one run; use shorter ids or reuse ids",
            [&] { bytes.preflight_assign(""); });
        // limit 64, need 65: the supplied first component has 65 bytes.
        text_refusal(data, 64, 65, "id longer than 64 bytes",
            [&] { bytes.preflight_assign(std::string(65, 'x')); });
        // limit 64, need 65=max(1,65): the second component is oversized.
        text_refusal(data, 64, 65, "id longer than 64 bytes",
            [&] { bytes.preflight_assign("x", std::string_view(std::string(65, 'y'))); });
    }
    // The old oversized fixture remains covered on the ordinary disk path.
    Store disk;
    const std::string first(513, 'a'), second(1025, 'b');
    require(disk.insert_or_assign(first, second, 17) && disk.lookup(first, second) == 17u,
            "unjailed id components retain their previous length domain");
}
} // namespace

int main() {
    try {
        seeded_sequences(false);
        seeded_sequences(true);
        boundaries();
        consumed_single_string();
        limit_messages_and_oversized_components();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_id_pair_store_ram: %s\n", error.what());
        return 1;
    }
}
