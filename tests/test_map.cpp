#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/map.hpp>

#include <pineforge/na.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using pineforge::PineMap;

template <typename Fn>
static void expect_runtime_error(Fn&& fn, const char* message) {
    bool threw = false;
    try {
        fn();
    } catch (const std::runtime_error& error) {
        threw = true;
        assert(std::string(error.what()) == message);
    }
    assert(threw);
}

template <typename Map, typename = void>
struct has_public_snapshot : std::false_type {};

template <typename Map>
struct has_public_snapshot<
    Map, std::void_t<decltype(std::declval<const Map&>().snapshot())>>
    : std::true_type {};

struct SnapshotUnsafeUdt {
    int value = 0;
};

struct HeapValueProbe {
    inline static std::size_t default_constructed = 0;

    HeapValueProbe() { ++default_constructed; }
    explicit HeapValueProbe(int input) : value(input) {}

    std::string payload;
    int value = 0;
};

static void test_na_id_and_stable_errors() {
    using Map = PineMap<std::string, int>;
    constexpr const char* na_error = "map operation on na ID";

    Map null_map;
    auto typed_na = pineforge::na<Map>();
    assert(pineforge::is_na(null_map));
    assert(pineforge::is_na(typed_na));

    auto allocated = Map::new_();
    assert(!pineforge::is_na(allocated));
    auto snapshot = allocated.snapshot();

    expect_runtime_error([&] { (void)null_map.put("x", 1); }, na_error);
    expect_runtime_error([&] { (void)null_map.get("x"); }, na_error);
    expect_runtime_error([&] { (void)null_map.remove("x"); }, na_error);
    expect_runtime_error([&] { (void)null_map.contains("x"); }, na_error);
    expect_runtime_error([&] { (void)null_map.size(); }, na_error);
    expect_runtime_error([&] { null_map.clear(); }, na_error);
    expect_runtime_error([&] { (void)null_map.keys(); }, na_error);
    expect_runtime_error([&] { (void)null_map.values(); }, na_error);
    expect_runtime_error([&] { (void)null_map.copy(); }, na_error);
    expect_runtime_error([&] { (void)null_map.snapshot(); }, na_error);
    expect_runtime_error([&] { null_map.put_all(allocated); }, na_error);
    expect_runtime_error([&] { allocated.put_all(null_map); }, na_error);
    auto active_snapshot = std::move(snapshot);
    expect_runtime_error(
        [&] { null_map.restore(snapshot); },
        "map restore from invalid snapshot");
    null_map.restore(active_snapshot);
    assert(!pineforge::is_na(null_map));
}

static void test_copy_aliases_and_explicit_copy_is_independent() {
    auto map = PineMap<std::string, double>::new_();
    const auto initial_previous = map.put("alpha", 1.0);
    assert(pineforge::is_na(initial_previous));

    auto alias = map;
    assert(alias.get("alpha") == 1.0);
    const auto alias_previous = alias.put("alpha", 2.0);
    assert(alias_previous == 1.0);
    assert(map.get("alpha") == 2.0);

    auto independent = map.copy();
    const auto independent_previous = independent.put("alpha", 3.0);
    const auto independent_missing = independent.put("beta", 4.0);
    assert(independent_previous == 2.0);
    assert(pineforge::is_na(independent_missing));
    assert(map.get("alpha") == 2.0);
    assert(!map.contains("beta"));

    auto assigned = PineMap<std::string, double>::new_();
    assigned = map;
    assigned.put("gamma", 5.0);
    assert(map.get("gamma") == 5.0);
}

static void test_move_preserves_source_handle_alias() {
    auto source = PineMap<std::string, int>::new_();
    source.put("a", 1);

    PineMap<std::string, int> constructed(std::move(source));
    constructed.put("b", 2);
    assert(source.get("b") == 2);

    auto assigned = PineMap<std::string, int>::new_();
    assigned.put("discarded", 99);
    assigned = std::move(source);
    assigned.put("c", 3);

    assert(source.get("c") == 3);
    assert(constructed.get("c") == 3);
    assert(!assigned.contains("discarded"));

}

