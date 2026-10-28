#include "p2_a5strmap_store_fixture.hpp"
#include <pineforge/map.hpp>
#include <pineforge/script_string.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace cleanup_probe {
struct Value {
    std::uint64_t number = 0;
};
struct Payload {
    std::uint64_t references;
    std::uint64_t number;
};
inline bool reenter = false;
inline bool throw_logic_error = false;
inline std::size_t releases = 0;
inline std::size_t collisions = 0;
inline pineforge::detail::o1::Id last_payload = pineforge::detail::o1::kNullId;
} // namespace cleanup_probe

namespace pineforge {
namespace detail {
template <>
struct MapExternalCodec<cleanup_probe::Value> {
    static constexpr bool enabled = true;
    using Rep = o1::Id;

    static cleanup_probe::Payload read(o1::Store& store, Rep id) {
        cleanup_probe::Payload payload{};
        if (store.read(id, 0, &payload, sizeof payload) != sizeof payload)
            throw std::runtime_error("cleanup probe read is short");
        return payload;
    }
    static void write(o1::Store& store, Rep id, const cleanup_probe::Payload& payload) {
        if (store.write(id, 0, &payload, sizeof payload) != sizeof payload)
            throw std::runtime_error("cleanup probe write is short");
    }
    static Rep encode(o1::Store& store, const cleanup_probe::Value& value) {
        const Rep id = store.create(sizeof(cleanup_probe::Payload), 8);
        write(store, id, cleanup_probe::Payload{1, value.number});
        cleanup_probe::last_payload = id;
        return id;
    }
    static cleanup_probe::Value decode(o1::Store& store, Rep id) {
        return cleanup_probe::Value{read(store, id).number};
    }
    static void retain(o1::Store& store, Rep id) {
        auto payload = read(store, id);
        ++payload.references;
        write(store, id, payload);
    }
    static void release(o1::Store& store, Rep id) {
        ++cleanup_probe::releases;
        if (cleanup_probe::throw_logic_error) {
            // Same text as RollbackOnly, but an unrelated codec exception:
            // classification must use the type, never the message.
            throw std::logic_error("o1 store: transaction is rollback-only");
        }
        if (cleanup_probe::reenter) {
            // The destructor already holds the sole cleanup handle.
            try {
                auto conflicting = store.begin_cleanup();
                conflicting->commit();
            } catch (const o1::TxPoolExhausted&) {
                ++cleanup_probe::collisions;
                throw;
            }
        }
        auto payload = read(store, id);
        if (--payload.references == 0) store.release(id);
        else write(store, id, payload);
    }
};
} // namespace detail
} // namespace pineforge