static void test_put_get_and_stable_insertion_order() {
    auto map = PineMap<std::string, int>::new_();
    const auto second_previous = map.put("second", 2);
    const auto first_previous = map.put("first", 1);
    const auto third_previous = map.put("third", 3);
    assert(pineforge::is_na(second_previous));
    assert(pineforge::is_na(first_previous));
    assert(pineforge::is_na(third_previous));

    const auto overwrite_previous = map.put("first", 10);
    assert(overwrite_previous == 1);
    assert((map.keys() == std::vector<std::string>{"second", "first", "third"}));
    assert((map.values() == std::vector<int>{2, 10, 3}));
    assert(map.size() == 3);
    assert(map.contains("first"));
    assert(!map.contains("missing"));
    assert(map.get("first") == 10);
    assert(pineforge::is_na(map.get("missing")));
}

static void test_float_keys_use_pine_na_and_zero_equality() {
    auto map = PineMap<double, int>::new_();
    const double na_one = std::numeric_limits<double>::quiet_NaN();
    const double na_two = std::nan("42");

    const auto na_previous = map.put(na_one, 1);
    assert(pineforge::is_na(na_previous));
    assert(map.contains(na_two));
    assert(map.get(na_two) == 1);
    const auto na_overwrite_previous = map.put(na_two, 2);
    assert(na_overwrite_previous == 1);
    assert(map.size() == 1);
    assert(map.keys().size() == 1);
    assert(std::isnan(map.keys().front()));

    const auto zero_previous = map.put(-0.0, 3);
    assert(pineforge::is_na(zero_previous));
    assert(map.contains(+0.0));
    assert(map.get(+0.0) == 3);
    const auto zero_overwrite_previous = map.put(+0.0, 4);
    assert(zero_overwrite_previous == 3);
    assert(map.size() == 2);

    const auto removed_na = map.remove(na_two);
    assert(removed_na == 2);
    assert(!map.contains(na_one));
    assert((map.values() == std::vector<int>{4}));
}

static void test_remove_updates_order_and_reinsert_appends() {
    auto map = PineMap<int, std::string>::new_();
    map.put(1, "one");
    map.put(2, "two");
    map.put(3, "three");
    map.put(4, "four");

    const auto removed_two = map.remove(2);
    assert(removed_two == "two");
    assert((map.keys() == std::vector<int>{1, 3, 4}));
    assert((map.values() == std::vector<std::string>{"one", "three", "four"}));
    const auto removed_missing = map.remove(99);
    assert(removed_missing.empty());
    assert((map.keys() == std::vector<int>{1, 3, 4}));

    const auto reinsert_previous = map.put(2, "two-again");
    assert(reinsert_previous.empty());
    assert((map.keys() == std::vector<int>{1, 3, 4, 2}));
}

static void test_put_all_overwrites_in_source_order() {
    auto target = PineMap<std::string, int>::new_();
    target.put("a", 1);
    target.put("b", 2);
    target.put("c", 3);

    auto source = PineMap<std::string, int>::new_();
    source.put("b", 20);
    source.put("d", 40);
    source.put("a", 10);
    source.put("e", 50);

    target.put_all(source);
    assert((target.keys() == std::vector<std::string>{"a", "b", "c", "d", "e"}));
    assert((target.values() == std::vector<int>{10, 20, 3, 40, 50}));

    // Aliases identify the same map ID; put_all on itself must remain safe and
    // must not duplicate or reorder entries.
    auto alias = target;
    target.put_all(alias);
    assert((target.keys() == std::vector<std::string>{"a", "b", "c", "d", "e"}));
    assert((target.values() == std::vector<int>{10, 20, 3, 40, 50}));
}

static void test_50000_pair_capacity_and_put_all_preflight() {
    using Map = PineMap<int, int>;
    constexpr const char* capacity_error =
        "map cannot contain more than 50000 key-value pairs";

    auto map = Map::new_();
    for (int key = 0; key < Map::max_pairs; ++key) {
        const auto previous = map.put(key, key);
        assert(pineforge::is_na(previous));
    }
    assert(map.size() == Map::max_pairs);

    // Overwriting at the limit is permitted and does not move or grow a key.
    const auto limit_overwrite_previous = map.put(0, -1);
    assert(limit_overwrite_previous == 0);
    assert(map.size() == Map::max_pairs);
    expect_runtime_error(
        [&] { (void)map.put(Map::max_pairs, 1); }, capacity_error);
    assert(map.size() == Map::max_pairs);

    const auto removed_for_capacity = map.remove(2);
    const auto replacement_previous = map.put(Map::max_pairs, 50000);
    assert(removed_for_capacity == 2);
    assert(pineforge::is_na(replacement_previous));
    assert(map.size() == Map::max_pairs);

    auto overwrite_only = Map::new_();
    overwrite_only.put(0, 100);
    overwrite_only.put(1, 101);
    map.put_all(overwrite_only);
    assert(map.get(0) == 100);
    assert(map.get(1) == 101);

    auto mixed = Map::new_();
    mixed.put(0, 200);
    mixed.put(Map::max_pairs + 1, 50001);
    expect_runtime_error([&] { map.put_all(mixed); }, capacity_error);

    // Capacity is preflighted: the overwrite before the new source key was
    // not partially applied.
    assert(map.get(0) == 100);
    assert(!map.contains(Map::max_pairs + 1));
    assert(map.size() == Map::max_pairs);
}

static void test_clear_mutates_alias_only() {
    auto map = PineMap<int, int>::new_();
    map.put(1, 10);
    map.put(2, 20);
    auto alias = map;
    auto independent = map.copy();

    alias.clear();
    assert(map.size() == 0);
    assert(alias.size() == 0);
    assert(independent.size() == 2);
    assert(independent.get(1) == 10);
}

static void test_typed_missing_values_for_primitive_templates() {
    auto doubles = PineMap<int, double>::new_();
    assert(pineforge::is_na(doubles.get(1)));
    const auto removed_double = doubles.remove(1);
    assert(pineforge::is_na(removed_double));

    auto integers = PineMap<std::string, int64_t>::new_();
    assert(pineforge::is_na(integers.get("missing")));
    const auto integer_previous = integers.put("answer", int64_t{42});
    assert(pineforge::is_na(integer_previous));
    assert(integers.get("answer") == 42);

    auto strings = PineMap<bool, std::string>::new_();
    assert(strings.get(true).empty());
    const auto removed_string = strings.remove(false);
    assert(removed_string.empty());

    auto booleans = PineMap<double, bool>::new_();
    assert(!booleans.get(1.5));
    const auto removed_bool = booleans.remove(1.5);
    const auto bool_previous = booleans.put(1.5, true);
    assert(!removed_bool);
    assert(!bool_previous);
    assert(booleans.get(1.5));
}

static void test_copy_of_nested_handle_is_outer_deep_inner_shallow() {
    using Inner = PineMap<int, int>;
    using Outer = PineMap<int, Inner>;

    auto inner = Inner::new_();
    inner.put(1, 10);
    auto outer = Outer::new_();
    const auto outer_previous = outer.put(7, inner);
    assert(pineforge::is_na(outer_previous));

    auto copied = outer.copy();
    copied.get(7).put(2, 20);
    assert(inner.get(2) == 20);
    assert(outer.get(7).get(2) == 20);

    copied.remove(7);
    assert(outer.contains(7));
    assert(!copied.contains(7));
}