namespace {
using namespace pineforge;
using Store = detail::o1::Store;
using Tx = detail::o1::Tx;
using TxPoolExhausted = detail::o1::TxPoolExhausted;
using Leaf = PineMap<ScriptString, ScriptString>;
using Branch = PineMap<std::string, Leaf>;
using Root = PineMap<std::string, Branch>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Leaf make_leaf(Store& store, std::size_t* created_maps = nullptr) {
    auto leaf = Leaf::new_();
    if (created_maps) ++*created_maps;
    ScriptString key(store, "nested destruction key with stored bytes");
    ScriptString value(store, "nested destruction value with stored bytes");
    (void)leaf.put(key, value);
    require(leaf.size() == 1 && leaf.get(key) == value,
            "nested map setup lost its string payload");
    return leaf;
}

std::unique_ptr<Root> make_root(Store& store, std::size_t* created_maps = nullptr) {
    auto leaf = make_leaf(store, created_maps);
    auto branch = Branch::new_();
    if (created_maps) ++*created_maps;
    (void)branch.put("leaf", leaf);
    auto root = std::make_unique<Root>(Root::new_());
    if (created_maps) ++*created_maps;
    (void)root->put("branch", branch);
    require(root->size() == 1 && root->get("branch").size() == 1 &&
                root->get("branch").get("leaf").size() == 1,
            "nested map setup did not reach three levels");
    // All local aliases die before the caller receives the root handle.
    return root;
}

template<class Action>
void with_handles(Store& store, bool pressure, Action&& action) {
    auto outer = store.begin();
    std::array<std::unique_ptr<Tx>, detail::o1::kMaxTxHandles> joined{};
    std::size_t held = 1;
    if (pressure) {
        for (;;) {
            try {
                auto next = store.begin();
                require(held < Store::max_caller_handles(),
                        "caller pool grew beyond its declared storage");
                joined[held - 1] = std::move(next);
                ++held;
            } catch (const TxPoolExhausted& error) {
                require(std::strcmp(error.what(),
                                    "o1 store: transaction handle pool exhausted") == 0,
                        "caller exhaustion changed its diagnostic");
                break;
            }
        }
        require(held == Store::max_caller_handles(),
                "caller pressure did not reach the declared pool size");
    }
    action(); // Every caller handle stays alive through the cleanup.
    for (auto& handle : joined)
        if (handle) handle->commit();
    // A rollback-only outer commit throws; the test never absorbs that error.
    outer->commit();
    require(store.stats().cleanup_skipped_rollback_only == 0 &&
                store.stats().cleanup_failed == 0,
            "normal cleanup was skipped or failed");
    if (pressure)
        require(store.stats().max_live_handles == Store::max_caller_handles(),
                "caller pressure was absent from the high-water count");
}

void scope_exit(Store& store, bool pressure) {
    auto root = make_root(store);
    with_handles(store, pressure, [&] {
        auto doomed = std::move(root);
    });
}

void clear(Store& store, bool pressure) {
    auto root = make_root(store);
    with_handles(store, pressure, [&] {
        root->clear();
        require(root->size() == 0, "nested map clear retained a live entry");
        // Release the prepared arenas while caller pressure is still present.
        root.reset();
    });
}

void replace_value(Store& store, bool pressure) {
    auto root = make_root(store);
    with_handles(store, pressure, [&] {
        auto previous = root->put("branch", Branch{});
        require(previous.size() == 1 && previous.get("leaf").size() == 1 &&
                    root->get("branch").is_na(),
                "nested map replacement did not detach the old branch");
        previous = Branch{};
        root.reset();
    });
}

void image_scope_exit(Store& store, bool pressure) {
    auto leaf = make_leaf(store);
    using Image = decltype(leaf.keys());
    auto keys = std::make_unique<Image>(leaf.keys());
    auto values = std::make_unique<Image>(leaf.values());
    require(keys->size() == 1 && values->size() == 1,
            "nested map image setup did not retain payloads");
    with_handles(store, pressure, [&] {
        auto doomed_keys = std::move(keys);
        auto doomed_values = std::move(values);
    });
    // Keep Storage destruction outside pressure to isolate ~MapImage.
    leaf = Leaf{};
}

void snapshot_scope_exit(Store& store, bool pressure) {
    auto leaf = make_leaf(store);
    using Snapshot = decltype(leaf.snapshot());
    auto snapshot = std::make_unique<Snapshot>(leaf.snapshot());
    with_handles(store, pressure, [&] { snapshot.reset(); });
    leaf = Leaf{};
}

template <std::size_t Depth>
struct MapTree {
    using Child = typename MapTree<Depth - 1>::Map;
    using Map = PineMap<int, Child>;