static void test_snapshot_surface_is_primitive_only() {
    using Primitive = PineMap<int, double>;
    using UdtValue = PineMap<int, SnapshotUnsafeUdt>;
    using CollectionValue = PineMap<int, std::vector<int>>;
    using HandleValue = PineMap<int, PineMap<int, int>>;

    static_assert(Primitive::snapshot_supported);
    static_assert(has_public_snapshot<Primitive>::value);
    static_assert(!UdtValue::snapshot_supported);
    static_assert(!CollectionValue::snapshot_supported);
    static_assert(!HandleValue::snapshot_supported);
    static_assert(!has_public_snapshot<UdtValue>::value);
    static_assert(!has_public_snapshot<CollectionValue>::value);
    static_assert(!has_public_snapshot<HandleValue>::value);
    static_assert(!pineforge::is_pine_map_snapshot_value_v<int&>);

    // Non-primitive values remain usable for ordinary Pine map operations;
    // only the unsafe public rollback surface is unavailable.
    auto udts = UdtValue::new_();
    const auto udt_previous = udts.put(1, SnapshotUnsafeUdt{7});
    assert(udt_previous.value == 0);
    assert(udts.get(1).value == 7);
}

static void test_snapshot_restores_contents_identity_and_rebinding() {
    auto live = PineMap<std::string, int>::new_();
    live.put("a", 1);
    live.put("b", 2);
    auto alias = live;
    auto snapshot = live.snapshot();

    alias.put("a", 10);
    alias.remove("b");
    alias.put("c", 3);

    auto replacement = PineMap<std::string, int>::new_();
    replacement.put("replacement", 99);
    auto replacement_alias = replacement;
    live = replacement;
    assert(live.contains("replacement"));

    live.restore(snapshot);
    assert((live.keys() == std::vector<std::string>{"a", "b"}));
    assert((live.values() == std::vector<int>{1, 2}));

    // The pre-snapshot alias sees the in-place rollback, and the restored
    // variable has reattached to that same original map ID.
    assert((alias.keys() == std::vector<std::string>{"a", "b"}));
    live.put("d", 4);
    assert(alias.get("d") == 4);

    // Reattaching live must not mutate the temporary replacement identity.
    assert(replacement_alias.size() == 1);
    assert(replacement_alias.get("replacement") == 99);
    assert(!replacement_alias.contains("d"));

    // Snapshots are reusable and remain detached from later mutations.
    alias.put("a", 100);
    alias.remove("b");
    alias.remove("d");
    live.restore(snapshot);
    assert((live.keys() == std::vector<std::string>{"a", "b"}));
    assert((live.values() == std::vector<int>{1, 2}));
}

static void test_snapshot_restores_after_rebind_to_na() {
    using Map = PineMap<std::string, int>;
    auto live = Map::new_();
    live.put("a", 1);
    live.put("b", 2);
    auto alias = live;
    auto snapshot = live.snapshot();

    live = Map{};
    assert(pineforge::is_na(live));
    alias.put("a", 10);
    alias.remove("b");
    alias.put("c", 3);

    live.restore(snapshot);
    assert(!pineforge::is_na(live));
    assert((live.keys() == std::vector<std::string>{"a", "b"}));
    assert((alias.values() == std::vector<int>{1, 2}));
    live.put("d", 4);
    assert(alias.get("d") == 4);

    // A snapshot is reusable even after another live -> na transition.
    live = Map{};
    assert(pineforge::is_na(live));
    alias.put("a", 100);
    alias.remove("b");
    alias.remove("d");
    live.restore(snapshot);
    assert((live.keys() == std::vector<std::string>{"a", "b"}));
    assert((live.values() == std::vector<int>{1, 2}));
    live.put("e", 5);
    assert(alias.get("e") == 5);
}

static void test_two_alias_members_restore_to_one_identity() {
    auto first = PineMap<int, int>::new_();
    first.put(1, 10);
    first.put(2, 20);
    auto second = first;

    auto first_snapshot = first.snapshot();
    auto second_snapshot = second.snapshot();

    first.put(1, 100);
    second.put(3, 30);
    first = PineMap<int, int>::new_();
    second = PineMap<int, int>::new_();

    first.restore(first_snapshot);
    second.restore(second_snapshot);
    assert((first.keys() == std::vector<int>{1, 2}));
    assert((second.keys() == std::vector<int>{1, 2}));

    second.put(4, 40);
    assert(first.get(4) == 40);
    first.remove(1);
    assert(!second.contains(1));
}

static void test_snapshot_keeps_original_identity_alive() {
    using Map = PineMap<int, int>;
    auto snapshot = [] {
        auto ephemeral = Map::new_();
        ephemeral.put(7, 70);
        return ephemeral.snapshot();
    }();

    auto restored = Map::new_();
    restored.put(8, 80);
    restored.restore(snapshot);
    assert(restored.size() == 1);
    assert(restored.get(7) == 70);
    assert(!restored.contains(8));
}

static void test_empty_maps_materialize_fixed_headers_only() {
    using Map = PineMap<std::string, double>;
    using Holder = pineforge::detail::MapStoreHolder;
    using Stats = pineforge::detail::o1::Stats;

    auto holder = std::make_shared<Holder>();
    pineforge::detail::MapStoreScope scope(holder);
    const Stats baseline = holder->store->stats();
    constexpr std::size_t map_count = 2000;
    constexpr std::uint64_t logical_bytes_per_map = UINT64_C(3862176);
    constexpr std::uint64_t max_backing_bytes_per_map = UINT64_C(512) * 1024;
    constexpr std::array<std::size_t, 3> marks{{1, 100, map_count}};
    std::vector<Map> maps;
    maps.reserve(map_count);

    std::size_t next_mark = 0;
    for (std::size_t index = 0; index < map_count; ++index) {
        maps.push_back(Map::new_());
        if (index + 1 == marks[next_mark]) {
            const Stats checkpoint = holder->store->stats();
            const std::uint64_t count = marks[next_mark];
            assert(checkpoint.objects_live == 2 * count);
            assert(checkpoint.bytes_live == count * logical_bytes_per_map);
            assert(checkpoint.bytes_backing >= baseline.bytes_backing);
            assert(checkpoint.bytes_backing - baseline.bytes_backing <=
                   count * max_backing_bytes_per_map);
            ++next_mark;
        }
    }

    assert(next_mark == marks.size());
    assert(maps.back().size() == 0);

    // The same handles can execute a real put/get workload after the empty
    // construction checkpoints. These values are inline, so no payload object
    // changes the two-map-object accounting above.
    for (std::size_t index = 0; index < maps.size(); index += 500) {
        maps[index].put("bar", static_cast<double>(index));
        assert(maps[index].get("bar") == static_cast<double>(index));
    }
}

static void test_map_growth_rolls_back_and_abandons_cleanly() {
    using Map = PineMap<std::string, int>;
    auto holder = std::make_shared<pineforge::detail::MapStoreHolder>();
    pineforge::detail::MapStoreScope scope(holder);
    auto map = Map::new_();
    const auto fill = [](Map& target) {
        for (int index = 0; index < 40; ++index)
            target.put("k" + std::to_string(index), index);
    };

    {
        auto transaction = holder->store->begin();
        fill(map);
        assert(map.size() == 40);
        assert(map.get("k39") == 39);
        transaction->rollback();
    }
    assert(map.size() == 0);
    assert(!map.contains("k39"));

    {
        auto transaction = holder->store->begin();
        fill(map);
        assert(map.size() == 40);
    }
    assert(map.size() == 0);
    assert(!map.contains("k0"));

    {
        auto transaction = holder->store->begin();
        fill(map);
        auto joined = holder->store->begin();
        joined->rollback();
        bool rollback_only = false;
        try {
            transaction->commit();
        } catch (const pineforge::detail::o1::RollbackOnly&) {
            rollback_only = true;
        }
        assert(rollback_only);
    }
    assert(map.size() == 0);
    assert(!map.contains("k20"));
}