    static Map build(Store& store) {
        auto node = Map::new_();
        auto child = MapTree<Depth - 1>::build(store);
        (void)node.put(static_cast<int>(Depth), child);
        return node;
    }
    static std::size_t depth(const Map& node, Store& store) {
        require(!node.is_na() && node.size() == 1 &&
                    node.contains(static_cast<int>(Depth)),
                "container tree lost a map level");
        return 1 + MapTree<Depth - 1>::depth(node.get(static_cast<int>(Depth)), store);
    }
};

template <>
struct MapTree<1> {
    using Map = Leaf;
    static Map build(Store& store) { return make_leaf(store); }
    static std::size_t depth(const Map& node, Store& store) {
        ScriptString key(store, "nested destruction key with stored bytes");
        ScriptString value(store, "nested destruction value with stored bytes");
        require(!node.is_na() && node.size() == 1 && node.get(key) == value,
                "container tree lost its stored string leaf");
        return 1;
    }
};

void depth_six() {
    a5_fixture::Session session;
    auto& store = *session.store;
    const auto baseline = store.stats().bytes_live;
    auto holder = std::make_shared<detail::MapStoreHolder>(store);
    detail::MapStoreScope map_scope(holder);
    ScriptStringScope string_scope(store);
    std::size_t explicit_begins = 0;
    auto outer = store.begin();  // Script hook.
    ++explicit_begins;
    auto joined = store.begin(); // Joined collection scope.
    ++explicit_begins;
    constexpr std::size_t levels = 6;
    {
        auto tree = MapTree<levels>::build(store);
        require(MapTree<levels>::depth(tree, store) == levels,
                "container depth proof did not reach six real maps");
        require(store.stats().bytes_live > baseline,
                "container depth proof never allocated map storage");
    } // All six real maps die before either enclosing handle finishes.
    const auto stats = store.stats();
    require(stats.bytes_live == baseline, "deep container destruction leaked bytes");
    require(stats.cleanup_skipped_rollback_only == 0 && stats.cleanup_failed == 0,
            "deep container cleanup was skipped or failed");
    require(stats.max_live_handles == explicit_begins,
            "container operations consumed a caller handle beyond explicit begin calls");
    joined->commit();
    outer->commit();
    require(store.stats().bytes_live == baseline, "deep container commit leaked bytes");
    std::printf("nested destroy: depth=%zu caller_high_water=%llu rollback_skips=%llu failed=%llu\n",
                levels, static_cast<unsigned long long>(stats.max_live_handles),
                static_cast<unsigned long long>(stats.cleanup_skipped_rollback_only),
                static_cast<unsigned long long>(stats.cleanup_failed));
}

void operations_join_full_pool() {
    a5_fixture::Session session;
    auto& store = *session.store;
    const auto baseline = store.stats().bytes_live;
    auto holder = std::make_shared<detail::MapStoreHolder>(store);
    detail::MapStoreScope map_scope(holder);
    ScriptStringScope string_scope(store);
    with_handles(store, true, [&] {
        auto leaf = make_leaf(store);
        ScriptString key(store, "nested destruction key with stored bytes");
        ScriptString value(store, "nested destruction value with stored bytes");
        auto snapshot = leaf.snapshot();
        auto copy = leaf.copy();
        auto keys = leaf.keys();
        auto values = leaf.values();
        require(keys.size() == 1 && keys[0] == key &&
                    values.size() == 1 && values[0] == value,
                "joined enumeration lost stored payloads");
        require(copy.remove(key) == value && copy.size() == 0,
                "joined remove lost its previous value");
        copy.put_all(leaf);
        require(copy.get(key) == value, "joined put_all lost a payload");
        leaf.clear();
        leaf.restore(snapshot);
        require(leaf.get(key) == value, "joined restore lost its checkpoint");
    });
    require(store.stats().bytes_live == baseline, "joined public operations leaked bytes");
}

void rollback_only_destruction() {
    a5_fixture::Session session;
    auto& store = *session.store;
    auto holder = std::make_shared<detail::MapStoreHolder>(store);
    detail::MapStoreScope map_scope(holder);
    ScriptStringScope string_scope(store);
    const std::uint64_t marker = UINT64_C(0x4f27b91a);
    const auto sentinel = store.create(sizeof marker, alignof(std::uint64_t));
    require(store.write(sentinel, 0, &marker, sizeof marker) == sizeof marker,
            "rollback baseline write is short");
    const auto baseline = store.stats().bytes_live;
    std::size_t destroyed_maps = 0;
    {
        auto outer = store.begin();
        auto root = make_root(store, &destroyed_maps);
        {
            auto abandoned = store.begin();
        } // Poison the outer transaction before the last map references die.
        root.reset(); // No exception may escape any of the three destructors.
        require(store.stats().cleanup_skipped_rollback_only == destroyed_maps,
                "rollback-only destruction did not record each skipped map cleanup");
        require(store.stats().cleanup_failed == 0,
                "rollback-only destruction was misclassified as failed cleanup");
        outer->rollback();
    }
    std::uint64_t restored = 0;
    require(store.stats().bytes_live == baseline &&
                store.stats().cleanup_skipped_rollback_only == destroyed_maps &&
                store.stats().cleanup_failed == 0 &&
                store.read(sentinel, 0, &restored, sizeof restored) == sizeof restored &&
                restored == marker,
            "rollback-only destruction changed the baseline snapshot");
    store.release(sentinel);
    std::printf("nested destroy: rollback-only cleanup restored baseline bytes\n");
}

void preexisting_rollback_only_destruction() {
    a5_fixture::Session session;
    auto& store = *session.store;
    auto holder = std::make_shared<detail::MapStoreHolder>(store);
    detail::MapStoreScope map_scope(holder);
    using ProbeMap = PineMap<int, cleanup_probe::Value>;
    auto map = std::make_unique<ProbeMap>(ProbeMap::new_());
    (void)map->put(7, cleanup_probe::Value{97});
    const auto payload = cleanup_probe::last_payload;
    require(store.alive(payload), "pre-existing map never stored its payload");
    const auto before_destroy = store.stats();
    require(before_destroy.cleanup_skipped_rollback_only == 0 &&
                before_destroy.cleanup_failed == 0,
            "pre-existing map setup skipped or failed cleanup");
    {
        auto outer = store.begin(); // The map and payload already existed.
        {
            auto abandoned = store.scope();
        }
        cleanup_probe::releases = 0;
        map.reset(); // Its last native reference dies while cleanup is refused.
        require(cleanup_probe::releases == 0,
                "rollback-only cleanup reached the payload release codec");
        require(store.stats().bytes_live == before_destroy.bytes_live &&
                    store.stats().cleanup_skipped_rollback_only == 1 &&
                    store.stats().cleanup_failed == 0,
                "pre-existing map refusal changed bytes or cleanup diagnostics");
        outer->rollback();
    }
    const auto after = store.stats();
    require(store.alive(payload) && after.objects_live == before_destroy.objects_live,
            "pre-existing object disappeared after rollback-only destruction");
    require(after.bytes_live == before_destroy.bytes_live,
            "pre-existing object bytes changed after refused cleanup and rollback");
    require(after.cleanup_skipped_rollback_only == 1 && after.cleanup_failed == 0,
            "pre-existing object refusal lost its benign cleanup trace");
    const auto retained =
        detail::MapExternalCodec<cleanup_probe::Value>::read(store, payload);
    require(retained.references == 1 && retained.number == 97,
            "pre-existing payload changed after its native reference died");
    // If the host continues after a refused container operation, one object stays
    // unreferenced until the run ends and cleanup_skipped_rollback_only is its trace.
    std::printf("nested destroy: pre-existing object retained after refused cleanup\n");
}

void observable_cleanup_failure(bool image, bool codec_error = false) {
    a5_fixture::Session session;
    auto& store = *session.store;
    const auto baseline = store.stats().bytes_live;
    auto holder = std::make_shared<detail::MapStoreHolder>(store);
    detail::MapStoreScope map_scope(holder);
    using ProbeMap = PineMap<int, cleanup_probe::Value>;
    auto map = std::make_unique<ProbeMap>(ProbeMap::new_());
    (void)map->put(7, cleanup_probe::Value{97});
    const auto payload = cleanup_probe::last_payload;
    require(store.alive(payload), "cleanup probe never stored a payload");

    using Image = decltype(map->values());
    std::unique_ptr<Image> values;
    if (image) {
        values = std::make_unique<Image>(map->values());
        require(values->size() == 1 && (*values)[0].number == 97,
                "cleanup probe image did not retain its payload");
        map.reset();
        require(store.stats().bytes_live ==
                    baseline + sizeof(cleanup_probe::Payload) + sizeof(detail::o1::Id),
                "cleanup probe image baseline has unexpected storage");
    }
    require(store.stats().cleanup_skipped_rollback_only == 0 &&
                store.stats().cleanup_failed == 0,
            "cleanup probe setup skipped or failed cleanup");
    const auto before = store.stats().bytes_live;
    cleanup_probe::releases = 0;
    cleanup_probe::collisions = 0;
    cleanup_probe::reenter = !codec_error;
    cleanup_probe::throw_logic_error = codec_error;
    if (image) values.reset();
    else map.reset();
    cleanup_probe::reenter = false;
    cleanup_probe::throw_logic_error = false;
    const auto stats = store.stats();
    require(cleanup_probe::releases == 1,
            "negative cleanup row did not call the release codec");
    require(cleanup_probe::collisions == (codec_error ? 0 : 1),
            "negative cleanup row did not exercise its expected failure");
    // In a destructor chain that poisons an outer transaction, the first failure
    // is counted failed and later releases hitting RollbackOnly count as benign,
    // so the gate still sees failed != 0.
    require(stats.cleanup_failed == 1 && stats.cleanup_skipped_rollback_only == 0,
            "swallowed cleanup failure was not observable");
    require(stats.bytes_live == before && stats.bytes_live > baseline &&
                store.alive(payload),
            "negative cleanup row did not preserve the expected leaked objects");
    const auto retained =
        detail::MapExternalCodec<cleanup_probe::Value>::read(store, payload);
    require(retained.references == 1 && retained.number == 97,
            "failed cleanup changed the retained payload");
    {
        auto probe = store.begin_cleanup();
        probe->commit();
    }
    require(store.stats().cleanup_failed == 1 &&
                store.stats().cleanup_skipped_rollback_only == 0,
            "successful cleanup erased the failure diagnostic");
    std::printf("nested destroy: %s %s; failed=%llu rollback_skips=%llu leaked=%llu\n",
                image ? "image" : "storage", codec_error ? "codec throw" : "recursive cleanup",
                static_cast<unsigned long long>(stats.cleanup_failed),
                static_cast<unsigned long long>(stats.cleanup_skipped_rollback_only),
                static_cast<unsigned long long>(stats.bytes_live - baseline));
}

struct Case {
    const char* name;
    void (*destroy)(Store&, bool);
};

bool run_case(const Case& test, bool pressure) {
    a5_fixture::Session session;
    // The sole expected byte count is the empty-store baseline, recorded
    // before any tested map, string, image or transaction exists.
    const std::uint64_t baseline = session.store->stats().bytes_live;
    auto holder = std::make_shared<detail::MapStoreHolder>(*session.store);
    detail::MapStoreScope map_scope(holder);
    ScriptStringScope string_scope(*session.store);
    std::printf("nested destroy: %s (%s)\n", test.name,
                pressure ? "caller pool full" : "one-handle control");
    std::fflush(stdout);
    test.destroy(*session.store, pressure);
    // All tested handles are destroyed. A new successful commit also proves
    // that the store accepts another transaction after the case.
    {
        auto probe = session.store->begin();
        probe->commit();
    }
    const auto stats = session.store->stats();
    const auto live = stats.bytes_live;
    const bool clean = live == baseline && stats.cleanup_skipped_rollback_only == 0 &&
                       stats.cleanup_failed == 0;
    std::printf("nested destroy: %s %s; baseline=%llu live=%llu rollback_skips=%llu failed=%llu\n",
                test.name, clean ? "clean" : "nested destroy leaked store bytes",
                static_cast<unsigned long long>(baseline),
                static_cast<unsigned long long>(live),
                static_cast<unsigned long long>(stats.cleanup_skipped_rollback_only),
                static_cast<unsigned long long>(stats.cleanup_failed));
    return clean;
}
} // namespace