static void test_map_clear_copy_and_snapshot_cross_growth_step() {
    using Map = PineMap<std::string, int>;
    auto map = Map::new_();
    const std::vector<std::string> expected_keys{
        "k0",  "k1",  "k2",  "k3",  "k4",  "k5",  "k6",  "k7",
        "k8",  "k9",  "k10", "k11", "k12", "k13", "k14", "k15"};
    const std::vector<int> expected_values{
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int index = 0; index < 16; ++index)
        map.put("k" + std::to_string(index), index);

    auto independent = map.copy();
    auto snapshot = map.snapshot();
    map.clear();
    assert(map.size() == 0);
    assert(independent.keys() == expected_keys);
    assert(independent.values() == expected_values);

    map.restore(snapshot);
    assert(map.keys() == expected_keys);
    assert(map.values() == expected_values);
    map.put("k16", 16);
    assert(map.size() == 17);
    map.restore(snapshot);
    assert(map.keys() == expected_keys);
    assert(map.values() == expected_values);
}

static void test_heap_value_maps_grow_the_value_table_on_use() {
    using Map = PineMap<int, HeapValueProbe>;
    // Open the shared Store before counting this map's own allocations.
    auto holder = std::make_shared<pineforge::detail::MapStoreHolder>();
    pineforge::detail::MapStoreScope scope(holder);
    HeapValueProbe::default_constructed = 0;
    const std::size_t bytes_before = global_allocation::live_bytes;
    global_allocation::recording = true;
    {
        auto map = Map::new_();
        const std::size_t creation_bytes = global_allocation::live_bytes - bytes_before;
        // reserve() constructs no probes, but its full-capacity allocation
        // alone reaches this limit on the eager implementation.
        assert(creation_bytes < std::size_t{50000} * sizeof(HeapValueProbe));
        assert(HeapValueProbe::default_constructed == 0);

        map.put(7, HeapValueProbe{42});
        assert(map.size() == 1);
        assert(map.get(7).value == 42);
    }
    // Destroy every recorded block while byte accounting is still enabled.
    global_allocation::recording = false;
}

int main() {
    static_assert(std::is_copy_constructible_v<PineMap<int, double>>);
    static_assert(std::is_copy_assignable_v<PineMap<int, double>>);
    static_assert(std::is_nothrow_default_constructible_v<PineMap<int, double>>);
    static_assert(std::is_copy_constructible_v<PineMap<int, double>::Snapshot>);
    static_assert(std::is_copy_assignable_v<PineMap<int, double>::Snapshot>);
    static_assert(std::is_same_v<
                  decltype(std::declval<PineMap<int, double>&>().put(1, 2.0)),
                  double>);

    test_na_id_and_stable_errors();
    test_copy_aliases_and_explicit_copy_is_independent();
    test_move_preserves_source_handle_alias();
    test_put_get_and_stable_insertion_order();
    test_float_keys_use_pine_na_and_zero_equality();
    test_remove_updates_order_and_reinsert_appends();
    test_put_all_overwrites_in_source_order();
    test_50000_pair_capacity_and_put_all_preflight();
    test_clear_mutates_alias_only();
    test_typed_missing_values_for_primitive_templates();
    test_copy_of_nested_handle_is_outer_deep_inner_shallow();
    test_snapshot_surface_is_primitive_only();
    test_snapshot_restores_contents_identity_and_rebinding();
    test_snapshot_restores_after_rebind_to_na();
    test_two_alias_members_restore_to_one_identity();
    test_snapshot_keeps_original_identity_alive();
    test_empty_maps_materialize_fixed_headers_only();
    test_map_growth_rolls_back_and_abandons_cleanly();
    test_map_clear_copy_and_snapshot_cross_growth_step();
    test_heap_value_maps_grow_the_value_table_on_use();

    std::printf("All test_map tests passed.\n");
    return 0;
}