int main(int argc, char** argv) {
    try {
        // The lab can select a named row to demonstrate each mutation
        // independently; CTest's no-argument invocation still runs every row.
        struct Row {
            const char* name;
            void (*run)();
        };
        const Row rows[] = {
            {"depth_six", depth_six},
            {"operations_join_full_pool", operations_join_full_pool},
            {"cleanup_storage", [] { observable_cleanup_failure(false); }},
            {"cleanup_image", [] { observable_cleanup_failure(true); }},
            {"codec_storage", [] { observable_cleanup_failure(false, true); }},
            {"codec_image", [] { observable_cleanup_failure(true, true); }},
            {"rollback_only_destruction", rollback_only_destruction},
            {"preexisting_rollback_only_destruction", preexisting_rollback_only_destruction},
        };
        require(argc == 1 || argc == 2, "expected at most one row name");
        const Case cases[] = {
            {"scope exit", scope_exit},
            {"clear", clear},
            {"replace value", replace_value},
            {"keys/values image scope exit", image_scope_exit},
            {"snapshot scope exit", snapshot_scope_exit},
        };
        if (argc == 1) {
            // A failed control stops the sequence before any pressure case.
            for (const auto& test : cases)
                require(run_case(test, false), "one-handle control leaked store bytes");
            bool clean = true;
            for (const auto& test : cases) {
                const bool case_clean = run_case(test, true);
                clean = case_clean && clean;
            }
            require(clean, "nested destroy leaked store bytes");
        }
        bool ran_row = false;
        for (const auto& row : rows) {
            if (argc == 2 && std::strcmp(argv[1], row.name) != 0) continue;
            std::printf("nested destroy: row=%s\n", row.name);
            std::fflush(stdout);
            row.run();
            ran_row = true;
        }
        require(ran_row, "unknown nested destruction row name");
        return 0;
    } catch (const std::exception& error) {
        cleanup_probe::reenter = false;
        cleanup_probe::throw_logic_error = false;
        std::fprintf(stderr, "nested destroy FAIL: %s\n", error.what());
        return 1;
    }
}
